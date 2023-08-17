//===--- ClangIncludePaths.cpp --------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "ClangIncludePaths.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsClangImporter.h"
#include "swift/Basic/Platform.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/ToolChain.h"
#include "clang/Frontend/CompilerInstance.h"
#include "llvm/WindowsDriver/MSVCPaths.h"

using namespace swift;

using Path = SmallString<128>;

static llvm::Optional<Path> getActualModuleMapPath(
    StringRef name, SearchPathOptions &Opts, const llvm::Triple &triple,
    bool isArchSpecific,
    const llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> &vfs) {
  StringRef platform = swift::getPlatformNameForTriple(triple);
  StringRef arch = swift::getMajorArchitectureName(triple);

  Path result;

  StringRef SDKPath = Opts.getSDKPath();
  if (!SDKPath.empty()) {
    result.append(SDKPath.begin(), SDKPath.end());
    llvm::sys::path::append(result, "usr", "lib", "swift");
    llvm::sys::path::append(result, platform);
    if (isArchSpecific) {
      llvm::sys::path::append(result, arch);
    }
    llvm::sys::path::append(result, name);

    // Only specify the module map if that file actually exists.  It may not;
    // for example in the case that `swiftc -target x86_64-unknown-linux-gnu
    // -emit-ir` is invoked using a Swift compiler not built for Linux targets.
    if (vfs->exists(result))
      return result;
  }

  if (!Opts.RuntimeResourcePath.empty()) {
    result.clear();
    result.append(Opts.RuntimeResourcePath.begin(),
                  Opts.RuntimeResourcePath.end());
    llvm::sys::path::append(result, platform);
    if (isArchSpecific) {
      llvm::sys::path::append(result, arch);
    }
    llvm::sys::path::append(result, name);

    // Only specify the module map if that file actually exists.  It may not;
    // for example in the case that `swiftc -target x86_64-unknown-linux-gnu
    // -emit-ir` is invoked using a Swift compiler not built for Linux targets.
    if (vfs->exists(result))
      return result;
  }

  return llvm::None;
}

/// Given an include path directory, returns a path to inject the module map to.
/// If a module map already exists, returns `None`.
static llvm::Optional<Path> getInjectedModuleMapPath(
    const Path &dir,
    const llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> &vfs) {
  Path legacyPath(dir);
  llvm::sys::path::append(legacyPath, "module.map");
  if (vfs->exists(legacyPath))
    return llvm::None;

  Path path(dir);
  llvm::sys::path::append(path, "module.modulemap");
  if (vfs->exists(path))
    return llvm::None;

  return path;
}

/// Finds the glibc.modulemap file relative to the provided resource dir.
///
/// Note that the module map used for Glibc depends on the target we're
/// compiling for, and is not included in the resource directory with the other
/// implicit module maps. It's at {freebsd|linux}/{arch}/glibc.modulemap.
static llvm::Optional<Path> getGlibcModuleMapPath(
    SearchPathOptions &Opts, const llvm::Triple &triple,
    const llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> &vfs) {
  return getActualModuleMapPath("glibc.modulemap", Opts, triple,
                                /*isArchSpecific*/ true, vfs);
}

static llvm::Optional<Path> getLibStdCxxModuleMapPath(
    SearchPathOptions &opts, const llvm::Triple &triple,
    const llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> &vfs) {
  return getActualModuleMapPath("libstdcxx.modulemap", opts, triple,
                                /*isArchSpecific*/ false, vfs);
}

llvm::Optional<SmallString<128>>
swift::getCxxShimModuleMapPath(SearchPathOptions &opts,
                               const llvm::Triple &triple) {
  return getActualModuleMapPath("libcxxshim.modulemap", opts, triple,
                                /*isArchSpecific*/ false,
                                llvm::vfs::getRealFileSystem());
}

static llvm::opt::InputArgList
parseClangDriverArgs(const clang::driver::Driver &clangDriver,
                     const ArrayRef<const char *> args) {
  unsigned unused1, unused2;
  return clangDriver.getOpts().ParseArgs(args, unused1, unused2);
}

static clang::driver::Driver
createClangDriver(const ASTContext &ctx,
                  const llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> &vfs) {
  auto clangDiags = clang::CompilerInstance::createDiagnostics(
      new clang::DiagnosticOptions());
  clang::driver::Driver clangDriver(ctx.ClangImporterOpts.clangPath,
                                    ctx.LangOpts.Target.str(), *clangDiags,
                                    "clang LLVM compiler", vfs);
  return clangDriver;
}

/// Given a list of include paths and a list of file names, finds the first
/// include path that contains files with all the names. This is useful for
/// finding the include path for a specific library among a list of include
/// paths.
///
/// \return a path without dots (`../`, './').
static llvm::Optional<Path> findFirstIncludeDir(
    const llvm::opt::InputArgList &args,
    const ArrayRef<const char *> expectedFileNames,
    const llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> &vfs) {
  // C++ stdlib paths are added as `-internal-isystem`.
  std::vector<std::string> includeDirs =
      args.getAllArgValues(clang::driver::options::OPT_internal_isystem);
  // C stdlib paths are added as `-internal-externc-isystem`.
  llvm::append_range(includeDirs,
                     args.getAllArgValues(
                         clang::driver::options::OPT_internal_externc_isystem));

  for (const auto &includeDir : includeDirs) {
    Path dir(includeDir);
    bool allExpectedExist = true;
    for (auto expectedFileName : expectedFileNames) {
      Path expectedFile(dir);
      llvm::sys::path::append(expectedFile, expectedFileName);
      if (!vfs->exists(expectedFile)) {
        allExpectedExist = false;
        break;
      }
    }

    if (allExpectedExist) {
      // VFS does not allow mapping paths that contain `../` or `./`.
      llvm::sys::path::remove_dots(dir, /*remove_dot_dot=*/true);
      return dir;
    }
  }
  return llvm::None;
}

static llvm::opt::InputArgList
createClangArgs(const ASTContext &ctx, clang::driver::Driver &clangDriver) {
  // Flags passed to Swift with `-Xcc` might affect include paths.
  std::vector<const char *> clangArgs;
  for (const auto &each : ctx.ClangImporterOpts.ExtraArgs) {
    clangArgs.push_back(each.c_str());
  }
  llvm::opt::InputArgList clangDriverArgs =
      parseClangDriverArgs(clangDriver, clangArgs);
  // If an SDK path was explicitly passed to Swift, make sure to pass it to
  // Clang driver as well. It affects the resulting include paths.
  auto sdkPath = ctx.SearchPathOpts.getSDKPath();
  if (!sdkPath.empty())
    clangDriver.SysRoot = sdkPath.str();
  return clangDriverArgs;
}

static bool shouldInjectGlibcModulemap(const llvm::Triple &triple) {
  return triple.isOSGlibc() || triple.isOSOpenBSD() || triple.isOSFreeBSD() ||
         triple.isAndroid();
}

static SmallVector<std::pair<std::string, std::string>, 2> getGlibcFileMapping(
    ASTContext &ctx,
    const llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> &vfs) {
  const llvm::Triple &triple = ctx.LangOpts.Target;
  if (!shouldInjectGlibcModulemap(triple))
    return {};

  // Extract the Glibc path from Clang driver.
  auto clangDriver = createClangDriver(ctx, vfs);
  auto clangDriverArgs = createClangArgs(ctx, clangDriver);

  llvm::opt::ArgStringList includeArgStrings;
  const auto &clangToolchain =
      clangDriver.getToolChain(clangDriverArgs, triple);
  clangToolchain.AddClangSystemIncludeArgs(clangDriverArgs, includeArgStrings);
  auto parsedIncludeArgs = parseClangDriverArgs(clangDriver, includeArgStrings);

  // Find the include path that contains Glibc headers. We use three arbitrarily
  // chosen headers to determine if the include path actually contains Glibc.
  // Ideally we would check that all of the headers referenced from the
  // modulemap are present.
  Path glibcDir;
  if (auto dir = findFirstIncludeDir(
          parsedIncludeArgs, {"inttypes.h", "unistd.h", "stdint.h"}, vfs)) {
    glibcDir = dir.value();
  } else {
    ctx.Diags.diagnose(SourceLoc(), diag::glibc_not_found, triple.str());
    return {};
  }

  Path actualModuleMapPath;
  if (auto path = getGlibcModuleMapPath(ctx.SearchPathOpts, triple, vfs))
    actualModuleMapPath = path.value();
  else
    // FIXME: Emit a warning of some kind.
    return {};

  // TODO: remove the SwiftGlibc.h header and reference all Glibc headers
  // directly from the modulemap.
  Path actualHeaderPath = actualModuleMapPath;
  llvm::sys::path::remove_filename(actualHeaderPath);
  llvm::sys::path::append(actualHeaderPath, "SwiftGlibc.h");

  Path injectedModuleMapPath(glibcDir);
  llvm::sys::path::append(injectedModuleMapPath, "module.modulemap");

  Path injectedHeaderPath(glibcDir);
  llvm::sys::path::append(injectedHeaderPath, "SwiftGlibc.h");

  return {
      {std::string(injectedModuleMapPath), std::string(actualModuleMapPath)},
      {std::string(injectedHeaderPath), std::string(actualHeaderPath)},
  };
}

static void getLibStdCxxFileMapping(
    ClangInvocationFileMapping &fileMapping, ASTContext &ctx,
    const llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> &vfs) {
  assert(ctx.LangOpts.EnableCXXInterop &&
         "libstdc++ is only injected if C++ interop is enabled");

  const llvm::Triple &triple = ctx.LangOpts.Target;
  // We currently only need this when building for Linux.
  if (!triple.isOSLinux())
    return;
  // Android uses libc++.
  if (triple.isAndroid())
    return;

  // Extract the libstdc++ installation path from Clang driver.
  auto clangDriver = createClangDriver(ctx, vfs);
  auto clangDriverArgs = createClangArgs(ctx, clangDriver);

  llvm::opt::ArgStringList stdlibArgStrings;
  const auto &clangToolchain =
      clangDriver.getToolChain(clangDriverArgs, triple);
  clangToolchain.AddClangCXXStdlibIncludeArgs(clangDriverArgs,
                                              stdlibArgStrings);
  auto parsedStdlibArgs = parseClangDriverArgs(clangDriver, stdlibArgStrings);

  Path cxxStdlibDir;
  if (auto dir = findFirstIncludeDir(parsedStdlibArgs,
                                     {"cstdlib", "string", "vector"}, vfs)) {
    cxxStdlibDir = dir.value();
  } else {
    ctx.Diags.diagnose(SourceLoc(), diag::libstdcxx_not_found, triple.str());
    return;
  }

  Path actualModuleMapPath;
  if (auto path = getLibStdCxxModuleMapPath(ctx.SearchPathOpts, triple, vfs))
    actualModuleMapPath = path.value();
  else
    return;
  // Only inject the module map if it actually exists. It may not, for example
  // if `swiftc -target x86_64-unknown-linux-gnu -emit-ir` is invoked using
  // a Swift compiler not built for Linux targets.
  if (!vfs->exists(actualModuleMapPath))
    // FIXME: emit a warning of some kind.
    return;
  // TODO: remove the libstdcxx.h header and reference all libstdc++ headers
  // directly from the modulemap.
  Path actualHeaderPath = actualModuleMapPath;
  llvm::sys::path::remove_filename(actualHeaderPath);
  llvm::sys::path::append(actualHeaderPath, "libstdcxx.h");

  // Inject a modulemap into VFS for the libstdc++ directory.
  // Only inject the module map if the module does not already exist at
  // {sysroot}/usr/include/module.{map,modulemap}.
  Path injectedModuleMapPath;
  if (auto path = getInjectedModuleMapPath(cxxStdlibDir, vfs))
    injectedModuleMapPath = path.value();
  else
    return;

  Path injectedHeaderPath(cxxStdlibDir);
  llvm::sys::path::append(injectedHeaderPath, "libstdcxx.h");

  // Add additional headers to the static libstdc++ module map, only when these
  // additional headers are present in libstdc++.
  auto file = vfs->openFileForRead(actualModuleMapPath);
  if (!file) {
    ctx.Diags.diagnose(SourceLoc(), diag::libstdcxx_modulemap_not_found,
                       triple.str());
    return;
  }
  auto buf = (*file)->getBuffer("libstdcxx.modulemap");
  if (!buf) {
    ctx.Diags.diagnose(SourceLoc(), diag::libstdcxx_modulemap_not_found,
                       triple.str());
    return;
  }

  fileMapping.redirectedFiles.push_back(
      {std::string(injectedHeaderPath), std::string(actualHeaderPath)});

  auto contents = (*buf)->getBuffer();
  auto headerInjectionPoint = contents.find("/// additional headers");
  if (headerInjectionPoint == StringRef::npos) {
    fileMapping.redirectedFiles.push_back(
        {std::string(injectedModuleMapPath), std::string(actualModuleMapPath)});
    return;
  }

  StringRef additionalFiles[] = {
      // libstdc++ 4.8.5 bundled with CentOS 7 does not include corecvt.
      "codecvt",
      // C++17 and newer:
      "any", "charconv", "filesystem", "memory_resource", "optional",
      "string_view", "variant", "bits/algorithmfwd.h", "bits/align.h",
      "bits/alloc_traits.h", "bits/allocated_ptr.h", "bits/allocator.h",
      "bits/atomic_base.h", "bits/atomic_futex.h",
      "bits/atomic_lockfree_defines.h", "bits/basic_ios.h",
      "bits/basic_string.h", "bits/c++0x_warning.h", "bits/char_traits.h",
      "bits/charconv.h", "bits/codecvt.h", "bits/concept_check.h",
      "bits/cpp_type_traits.h", "bits/cxxabi_forced.h",
      "bits/cxxabi_init_exception.h", "bits/enable_special_members.h",
      "bits/erase_if.h", "bits/exception.h", "bits/exception_defines.h",
      "bits/exception_ptr.h", "bits/forward_list.h", "bits/fs_dir.h",
      "bits/fs_fwd.h", "bits/fs_ops.h", "bits/fs_path.h", "bits/functexcept.h",
      "bits/functional_hash.h", "bits/gslice.h", "bits/gslice_array.h",
      "bits/hash_bytes.h", "bits/hashtable.h", "bits/hashtable_policy.h",
      "bits/indirect_array.h", "bits/invoke.h", "bits/ios_base.h",
      "bits/iterator_concepts.h", "bits/locale_classes.h", "bits/locale_conv.h",
      "bits/locale_facets.h", "bits/locale_facets_nonio.h", "bits/localefwd.h",
      "bits/mask_array.h", "bits/max_size_type.h", "bits/memoryfwd.h",
      "bits/move.h", "bits/nested_exception.h", "bits/node_handle.h",
      "bits/ostream_insert.h", "bits/parse_numbers.h", "bits/postypes.h",
      "bits/predefined_ops.h", "bits/ptr_traits.h", "bits/quoted_string.h",
      "bits/random.h", "bits/range_access.h", "bits/ranges_algo.h",
      "bits/ranges_algobase.h", "bits/ranges_base.h", "bits/ranges_cmp.h",
      "bits/ranges_uninitialized.h", "bits/ranges_util.h", "bits/refwrap.h",
      "bits/shared_ptr.h", "bits/shared_ptr_atomic.h", "bits/shared_ptr_base.h",
      "bits/slice_array.h", "bits/std_abs.h", "bits/std_function.h",
      "bits/std_mutex.h", "bits/std_thread.h", "bits/stl_algo.h",
      "bits/stl_algobase.h", "bits/stl_bvector.h", "bits/stl_construct.h",
      "bits/stl_deque.h", "bits/stl_function.h", "bits/stl_heap.h",
      "bits/stl_iterator.h", "bits/stl_iterator_base_funcs.h",
      "bits/stl_iterator_base_types.h", "bits/stl_list.h", "bits/stl_map.h",
      "bits/stl_multimap.h", "bits/stl_multiset.h", "bits/stl_numeric.h",
      "bits/stl_pair.h", "bits/stl_queue.h", "bits/stl_raw_storage_iter.h",
      "bits/stl_relops.h", "bits/stl_set.h", "bits/stl_stack.h",
      "bits/stl_tempbuf.h", "bits/stl_tree.h", "bits/stl_uninitialized.h",
      "bits/stl_vector.h", "bits/stream_iterator.h",
      "bits/streambuf_iterator.h", "bits/stringfwd.h",
      "bits/this_thread_sleep.h", "bits/uniform_int_dist.h",
      "bits/unique_lock.h", "bits/unique_ptr.h", "bits/unordered_map.h",
      "bits/unordered_set.h", "bits/uses_allocator.h",
      "bits/uses_allocator_args.h", "bits/valarray_after.h",
      "bits/valarray_array.h", "bits/valarray_before.h"};
  std::string additionalHeaderDirectives;
  llvm::raw_string_ostream os(additionalHeaderDirectives);
  os << contents.substr(0, headerInjectionPoint);
  SmallString<256> filePath;
  auto includeHeaderInModuleMap = [&](StringRef filename) {
    filePath.assign(cxxStdlibDir);
    llvm::sys::path::append(filePath, filename);
    if (vfs->exists(filePath))
      os << "header \"" << filename << "\"\n  ";
  };
  for (StringRef additionalFile : additionalFiles)
    includeHeaderInModuleMap(additionalFile);
  os << contents.substr(headerInjectionPoint);

  fileMapping.overridenFiles.push_back(
      {std::string(injectedModuleMapPath), std::move(os.str())});
}

namespace {
std::string
GetWindowsAuxiliaryFile(StringRef modulemap, const SearchPathOptions &Options) {
  StringRef SDKPath = Options.getSDKPath();
  if (!SDKPath.empty()) {
    llvm::SmallString<261> path{SDKPath};
    llvm::sys::path::append(path, "usr", "share", modulemap);
    if (llvm::sys::fs::exists(path))
      return path.str().str();
  }

  if (!Options.RuntimeResourcePath.empty()) {
    llvm::SmallString<261> path{Options.RuntimeResourcePath};
    llvm::sys::path::append(path, "windows", modulemap);
    if (llvm::sys::fs::exists(path))
      return path.str().str();
  }

  return "";
}

SmallVector<std::pair<std::string, std::string>, 2> GetWindowsFileMappings(
    ASTContext &Context,
    const llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> &driverVFS) {
  const llvm::Triple &Triple = Context.LangOpts.Target;
  const SearchPathOptions &SearchPathOpts = Context.SearchPathOpts;
  SmallVector<std::pair<std::string, std::string>, 2> Mappings;
  std::string AuxiliaryFile;

  if (!Triple.isWindowsMSVCEnvironment())
    return Mappings;

  clang::driver::Driver Driver = createClangDriver(Context, driverVFS);
  const llvm::opt::InputArgList Args = createClangArgs(Context, Driver);
  const clang::driver::ToolChain &ToolChain = Driver.getToolChain(Args, Triple);
  llvm::vfs::FileSystem &VFS = ToolChain.getVFS();

  struct {
    std::string Path;
    std::string IncludeVersion;
    std::string LibraryVersion;
    int MajorVersion;
  } WindowsSDK;
  if (llvm::getWindowsSDKDir(VFS, SearchPathOpts.getWinSDKRoot(),
                             SearchPathOpts.getWinSDKVersion(), {},
                             WindowsSDK.Path, WindowsSDK.MajorVersion,
                             WindowsSDK.IncludeVersion,
                             WindowsSDK.LibraryVersion)) {
    llvm::SmallString<261> WinSDKInjection{WindowsSDK.Path};
    llvm::sys::path::append(WinSDKInjection, "Include");
    if (WindowsSDK.MajorVersion > 8)
      llvm::sys::path::append(WinSDKInjection, WindowsSDK.IncludeVersion, "um");
    llvm::sys::path::append(WinSDKInjection, "module.modulemap");

    AuxiliaryFile = GetWindowsAuxiliaryFile("winsdk.modulemap", SearchPathOpts);
    if (!AuxiliaryFile.empty())
      Mappings.emplace_back(std::string(WinSDKInjection), AuxiliaryFile);
  }

  struct {
    std::string Path;
    std::string Version;
  } UCRTSDK;
  if (llvm::getUniversalCRTSdkDir(VFS, SearchPathOpts.getWinSDKRoot(),
                                  SearchPathOpts.getWinSDKVersion(), {},
                                  UCRTSDK.Path, UCRTSDK.Version)) {
    llvm::SmallString<261> UCRTInjection{UCRTSDK.Path};
    llvm::sys::path::append(UCRTInjection, "Include", UCRTSDK.Version, "ucrt");
    llvm::sys::path::append(UCRTInjection, "module.modulemap");

    AuxiliaryFile = GetWindowsAuxiliaryFile("ucrt.modulemap", SearchPathOpts);
    if (!AuxiliaryFile.empty())
      Mappings.emplace_back(std::string(UCRTInjection), AuxiliaryFile);
  }

  struct {
    std::string Path;
    llvm::ToolsetLayout Layout;
  } VCTools;
  if (llvm::findVCToolChainViaCommandLine(VFS, SearchPathOpts.getVCToolsRoot(),
                                          SearchPathOpts.getVCToolsVersion(),
                                          {}, VCTools.Path, VCTools.Layout) ||
      llvm::findVCToolChainViaEnvironment(VFS, VCTools.Path, VCTools.Layout) ||
      llvm::findVCToolChainViaSetupConfig(VFS,
                                          SearchPathOpts.getVCToolsVersion(),
                                          VCTools.Path, VCTools.Layout)) {
    assert(VCTools.Layout == llvm::ToolsetLayout::VS2017OrNewer &&
           "unsupported toolset layout (VS2017+ required)");

    llvm::SmallString<261> VCToolsInjection{VCTools.Path};
    llvm::sys::path::append(VCToolsInjection, "include");

    llvm::sys::path::append(VCToolsInjection, "module.modulemap");
    AuxiliaryFile =
        GetWindowsAuxiliaryFile("vcruntime.modulemap", SearchPathOpts);
    if (!AuxiliaryFile.empty())
      Mappings.emplace_back(std::string(VCToolsInjection), AuxiliaryFile);

    llvm::sys::path::remove_filename(VCToolsInjection);
    llvm::sys::path::append(VCToolsInjection, "vcruntime.apinotes");
    AuxiliaryFile =
        GetWindowsAuxiliaryFile("vcruntime.apinotes", SearchPathOpts);
    if (!AuxiliaryFile.empty())
      Mappings.emplace_back(std::string(VCToolsInjection), AuxiliaryFile);
  }

  return Mappings;
}
} // namespace

ClangInvocationFileMapping swift::getClangInvocationFileMapping(
    ASTContext &ctx, llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs) {
  ClangInvocationFileMapping result;
  if (!vfs)
    vfs = llvm::vfs::getRealFileSystem();
  // Android/BSD/Linux Mappings
  result.redirectedFiles.append(getGlibcFileMapping(ctx, vfs));
  if (ctx.LangOpts.EnableCXXInterop)
    getLibStdCxxFileMapping(result, ctx, vfs);

  result.redirectedFiles.append(GetWindowsFileMappings(ctx, vfs));

  return result;
}
