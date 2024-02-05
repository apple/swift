//===--- ModuleLoadingTests.cpp -------------------------------------------===//
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

#include "gtest/gtest.h"
#include "swift/AST/ASTContext.h"
#include "swift/Basic/Defer.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/ModuleInterfaceLoader.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/Serialization/Validation.h"
#include "swift/SymbolGraphGen/SymbolGraphOptions.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/VirtualFileSystem.h"

using namespace swift;

static std::string createFilename(StringRef base, StringRef name) {
  SmallString<256> path = base;
  llvm::sys::path::append(path, name);
  return llvm::Twine(path).str();
}

static bool emitFileWithContents(StringRef path, StringRef contents,
                                 std::string *pathOut = nullptr) {
  int fd;
  if (llvm::sys::fs::openFileForWrite(path, fd))
    return true;
  if (pathOut)
    *pathOut = path.str();
  llvm::raw_fd_ostream file(fd, /*shouldClose=*/true);
  file << contents;
  return false;
}

static bool emitFileWithContents(StringRef base, StringRef name,
                                 StringRef contents,
                                 std::string *pathOut = nullptr) {
  return emitFileWithContents(createFilename(base, name), contents, pathOut);
}

namespace unittest {

class OpenTrackingFileSystem : public llvm::vfs::ProxyFileSystem {
  llvm::StringMap<unsigned> numberOfOpensPerFile;
public:
  OpenTrackingFileSystem(llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fs)
    : llvm::vfs::ProxyFileSystem(fs) {}

  llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>>
  openFileForRead(const Twine &Path) override {
    numberOfOpensPerFile[Path.str()] += 1;
    return ProxyFileSystem::openFileForRead(Path);
  }

  unsigned numberOfOpens(StringRef path) {
    return numberOfOpensPerFile[path];
  }
};

class ModuleInterfaceLoaderTest : public testing::Test {
protected:
  void setupAndLoadModuleInterface() {
    SmallString<256> tempDir;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(
        "ModuleInterfaceBufferTests.emitModuleInMemory", tempDir));
    SWIFT_DEFER { llvm::sys::fs::remove_directories(tempDir); };

    auto cacheDir = createFilename(tempDir, "ModuleCache");
    ASSERT_FALSE(llvm::sys::fs::create_directory(cacheDir));

    auto prebuiltCacheDir = createFilename(tempDir, "PrebuiltModuleCache");
    ASSERT_FALSE(llvm::sys::fs::create_directory(prebuiltCacheDir));

    // Emit an interface file that we can attempt to compile.
    ASSERT_FALSE(emitFileWithContents(tempDir, "Library.swiftinterface",
        "// swift-interface-format-version: 1.0\n"
        "// swift-module-flags: -module-name TestModule -parse-stdlib\n"
        "public func foo()\n"));

    SourceManager sourceMgr;

    // Create a file system that tracks how many times a file has been opened.
    llvm::IntrusiveRefCntPtr<OpenTrackingFileSystem> fs(
      new OpenTrackingFileSystem(sourceMgr.getFileSystem()));

    sourceMgr.setFileSystem(fs);
    PrintingDiagnosticConsumer printingConsumer;
    DiagnosticEngine diags(sourceMgr);
    diags.addConsumer(printingConsumer);
    TypeCheckerOptions typecheckOpts;
    LangOptions langOpts;
    langOpts.Target = llvm::Triple(llvm::sys::getDefaultTargetTriple());
    SearchPathOptions searchPathOpts;
    ClangImporterOptions clangImpOpts;
    symbolgraphgen::SymbolGraphOptions symbolGraphOpts;
    SILOptions silOpts;
    auto ctx = ASTContext::get(langOpts, typecheckOpts, silOpts, searchPathOpts,
                               clangImpOpts, symbolGraphOpts, sourceMgr, diags);

    ctx->addModuleInterfaceChecker(
      std::make_unique<ModuleInterfaceCheckerImpl>(*ctx, cacheDir,
        prebuiltCacheDir, ModuleInterfaceLoaderOptions(),
        swift::RequireOSSAModules_t(silOpts),
        swift::RequireNoncopyableGenerics_t(langOpts)));

    auto loader = ModuleInterfaceLoader::create(
        *ctx, *static_cast<ModuleInterfaceCheckerImpl*>(
          ctx->getModuleInterfaceChecker()),
        /*dependencyTracker*/nullptr,
        ModuleLoadingMode::PreferSerialized);

    Identifier moduleName = ctx->getIdentifier("TestModule");

    std::unique_ptr<llvm::MemoryBuffer> moduleBuffer;
    std::unique_ptr<llvm::MemoryBuffer> moduleDocBuffer;
    std::unique_ptr<llvm::MemoryBuffer> moduleSourceInfoBuffer;

    auto error =
      loader->findModuleFilesInDirectory({moduleName, SourceLoc()},
        SerializedModuleBaseName(tempDir, SerializedModuleBaseName("Library")),
        /*ModuleInterfacePath=*/nullptr, /*ModuleInterfaceSourcePath=*/nullptr,
        &moduleBuffer, &moduleDocBuffer, &moduleSourceInfoBuffer,
        /*skipBuildingInterface*/ false, /*IsFramework*/false);
    ASSERT_FALSE(error);
    ASSERT_FALSE(diags.hadAnyError());

    ASSERT_NE(nullptr, moduleBuffer);

    // We should not have written a module doc file.
    ASSERT_EQ(nullptr, moduleDocBuffer);

    // Make sure the buffer identifier points to the written module.
    StringRef cachedModulePath = moduleBuffer->getBufferIdentifier();
    ASSERT_TRUE(fs->exists(cachedModulePath));

    // Assert that we've only opened this file once, to write it.
    ASSERT_EQ((unsigned)1, fs->numberOfOpens(cachedModulePath));

    auto bufOrErr = fs->getBufferForFile(cachedModulePath);
    ASSERT_TRUE(bufOrErr);

    auto bufData = (*bufOrErr)->getBuffer();
    auto validationInfo = serialization::validateSerializedAST(
        bufData, silOpts.EnableOSSAModules,
        langOpts.AssumesNoncopyableGenerics,
        /*requiredSDK*/StringRef());
    ASSERT_EQ(serialization::Status::Valid, validationInfo.status);
    ASSERT_EQ(bufData, moduleBuffer->getBuffer());
  }
};

TEST_F(ModuleInterfaceLoaderTest, LoadModuleFromBuffer) {
  setupAndLoadModuleInterface();
}

} // end namespace unittest
