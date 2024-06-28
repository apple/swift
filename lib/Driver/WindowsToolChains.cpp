//===---- WindowsToolChains.cpp - Job invocations (Windows-specific) ------===//
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

#include "ToolChains.h"

#include "swift/Basic/Assertions.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Platform.h"
#include "swift/Basic/Range.h"
#include "swift/Basic/TaskQueue.h"
#include "swift/Config.h"
#include "swift/Driver/Compilation.h"
#include "swift/Driver/Driver.h"
#include "swift/Driver/Job.h"
#include "swift/Option/Options.h"
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

std::string toolchains::Windows::sanitizerRuntimeLibName(StringRef Sanitizer,
                                                         bool shared) const {
  return (Twine("clang_rt.") + Sanitizer + "-" +
          this->getTriple().getArchName() + ".lib")
      .str();
}

void
toolchains::Windows::addPluginArguments(const ArgList &Args,
                                        ArgStringList &Arguments) const {
  SmallString<261> LibraryPath = StringRef(getDriver().getSwiftProgramPath());
  llvm::sys::path::remove_filename(LibraryPath); // Remove `swift`

  // In-process plugin server path.
  SmallString<261> InProcPluginServerPath = LibraryPath;
  llvm::sys::path::append(InProcPluginServerPath,
                          "SwiftInProcPluginServer.dll");
  Arguments.push_back("-in-process-plugin-server-path");
  Arguments.push_back(Args.MakeArgString(InProcPluginServerPath));

  // Default plugin path.
  Arguments.push_back("-plugin-path");
  Arguments.push_back(Args.MakeArgString(LibraryPath));
}

ToolChain::InvocationInfo
toolchains::Windows::constructInvocation(const DynamicLinkJobAction &job,
                                         const JobContext &context) const {
  assert(context.Output.getPrimaryOutputType() == file_types::TY_Image &&
         "Invalid linker output type.");

  ArgStringList Arguments;

  std::string Target = getTriple().str();
  if (!Target.empty()) {
    Arguments.push_back("-target");
    Arguments.push_back(context.Args.MakeArgString(Target));
  }

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
    llvm_unreachable("invalid link kind");
  }

  // Select the linker to use.
  std::string Linker;
  if (const Arg *A = context.Args.getLastArg(options::OPT_use_ld)) {
    Linker = A->getValue();
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

  if (Linker.empty() && context.OI.LTOVariant != OutputInfo::LTOKind::None) {
    // Force to use lld for LTO on Windows because we don't support link LTO or
    // something else except for lld LTO at this time.
    Linker = "lld";
  }

  if (!Linker.empty())
    Arguments.push_back(context.Args.MakeArgString("-fuse-ld=" + Linker));

  if (context.OI.DebugInfoFormat == IRGenDebugInfoFormat::CodeView) {
      Arguments.push_back("-Xlinker");
      Arguments.push_back("/DEBUG");
  }

  // Rely on `-libc` to correctly identify the MSVC Runtime Library.  We use
  // `-nostartfiles` as that limits the difference to just the
  // `-defaultlib:libcmt` which is passed unconditionally with the `clang`
  // driver rather than the `clang-cl` driver.
  Arguments.push_back("-nostartfiles");

  bool wantsStaticStdlib =
      context.Args.hasFlag(options::OPT_static_stdlib,
                           options::OPT_no_static_stdlib, false);

  SmallVector<std::string, 4> RuntimeLibPaths;
  getRuntimeLibraryPaths(RuntimeLibPaths, context.Args, context.OI.SDKPath,
                         /*Shared=*/!wantsStaticStdlib);

  for (auto path : RuntimeLibPaths) {
    Arguments.push_back("-L");
    // Since Windows has separate libraries per architecture, link against the
    // architecture specific version of the static library.
    Arguments.push_back(context.Args.MakeArgString(path + "/" +
                                                   getTriple().getArchName()));
  }

  SmallString<128> SharedResourceDirPath;
  getResourceDirPath(SharedResourceDirPath, context.Args, /*Shared=*/true);

  SmallString<128> swiftrtPath = SharedResourceDirPath;
  llvm::sys::path::append(swiftrtPath,
                          swift::getMajorArchitectureName(getTriple()));
  llvm::sys::path::append(swiftrtPath, "swiftrt.obj");
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
    Arguments.push_back("-I");
    Arguments.push_back(context.Args.MakeArgString(context.OI.SDKPath));
  }

  // Link against the desired C++ standard library.
  if (const Arg *A =
      context.Args.getLastArg(options::OPT_experimental_cxx_stdlib)) {
    Arguments.push_back(context.Args.MakeArgString(
        Twine("-stdlib=") + A->getValue()));
  }

  if (job.getKind() == LinkKind::Executable) {
    if (context.OI.SelectedSanitizers & SanitizerKind::Address)
      addLinkRuntimeLib(context.Args, Arguments,
                        sanitizerRuntimeLibName("asan"));

    if (context.OI.SelectedSanitizers & SanitizerKind::Undefined)
      addLinkRuntimeLib(context.Args, Arguments,
                        sanitizerRuntimeLibName("ubsan"));
  }

  if (context.Args.hasArg(options::OPT_profile_generate)) {
    Arguments.push_back(context.Args.MakeArgString("-Xlinker"));
    Arguments.push_back(context.Args.MakeArgString(
        Twine({"-include:", llvm::getInstrProfRuntimeHookVarName()})));
    Arguments.push_back(context.Args.MakeArgString("-lclang_rt.profile"));
  }

  context.Args.AddAllArgs(Arguments, options::OPT_Xlinker);
  context.Args.AddAllArgsExcept(Arguments, {options::OPT_linker_option_Group},
                                {options::OPT_l});
  ToolChain::addLinkedLibArgs(context.Args, Arguments);
  context.Args.AddAllArgValues(Arguments, options::OPT_Xclang_linker);

  // Run clang in verbose mode if "-v" is set
  if (context.Args.hasArg(options::OPT_v)) {
    Arguments.push_back("-v");
  }

  // This should be the last option, for convenience in checking output.
  Arguments.push_back("-o");
  Arguments.push_back(
      context.Args.MakeArgString(context.Output.getPrimaryOutputFilename()));

  InvocationInfo II{getClangLinkerDriver(context.Args), Arguments};
  II.allowsResponseFiles = true;

  return II;
}

ToolChain::InvocationInfo
toolchains::Windows::constructInvocation(const StaticLinkJobAction &job,
                                         const JobContext &context) const {
   assert(context.Output.getPrimaryOutputType() == file_types::TY_Image &&
         "Invalid linker output type.");

  ArgStringList Arguments;

  const char *Linker = "link";
  if (const Arg *A = context.Args.getLastArg(options::OPT_use_ld))
    Linker = context.Args.MakeArgString(A->getValue());

  Arguments.push_back("/lib");
  Arguments.push_back("-nologo");

  addPrimaryInputsOfType(Arguments, context.Inputs, context.Args,
                         file_types::TY_Object);
  addInputsOfType(Arguments, context.InputActions, file_types::TY_Object);

  StringRef OutputFile = context.Output.getPrimaryOutputFilename();
  Arguments.push_back(context.Args.MakeArgString(Twine("/OUT:") + OutputFile));

  InvocationInfo II{Linker, Arguments};
  II.allowsResponseFiles = true;

  return II;
}
