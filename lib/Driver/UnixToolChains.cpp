//===------ UnixToolChains.cpp - Job invocations (non-Darwin Unix) --------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include <fstream>

#include "ToolChains.h"

#include "swift/Basic/LLVM.h"
#include "swift/Basic/Platform.h"
#include "swift/Basic/Range.h"
#include "swift/Basic/TaskQueue.h"
#include "swift/Config.h"
#include "swift/Driver/Compilation.h"
#include "swift/Driver/Driver.h"
#include "swift/Driver/Job.h"
#include "swift/IDETool/CompilerInvocation.h"
#include "swift/Option/Options.h"
#include "swift/Option/SanitizerOptions.h"
#include "clang/Basic/Version.h"
#include "clang/Driver/Util.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"

using namespace swift;
using namespace swift::driver;
using namespace llvm::opt;

std::string
toolchains::GenericUnix::sanitizerRuntimeLibName(StringRef Sanitizer,
                                                 bool shared) const {
  return (Twine("libclang_rt.") + Sanitizer + "-" +
          this->getTriple().getArchName() +
          (this->getTriple().isAndroid() ? "-android" : "") + ".a")
      .str();
}

void
toolchains::GenericUnix::addPluginArguments(const ArgList &Args,
                                            ArgStringList &Arguments) const {
  SmallString<64> pluginPath;
  auto programPath = getDriver().getSwiftProgramPath();
  CompilerInvocation::computeRuntimeResourcePathFromExecutablePath(
      programPath, /*shared=*/true, pluginPath);

  auto defaultPluginPath = pluginPath;
  llvm::sys::path::append(defaultPluginPath, "host", "plugins");

  // Default plugin path.
  Arguments.push_back("-plugin-path");
  Arguments.push_back(Args.MakeArgString(defaultPluginPath));

  // Local plugin path.
  llvm::sys::path::remove_filename(pluginPath); // Remove "swift"
  llvm::sys::path::remove_filename(pluginPath); // Remove "lib"
  llvm::sys::path::append(pluginPath, "local", "lib");
  llvm::sys::path::append(pluginPath, "swift");
  llvm::sys::path::append(pluginPath, "host", "plugins");
  Arguments.push_back("-plugin-path");
  Arguments.push_back(Args.MakeArgString(pluginPath));
}

ToolChain::InvocationInfo
toolchains::GenericUnix::constructInvocation(const InterpretJobAction &job,
                                             const JobContext &context) const {
  InvocationInfo II = ToolChain::constructInvocation(job, context);

  SmallVector<std::string, 4> runtimeLibraryPaths;
  getRuntimeLibraryPaths(runtimeLibraryPaths, context.Args, context.OI.SDKPath,
                         /*Shared=*/true);

  addPathEnvironmentVariableIfNeeded(II.ExtraEnvironment, "LD_LIBRARY_PATH",
                                     ":", options::OPT_L, context.Args,
                                     runtimeLibraryPaths);
  return II;
}

ToolChain::InvocationInfo toolchains::GenericUnix::constructInvocation(
    const AutolinkExtractJobAction &job, const JobContext &context) const {
  assert(context.Output.getPrimaryOutputType() == file_types::TY_AutolinkFile);

  InvocationInfo II{"swift-autolink-extract"};
  ArgStringList &Arguments = II.Arguments;
  II.allowsResponseFiles = true;

  addPrimaryInputsOfType(Arguments, context.Inputs, context.Args,
                         file_types::TY_Object);
  addPrimaryInputsOfType(Arguments, context.Inputs, context.Args,
                         file_types::TY_LLVM_BC);
  addInputsOfType(Arguments, context.InputActions, file_types::TY_Object);
  addInputsOfType(Arguments, context.InputActions, file_types::TY_LLVM_BC);

  Arguments.push_back("-o");
  Arguments.push_back(
      context.Args.MakeArgString(context.Output.getPrimaryOutputFilename()));

  return II;
}
// Amazon Linux 2023 requires lld as the default linker.
bool isAmazonLinux2023Host() {
      std::ifstream file("/etc/os-release");
      std::string line;

      while (std::getline(file, line)) {
        if (line.substr(0, 12) == "PRETTY_NAME=") {
          if (line.substr(12) == "\"Amazon Linux 2023\"") {
            file.close();
            return true;
          }
        }
      }
      return false;
    }

std::string toolchains::GenericUnix::getDefaultLinker() const {
  if (getTriple().isAndroid() || isAmazonLinux2023Host())
    return "lld";

  switch (getTriple().getArch()) {
  case llvm::Triple::arm:
  case llvm::Triple::aarch64:
  case llvm::Triple::aarch64_32:
  case llvm::Triple::armeb:
  case llvm::Triple::thumb:
  case llvm::Triple::thumbeb:
    // BFD linker has issues wrt relocation of the protocol conformance
    // section on these targets, it also generates COPY relocations for
    // final executables, as such, unless specified, we default to gold
    // linker.
    return "gold";
  case llvm::Triple::x86:
  case llvm::Triple::x86_64:
  case llvm::Triple::ppc64:
  case llvm::Triple::ppc64le:
  case llvm::Triple::systemz:
    // BFD linker has issues wrt relocations against protected symbols.
    return "gold";
  default:
    // Otherwise, use the default BFD linker.
    return "";
  }
}

bool toolchains::GenericUnix::addRuntimeRPath(const llvm::Triple &T,
                                              const llvm::opt::ArgList &Args) const {
  // If we are building a static executable, do not add a rpath for the runtime
  // as it is a static binary and the loader will not be invoked.
  if (Args.hasFlag(options::OPT_static_executable,
                   options::OPT_no_static_executable, false))
    return false;

  // If we are building with a static standard library, do not add a rpath for
  // the runtime because the runtime will be part of the binary and the rpath is
  // no longer necessary.
  if (Args.hasFlag(options::OPT_static_stdlib, options::OPT_no_static_stdlib,
                   false))
    return false;

  // FIXME: We probably shouldn't be adding an rpath here unless we know ahead
  // of time the standard library won't be copied.

  // Honour the user's request to add a rpath to the binary.  This defaults to
  // `true` on non-android and `false` on android since the library must be
  // copied into the bundle. An exception is made for the Termux app as it
  // builds and runs natively like a Unix environment on Android.
#if defined(__TERMUX__)
  bool apply_rpath = true;
#else
  bool apply_rpath = !T.isAndroid();
#endif
  return Args.hasFlag(options::OPT_toolchain_stdlib_rpath,
                      options::OPT_no_toolchain_stdlib_rpath, apply_rpath);
}

ToolChain::InvocationInfo
toolchains::GenericUnix::constructInvocation(const DynamicLinkJobAction &job,
                                             const JobContext &context) const {
  assert(context.Output.getPrimaryOutputType() == file_types::TY_Image &&
         "Invalid linker output type.");

  ArgStringList Arguments;

  switch (job.getKind()) {
  case LinkKind::None:
    llvm_unreachable("invalid link kind");
  case LinkKind::Executable:
    // Default case, nothing extra needed.
    break;
  case LinkKind::DynamicLibrary:
    Arguments.push_back("-shared");
    break;
  case LinkKind::StaticLibrary:
    llvm_unreachable("the dynamic linker cannot build static libraries");
  }

  // Select the linker to use.
  std::string Linker;
  if (context.OI.LTOVariant != OutputInfo::LTOKind::None) {
    // Force to use lld for LTO on Unix-like platform (not including Darwin)
    // because we don't support gold LTO or something else except for lld LTO
    // at this time.
    Linker = "lld";
  }

  if (const Arg *A = context.Args.getLastArg(options::OPT_use_ld)) {
    Linker = A->getValue();
  }

  if (Linker.empty()) {
    Linker = getDefaultLinker();
  }
  if (!Linker.empty()) {
#if defined(__HAIKU__)
    // For now, passing -fuse-ld on Haiku doesn't work as swiftc doesn't
    // recognise it. Passing -use-ld= as the argument works fine.
    Arguments.push_back(context.Args.MakeArgString("-use-ld=" + Linker));
#else
    Arguments.push_back(context.Args.MakeArgString("-fuse-ld=" + Linker));
#endif
    // Starting with lld 13, Swift stopped working with the lld --gc-sections
    // implementation for ELF, unless -z nostart-stop-gc is also passed to lld:
    //
    // https://reviews.llvm.org/D96914
    if (Linker == "lld" || (Linker.length() > 5 &&
                            Linker.substr(Linker.length() - 6) == "ld.lld")) {
      Arguments.push_back("-Xlinker");
      Arguments.push_back("-z");
      Arguments.push_back("-Xlinker");
      Arguments.push_back("nostart-stop-gc");
    }
  }

  // Configure the toolchain.
  if (const Arg *A = context.Args.getLastArg(options::OPT_tools_directory)) {
    StringRef toolchainPath(A->getValue());

    // Look for binutils in the toolchain folder.
    Arguments.push_back("-B");
    Arguments.push_back(context.Args.MakeArgString(A->getValue()));
  }

  if (getTriple().getObjectFormat() == llvm::Triple::ELF &&
      job.getKind() == LinkKind::Executable &&
      !context.Args.hasFlag(options::OPT_static_executable,
                            options::OPT_no_static_executable, false)) {
    Arguments.push_back("-pie");
  }

  switch (context.OI.LTOVariant) {
  case OutputInfo::LTOKind::LLVMThin:
    Arguments.push_back("-flto=thin");
    break;
  case OutputInfo::LTOKind::LLVMFull:
    Arguments.push_back("-flto=full");
    break;
  case OutputInfo::LTOKind::None:
    break;
  }

  bool staticExecutable = false;
  bool staticStdlib = false;

  if (context.Args.hasFlag(options::OPT_static_executable,
                           options::OPT_no_static_executable, false)) {
    staticExecutable = true;
  } else if (context.Args.hasFlag(options::OPT_static_stdlib,
                                  options::OPT_no_static_stdlib, false)) {
    staticStdlib = true;
  }

  SmallVector<std::string, 4> RuntimeLibPaths;
  getRuntimeLibraryPaths(RuntimeLibPaths, context.Args, context.OI.SDKPath,
                         /*Shared=*/!(staticExecutable || staticStdlib));

  if (addRuntimeRPath(getTriple(), context.Args)) {
    for (auto path : RuntimeLibPaths) {
      Arguments.push_back("-Xlinker");
      Arguments.push_back("-rpath");
      Arguments.push_back("-Xlinker");
      Arguments.push_back(context.Args.MakeArgString(path));
    }
  }

  SmallString<128> SharedResourceDirPath;
  getResourceDirPath(SharedResourceDirPath, context.Args, /*Shared=*/true);

  SmallString<128> swiftrtPath = SharedResourceDirPath;
  llvm::sys::path::append(swiftrtPath,
                          swift::getMajorArchitectureName(getTriple()));
  llvm::sys::path::append(swiftrtPath, "swiftrt.o");
  Arguments.push_back(context.Args.MakeArgString(swiftrtPath));

  addPrimaryInputsOfType(Arguments, context.Inputs, context.Args,
                         file_types::TY_Object);
  addPrimaryInputsOfType(Arguments, context.Inputs, context.Args,
                         file_types::TY_LLVM_BC);
  addInputsOfType(Arguments, context.InputActions, file_types::TY_Object);
  addInputsOfType(Arguments, context.InputActions, file_types::TY_LLVM_BC);

  for (const Arg *arg :
       context.Args.filtered(options::OPT_F, options::OPT_Fsystem)) {
    if (arg->getOption().matches(options::OPT_Fsystem))
      Arguments.push_back("-iframework");
    else
      Arguments.push_back(context.Args.MakeArgString(arg->getSpelling()));
    Arguments.push_back(arg->getValue());
  }

  if (!context.OI.SDKPath.empty()) {
    Arguments.push_back("--sysroot");
    Arguments.push_back(context.Args.MakeArgString(context.OI.SDKPath));
  }

  // If we are linking statically, we need to add all
  // dependencies to a library search group to resolve
  // potential circular dependencies
  if (staticExecutable || staticStdlib) {
    Arguments.push_back("-Xlinker");
    Arguments.push_back("--start-group");
  }

  // Add any autolinking scripts to the arguments
  for (const Job *Cmd : context.Inputs) {
    auto &OutputInfo = Cmd->getOutput();
    if (OutputInfo.getPrimaryOutputType() == file_types::TY_AutolinkFile)
      Arguments.push_back(context.Args.MakeArgString(
          Twine("@") + OutputInfo.getPrimaryOutputFilename()));
  }

  if (staticExecutable || staticStdlib) {
    Arguments.push_back("-Xlinker");
    Arguments.push_back("--end-group");
  }

  // Add the runtime library link paths.
  for (auto path : RuntimeLibPaths) {
    Arguments.push_back("-L");
    Arguments.push_back(context.Args.MakeArgString(path));
  }

  // Link the standard library. In two paths, we do this using a .lnk file;
  // if we're going that route, we'll set `linkFilePath` to the path to that
  // file.
  SmallString<128> linkFilePath;
  getResourceDirPath(linkFilePath, context.Args, /*Shared=*/false);

  if (staticExecutable) {
    llvm::sys::path::append(linkFilePath, "static-executable-args.lnk");
  } else if (staticStdlib) {
    llvm::sys::path::append(linkFilePath, "static-stdlib-args.lnk");
  } else {
    linkFilePath.clear();
    Arguments.push_back("-lswiftCore");
  }

  if (!linkFilePath.empty()) {
    if (llvm::sys::fs::is_regular_file(linkFilePath)) {
      Arguments.push_back(
          context.Args.MakeArgString(Twine("@") + linkFilePath));
    } else {
      llvm::report_fatal_error(Twine(linkFilePath) + " not found");
    }
  }

  // Link against the desired C++ standard library.
  if (const Arg *A =
          context.Args.getLastArg(options::OPT_experimental_cxx_stdlib)) {
    Arguments.push_back(
        context.Args.MakeArgString(Twine("-stdlib=") + A->getValue()));
  }

  // Explicitly pass the target to the linker
  Arguments.push_back(
      context.Args.MakeArgString("--target=" + getTriple().str()));

  // Delegate to Clang for sanitizers. It will figure out the correct linker
  // options.
  if (job.getKind() == LinkKind::Executable && context.OI.SelectedSanitizers) {
    Arguments.push_back(context.Args.MakeArgString(
        "-fsanitize=" + getSanitizerList(context.OI.SelectedSanitizers)));

    // The TSan runtime depends on the blocks runtime and libdispatch.
    if (context.OI.SelectedSanitizers & SanitizerKind::Thread) {
      Arguments.push_back("-lBlocksRuntime");
      Arguments.push_back("-ldispatch");
    }
  }

  if (context.Args.hasArg(options::OPT_profile_generate)) {
    SmallString<128> LibProfile(SharedResourceDirPath);
    llvm::sys::path::remove_filename(LibProfile); // remove platform name
    llvm::sys::path::append(LibProfile, "clang", "lib");

    llvm::sys::path::append(LibProfile, getTriple().getOSName(),
                            Twine("libclang_rt.profile-") +
                                getTriple().getArchName() + ".a");
    Arguments.push_back(context.Args.MakeArgString(LibProfile));
    Arguments.push_back(context.Args.MakeArgString(
        Twine("-u", llvm::getInstrProfRuntimeHookVarName())));
  }

  // Run clang in verbose mode if "-v" is set
  if (context.Args.hasArg(options::OPT_v)) {
    Arguments.push_back("-v");
  }

  // These custom arguments should be right before the object file at the end.
  context.Args.AddAllArgsExcept(Arguments, {options::OPT_linker_option_Group},
                                {options::OPT_l});
  ToolChain::addLinkedLibArgs(context.Args, Arguments);
  context.Args.AddAllArgs(Arguments, options::OPT_Xlinker);
  context.Args.AddAllArgValues(Arguments, options::OPT_Xclang_linker);

  // This should be the last option, for convenience in checking output.
  Arguments.push_back("-o");
  Arguments.push_back(
      context.Args.MakeArgString(context.Output.getPrimaryOutputFilename()));

  InvocationInfo II{getClangLinkerDriver(context.Args), Arguments};
  II.allowsResponseFiles = true;

  return II;
}


ToolChain::InvocationInfo
toolchains::GenericUnix::constructInvocation(const StaticLinkJobAction &job,
                               const JobContext &context) const {
   assert(context.Output.getPrimaryOutputType() == file_types::TY_Image &&
         "Invalid linker output type.");

  ArgStringList Arguments;

  const char *AR;
  // Configure the toolchain.
  if (getTriple().isAndroid())
    AR = "llvm-ar";
  else
    AR = context.OI.LTOVariant != OutputInfo::LTOKind::None ? "llvm-ar" : "ar";
  Arguments.push_back("crs");

  Arguments.push_back(
      context.Args.MakeArgString(context.Output.getPrimaryOutputFilename()));

  addPrimaryInputsOfType(Arguments, context.Inputs, context.Args,
                         file_types::TY_Object);
  addPrimaryInputsOfType(Arguments, context.Inputs, context.Args,
                         file_types::TY_LLVM_BC);
  addInputsOfType(Arguments, context.InputActions, file_types::TY_Object);
  addInputsOfType(Arguments, context.InputActions, file_types::TY_LLVM_BC);

  InvocationInfo II{AR, Arguments};

  return II;
}

std::string toolchains::Cygwin::getDefaultLinker() const {
  // Cygwin uses the default BFD linker, even on ARM.
  return "";
}

std::string toolchains::OpenBSD::getDefaultLinker() const {
  return "lld";
}

std::string toolchains::FreeBSD::getDefaultLinker() const {
  return "lld";
}
