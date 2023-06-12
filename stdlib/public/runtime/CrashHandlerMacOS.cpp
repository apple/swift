//===--- CrashHandlerMacOS.cpp - Swift crash handler for macOS ----------- ===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// The macOS crash handler implementation.
//
// We use signal handling rather than trying to use Mach exceptions here,
// because the latter would entail running a separate Mach server thread, and
// creates a much greater risk of interfering with the system wide Crash
// Reporter, which is a no-no.
//
//===----------------------------------------------------------------------===//

#ifdef __APPLE__

#include <TargetConditionals.h>

#if TARGET_OS_OSX || TARGET_OS_MACCATALYST

#include <mach/mach.h>
#include <mach/task.h>
#include <mach/thread_act.h>

#include <sys/mman.h>
#include <sys/ucontext.h>
#include <sys/wait.h>

#include <os/lock.h>

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>

#include "swift/Runtime/Backtrace.h"

#include <cstring>

#ifndef lengthof
#define lengthof(x)     (sizeof(x) / sizeof(x[0]))
#endif

using namespace swift::runtime::backtrace;

namespace {

void handle_fatal_signal(int signum, siginfo_t *pinfo, void *uctx);
void suspend_other_threads();
void resume_other_threads();
bool run_backtracer(void);

CrashInfo crashInfo;

os_unfair_lock crashLock = OS_UNFAIR_LOCK_INIT;

const int signalsToHandle[] = {
  SIGQUIT,
  SIGABRT,
  SIGBUS,
  SIGFPE,
  SIGILL,
  SIGSEGV,
  SIGTRAP
};

} // namespace

namespace swift {
namespace runtime {
namespace backtrace {

SWIFT_RUNTIME_STDLIB_INTERNAL int
_swift_installCrashHandler()
{
  stack_t ss;

  // See if an alternate signal stack already exists
  if (sigaltstack(NULL, &ss) < 0)
    return errno;

  if (ss.ss_sp == 0) {
    // No, so set one up
    ss.ss_flags = 0;
    ss.ss_size = SIGSTKSZ;
    ss.ss_sp = mmap(0, ss.ss_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ss.ss_sp == MAP_FAILED)
      return errno;

    if (sigaltstack(&ss, NULL) < 0)
      return errno;
  }

  // Now register signal handlers
  struct sigaction sa;

  sigfillset(&sa.sa_mask);
  for (unsigned n = 0; n < lengthof(signalsToHandle); ++n) {
    sigdelset(&sa.sa_mask, signalsToHandle[n]);
  }

  sa.sa_handler = NULL;
  sa.sa_flags = SA_ONSTACK | SA_SIGINFO | SA_NODEFER;
  sa.sa_sigaction = handle_fatal_signal;

  for (unsigned n = 0; n < lengthof(signalsToHandle); ++n) {
    struct sigaction osa;

    // See if a signal handler for this signal is already installed
    if (sigaction(signalsToHandle[n], NULL, &osa) < 0)
      return errno;

    if (osa.sa_handler == SIG_DFL) {
      // No, so install ours
      if (sigaction(signalsToHandle[n], &sa, NULL) < 0)
        return errno;
    }
  }

  return 0;
}

} // namespace backtrace
} // namespace runtime
} // namespace swift

namespace {

void
suspend_other_threads()
{
  os_unfair_lock_lock(&crashLock);

  thread_t self = mach_thread_self();
  thread_act_array_t threads;
  mach_msg_type_number_t count = 0;

  kern_return_t kr = task_threads(mach_task_self(), &threads, &count);

  if (kr != KERN_SUCCESS)
    return;

  for (unsigned n = 0; n < count; ++n) {
    if (threads[n] == self)
      continue;

    // Ignore the results of these two; if they fail there's nothing we can do
    (void)thread_suspend(threads[n]);
    (void)mach_port_deallocate(mach_task_self(), threads[n]);
  }

  vm_deallocate(mach_task_self(),
                (vm_address_t)threads,
                count * sizeof(threads[0]));

  os_unfair_lock_unlock(&crashLock);
}

void
resume_other_threads()
{
  os_unfair_lock_lock(&crashLock);

  thread_t self = mach_thread_self();
  thread_act_array_t threads;
  mach_msg_type_number_t count = 0;

  kern_return_t kr = task_threads(mach_task_self(), &threads, &count);

  if (kr != KERN_SUCCESS)
    return;

  for (unsigned n = 0; n < count; ++n) {
    if (threads[n] == self)
      continue;

    // Ignore the results of these two; if they fail there's nothing we can do
    (void)thread_resume(threads[n]);
    (void)mach_port_deallocate(mach_task_self(), threads[n]);
  }

  vm_deallocate(mach_task_self(),
                (vm_address_t)threads,
                count * sizeof(threads[0]));

  os_unfair_lock_unlock(&crashLock);
}

void
handle_fatal_signal(int signum,
                    siginfo_t *pinfo,
                    void *uctx)
{
  int old_err = errno;

  // Prevent this from exploding if more than one thread gets here at once
  suspend_other_threads();

  // Remove our signal handlers; crashes should kill us here
  for (unsigned n = 0; n < lengthof(signalsToHandle); ++n)
    signal(signalsToHandle[n], SIG_DFL);

  // Get our thread identifier
  thread_identifier_info_data_t ident_info;
  mach_msg_type_number_t ident_size = THREAD_IDENTIFIER_INFO_COUNT;

  int ret = thread_info(mach_thread_self(),
                        THREAD_IDENTIFIER_INFO,
                        (int *)&ident_info,
                        &ident_size);
  if (ret != KERN_SUCCESS)
    return;

  // Fill in crash info
  crashInfo.crashing_thread = ident_info.thread_id;
  crashInfo.signal = signum;
  crashInfo.fault_address = (uint64_t)pinfo->si_addr;
  crashInfo.mctx = (uint64_t)(((ucontext_t *)uctx)->uc_mcontext);

  /* Start the backtracer; this will suspend the process, so there's no need
     to try to suspend other threads from here. */
  run_backtracer();

  // Restart the other threads
  resume_other_threads();

  // Restore errno and exit (to crash)
  errno = old_err;
}

char addr_buf[18];
char timeout_buf[22];
char limit_buf[22];
char top_buf[22];
const char *backtracer_argv[] = {
  "swift-backtrace",            // 0
  "--unwind",                   // 1
  "precise",                    // 2
  "--demangle",                 // 3
  "true",                       // 4
  "--interactive",              // 5
  "true",                       // 6
  "--color",                    // 7
  "true",                       // 8
  "--timeout",                  // 9
  timeout_buf,                  // 10
  "--preset",                   // 11
  "friendly",                   // 12
  "--crashinfo",                // 13
  addr_buf,                     // 14
  "--threads",                  // 15
  "preset",                     // 16
  "--registers",                // 17
  "preset",                     // 18
  "--images",                   // 19
  "preset",                     // 20
  "--limit",                    // 21
  limit_buf,                    // 22
  "--top",                      // 23
  top_buf,                      // 24
  "--sanitize",                 // 25
  "preset",                     // 26
  "--cache",                    // 27
  "true",                       // 28
  "--output-to",                // 29
  "stdout",                     // 30
  NULL
};

// We can't call sprintf() here because we're in a signal handler,
// so we need to be async-signal-safe.
void
format_address(uintptr_t addr, char buffer[18])
{
  char *ptr = buffer + 18;
  *--ptr = '\0';
  while (ptr > buffer) {
    char digit = '0' + (addr & 0xf);
    if (digit > '9')
      digit += 'a' - '0' - 10;
    *--ptr = digit;
    addr >>= 4;
    if (!addr)
      break;
  }

  // Left-justify in the buffer
  if (ptr > buffer) {
    char *pt2 = buffer;
    while (*ptr)
      *pt2++ = *ptr++;
    *pt2++ = '\0';
  }
}
void
format_address(const void *ptr, char buffer[18])
{
  format_address(reinterpret_cast<uintptr_t>(ptr), buffer);
}

// See above; we can't use sprintf() here.
void
format_unsigned(unsigned u, char buffer[22])
{
  char *ptr = buffer + 22;
  *--ptr = '\0';
  while (ptr > buffer) {
    char digit = '0' + (u % 10);
    *--ptr = digit;
    u /= 10;
    if (!u)
      break;
  }

  // Left-justify in the buffer
  if (ptr > buffer) {
    char *pt2 = buffer;
    while (*ptr)
      *pt2++ = *ptr++;
    *pt2++ = '\0';
  }
}

const char *
trueOrFalse(bool b) {
  return b ? "true" : "false";
}

const char *
trueOrFalse(OnOffTty oot) {
  return trueOrFalse(oot == OnOffTty::On);
}

bool
run_backtracer()
{
  // Set-up the backtracer's command line arguments
  switch (_swift_backtraceSettings.algorithm) {
  case UnwindAlgorithm::Fast:
    backtracer_argv[2] = "fast";
    break;
  default:
    backtracer_argv[2] = "precise";
    break;
  }

  // (The TTY option has already been handled at this point, so these are
  //  all either "On" or "Off".)
  backtracer_argv[4] = trueOrFalse(_swift_backtraceSettings.demangle);
  backtracer_argv[6] = trueOrFalse(_swift_backtraceSettings.interactive);
  backtracer_argv[8] = trueOrFalse(_swift_backtraceSettings.color);

  switch (_swift_backtraceSettings.threads) {
  case ThreadsToShow::Preset:
    backtracer_argv[16] = "preset";
    break;
  case ThreadsToShow::All:
    backtracer_argv[16] = "all";
    break;
  case ThreadsToShow::Crashed:
    backtracer_argv[16] = "crashed";
    break;
  }

  switch (_swift_backtraceSettings.registers) {
  case RegistersToShow::Preset:
    backtracer_argv[18] = "preset";
    break;
  case RegistersToShow::None:
    backtracer_argv[18] = "none";
    break;
  case RegistersToShow::All:
    backtracer_argv[18] = "all";
    break;
  case RegistersToShow::Crashed:
    backtracer_argv[18] = "crashed";
    break;
  }

  switch (_swift_backtraceSettings.images) {
  case ImagesToShow::Preset:
    backtracer_argv[20] = "preset";
    break;
  case ImagesToShow::None:
    backtracer_argv[20] = "none";
    break;
  case ImagesToShow::All:
    backtracer_argv[20] = "all";
    break;
  case ImagesToShow::Mentioned:
    backtracer_argv[20] = "mentioned";
    break;
  }

  switch (_swift_backtraceSettings.preset) {
  case Preset::Friendly:
    backtracer_argv[12] = "friendly";
    break;
  case Preset::Medium:
    backtracer_argv[12] = "medium";
    break;
  default:
    backtracer_argv[12] = "full";
    break;
  }

  switch (_swift_backtraceSettings.sanitize) {
  case SanitizePaths::Preset:
    backtracer_argv[26] = "preset";
    break;
  case SanitizePaths::Off:
    backtracer_argv[26] = "false";
    break;
  case SanitizePaths::On:
    backtracer_argv[26] = "true";
    break;
  }

  switch (_swift_backtraceSettings.outputTo) {
  case OutputTo::Stdout:
    backtracer_argv[30] = "stdout";
    break;
  case OutputTo::Auto: // Shouldn't happen, but if it does pick stderr
  case OutputTo::Stderr:
    backtracer_argv[30] = "stderr";
    break;
  }

  backtracer_argv[28] = trueOrFalse(_swift_backtraceSettings.cache);

  format_unsigned(_swift_backtraceSettings.timeout, timeout_buf);

  if (_swift_backtraceSettings.limit < 0)
    std::strcpy(limit_buf, "none");
  else
    format_unsigned(_swift_backtraceSettings.limit, limit_buf);

  format_unsigned(_swift_backtraceSettings.top, top_buf);
  format_address(&crashInfo, addr_buf);

  // Actually execute it
  return _swift_spawnBacktracer(backtracer_argv);
}

} // namespace

#endif // TARGET_OS_OSX || TARGET_OS_MACCATALYST

#endif // __APPLE__

