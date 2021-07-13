//===------ DarwinToolChains.cpp - Job invocations (Darwin-specific) ------===//
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

#include "swift/AST/DiagnosticsDriver.h"
#include "swift/AST/PlatformKind.h"
#include "swift/Basic/Dwarf.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Platform.h"
#include "swift/Basic/Range.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/TaskQueue.h"
#include "swift/Config.h"
#include "swift/Driver/Compilation.h"
#include "clang/Driver/DarwinSDKInfo.h"
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
#include "llvm/Support/VersionTuple.h"

using namespace swift;
using namespace swift::driver;
using namespace llvm::opt;

std::string
toolchains::Darwin::findProgramRelativeToSwiftImpl(StringRef name) const {
  StringRef swiftPath = getDriver().getSwiftProgramPath();
  StringRef swiftBinDir = llvm::sys::path::parent_path(swiftPath);

  // See if we're in an Xcode toolchain.
  bool hasToolchain = false;
  llvm::SmallString<128> path{swiftBinDir};
  llvm::sys::path::remove_filename(path); // bin
  llvm::sys::path::remove_filename(path); // usr
  if (llvm::sys::path::extension(path) == ".xctoolchain") {
    hasToolchain = true;
    llvm::sys::path::remove_filename(path); // *.xctoolchain
    llvm::sys::path::remove_filename(path); // Toolchains
    llvm::sys::path::append(path, "usr", "bin");
  }

  StringRef paths[] = {swiftBinDir, path};
  auto pathsRef = llvm::makeArrayRef(paths);
  if (!hasToolchain)
    pathsRef = pathsRef.drop_back();

  auto result = llvm::sys::findProgramByName(name, pathsRef);
  if (result)
    return result.get();
  return {};
}

ToolChain::InvocationInfo
toolchains::Darwin::constructInvocation(const InterpretJobAction &job,
                                        const JobContext &context) const {
  InvocationInfo II = ToolChain::constructInvocation(job, context);

  SmallVector<std::string, 4> runtimeLibraryPaths;
  getRuntimeLibraryPaths(runtimeLibraryPaths, context.Args, context.OI.SDKPath,
                         /*Shared=*/true);

  addPathEnvironmentVariableIfNeeded(II.ExtraEnvironment, "DYLD_LIBRARY_PATH",
                                     ":", options::OPT_L, context.Args,
                                     runtimeLibraryPaths);
  addPathEnvironmentVariableIfNeeded(II.ExtraEnvironment, "DYLD_FRAMEWORK_PATH",
                                     ":", options::OPT_F, context.Args);
  // FIXME: Add options::OPT_Fsystem paths to DYLD_FRAMEWORK_PATH as well.
  return II;
}

static StringRef
getDarwinLibraryNameSuffixForTriple(const llvm::Triple &triple) {
  const DarwinPlatformKind kind = getDarwinPlatformKind(triple);
  switch (kind) {
  case DarwinPlatformKind::MacOS:
    return "osx";
  case DarwinPlatformKind::IPhoneOS:
    // Here we return "osx" under the assumption that all the
    // darwin runtime libraries are zippered and so the "osx" variants
    // should be used for macCatalyst targets.
    if (tripleIsMacCatalystEnvironment(triple))
        return "osx";
    return "ios";
  case DarwinPlatformKind::IPhoneOSSimulator:
    return "iossim";
  case DarwinPlatformKind::TvOS:
    return "tvos";
  case DarwinPlatformKind::TvOSSimulator:
    return "tvossim";
  case DarwinPlatformKind::WatchOS:
    return "watchos";
  case DarwinPlatformKind::WatchOSSimulator:
    return "watchossim";
  }
  llvm_unreachable("Unsupported Darwin platform");
}

std::string toolchains::Darwin::sanitizerRuntimeLibName(StringRef Sanitizer,
                                                        bool shared) const {
  return (Twine("libclang_rt.") + Sanitizer + "_" +
          getDarwinLibraryNameSuffixForTriple(this->getTriple()) +
          (shared ? "_dynamic.dylib" : ".a"))
      .str();
}

static void addLinkRuntimeLibRPath(const ArgList &Args,
                                   ArgStringList &Arguments,
                                   StringRef DarwinLibName,
                                   const ToolChain &TC) {
  // Adding the rpaths might negatively interact when other rpaths are involved,
  // so we should make sure we add the rpaths last, after all user-specified
  // rpaths. This is currently true from this place, but we need to be
  // careful if this function is ever called before user's rpaths are emitted.
  assert(DarwinLibName.endswith(".dylib") && "must be a dynamic library");

  // Add @executable_path to rpath to support having the dylib copied with
  // the executable.
  Arguments.push_back("-rpath");
  Arguments.push_back("@executable_path");

  // Add the path to the resource dir to rpath to support using the dylib
  // from the default location without copying.

  SmallString<128> ClangLibraryPath;
  TC.getClangLibraryPath(Args, ClangLibraryPath);

  Arguments.push_back("-rpath");
  Arguments.push_back(Args.MakeArgString(ClangLibraryPath));
}

static void addLinkSanitizerLibArgsForDarwin(const ArgList &Args,
                                             ArgStringList &Arguments,
                                             StringRef Sanitizer,
                                             const ToolChain &TC,
                                             bool shared = true) {
  // Sanitizer runtime libraries requires C++.
  Arguments.push_back("-lc++");
  // Add explicit dependency on -lc++abi, as -lc++ doesn't re-export
  // all RTTI-related symbols that are used.
  Arguments.push_back("-lc++abi");

  auto LibName = TC.sanitizerRuntimeLibName(Sanitizer, shared);
  TC.addLinkRuntimeLib(Args, Arguments, LibName);

  if (shared)
    addLinkRuntimeLibRPath(Args, Arguments, LibName, TC);
}

/// Runs <code>xcrun -f clang</code> in order to find the location of Clang for
/// the currently active Xcode.
///
/// We get the "currently active" part by passing through the DEVELOPER_DIR
/// environment variable (along with the rest of the environment).
static bool findXcodeClangPath(llvm::SmallVectorImpl<char> &path) {
  assert(path.empty());

  auto xcrunPath = llvm::sys::findProgramByName("xcrun");
  if (!xcrunPath.getError()) {
    // Explicitly ask for the default toolchain so that we don't find a Clang
    // included with an open-source toolchain.
    const char *args[] = {"-toolchain", "default", "-f", "clang", nullptr};
    sys::TaskQueue queue;
    queue.addTask(xcrunPath->c_str(), args, /*Env=*/llvm::None,
                  /*Context=*/nullptr,
                  /*SeparateErrors=*/true);
    queue.execute(nullptr,
                  [&path](sys::ProcessId PID, int returnCode, StringRef output,
                          StringRef errors,
                          sys::TaskProcessInformation ProcInfo,
                          void *unused) -> sys::TaskFinishedResponse {
                    if (returnCode == 0) {
                      output = output.rtrim();
                      path.append(output.begin(), output.end());
                    }
                    return sys::TaskFinishedResponse::ContinueExecution;
                  });
  }

  return !path.empty();
}

static bool findXcodeClangLibPath(const Twine &libName,
                                  llvm::SmallVectorImpl<char> &path) {
  assert(path.empty());

  if (!findXcodeClangPath(path)) {
    return false;
  }
  llvm::sys::path::remove_filename(path); // 'clang'
  llvm::sys::path::remove_filename(path); // 'bin'
  llvm::sys::path::append(path, "lib", libName);
  return true;
}

static void addVersionString(const ArgList &inputArgs, ArgStringList &arguments,
                             unsigned major, unsigned minor, unsigned micro) {
  llvm::SmallString<8> buf;
  llvm::raw_svector_ostream os{buf};
  os << major << '.' << minor << '.' << micro;
  arguments.push_back(inputArgs.MakeArgString(os.str()));
}

/// Returns true if the compiler depends on features provided by the ObjC
/// runtime that are not present on the deployment target indicated by
/// \p triple.
static bool wantsObjCRuntime(const llvm::Triple &triple) {
  assert((!triple.isTvOS() || triple.isiOS()) &&
         "tvOS is considered a kind of iOS");

  // When updating the versions listed here, please record the most recent
  // feature being depended on and when it was introduced:
  //
  // - Make assigning 'nil' to an NSMutableDictionary subscript delete the
  //   entry, like it does for Swift.Dictionary, rather than trap.
  if (triple.isiOS())
    return triple.isOSVersionLT(9);
  if (triple.isMacOSX())
    return triple.isMacOSXVersionLT(10, 11);
  if (triple.isWatchOS())
    return false;
  llvm_unreachable("unknown Darwin OS");
}

void
toolchains::Darwin::addLinkerInputArgs(InvocationInfo &II,
                                       const JobContext &context) const {
  ArgStringList &Arguments = II.Arguments;
  if (context.shouldUseInputFileList()) {
    Arguments.push_back("-filelist");
    Arguments.push_back(context.getTemporaryFilePath("inputs", "LinkFileList"));
    II.FilelistInfos.push_back(
        {Arguments.back(), context.OI.CompilerOutputType,
         FilelistInfo::WhichFiles::InputJobsAndSourceInputActions});
  } else {
    addPrimaryInputsOfType(Arguments, context.Inputs, context.Args,
                           file_types::TY_Object);
    addPrimaryInputsOfType(Arguments, context.Inputs, context.Args,
                           file_types::TY_TBD);
    addPrimaryInputsOfType(Arguments, context.Inputs, context.Args,
                           file_types::TY_LLVM_BC);
    addInputsOfType(Arguments, context.InputActions, file_types::TY_Object);
    addInputsOfType(Arguments, context.InputActions, file_types::TY_TBD);
    addInputsOfType(Arguments, context.InputActions, file_types::TY_LLVM_BC);
  }


  if (context.OI.CompilerMode == OutputInfo::Mode::SingleCompile)
    addInputsOfType(Arguments, context.Inputs, context.Args,
                    file_types::TY_SwiftModuleFile, "-add_ast_path");
  else
    addPrimaryInputsOfType(Arguments, context.Inputs, context.Args,
                           file_types::TY_SwiftModuleFile, "-add_ast_path");

  // Add all .swiftmodule file inputs as arguments, preceded by the
  // "-add_ast_path" linker option.
  addInputsOfType(Arguments, context.InputActions,
                  file_types::TY_SwiftModuleFile, "-add_ast_path");
}

static void findARCLiteLibPath(const toolchains::Darwin &TC,
                               llvm::SmallVectorImpl<char> &ARCLiteLib) {
  auto& D = TC.getDriver();
  llvm::sys::path::append(ARCLiteLib, D.getSwiftProgramPath());
  
  llvm::sys::path::remove_filename(ARCLiteLib); // 'swift'
  llvm::sys::path::remove_filename(ARCLiteLib); // 'bin'
  llvm::sys::path::append(ARCLiteLib, "lib", "arc");

  if (!llvm::sys::fs::is_directory(ARCLiteLib)) {
    // If we don't have a 'lib/arc/' directory, find the "arclite" library
    // relative to the Clang in the active Xcode.
    ARCLiteLib.clear();
    findXcodeClangLibPath("arc", ARCLiteLib);
  }
}

void
toolchains::Darwin::addArgsToLinkARCLite(ArgStringList &Arguments,
                                         const JobContext &context) const {
  if (!context.Args.hasFlag(options::OPT_link_objc_runtime,
                            options::OPT_no_link_objc_runtime,
                            /*Default=*/wantsObjCRuntime(getTriple())))
    return;

  llvm::SmallString<128> ARCLiteLib;
  findARCLiteLibPath(*this, ARCLiteLib);

  if (!ARCLiteLib.empty()) {
    llvm::sys::path::append(ARCLiteLib, "libarclite_");
    ARCLiteLib += getPlatformNameForTriple(getTriple());
    ARCLiteLib += ".a";

    Arguments.push_back("-force_load");
    Arguments.push_back(context.Args.MakeArgString(ARCLiteLib));

    // Arclite depends on CoreFoundation.
    Arguments.push_back("-framework");
    Arguments.push_back("CoreFoundation");
  }
}

void toolchains::Darwin::addLTOLibArgs(ArgStringList &Arguments,
                                       const JobContext &context) const {
  if (!context.OI.LibLTOPath.empty()) {
    // Check for user-specified LTO library.
    Arguments.push_back("-lto_library");
    Arguments.push_back(context.Args.MakeArgString(context.OI.LibLTOPath));
  } else {
    // Check for relative libLTO.dylib. This would be the expected behavior in an
    // Xcode toolchain.
    StringRef P = llvm::sys::path::parent_path(getDriver().getSwiftProgramPath());
    llvm::SmallString<128> LibLTOPath(P);
    llvm::sys::path::remove_filename(LibLTOPath); // Remove '/bin'
    llvm::sys::path::append(LibLTOPath, "lib");
    llvm::sys::path::append(LibLTOPath, "libLTO.dylib");
    if (llvm::sys::fs::exists(LibLTOPath)) {
      Arguments.push_back("-lto_library");
      Arguments.push_back(context.Args.MakeArgString(LibLTOPath));
    } else {
      // Use libLTO.dylib from the default toolchain if a relative one does not exist.
      llvm::SmallString<128> LibLTOPath;
      if (findXcodeClangLibPath("libLTO.dylib", LibLTOPath)) {
        Arguments.push_back("-lto_library");
        Arguments.push_back(context.Args.MakeArgString(LibLTOPath));
      }
    }
  }
}

void
toolchains::Darwin::addSanitizerArgs(ArgStringList &Arguments,
                                     const DynamicLinkJobAction &job,
                                     const JobContext &context) const {
  // Linking sanitizers will add rpaths, which might negatively interact when
  // other rpaths are involved, so we should make sure we add the rpaths after
  // all user-specified rpaths.
  if (context.OI.SelectedSanitizers & SanitizerKind::Address)
    addLinkSanitizerLibArgsForDarwin(context.Args, Arguments, "asan", *this);

  if (context.OI.SelectedSanitizers & SanitizerKind::Thread)
    addLinkSanitizerLibArgsForDarwin(context.Args, Arguments, "tsan", *this);

  if (context.OI.SelectedSanitizers & SanitizerKind::Undefined)
    addLinkSanitizerLibArgsForDarwin(context.Args, Arguments, "ubsan", *this);

  // Only link in libFuzzer for executables.
  if (job.getKind() == LinkKind::Executable &&
      (context.OI.SelectedSanitizers & SanitizerKind::Fuzzer))
    addLinkSanitizerLibArgsForDarwin(context.Args, Arguments, "fuzzer", *this,
                                     /*shared=*/false);
}

namespace {

enum class BackDeployLibFilter {
  executable,
  all
};

// Whether the given job matches the backward-deployment library filter.
bool jobMatchesFilter(LinkKind jobKind, BackDeployLibFilter filter) {
  switch (filter) {
  case BackDeployLibFilter::executable:
    return jobKind == LinkKind::Executable;
    
  case BackDeployLibFilter::all:
    return true;
  }
  llvm_unreachable("unhandled back deploy lib filter!");
}

}

void
toolchains::Darwin::addArgsToLinkStdlib(ArgStringList &Arguments,
                                        const DynamicLinkJobAction &job,
                                        const JobContext &context) const {

  // Link compatibility libraries, if we're deploying back to OSes that
  // have an older Swift runtime.
  SmallString<128> SharedResourceDirPath;
  getResourceDirPath(SharedResourceDirPath, context.Args, /*Shared=*/true);
  Optional<llvm::VersionTuple> runtimeCompatibilityVersion;
  
  if (context.Args.hasArg(options::OPT_runtime_compatibility_version)) {
    auto value = context.Args.getLastArgValue(
                                    options::OPT_runtime_compatibility_version);
    if (value.equals("5.0")) {
      runtimeCompatibilityVersion = llvm::VersionTuple(5, 0);
    } else if (value.equals("5.1")) {
      runtimeCompatibilityVersion = llvm::VersionTuple(5, 1);
    } else if (value.equals("none")) {
      runtimeCompatibilityVersion = None;
    } else {
      // TODO: diagnose unknown runtime compatibility version?
    }
  } else if (job.getKind() == LinkKind::Executable) {
    runtimeCompatibilityVersion
                   = getSwiftRuntimeCompatibilityVersionForTarget(getTriple());
  }
  
  if (runtimeCompatibilityVersion) {
    auto addBackDeployLib = [&](llvm::VersionTuple version,
                                BackDeployLibFilter filter,
                                StringRef libraryName) {
      if (*runtimeCompatibilityVersion > version)
        return;

      if (!jobMatchesFilter(job.getKind(), filter))
        return;
      
      SmallString<128> BackDeployLib;
      BackDeployLib.append(SharedResourceDirPath);
      llvm::sys::path::append(BackDeployLib, "lib" + libraryName + ".a");
      
      if (llvm::sys::fs::exists(BackDeployLib)) {
        Arguments.push_back("-force_load");
        Arguments.push_back(context.Args.MakeArgString(BackDeployLib));
      }
    };

    #define BACK_DEPLOYMENT_LIB(Version, Filter, LibraryName) \
      addBackDeployLib(                                       \
          llvm::VersionTuple Version, BackDeployLibFilter::Filter, LibraryName);
    #include "swift/Frontend/BackDeploymentLibs.def"
  }
    
  // Add the runtime library link path, which is platform-specific and found
  // relative to the compiler.
  SmallVector<std::string, 4> RuntimeLibPaths;
  getRuntimeLibraryPaths(RuntimeLibPaths, context.Args,
                         context.OI.SDKPath, /*Shared=*/true);

  for (auto path : RuntimeLibPaths) {
    Arguments.push_back("-L");
    Arguments.push_back(context.Args.MakeArgString(path));
  }

  if (context.Args.hasFlag(options::OPT_toolchain_stdlib_rpath,
                           options::OPT_no_toolchain_stdlib_rpath, false)) {
    // If the user has explicitly asked for a toolchain stdlib, we should
    // provide one using -rpath. This used to be the default behaviour but it
    // was considered annoying in at least the SwiftPM scenario (see
    // https://bugs.swift.org/browse/SR-1967) and is obsolete in all scenarios
    // of deploying for Swift-in-the-OS. We keep it here as an optional
    // behaviour so that people downloading snapshot toolchains for testing new
    // stdlibs will be able to link to the stdlib bundled in that toolchain.
    for (auto path : RuntimeLibPaths) {
      Arguments.push_back("-rpath");
      Arguments.push_back(context.Args.MakeArgString(path));
    }
  } else if (!tripleRequiresRPathForSwiftInOS(getTriple()) ||
             context.Args.hasArg(options::OPT_no_stdlib_rpath)) {
    // If targeting an OS with Swift in /usr/lib/swift, the LC_ID_DYLIB
    // install_name the stdlib will be an absolute path like
    // /usr/lib/swift/libswiftCore.dylib, and we do not need to provide an rpath
    // at all.
    //
    // Also, if the user explicitly asks for no rpath entry, we assume they know
    // what they're doing and do not add one here.
  } else {
    // The remaining cases are back-deploying (to OSs predating
    // Swift-in-the-OS). In these cases, the stdlib will be giving us (via
    // stdlib/linker-support/magic-symbols-for-install-name.c) an LC_ID_DYLIB
    // install_name that is rpath-relative, like @rpath/libswiftCore.dylib.
    //
    // If we're linking an app bundle, it's possible there's an embedded stdlib
    // in there, in which case we'd want to put @executable_path/../Frameworks
    // in the rpath to find and prefer it, but (a) we don't know when we're
    // linking an app bundle and (b) we probably _never_ will be because Xcode
    // links using clang, not the swift driver.
    //
    // So that leaves us with the case of linking a command-line app. These are
    // only supported by installing a secondary package that puts some frozen
    // Swift-in-OS libraries in the /usr/lib/swift location. That's the best we
    // can give for rpath, though it might fail at runtime if the support
    // package isn't installed.
    Arguments.push_back("-rpath");
    Arguments.push_back(context.Args.MakeArgString("/usr/lib/swift"));
    // We don't need an rpath for /System/iOSSupport/usr/lib/swift because...
    assert(!tripleIsMacCatalystEnvironment(getTriple())
           && "macCatalyst not supported without Swift-in-the-OS");
  }
}

void
toolchains::Darwin::addProfileGenerationArgs(ArgStringList &Arguments,
                                             const JobContext &context) const {
  const llvm::Triple &Triple = getTriple();
  if (context.Args.hasArg(options::OPT_profile_generate)) {
    SmallString<128> LibProfile;
    getClangLibraryPath(context.Args, LibProfile);

    StringRef RT;
    if (Triple.isiOS()) {
      if (Triple.isTvOS())
        RT = "tvos";
      else
        RT = "ios";
    } else if (Triple.isWatchOS()) {
      RT = "watchos";
    } else {
      assert(Triple.isMacOSX());
      RT = "osx";
    }

    StringRef Sim;
    if (Triple.isSimulatorEnvironment()) {
      Sim = "sim";
    }

    llvm::sys::path::append(LibProfile,
                            "libclang_rt.profile_" + RT + Sim + ".a");

    // FIXME: Continue accepting the old path for simulator libraries for now.
    if (!Sim.empty() && !llvm::sys::fs::exists(LibProfile)) {
      llvm::sys::path::remove_filename(LibProfile);
      llvm::sys::path::append(LibProfile, "libclang_rt.profile_" + RT + ".a");
    }

    Arguments.push_back(context.Args.MakeArgString(LibProfile));
  }
}

Optional<llvm::VersionTuple>
toolchains::Darwin::getTargetSDKVersion(const llvm::Triple &triple) const {
  if (!SDKInfo)
    return None;
  return swift::getTargetSDKVersion(*SDKInfo, triple);
}

void
toolchains::Darwin::addDeploymentTargetArgs(ArgStringList &Arguments,
                                            const JobContext &context) const {
  auto addPlatformVersionArg = [&](const llvm::Triple &triple) {
    // Compute the name of the platform for the linker.
    const char *platformName;
    if (tripleIsMacCatalystEnvironment(triple)) {
      platformName = "mac-catalyst";
    } else {
      switch (getDarwinPlatformKind(triple)) {
      case DarwinPlatformKind::MacOS:
        platformName = "macos";
        break;
      case DarwinPlatformKind::IPhoneOS:
        platformName = "ios";
        break;
      case DarwinPlatformKind::IPhoneOSSimulator:
        platformName = "ios-simulator";
        break;
      case DarwinPlatformKind::TvOS:
        platformName = "tvos";
        break;
      case DarwinPlatformKind::TvOSSimulator:
        platformName = "tvos-simulator";
        break;
      case DarwinPlatformKind::WatchOS:
        platformName = "watchos";
        break;
      case DarwinPlatformKind::WatchOSSimulator:
        platformName = "watchos-simulator";
        break;
      }
    }

    // Compute the platform version.
    unsigned major, minor, micro;
    if (tripleIsMacCatalystEnvironment(triple)) {
      triple.getiOSVersion(major, minor, micro);

      // Mac Catalyst on arm was introduced with an iOS deployment target of
      // 14.0; the linker doesn't want to see a deployment target before that.
      if (major < 14 && triple.isAArch64()) {
        major = 14;
        minor = 0;
        micro = 0;
      }

      // Mac Catalyst was introduced with an iOS deployment target of 13.0;
      // the linker doesn't want to see a deployment target before that.
      if (major < 13) {
        major = 13;
        minor = 0;
        micro = 0;
      }
    } else {
      switch (getDarwinPlatformKind((triple))) {
      case DarwinPlatformKind::MacOS:
        triple.getMacOSXVersion(major, minor, micro);

        // The first deployment of arm64 for macOS is version 10.16;
        if (triple.isAArch64() && major <= 10 && minor < 16) {
          llvm::VersionTuple firstMacARM64e(10, 16, 0);
          firstMacARM64e = canonicalizePlatformVersion(PlatformKind::macOS,
                                                       firstMacARM64e);
          major = firstMacARM64e.getMajor();
          minor = firstMacARM64e.getMinor().getValueOr(0);
          micro = firstMacARM64e.getSubminor().getValueOr(0);
        }

        break;
      case DarwinPlatformKind::IPhoneOS:
      case DarwinPlatformKind::IPhoneOSSimulator:
      case DarwinPlatformKind::TvOS:
      case DarwinPlatformKind::TvOSSimulator:
        triple.getiOSVersion(major, minor, micro);

        // The first deployment of arm64 simulators is iOS/tvOS 14.0;
        // the linker doesn't want to see a deployment target before that.
        if (triple.isSimulatorEnvironment() && triple.isAArch64() &&
            major < 14) {
          major = 14;
          minor = 0;
          micro = 0;
        }

        break;
      case DarwinPlatformKind::WatchOS:
      case DarwinPlatformKind::WatchOSSimulator:
        triple.getOSVersion(major, minor, micro);
        break;
      }
    }

    // Compute the SDK version.
    unsigned sdkMajor = 0, sdkMinor = 0, sdkMicro = 0;
    if (auto sdkVersion = getTargetSDKVersion(triple)) {
      sdkMajor = sdkVersion->getMajor();
      sdkMinor = sdkVersion->getMinor().getValueOr(0);
      sdkMicro = sdkVersion->getSubminor().getValueOr(0);
    }

    Arguments.push_back("-platform_version");
    Arguments.push_back(platformName);
    addVersionString(context.Args, Arguments, major, minor, micro);
    addVersionString(context.Args, Arguments, sdkMajor, sdkMinor, sdkMicro);
  };

  addPlatformVersionArg(getTriple());

  if (auto targetVariant = getTargetVariant()) {
    assert(triplesAreValidForZippering(getTriple(), *targetVariant));
    addPlatformVersionArg(*targetVariant);
  }
}

void toolchains::Darwin::addCommonFrontendArgs(
    const OutputInfo &OI, const CommandOutput &output,
    const llvm::opt::ArgList &inputArgs,
    llvm::opt::ArgStringList &arguments) const {
  ToolChain::addCommonFrontendArgs(OI, output, inputArgs, arguments);

  if (auto sdkVersion = getTargetSDKVersion(getTriple())) {
    arguments.push_back("-target-sdk-version");
    arguments.push_back(inputArgs.MakeArgString(sdkVersion->getAsString()));
  }

  if (auto targetVariant = getTargetVariant()) {
    if (auto variantSDKVersion = getTargetSDKVersion(*targetVariant)) {
      arguments.push_back("-target-variant-sdk-version");
      arguments.push_back(
          inputArgs.MakeArgString(variantSDKVersion->getAsString()));
    }
  }
}

ToolChain::InvocationInfo
toolchains::Darwin::constructInvocation(const DynamicLinkJobAction &job,
                                        const JobContext &context) const {
  assert(context.Output.getPrimaryOutputType() == file_types::TY_Image &&
         "Invalid linker output type.");

  if (context.Args.hasFlag(options::OPT_static_executable,
                           options::OPT_no_static_executable, false)) {
    llvm::report_fatal_error("-static-executable is not supported on Darwin");
  }

  const llvm::Triple &Triple = getTriple();

  // Configure the toolchain.
  // By default, use the system `ld` to link.
  const char *LD = "ld";
  if (const Arg *A = context.Args.getLastArg(options::OPT_tools_directory)) {
    StringRef toolchainPath(A->getValue());

    // If there is a 'ld' in the toolchain folder, use that instead.
    if (auto toolchainLD =
            llvm::sys::findProgramByName("ld", {toolchainPath})) {
      LD = context.Args.MakeArgString(toolchainLD.get());
    }
  }

  InvocationInfo II = {LD};
  ArgStringList &Arguments = II.Arguments;

  addLinkerInputArgs(II, context);

  switch (job.getKind()) {
  case LinkKind::None:
    llvm_unreachable("invalid link kind");
  case LinkKind::Executable:
    // The default for ld; no extra flags necessary.
    break;
  case LinkKind::DynamicLibrary:
    Arguments.push_back("-dylib");
    break;
  case LinkKind::StaticLibrary:
    llvm_unreachable("the dynamic linker cannot build static libraries");
  }

  assert(Triple.isOSDarwin());

  // FIXME: If we used Clang as a linker instead of going straight to ld,
  // we wouldn't have to replicate a bunch of Clang's logic here.

  // Always link the regular compiler_rt if it's present.
  //
  // Note: Normally we'd just add this unconditionally, but it's valid to build
  // Swift and use it as a linker without building compiler_rt.
  SmallString<128> CompilerRTPath;
  getClangLibraryPath(context.Args, CompilerRTPath);
  llvm::sys::path::append(
      CompilerRTPath,
      Twine("libclang_rt.") +
        getDarwinLibraryNameSuffixForTriple(Triple) +
        ".a");
  if (llvm::sys::fs::exists(CompilerRTPath))
    Arguments.push_back(context.Args.MakeArgString(CompilerRTPath));

  addArgsToLinkARCLite(Arguments, context);

  if (job.shouldPerformLTO()) {
    addLTOLibArgs(Arguments, context);
  }

  for (const Arg *arg :
       context.Args.filtered(options::OPT_F, options::OPT_Fsystem)) {
    Arguments.push_back("-F");
    Arguments.push_back(arg->getValue());
  }

  if (context.Args.hasArg(options::OPT_enable_app_extension)) {
    // Keep this string fixed in case the option used by the
    // compiler itself changes.
    Arguments.push_back("-application_extension");
  }

  addSanitizerArgs(Arguments, job, context);

  if (context.Args.hasArg(options::OPT_embed_bitcode,
                          options::OPT_embed_bitcode_marker)) {
    Arguments.push_back("-bitcode_bundle");
  }

  if (!context.OI.SDKPath.empty()) {
    Arguments.push_back("-syslibroot");
    Arguments.push_back(context.Args.MakeArgString(context.OI.SDKPath));
  }

  Arguments.push_back("-lobjc");
  Arguments.push_back("-lSystem");

  Arguments.push_back("-arch");
  Arguments.push_back(context.Args.MakeArgString(getTriple().getArchName()));

  // On Darwin, we only support libc++.
  if (context.Args.hasArg(options::OPT_enable_experimental_cxx_interop)) {
    Arguments.push_back("-lc++");
  }

  addArgsToLinkStdlib(Arguments, job, context);

  addProfileGenerationArgs(Arguments, context);
  addDeploymentTargetArgs(Arguments, context);

  Arguments.push_back("-no_objc_category_merging");

  // These custom arguments should be right before the object file at the end.
  context.Args.AddAllArgs(Arguments, options::OPT_linker_option_Group);
  context.Args.AddAllArgValues(Arguments, options::OPT_Xlinker);

  // This should be the last option, for convenience in checking output.
  Arguments.push_back("-o");
  Arguments.push_back(
      context.Args.MakeArgString(context.Output.getPrimaryOutputFilename()));

  return II;
}


ToolChain::InvocationInfo
toolchains::Darwin::constructInvocation(const StaticLinkJobAction &job,
                                        const JobContext &context) const {
   assert(context.Output.getPrimaryOutputType() == file_types::TY_Image &&
         "Invalid linker output type.");

  // Configure the toolchain.
  const char *LibTool = "libtool";

  InvocationInfo II = {LibTool};
  ArgStringList &Arguments = II.Arguments;

  Arguments.push_back("-static");

  if (context.shouldUseInputFileList()) {
    Arguments.push_back("-filelist");
    Arguments.push_back(context.getTemporaryFilePath("inputs", "LinkFileList"));
    II.FilelistInfos.push_back({Arguments.back(), context.OI.CompilerOutputType,
                                FilelistInfo::WhichFiles::InputJobs});
  } else {
    addPrimaryInputsOfType(Arguments, context.Inputs, context.Args,
                           file_types::TY_Object);
    addPrimaryInputsOfType(Arguments, context.Inputs, context.Args,
                           file_types::TY_LLVM_BC);
  }

  addInputsOfType(Arguments, context.InputActions, file_types::TY_Object);
  addInputsOfType(Arguments, context.InputActions, file_types::TY_LLVM_BC);

  Arguments.push_back("-o");

  Arguments.push_back(
      context.Args.MakeArgString(context.Output.getPrimaryOutputFilename()));

  return II;
}

bool toolchains::Darwin::shouldStoreInvocationInDebugInfo() const {
  // This matches the behavior in Clang (see
  // clang/lib/driver/ToolChains/Darwin.cpp).
  if (const char *S = ::getenv("RC_DEBUG_OPTIONS"))
    return S[0] != '\0';
  return false;
}

static void validateLinkObjcRuntimeARCLiteLib(const toolchains::Darwin &TC,
                                              DiagnosticEngine &diags,
                                              const llvm::opt::ArgList &args) {
  auto Triple = TC.getTriple();
  if (args.hasFlag(options::OPT_link_objc_runtime,
                   options::OPT_no_link_objc_runtime,
                   /*Default=*/wantsObjCRuntime(Triple))) {
    llvm::SmallString<128> ARCLiteLib;
    findARCLiteLibPath(TC, ARCLiteLib);
    
    if (ARCLiteLib.empty()) {
      diags.diagnose(SourceLoc(),
                     diag::warn_arclite_not_found_when_link_objc_runtime);
    }
  }
}

static void validateDeploymentTarget(const toolchains::Darwin &TC,
                                     DiagnosticEngine &diags,
                                     const llvm::opt::ArgList &args) {
  // Check minimum supported OS versions.
  auto triple = TC.getTriple();
  if (triple.isMacOSX()) {
    if (triple.isMacOSXVersionLT(10, 9))
      diags.diagnose(SourceLoc(), diag::error_os_minimum_deployment,
                     "OS X 10.9");
  } else if (triple.isiOS()) {
    if (triple.isTvOS()) {
      if (triple.isOSVersionLT(9, 0)) {
        diags.diagnose(SourceLoc(), diag::error_os_minimum_deployment,
                       "tvOS 9.0");
        return;
      }
    }
    if (triple.isOSVersionLT(7))
      diags.diagnose(SourceLoc(), diag::error_os_minimum_deployment,
                     "iOS 7");
    if (triple.isArch32Bit() && !triple.isOSVersionLT(11)) {
      diags.diagnose(SourceLoc(), diag::error_ios_maximum_deployment_32,
                     triple.getOSMajorVersion());
    }
  } else if (triple.isWatchOS()) {
    if (triple.isOSVersionLT(2, 0)) {
      diags.diagnose(SourceLoc(), diag::error_os_minimum_deployment,
                     "watchOS 2.0");
      return;
    }
  }
}

static void validateTargetVariant(const toolchains::Darwin &TC,
                                  DiagnosticEngine &diags,
                                  const llvm::opt::ArgList &args,
                                  StringRef defaultTarget) {
  if (TC.getTargetVariant().hasValue()) {
    auto target = TC.getTriple();
    auto variant = *TC.getTargetVariant();

    if (!triplesAreValidForZippering(target, variant)) {
      diags.diagnose(SourceLoc(), diag::error_unsupported_target_variant,
                    variant.str(),
                    variant.isiOS());
    }
  }
}

void 
toolchains::Darwin::validateArguments(DiagnosticEngine &diags,
                                      const llvm::opt::ArgList &args,
                                      StringRef defaultTarget) const {
  if (!getDriver().isDummyDriverForFrontendInvocation()) {
    // Validating arclite library path when link-objc-runtime.
    // If the driver is just set up to retrieve the swift-frontend invocation,
    // we don't care about link-time, so we can skip this step, which may be
    // expensive since it might call to `xcrun` to find `clang` and `arclite`
    // relative to `clang`.
    validateLinkObjcRuntimeARCLiteLib(*this, diags, args);
  }

  // Validating apple platforms deployment targets.
  validateDeploymentTarget(*this, diags, args);
  validateTargetVariant(*this, diags, args, defaultTarget);

  // Validating darwin unsupported -static-stdlib argument.
  if (args.hasArg(options::OPT_static_stdlib)) {
    diags.diagnose(SourceLoc(), diag::error_darwin_static_stdlib_not_supported);
  }

  // If a C++ standard library is specified, it has to be libc++.
  if (auto arg = args.getLastArg(options::OPT_experimental_cxx_stdlib)) {
    if (StringRef(arg->getValue()) != "libc++") {
      diags.diagnose(SourceLoc(), diag::error_darwin_only_supports_libcxx); 
    }
  }
}

void
toolchains::Darwin::validateOutputInfo(DiagnosticEngine &diags,
                                       const OutputInfo &outputInfo) const {
  // If we have been provided with an SDK, go read the SDK information.
  if (!outputInfo.SDKPath.empty()) {
    auto SDKInfoOrErr = clang::driver::parseDarwinSDKInfo(
        *llvm::vfs::getRealFileSystem(), outputInfo.SDKPath);
    if (SDKInfoOrErr) {
      SDKInfo = *SDKInfoOrErr;
    } else {
      llvm::consumeError(SDKInfoOrErr.takeError());
      diags.diagnose(SourceLoc(), diag::warn_drv_darwin_sdk_invalid_settings);
    }
  }
}
