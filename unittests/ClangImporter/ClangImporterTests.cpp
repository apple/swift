#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/SearchPathOptions.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Basic/LangOptions.h"
#include "swift/Basic/SourceManager.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/SymbolGraphGen/SymbolGraphOptions.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualOutputBackends.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

using namespace swift;

static std::string createFilename(StringRef base, StringRef name) {
  SmallString<256> path = base;
  llvm::sys::path::append(path, name);
  return llvm::Twine(path).str();
}

static bool emitFileWithContents(StringRef path, StringRef contents,
                                 std::string *pathOut = nullptr) {
  int FD;
  if (llvm::sys::fs::openFileForWrite(path, FD))
    return true;
  if (pathOut)
    *pathOut = path.str();
  llvm::raw_fd_ostream file(FD, /*shouldClose=*/true);
  file << contents;
  return false;
}

static bool emitFileWithContents(StringRef base, StringRef name,
                                 StringRef contents,
                                 std::string *pathOut = nullptr) {
  return emitFileWithContents(createFilename(base, name), contents, pathOut);
}

TEST(ClangImporterTest, emitPCHInMemory) {
  // Create a temporary cache on disk and clean it up at the end.
  ClangImporterOptions options;
  SmallString<256> temp;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(
      "ClangImporterTest.emitPCHInMemory", temp));
  SWIFT_DEFER { llvm::sys::fs::remove_directories(temp); };

  // Create a cache subdirectory for the modules and PCH.
  std::string cache = createFilename(temp, "cache");
  ASSERT_FALSE(llvm::sys::fs::create_directory(cache));
  options.ModuleCachePath = cache;
  options.PrecompiledHeaderOutputDir = cache;

  // Create the includes.
  std::string include = createFilename(temp, "include");
  ASSERT_FALSE(llvm::sys::fs::create_directory(include));
  options.ExtraArgs.emplace_back("-nosysteminc");
  options.ExtraArgs.emplace_back((llvm::Twine("-I") + include).str());
  ASSERT_FALSE(emitFileWithContents(include, "module.modulemap",
                                    "module A {\n"
                                    "  header \"A.h\"\n"
                                    "}\n"));
  ASSERT_FALSE(emitFileWithContents(include, "A.h", "int foo(void);\n"));

  // Create a bridging header.
  ASSERT_FALSE(emitFileWithContents(temp, "bridging.h", "#import <A.h>\n",
                                    &options.BridgingHeader));

  // Set up the importer and emit a bridging PCH.
  swift::LangOptions langOpts;
  langOpts.Target = llvm::Triple("x86_64", "apple", "darwin");
  swift::SILOptions silOpts;
  swift::TypeCheckerOptions typecheckOpts;
  INITIALIZE_LLVM();
  swift::SearchPathOptions searchPathOpts;
  swift::symbolgraphgen::SymbolGraphOptions symbolGraphOpts;
  swift::SourceManager sourceMgr;
  swift::DiagnosticEngine diags(sourceMgr);
  std::unique_ptr<ASTContext> context(
      ASTContext::get(langOpts, typecheckOpts, silOpts, searchPathOpts, options,
                      symbolGraphOpts, sourceMgr, diags));
  auto importer = ClangImporter::create(*context);

  std::string PCH = createFilename(cache, "bridging.h.pch");
  ASSERT_FALSE(importer->canReadPCH(PCH));
  ASSERT_FALSE(importer->emitBridgingPCH(options.BridgingHeader, PCH, true));
  ASSERT_TRUE(importer->canReadPCH(PCH));

  // Overwrite the PCH with garbage.  We should still be able to read it from
  // the in-memory cache.
  ASSERT_FALSE(emitFileWithContents(PCH, "garbage"));
  ASSERT_TRUE(importer->canReadPCH(PCH));
}

static StringRef getLibstdcxxModulemapContents() {
  static std::string value = ([]() -> std::string {
    auto executablePath = testing::internal::GetArgvs()[0];
    SmallString<256> libstdcxxModuleMapPath(executablePath);
    llvm::sys::path::remove_filename(libstdcxxModuleMapPath);
    llvm::sys::path::append(libstdcxxModuleMapPath, "libstdcxx.modulemap");

    auto fs = llvm::vfs::getRealFileSystem();
    auto file = fs->openFileForRead(libstdcxxModuleMapPath);
    if (!file)
      return "";
    auto buf = (*file)->getBuffer("");
    if (!buf)
      return "";
    return (*buf)->getBuffer().str();
  })();
  return value;
}

struct LibStdCxxInjectionVFS {
  llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> vfs;

  LibStdCxxInjectionVFS(const llvm::Triple &triple = llvm::Triple("x86_64",
                                                                  "redhat",
                                                                  "linux")) {
    vfs = new llvm::vfs::InMemoryFileSystem;
    tripleString =
        llvm::Twine(triple.getArchName() + "-" + triple.getVendorName() + "-" +
                    triple.getOSName())
            .str();
    osString = triple.getOSName().str();
    archString = triple.getArchName().str();
  }

  // Add devtoolset installation.
  LibStdCxxInjectionVFS &devtoolSet(StringRef version,
                                    StringRef dtName = "devtoolset") {
    // Two files needed for clang to detect the right paths / files.
    newFile(llvm::Twine("/opt/rh/" + dtName + "-") + version + "/lib/gcc/" +
            tripleString + "/" + version + "/crtbegin.o");
    newFile(llvm::Twine("/opt/rh/" + dtName + "-") + version +
            "/root/usr/lib/gcc/" + tripleString + "/" + version +
            "/crtbegin.o");

    // Libstdc++ headers needed to detect libstdc++.
    stdlibPath = (llvm::Twine("/opt/rh/" + dtName + "-") + version +
                  "/root/usr/include/c++/" + version + "/")
                     .str();
    cxxStdlibHeader("string");
    cxxStdlibHeader("cstdlib");
    cxxStdlibHeader("vector");
    return *this;
  }

  LibStdCxxInjectionVFS &cxxStdlibHeader(StringRef name) {
    assert(!stdlibPath.empty());
    newFile(llvm::Twine(stdlibPath) + name);
    return *this;
  }

  // Add a libstdc++ modulemap that's part of Swift's distribution.
  LibStdCxxInjectionVFS &libstdCxxModulemap(StringRef contents = "") {
    newFile("/usr/lib/swift/" + osString + "/" + archString +
                "/libstdcxx.modulemap",
            contents.empty() ? getLibstdcxxModulemapContents() : contents);
    return *this;
  }

private:
  std::string tripleString;
  std::string osString;
  std::string archString;
  std::string stdlibPath;

  void newFile(const llvm::Twine &path, StringRef contents = "\n") {
    vfs->addFile(path, 0, llvm::MemoryBuffer::getMemBuffer(contents));
  }
};

TEST(ClangImporterTest, libStdCxxInjectionTest) {
  // Ignore this test on Windows hosts.
  llvm::Triple Host(llvm::sys::getProcessTriple());
  if (Host.isOSWindows())
    GTEST_SKIP();

  swift::LangOptions langOpts;
  langOpts.EnableCXXInterop = true;
  langOpts.Target = llvm::Triple("x86_64", "unknown", "linux", "gnu");
  swift::SILOptions silOpts;
  swift::TypeCheckerOptions typecheckOpts;
  INITIALIZE_LLVM();
  swift::SearchPathOptions searchPathOpts;
  searchPathOpts.RuntimeResourcePath = "/usr/lib/swift";
  swift::symbolgraphgen::SymbolGraphOptions symbolGraphOpts;
  swift::SourceManager sourceMgr;
  swift::DiagnosticEngine diags(sourceMgr);
  ClangImporterOptions options;
  options.clangPath = "/usr/bin/clang";
  options.ExtraArgs.push_back(
      (llvm::Twine("--gcc-toolchain=") + "/opt/rh/devtoolset-9/root/usr")
          .str());
  options.ExtraArgs.push_back("--gcc-toolchain");
  std::unique_ptr<ASTContext> context(
      ASTContext::get(langOpts, typecheckOpts, silOpts, searchPathOpts, options,
                      symbolGraphOpts, sourceMgr, diags));

  {
    LibStdCxxInjectionVFS vfs;
    vfs.devtoolSet("9").libstdCxxModulemap("\n");
    auto paths = swift::getClangInvocationFileMapping(*context, vfs.vfs);
    ASSERT_TRUE(paths.redirectedFiles.size() == 2);
    ASSERT_TRUE(paths.overridenFiles.empty());
    EXPECT_EQ(paths.redirectedFiles[0].first,
              "/opt/rh/devtoolset-9/root/usr/include/c++/9/libstdcxx.h");
    EXPECT_EQ(paths.redirectedFiles[0].second,
              "/usr/lib/swift/linux/x86_64/libstdcxx.h");
    EXPECT_EQ(paths.redirectedFiles[1].first,
              "/opt/rh/devtoolset-9/root/usr/include/c++/9/module.modulemap");
    EXPECT_EQ(paths.redirectedFiles[1].second,
              "/usr/lib/swift/linux/x86_64/libstdcxx.modulemap");
  }

  {
    LibStdCxxInjectionVFS vfs;
    vfs.devtoolSet("9").cxxStdlibHeader("string_view").libstdCxxModulemap();
    auto paths = swift::getClangInvocationFileMapping(*context, vfs.vfs);
    ASSERT_TRUE(paths.redirectedFiles.size() == 1);
    ASSERT_TRUE(paths.overridenFiles.size() == 1);
    EXPECT_EQ(paths.redirectedFiles[0].first,
              "/opt/rh/devtoolset-9/root/usr/include/c++/9/libstdcxx.h");
    EXPECT_EQ(paths.redirectedFiles[0].second,
              "/usr/lib/swift/linux/x86_64/libstdcxx.h");
    EXPECT_EQ(paths.overridenFiles[0].first,
              "/opt/rh/devtoolset-9/root/usr/include/c++/9/module.modulemap");
    EXPECT_NE(paths.overridenFiles[0].second.find(
                  "header \"string_view\"\n  /// additional headers."),
              StringRef::npos);
  }

  {
    LibStdCxxInjectionVFS vfs;
    vfs.devtoolSet("9")
        .cxxStdlibHeader("string_view")
        .cxxStdlibHeader("codecvt")
        .cxxStdlibHeader("variant")
        .cxxStdlibHeader("optional")
        .cxxStdlibHeader("memory_resource")
        .cxxStdlibHeader("filesystem")
        .cxxStdlibHeader("charconv")
        .cxxStdlibHeader("any")
        .libstdCxxModulemap();
    auto paths = swift::getClangInvocationFileMapping(*context, vfs.vfs);
    ASSERT_TRUE(paths.redirectedFiles.size() == 1);
    ASSERT_TRUE(paths.overridenFiles.size() == 1);
    EXPECT_EQ(paths.redirectedFiles[0].first,
              "/opt/rh/devtoolset-9/root/usr/include/c++/9/libstdcxx.h");
    EXPECT_EQ(paths.redirectedFiles[0].second,
              "/usr/lib/swift/linux/x86_64/libstdcxx.h");
    EXPECT_EQ(paths.overridenFiles[0].first,
              "/opt/rh/devtoolset-9/root/usr/include/c++/9/module.modulemap");
    EXPECT_NE(
        paths.overridenFiles[0].second.find(
            "header \"codecvt\"\n  header \"any\"\n  header \"charconv\"\n  "
            "header \"filesystem\"\n  header \"memory_resource\"\n  header "
            "\"optional\"\n  header \"string_view\"\n  header \"variant\"\n  "
            "/// additional headers."),
        StringRef::npos);
  }
}
