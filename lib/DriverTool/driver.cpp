//===--- driver.cpp - Swift Compiler Driver -------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This is the entry point to the swift compiler driver.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsDriver.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Basic/InitializeSwiftModules.h"
#include "swift/Basic/PrettyStackTrace.h"
#include "swift/Basic/Program.h"
#include "swift/Basic/TaskQueue.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Driver/Compilation.h"
#include "swift/Driver/Driver.h"
#include "swift/Driver/FrontendUtil.h"
#include "swift/Driver/Job.h"
#include "swift/Driver/ToolChain.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/FrontendTool/FrontendTool.h"
#include "swift/DriverTool/DriverTool.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Errno.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <memory>
#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace swift;
using namespace swift::driver;

std::string getExecutablePath(const char *FirstArg) {
  void *P = (void *)(intptr_t)getExecutablePath;
  return llvm::sys::fs::getMainExecutable(FirstArg, P);
}

/// Run 'sil-opt'
extern int sil_opt_main(ArrayRef<const char *> argv, void *MainAddr);

/// Run 'sil-func-extractor'
extern int sil_func_extractor_main(ArrayRef<const char *> argv, void *MainAddr);

/// Run 'sil-nm'
extern int sil_nm_main(ArrayRef<const char *> argv, void *MainAddr);

/// Run 'sil-llvm-gen'
extern int sil_llvm_gen_main(ArrayRef<const char *> argv, void *MainAddr);

/// Run 'sil-passpipeline-dumper'
extern int sil_passpipeline_dumper_main(ArrayRef<const char *> argv, void *MainAddr);

/// Run 'swift-dependency-tool'
extern int swift_dependency_tool_main(ArrayRef<const char *> argv, void *MainAddr);

/// Run 'swift-llvm-opt'
extern int swift_llvm_opt_main(ArrayRef<const char *> argv, void *MainAddr);

/// Run 'swift-autolink-extract'.
extern int autolink_extract_main(ArrayRef<const char *> Args, const char *Argv0,
                                 void *MainAddr);

extern int modulewrap_main(ArrayRef<const char *> Args, const char *Argv0,
                           void *MainAddr);

/// Run 'swift-indent'
extern int swift_indent_main(ArrayRef<const char *> Args, const char *Argv0,
                             void *MainAddr);

/// Run 'swift-symbolgraph-extract'
extern int swift_symbolgraph_extract_main(ArrayRef<const char *> Args, const char *Argv0,
void *MainAddr);

/// Run 'swift-api-digester'
extern int swift_api_digester_main(ArrayRef<const char *> Args,
                                   const char *Argv0, void *MainAddr);

/// Run 'swift-cache-tool'
extern int swift_cache_tool_main(ArrayRef<const char *> Args, const char *Argv0,
                                 void *MainAddr);

/// Run 'swift-parse-test'
extern int swift_parse_test_main(ArrayRef<const char *> Args, const char *Argv0,
                                 void *MainAddr);

/// Determine if the given invocation should run as a "subcommand".
///
/// Examples of "subcommands" are 'swift build' or 'swift test', which are
/// usually used to invoke the Swift package manager executables 'swift-build'
/// and 'swift-test', respectively.
///
/// \param ExecName The name of the argv[0] we were invoked as.
/// \param SubcommandName On success, the full name of the subcommand to invoke.
/// \param Args On return, the adjusted program arguments to use.
/// \returns True if running as a subcommand.
static bool shouldRunAsSubcommand(StringRef ExecName,
                                  SmallString<256> &SubcommandName,
                                  const ArrayRef<const char *> Args) {
  assert(!Args.empty());

  // If we are not run as 'swift', don't do anything special. This doesn't work
  // with symlinks with alternate names, but we can't detect 'swift' vs 'swiftc'
  // if we try and resolve using the actual executable path.
  if (ExecName != "swift" && ExecName != "swift-legacy-driver")
    return false;

  // If there are no program arguments, always invoke as normal.
  if (Args.size() == 1)
    return false;

  // Otherwise, we have a program argument. If it looks like an option or a
  // path, then invoke in interactive mode with the arguments as given.
  StringRef FirstArg(Args[1]);
  if (FirstArg.starts_with("-") || FirstArg.contains('.') ||
      FirstArg.contains('/'))
    return false;

  // Otherwise, we should have some sort of subcommand. Get the subcommand name
  // and remove it from the program arguments.
  StringRef Subcommand = Args[1];

  // If the subcommand is the "built-in" 'repl', then use the
  // normal driver.
  if (Subcommand == "repl") {
    return false;
  }

  // Form the subcommand name.
  SubcommandName.assign("swift-");
  SubcommandName.append(Subcommand);

  return true;
}

static bool shouldDisallowNewDriver(DiagnosticEngine &diags,
                                    StringRef ExecName,
                                    const ArrayRef<const char *> argv) {
  // We are expected to use the legacy driver to `exec` an overload
  // for testing purposes.
  if (llvm::sys::Process::GetEnv("SWIFT_OVERLOAD_DRIVER").has_value()) {
    return false;
  }
  StringRef disableArg = "-disallow-use-new-driver";
  StringRef disableEnv = "SWIFT_USE_OLD_DRIVER";
  auto shouldWarn = !llvm::sys::Process::
    GetEnv("SWIFT_AVOID_WARNING_USING_OLD_DRIVER").has_value();

  // We explicitly are on the fallback to the legacy driver from the new driver.
  // Do not forward.
  if (ExecName == "swift-legacy-driver" ||
      ExecName == "swiftc-legacy-driver") {
    if (shouldWarn)
      diags.diagnose(SourceLoc(), diag::old_driver_deprecated, disableArg);
    return true;
  }

  // We are not invoking the driver, so don't forward.
  if (ExecName != "swift" && ExecName != "swiftc") {
    return true;
  }

  // If user specified using the old driver, don't forward.
  if (llvm::find_if(argv, [&](const char* arg) {
    return StringRef(arg) == disableArg;
  }) != argv.end()) {
    if (shouldWarn)
      diags.diagnose(SourceLoc(), diag::old_driver_deprecated, disableArg);
    return true;
  }
  if (llvm::sys::Process::GetEnv(disableEnv).has_value()) {
    if (shouldWarn)
      diags.diagnose(SourceLoc(), diag::old_driver_deprecated, disableEnv);
    return true;
  }
  return false;
}

static bool appendSwiftDriverName(SmallString<256> &buffer) {
  assert(llvm::sys::fs::exists(buffer));
  if (auto driverNameOp = llvm::sys::Process::GetEnv("SWIFT_OVERLOAD_DRIVER")) {
    llvm::sys::path::append(buffer, *driverNameOp);
    return true;
  }

  StringRef execSuffix(llvm::Triple(llvm::sys::getProcessTriple()).isOSWindows() ? ".exe" : "");
  llvm::sys::path::append(buffer, "swift-driver" + execSuffix);
  if (llvm::sys::fs::exists(buffer)) {
    return true;
  }
  llvm::sys::path::remove_filename(buffer);
  llvm::sys::path::append(buffer, "swift-driver-new" + execSuffix);
  if (llvm::sys::fs::exists(buffer)) {
    return true;
  }

  return false;
}

static llvm::SmallVector<const char *, 32> eraseFirstArg(ArrayRef<const char *> argv){
  llvm::SmallVector<const char *, 32> newArgv;
  newArgv.push_back(argv[0]);
  for (const char *arg : argv.slice(2)) {
    newArgv.push_back(arg);
  }
  return newArgv;
}

static int run_driver(StringRef ExecName,
                       ArrayRef<const char *> argv,
                       const ArrayRef<const char *> originalArgv) {
  // This is done here and not done in FrontendTool.cpp, because
  // FrontendTool.cpp is linked to tools, which don't use swift modules.
  initializeSwiftModules();

  bool isRepl = false;

  // Handle integrated tools.
  StringRef DriverModeArg;
  if (argv.size() > 1) {
    StringRef FirstArg(argv[1]);

    if (FirstArg == "-frontend") {
      return performFrontend(
          llvm::ArrayRef(argv.data() + 2, argv.data() + argv.size()), argv[0],
          (void *)(intptr_t)getExecutablePath);
    }
    if (FirstArg == "-modulewrap") {
      return modulewrap_main(
          llvm::ArrayRef(argv.data() + 2, argv.data() + argv.size()), argv[0],
          (void *)(intptr_t)getExecutablePath);
    }
    if (FirstArg == "-sil-opt") {
      return sil_opt_main(eraseFirstArg(argv),
                          (void *)(intptr_t)getExecutablePath);
    }
    if (FirstArg == "-sil-func-extractor") {
      return sil_func_extractor_main(eraseFirstArg(argv),
                                     (void *)(intptr_t)getExecutablePath);
    }
    if (FirstArg == "-sil-nm") {
      return sil_nm_main(eraseFirstArg(argv),
                         (void *)(intptr_t)getExecutablePath);
    }
    if (FirstArg == "-sil-llvm-gen") {
      return sil_llvm_gen_main(eraseFirstArg(argv),
                               (void *)(intptr_t)getExecutablePath);
    }
    if (FirstArg == "-sil-passpipeline-dumper") {
      return sil_passpipeline_dumper_main(eraseFirstArg(argv),
                                          (void *)(intptr_t)getExecutablePath);
    }
    if (FirstArg == "-swift-dependency-tool") {
      return swift_dependency_tool_main(eraseFirstArg(argv),
                                        (void *)(intptr_t)getExecutablePath);
    }
    if (FirstArg == "-swift-llvm-opt") {
      return swift_llvm_opt_main(eraseFirstArg(argv),
                                 (void *)(intptr_t)getExecutablePath);
    }

    // Run the integrated Swift frontend when called as "swift-frontend" but
    // without a leading "-frontend".
    if (!FirstArg.starts_with("--driver-mode=")
        && ExecName == "swift-frontend") {
      return performFrontend(
          llvm::ArrayRef(argv.data() + 1, argv.data() + argv.size()), argv[0],
          (void *)(intptr_t)getExecutablePath);
    }

    if (FirstArg == "repl") {
      isRepl = true;
      argv = argv.drop_front();
    } else if (FirstArg.starts_with("--driver-mode=")) {
      DriverModeArg = FirstArg;
    }
  }

  std::string Path = getExecutablePath(argv[0]);

  PrintingDiagnosticConsumer PDC;

  SourceManager SM;
  DiagnosticEngine Diags(SM);
  Diags.addConsumer(PDC);

  // Forwarding calls to the swift driver if the C++ driver is invoked as `swift`
  // or `swiftc`, and an environment variable SWIFT_USE_NEW_DRIVER is defined.
  if (!shouldDisallowNewDriver(Diags, ExecName, argv)) {
    SmallString<256> NewDriverPath(llvm::sys::path::parent_path(Path));
    if (appendSwiftDriverName(NewDriverPath) &&
        llvm::sys::fs::exists(NewDriverPath)) {
      std::vector<const char *> subCommandArgs;
      // Rewrite the program argument.
      subCommandArgs.push_back(NewDriverPath.c_str());
      if (!DriverModeArg.empty()) {
        subCommandArgs.push_back(DriverModeArg.data());
      } else if (ExecName == "swiftc") {
        subCommandArgs.push_back("--driver-mode=swiftc");
      } else if (ExecName == "swift") {
        subCommandArgs.push_back("--driver-mode=swift");
      }
      // Push these non-op frontend arguments so the build log can indicate
      // the new driver is used.
      subCommandArgs.push_back("-Xfrontend");
      subCommandArgs.push_back("-new-driver-path");
      subCommandArgs.push_back("-Xfrontend");
      subCommandArgs.push_back(NewDriverPath.c_str());

      // Push on the source program arguments
      if (isRepl) {
        subCommandArgs.push_back("-repl");
        subCommandArgs.insert(subCommandArgs.end(),
                              originalArgv.begin() + 2, originalArgv.end());
      } else {
        subCommandArgs.insert(subCommandArgs.end(),
                              originalArgv.begin() + 1, originalArgv.end());
      }

      // Execute the subcommand.
      subCommandArgs.push_back(nullptr);
      ExecuteInPlace(NewDriverPath.c_str(), subCommandArgs.data());

      // If we reach here then an error occurred (typically a missing path).
      std::string ErrorString = llvm::sys::StrError();
      llvm::errs() << "error: unable to invoke subcommand: " << subCommandArgs[0]
                   << " (" << ErrorString << ")\n";
      return 2;
    } else
      Diags.diagnose(SourceLoc(), diag::new_driver_not_found, NewDriverPath);
  }
  
  // We are in the fallback to legacy driver mode.
  // Now that we have determined above that we are not going to
  // forward the invocation to the new driver, ensure the rest of the
  // downstream driver execution refers to itself by the appropriate name.
  if (ExecName == "swift-legacy-driver")
    ExecName = "swift";
  else if (ExecName == "swiftc-legacy-driver")
    ExecName = "swiftc";
  
  Driver TheDriver(Path, ExecName, argv, Diags);
  switch (TheDriver.getDriverKind()) {
  case Driver::DriverKind::SILOpt:
    return sil_opt_main(argv, (void *)(intptr_t)getExecutablePath);
  case Driver::DriverKind::SILFuncExtractor:
    return sil_func_extractor_main(argv, (void *)(intptr_t)getExecutablePath);
  case Driver::DriverKind::SILNM:
    return sil_nm_main(argv, (void *)(intptr_t)getExecutablePath);
  case Driver::DriverKind::SILLLVMGen:
    return sil_llvm_gen_main(argv, (void *)(intptr_t)getExecutablePath);
  case Driver::DriverKind::SILPassPipelineDumper:
    return sil_passpipeline_dumper_main(argv, (void *)(intptr_t)getExecutablePath);
  case Driver::DriverKind::SwiftDependencyTool:
    return swift_dependency_tool_main(argv, (void *)(intptr_t)getExecutablePath);
  case Driver::DriverKind::SwiftLLVMOpt:
    return swift_llvm_opt_main(argv, (void *)(intptr_t)getExecutablePath);
  case Driver::DriverKind::AutolinkExtract:
    return autolink_extract_main(
      TheDriver.getArgsWithoutProgramNameAndDriverMode(argv),
      argv[0], (void *)(intptr_t)getExecutablePath);
  case Driver::DriverKind::SwiftIndent:
    return swift_indent_main(
      TheDriver.getArgsWithoutProgramNameAndDriverMode(argv),
      argv[0], (void *)(intptr_t)getExecutablePath);
  case Driver::DriverKind::SymbolGraph:
      return swift_symbolgraph_extract_main(TheDriver.getArgsWithoutProgramNameAndDriverMode(argv), argv[0], (void *)(intptr_t)getExecutablePath);
  case Driver::DriverKind::APIDigester:
    return swift_api_digester_main(
        TheDriver.getArgsWithoutProgramNameAndDriverMode(argv), argv[0],
        (void *)(intptr_t)getExecutablePath);
  case Driver::DriverKind::CacheTool:
    return swift_cache_tool_main(
        TheDriver.getArgsWithoutProgramNameAndDriverMode(argv), argv[0],
        (void *)(intptr_t)getExecutablePath);
  case Driver::DriverKind::ParseTest:
    return swift_parse_test_main(argv, argv[0],
                                 (void *)(intptr_t)getExecutablePath);
  default:
    break;
  }

  std::unique_ptr<llvm::opt::InputArgList> ArgList =
    TheDriver.parseArgStrings(ArrayRef<const char*>(argv).slice(1));
  if (Diags.hadAnyError())
    return 1;

  std::unique_ptr<ToolChain> TC = TheDriver.buildToolChain(*ArgList);
  if (Diags.hadAnyError())
    return 1;

  for (auto arg: ArgList->getArgs()) {
    if (arg->getOption().hasFlag(options::NewDriverOnlyOption)) {
      Diags.diagnose(SourceLoc(), diag::warning_unsupported_driver_option,
                     arg->getSpelling());
    }
  }

  std::unique_ptr<Compilation> C =
      TheDriver.buildCompilation(*TC, std::move(ArgList));

  if (Diags.hadAnyError())
    return 1;

  if (C) {
    std::unique_ptr<sys::TaskQueue> TQ = TheDriver.buildTaskQueue(*C);
    if (!TQ)
        return 1;
    return C->performJobs(std::move(TQ)).exitCode;
  }

  return 0;
}

int swift::mainEntry(int argc_, const char **argv_) {
#if defined(_WIN32)
  LPWSTR *wargv_ = CommandLineToArgvW(GetCommandLineW(), &argc_);
  std::vector<std::string> utf8Args;
  // We use UTF-8 as the internal character encoding. On Windows,
  // arguments passed to wmain are encoded in UTF-16
  for (int i = 0; i < argc_; i++) {
    const wchar_t *wideArg = wargv_[i];
    int wideArgLen = std::wcslen(wideArg);
    utf8Args.push_back("");
    llvm::ArrayRef<char> uRef((const char *)wideArg,
                              (const char *)(wideArg + wideArgLen));
    llvm::convertUTF16ToUTF8String(uRef, utf8Args[i]);
  }

  std::vector<const char *> utf8CStrs;
  llvm::transform(utf8Args, std::back_inserter(utf8CStrs),
                  std::mem_fn(&std::string::c_str));
  argv_ = utf8CStrs.data();
#endif
  // Expand any response files in the command line argument vector - arguments
  // may be passed through response files in the event of command line length
  // restrictions.
  SmallVector<const char *, 256> ExpandedArgs(&argv_[0], &argv_[argc_]);
  llvm::BumpPtrAllocator Allocator;
  llvm::StringSaver Saver(Allocator);
  swift::driver::ExpandResponseFilesWithRetry(Saver, ExpandedArgs);

  // Initialize the stack trace using the parsed argument vector with expanded
  // response files.

  // PROGRAM_START/InitLLVM overwrites the passed in arguments with UTF-8
  // versions of them on Windows. This also has the effect of overwriting the
  // response file expansion. Since we handle the UTF-8 conversion above, we
  // pass in a copy and throw away the modifications.
  int ThrowawayExpandedArgc = ExpandedArgs.size();
  const char **ThrowawayExpandedArgv = ExpandedArgs.data();
  PROGRAM_START(ThrowawayExpandedArgc, ThrowawayExpandedArgv);
  ArrayRef<const char *> argv(ExpandedArgs);

  PrettyStackTraceSwiftVersion versionStackTrace;

  // Check if this invocation should execute a subcommand.
  StringRef ExecName = llvm::sys::path::stem(argv[0]);
  SmallString<256> SubcommandName;
  if (shouldRunAsSubcommand(ExecName, SubcommandName, argv)) {
    // Preserve argv for the stack trace.
    SmallVector<const char *, 256> subCommandArgs(argv.begin(), argv.end());
    subCommandArgs.erase(&subCommandArgs[1]);
    // We are running as a subcommand, try to find the subcommand adjacent to
    // the executable we are running as.
    SmallString<256> SubcommandPath(SubcommandName);
    auto result = llvm::sys::findProgramByName(SubcommandName,
      { llvm::sys::path::parent_path(getExecutablePath(argv[0])) });
    if (!result.getError()) {
      SubcommandPath = *result;
    } else {
      // If we didn't find the tool there, let the OS search for it.
      result = llvm::sys::findProgramByName(SubcommandName);
      // Search for the program and use the path if found. If there was an
      // error, ignore it and just let the exec fail.
      if (!result.getError())
        SubcommandPath = *result;
    }

    // Rewrite the program argument.
    subCommandArgs[0] = SubcommandPath.c_str();

    // Execute the subcommand.
    subCommandArgs.push_back(nullptr);
    ExecuteInPlace(SubcommandPath.c_str(), subCommandArgs.data());

    // If we reach here then an error occurred (typically a missing path).
    std::string ErrorString = llvm::sys::StrError();
    llvm::errs() << "error: unable to invoke subcommand: " << subCommandArgs[0]
                 << " (" << ErrorString << ")\n";
    return 2;
  }

  ArrayRef<const char *> originalArgv(argv_, &argv_[argc_]);
  return run_driver(ExecName, argv, originalArgv);
}
