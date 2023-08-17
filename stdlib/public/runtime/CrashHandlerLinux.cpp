//===--- CrashHandlerLinux.cpp - Swift crash handler for Linux ----------- ===//
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
// The Linux crash handler implementation.
//
//===----------------------------------------------------------------------===//

#ifdef __linux__

#include <linux/capability.h>
#include <linux/futex.h>

#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "swift/Runtime/Backtrace.h"

#include <cstring>

// Run the memserver in a thread (0) or separate process (1)
#define MEMSERVER_USE_PROCESS 0

#ifndef lengthof
#define lengthof(x)     (sizeof(x) / sizeof(x[0]))
#endif

using namespace swift::runtime::backtrace;

namespace {

void handle_fatal_signal(int signum, siginfo_t *pinfo, void *uctx);
void suspend_other_threads(struct thread *self);
void resume_other_threads();
void take_thread_lock();
void release_thread_lock();
void notify_paused();
uint32_t currently_paused();
void wait_paused(uint32_t expected, const struct timespec *timeout);
int  memserver_start();
int  memserver_entry(void *);
bool run_backtracer(int fd);
void format_unsigned(unsigned u, char buffer[22]);

ssize_t safe_read(int fd, void *buf, size_t len) {
  uint8_t *ptr = (uint8_t *)buf;
  uint8_t *end = ptr + len;
  ssize_t total = 0;

  while (ptr < end) {
    ssize_t ret;
    do {
      ret = read(fd, buf, len);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0)
      return ret;
    total += ret;
    ptr += ret;
    len -= ret;
  }

  return total;
}

ssize_t safe_write(int fd, const void *buf, size_t len) {
  const uint8_t *ptr = (const uint8_t *)buf;
  const uint8_t *end = ptr + len;
  ssize_t total = 0;

  while (ptr < end) {
    ssize_t ret;
    do {
      ret = write(fd, buf, len);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0)
      return ret;
    total += ret;
    ptr += ret;
    len -= ret;
  }

  return total;
}

CrashInfo crashInfo;

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
    /* No, so set one up; note that if we end up having to do a PLT lookup
       for a function we call from the signal handler, we need additional
       stack space for the dynamic linker, or we'll just explode.  That's
       what the extra 16KB is for here. */
    ss.ss_flags = 0;
    ss.ss_size = SIGSTKSZ + 16384;
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

// Older glibc and musl don't have these two syscalls
pid_t
gettid()
{
  return (pid_t)syscall(SYS_gettid);
}

int
tgkill(int tgid, int tid, int sig) {
  return syscall(SYS_tgkill, tgid, tid, sig);
}

void
reset_signal(int signum)
{
  struct sigaction sa;
  sa.sa_handler = SIG_DFL;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);

  sigaction(signum, &sa, NULL);
}

void
handle_fatal_signal(int signum,
                    siginfo_t *pinfo,
                    void *uctx)
{
  int old_err = errno;
  struct thread self = { 0, (int64_t)gettid(), (uint64_t)uctx };

  // Prevent this from exploding if more than one thread gets here at once
  suspend_other_threads(&self);

  // Remove our signal handlers; crashes should kill us here
  for (unsigned n = 0; n < lengthof(signalsToHandle); ++n)
    reset_signal(signalsToHandle[n]);

  // Fill in crash info
  crashInfo.crashing_thread = self.tid;
  crashInfo.signal = signum;
  crashInfo.fault_address = (uint64_t)pinfo->si_addr;

  // Start the memory server
  int fd = memserver_start();

  // Actually start the backtracer
  run_backtracer(fd);

#if !MEMSERVER_USE_PROCESS
  /* If the memserver is in-process, it may have set signal handlers,
     so reset SIGSEGV and SIGBUS again */
  reset_signal(SIGSEGV);
  reset_signal(SIGBUS);
#endif

  // Restart the other threads
  resume_other_threads();

  // Restore errno and exit (to crash)
  errno = old_err;
}

// .. Thread handling ..........................................................

void
reset_threads(struct thread *first) {
  __atomic_store_n(&crashInfo.thread_list, (uint64_t)first, __ATOMIC_RELEASE);
}

void
add_thread(struct thread *thread) {
  uint64_t next = __atomic_load_n(&crashInfo.thread_list, __ATOMIC_ACQUIRE);
  do {
    thread->next = next;
  } while (!__atomic_compare_exchange_n(&crashInfo.thread_list, &next,
                                        (uint64_t)thread,
                                        false,
                                        __ATOMIC_RELEASE, __ATOMIC_ACQUIRE));
}

bool
seen_thread(pid_t tid) {
  uint64_t next = __atomic_load_n(&crashInfo.thread_list, __ATOMIC_ACQUIRE);
  while (next) {
    struct thread *pthread = (struct thread *)next;
    if (pthread->tid == tid)
      return true;
    next = pthread->next;
  }
  return false;
}

void
pause_thread(int signum __attribute__((unused)),
             siginfo_t *pinfo  __attribute__((unused)),
             void *uctx)
{
  int old_err = errno;
  struct thread self = { 0, (int64_t)gettid(), (uint64_t)uctx };

  add_thread(&self);

  notify_paused();

  take_thread_lock();
  release_thread_lock();

  errno = old_err;
}

struct linux_dirent64 {
  ino64_t        d_ino;
  off64_t        d_off;
  unsigned short d_reclen;
  unsigned char  d_type;
  char           d_name[256];
};

int
getdents(int fd, void *buf, size_t bufsiz)
{
  return syscall(SYS_getdents64, fd, buf, bufsiz);
}

/* Find the signal to use to suspend the given thread.

   Sadly, libdispatch blocks SIGUSR1, so we can't just use that everywhere;
   and on Ubuntu 20.04 *something* is starting a thread with SIGPROF blocked,
   so we can't just use that either.

   We also can't modify the signal mask for another thread, since there is
   no syscall to do that.

   As a workaround, read /proc/<pid>/task/<tid>/status to find the signal
   mask so that we can decide which signal to try and send. */
int
signal_for_suspend(int pid, int tid)
{
  char pid_buffer[22];
  char tid_buffer[22];

  format_unsigned((unsigned)pid, pid_buffer);
  format_unsigned((unsigned)tid, tid_buffer);

  char status_file[6 + 22 + 6 + 22 + 7 + 1];

  strcpy(status_file, "/proc/");    // 6
  strcat(status_file, pid_buffer);  // 22
  strcat(status_file, "/task/");    // 6
  strcat(status_file, tid_buffer);  // 22
  strcat(status_file, "/status");   // 7 + 1 for NUL

  int fd = open(status_file, O_RDONLY);
  if (fd < 0)
    return -1;

  enum match_state {
    Matching,
    EatLine,
    AfterMatch,
    InHex,

    // states after this terminate the loop
    Done,
    Bad
  };

  enum match_state state = Matching;
  const char *toMatch = "SigBlk:";
  const char *matchPtr = toMatch;
  char buffer[256];
  uint64_t mask = 0;
  ssize_t count;
  while (state < Done && (count = read(fd, buffer, sizeof(buffer))) > 0) {
    char *ptr = buffer;
    char *end = buffer + count;

    while (state < Done && ptr < end) {
      int ch = *ptr++;

      switch (state) {
      case Matching:
        if (ch != *matchPtr) {
          state = EatLine;
          matchPtr = toMatch;
        } else if (!*++matchPtr) {
          state = AfterMatch;
        }
        break;
      case EatLine:
        if (ch == '\n')
          state = Matching;
        break;
      case AfterMatch:
        if (ch == ' ' || ch == '\t') {
          break;
        }
        state = InHex;
        SWIFT_FALLTHROUGH;
      case InHex:
        if (ch >= '0' && ch <= '9') {
          mask = (mask << 4) | (ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
          mask = (mask << 4) | (ch - 'a' + 10);
        } else if (ch >= 'A' && ch <= 'F') {
          mask = (mask << 4) | (ch - 'A' + 10);
        } else if (ch == '\n') {
          state = Done;
          break;
        } else {
          state = Bad;
        }
        break;
      case Done:
      case Bad:
        break;
      }
    }
  }

  close(fd);

  if (state == Done) {
    if (!(mask & (1 << (SIGUSR1 - 1))))
      return SIGUSR1;
    else if (!(mask & (1 << (SIGUSR2 - 1))))
      return SIGUSR2;
    else if (!(mask & (1 << (SIGPROF - 1))))
      return SIGPROF;
    else
      return -1;
  }

  return -1;
}

// Write a string to stderr
void
warn(const char *str) {
  write(STDERR_FILENO, str, strlen(str));
}

/* Stop all other threads in this process; we do this by establishing a
   signal handler for SIGPROF, then iterating through the threads sending
   SIGPROF.

   Finding the other threads is a pain, because Linux has no actual API
   for that; instead, you have to read /proc.  Unfortunately, opendir()
   and readdir() are not async signal safe, so we get to do this with
   the getdents system call instead.

   The SIGPROF signals also serve to build the thread list. */
void
suspend_other_threads(struct thread *self)
{
  struct sigaction sa, sa_old_prof, sa_old_usr1, sa_old_usr2;

  // Take the lock
  take_thread_lock();

  // Start the thread list with this thread
  reset_threads(self);

  // Swap out the signal handlers first
  sigfillset(&sa.sa_mask);
  sa.sa_flags = 0;
  sa.sa_handler = NULL;
  sa.sa_sigaction = pause_thread;

  sigaction(SIGPROF, &sa, &sa_old_prof);
  sigaction(SIGUSR1, &sa, &sa_old_usr1);
  sigaction(SIGUSR2, &sa, &sa_old_usr2);

  /* Now scan /proc/self/task to get the tids of the threads in this
     process.  We need to ignore our own thread. */
  int fd = open("/proc/self/task",
                O_RDONLY|O_NDELAY|O_DIRECTORY|O_LARGEFILE|O_CLOEXEC);
  int our_pid = getpid();
  char buffer[4096];
  size_t offset = 0;
  size_t count = 0;

  unsigned max_loops = 15;
  uint32_t pending = 0;

  do {
    uint32_t paused = currently_paused();

    pending = 0;

    lseek(fd, 0, SEEK_SET);

    for (;;) {
      if (offset >= count) {
        ssize_t bytes = getdents(fd, buffer, sizeof(buffer));
        if (bytes <= 0)
          break;
        count = (size_t)bytes;
        offset = 0;
      }

      struct linux_dirent64 *dp = (struct linux_dirent64 *)&buffer[offset];
      offset += dp->d_reclen;

      if (strcmp(dp->d_name, ".") == 0
          || strcmp(dp->d_name, "..") == 0)
        continue;

      int tid = atoi(dp->d_name);

      if ((int64_t)tid != self->tid && !seen_thread(tid)) {
        int sig_to_use = signal_for_suspend(our_pid, tid);

        if (sig_to_use > 0) {
          tgkill(our_pid, tid, sig_to_use);
          ++pending;
        } else {
          warn("swift-runtime: unable to suspend thread ");
          warn(dp->d_name);
          warn("\n");
        }
      }
    }

    // If we find no new threads, we're done
    if (!pending)
      break;

    // Wait for the threads to suspend
    struct timespec timeout = { 2, 0 };
    wait_paused(paused + pending, &timeout);
  } while (max_loops--);

  // Close the directory
  close(fd);

  // Finally, reset the signal handlers
  sigaction(SIGPROF, &sa_old_prof, NULL);
  sigaction(SIGUSR1, &sa_old_usr1, NULL);
  sigaction(SIGUSR2, &sa_old_usr2, NULL);
}

void
resume_other_threads()
{
  // All we need to do here is release the lock.
  release_thread_lock();
}

// .. Locking ..................................................................

/* We use a futex to block the threads; we also use one to let us work out
   when all the threads we've asked to pause have actually paused. */
int
futex(uint32_t *uaddr, int futex_op, uint32_t val,
      const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3)
{
  return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
}

uint32_t thread_lock = 0;

void
take_thread_lock()
{
  do {
    uint32_t zero = 0;
    if (__atomic_compare_exchange_n(&thread_lock,
                                    &zero,
                                    1,
                                    true,
                                    __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED))
      return;
  } while (!futex(&thread_lock, FUTEX_WAIT, 1, NULL, NULL, 0)
           || errno == EAGAIN);
}

void
release_thread_lock()
{
  __atomic_store_n(&thread_lock, 0, __ATOMIC_RELEASE);
  futex(&thread_lock, FUTEX_WAKE, 1, NULL, NULL, 0);
}

uint32_t threads_paused = 0;

void
notify_paused()
{
  __atomic_fetch_add(&threads_paused, 1, __ATOMIC_RELEASE);
  futex(&threads_paused, FUTEX_WAKE, 1, NULL, NULL, 0);
}

uint32_t
currently_paused()
{
  return __atomic_load_n(&threads_paused, __ATOMIC_ACQUIRE);
}

void
wait_paused(uint32_t expected, const struct timespec *timeout)
{
  uint32_t current;
  do {
    current = __atomic_load_n(&threads_paused, __ATOMIC_ACQUIRE);
    if (current == expected)
      return;
  } while (!futex(&threads_paused, FUTEX_WAIT, current, timeout, NULL, 0)
           || errno == EAGAIN);
}

// .. Memory server ............................................................

/* The memory server exists so that we can gain access to the crashing
   process's memory space from the backtracer without having to use ptrace()
   or process_vm_readv(), both of which need CAP_SYS_PTRACE.

   We don't want to require CAP_SYS_PTRACE because we're potentially being
   used inside of a Docker container, which won't have that enabled. */

char memserver_stack[4096] __attribute__((aligned(SWIFT_PAGE_SIZE)));
char memserver_buffer[4096];
int memserver_fd;
bool memserver_has_ptrace;
sigjmp_buf memserver_fault_buf;
pid_t memserver_pid;

int
memserver_start()
{
  int ret;
  int fds[2];

  ret = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
  if (ret < 0)
    return ret;

  memserver_fd = fds[0];
  ret = clone(memserver_entry, memserver_stack + sizeof(memserver_stack),
#if MEMSERVER_USE_PROCESS
              0,
#else
              CLONE_THREAD | CLONE_VM | CLONE_FILES
              | CLONE_FS | CLONE_IO | CLONE_SIGHAND,
#endif
              NULL);
  if (ret < 0)
    return ret;

#if MEMSERVER_USE_PROCESS
  memserver_pid = ret;

  /* Tell the Yama LSM module, if it's running, that it's OK for
     the memserver to read process memory */
  prctl(PR_SET_PTRACER, ret);

  close(fds[0]);
#else
  memserver_pid = getpid();
#endif

  return fds[1];
}

void
memserver_fault(int sig) {
  (void)sig;
  siglongjmp(memserver_fault_buf, -1);
}

ssize_t __attribute__((noinline))
memserver_read(void *to, const void *from, size_t len) {
  if (memserver_has_ptrace) {
// This won't run for older Android APIs anyway, but it can't be compiled
// either, as process_vm_readv() isn't available.
#if !(defined(__ANDROID_API__) && __ANDROID_API__ < 23)
    struct iovec local = { to, len };
    struct iovec remote = { const_cast<void *>(from), len };
    return process_vm_readv(memserver_pid, &local, 1, &remote, 1, 0);
#endif
  } else {
    if (!sigsetjmp(memserver_fault_buf, 1)) {
      memcpy(to, from, len);
      return len;
    } else {
      return 1;
    }
  }
}

int
memserver_entry(void *dummy __attribute__((unused))) {
  int fd = memserver_fd;
  int result = 1;

#if MEMSERVER_USE_PROCESS
  prctl(PR_SET_NAME, "[backtrace]");
#endif

// process_vm_readv() is not available for older Android APIs.
#if defined(__ANDROID_API__) && __ANDROID_API__ < 23
  memserver_has_ptrace = false;
#else
  memserver_has_ptrace = !!prctl(PR_CAPBSET_READ, CAP_SYS_PTRACE);
#endif

  if (!memserver_has_ptrace) {
    struct sigaction sa;
    sigfillset(&sa.sa_mask);
    sa.sa_handler = memserver_fault;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
  }

  for (;;) {
    struct memserver_req req;
    ssize_t ret;

    ret = safe_read(fd, &req, sizeof(req));
    if (ret != sizeof(req))
      break;

    uint64_t addr = req.addr;
    uint64_t bytes = req.len;

    while (bytes) {
      uint64_t todo = (bytes < sizeof(memserver_buffer)
                       ? bytes : sizeof(memserver_buffer));

      ret = memserver_read(memserver_buffer, (void *)addr, (size_t)todo);

      struct memserver_resp resp;

      resp.addr = addr;
      resp.len = ret;

      ret = safe_write(fd, &resp, sizeof(resp));
      if (ret != sizeof(resp))
        goto fail;

      if (resp.len < 0)
        break;

      ret = safe_write(fd, memserver_buffer, resp.len);
      if (ret != resp.len)
        goto fail;

      addr += resp.len;
      bytes -= resp.len;
    }
  }

  result = 0;

 fail:
  close(fd);
  return result;
}

// .. Starting the backtracer ..................................................

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
run_backtracer(int memserver_fd)
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
  return _swift_spawnBacktracer(backtracer_argv, memserver_fd);
}

} // namespace

#endif // __linux__

