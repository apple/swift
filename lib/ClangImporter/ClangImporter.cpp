//===--- ClangImporter.cpp - Import Clang Modules -------------------------===//
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
//
// This file implements support for loading Clang modules into Swift.
//
//===----------------------------------------------------------------------===//
#include "swift/ClangImporter/ClangImporter.h"
#include "ClangDiagnosticConsumer.h"
#include "ImporterImpl.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ClangModuleLoader.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsClangImporter.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/ImportCache.h"
#include "swift/AST/LinkLibrary.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/Platform.h"
#include "swift/Basic/Range.h"
#include "swift/Basic/StringExtras.h"
#include "swift/Basic/Version.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Config.h"
#include "swift/Demangling/Demangle.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/Parser.h"
#include "swift/Strings.h"
#include "swift/Subsystems.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Mangle.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Basic/Module.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/Version.h"
#include "clang/CodeGen/ObjectFilePCHContainerOperations.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/Utils.h"
#include "clang/Index/IndexingAction.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Parse/Parser.h"
#include "clang/Rewrite/Frontend/FrontendActions.h"
#include "clang/Rewrite/Frontend/Rewriters.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Sema.h"
#include "clang/Serialization/ASTReader.h"
#include "clang/Serialization/ASTWriter.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/FileCollector.h"
#include "llvm/Support/Memory.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/YAMLParser.h"
#include <algorithm>
#include <memory>

using namespace swift;
using namespace importer;

// Commonly-used Clang classes.
using clang::CompilerInstance;
using clang::CompilerInvocation;

#pragma mark Internal data structures

namespace {
  class HeaderImportCallbacks : public clang::PPCallbacks {
    ClangImporter::Implementation &Impl;
  public:
    HeaderImportCallbacks(ClangImporter::Implementation &impl)
      : Impl(impl) {}

    void handleImport(const clang::Module *imported) {
      if (!imported)
        return;
      Impl.ImportedHeaderExports.push_back(
          const_cast<clang::Module *>(imported));
    }

    void InclusionDirective(clang::SourceLocation HashLoc,
                            const clang::Token &IncludeTok,
                            StringRef FileName,
                            bool IsAngled,
                            clang::CharSourceRange FilenameRange,
                            const clang::FileEntry *File,
                            StringRef SearchPath,
                            StringRef RelativePath,
                            const clang::Module *Imported,
                            clang::SrcMgr::CharacteristicKind FileType) override {
      handleImport(Imported);
    }

    void moduleImport(clang::SourceLocation ImportLoc,
                              clang::ModuleIdPath Path,
                              const clang::Module *Imported) override {
      handleImport(Imported);
    }
  };

  class PCHDeserializationCallbacks : public clang::ASTDeserializationListener {
    ClangImporter::Implementation &Impl;
  public:
    explicit PCHDeserializationCallbacks(ClangImporter::Implementation &impl)
      : Impl(impl) {}
    void ModuleImportRead(clang::serialization::SubmoduleID ID,
                          clang::SourceLocation ImportLoc) override {
      if (Impl.IsReadingBridgingPCH) {
        Impl.PCHImportedSubmodules.push_back(ID);
      }
    }
  };

  class HeaderParsingASTConsumer : public clang::ASTConsumer {
    SmallVector<clang::DeclGroupRef, 4> DeclGroups;
    PCHDeserializationCallbacks PCHCallbacks;
  public:
    explicit HeaderParsingASTConsumer(ClangImporter::Implementation &impl)
      : PCHCallbacks(impl) {}
    void
    HandleTopLevelDeclInObjCContainer(clang::DeclGroupRef decls) override {
      DeclGroups.push_back(decls);
    }

    ArrayRef<clang::DeclGroupRef> getAdditionalParsedDecls() {
      return DeclGroups;
    }

    clang::ASTDeserializationListener *GetASTDeserializationListener() override {
      return &PCHCallbacks;
    }

    void reset() {
      DeclGroups.clear();
    }
  };

  class ParsingAction : public clang::ASTFrontendAction {
    ASTContext &Ctx;
    ClangImporter &Importer;
    ClangImporter::Implementation &Impl;
    const ClangImporterOptions &ImporterOpts;
    std::string SwiftPCHHash;
  public:
    explicit ParsingAction(ASTContext &ctx,
                           ClangImporter &importer,
                           ClangImporter::Implementation &impl,
                           const ClangImporterOptions &importerOpts,
                           std::string swiftPCHHash)
      : Ctx(ctx), Importer(importer), Impl(impl), ImporterOpts(importerOpts),
        SwiftPCHHash(swiftPCHHash) {}
    std::unique_ptr<clang::ASTConsumer>
    CreateASTConsumer(clang::CompilerInstance &CI, StringRef InFile) override {
      return std::make_unique<HeaderParsingASTConsumer>(Impl);
    }
    bool BeginSourceFileAction(clang::CompilerInstance &CI) override {
      // Prefer frameworks over plain headers.
      // We add search paths here instead of when building the initial invocation
      // so that (a) we use the same code as search paths for imported modules,
      // and (b) search paths are always added after -Xcc options.
      SearchPathOptions &searchPathOpts = Ctx.SearchPathOpts;
      for (const auto &framepath : searchPathOpts.FrameworkSearchPaths) {
        Importer.addSearchPath(framepath.Path, /*isFramework*/true,
                               framepath.IsSystem);
      }

      for (auto path : searchPathOpts.ImportSearchPaths) {
        Importer.addSearchPath(path, /*isFramework*/false, /*isSystem=*/false);
      }

      auto PCH = Importer.getOrCreatePCH(ImporterOpts, SwiftPCHHash);
      if (PCH.hasValue()) {
        Impl.getClangInstance()->getPreprocessorOpts().ImplicitPCHInclude =
            PCH.getValue();
        Impl.IsReadingBridgingPCH = true;
        Impl.setSinglePCHImport(PCH.getValue());
      }

      return true;
    }
  };

  class StdStringMemBuffer : public llvm::MemoryBuffer {
    const std::string storage;
    const std::string name;
  public:
    StdStringMemBuffer(std::string &&source, StringRef name)
        : storage(std::move(source)), name(name.str()) {
      init(storage.data(), storage.data() + storage.size(),
           /*null-terminated=*/true);
    }

    StringRef getBufferIdentifier() const override {
      return name;
    }

    BufferKind getBufferKind() const override {
      return MemoryBuffer_Malloc;
    }
  };

  class ZeroFilledMemoryBuffer : public llvm::MemoryBuffer {
    const std::string name;
  public:
    explicit ZeroFilledMemoryBuffer(size_t size, StringRef name)
        : name(name.str()) {
      assert(size > 0);
      std::error_code error;
      llvm::sys::MemoryBlock memory =
          llvm::sys::Memory::allocateMappedMemory(size, nullptr,
                                                  llvm::sys::Memory::MF_READ,
                                                  error);
      assert(!error && "failed to allocated read-only zero-filled memory");
      init(static_cast<char *>(memory.base()),
           static_cast<char *>(memory.base()) + memory.allocatedSize() - 1,
           /*null-terminated*/true);
    }

    ~ZeroFilledMemoryBuffer() override {
      llvm::sys::MemoryBlock memory{const_cast<char *>(getBufferStart()),
        getBufferSize()};
      std::error_code error = llvm::sys::Memory::releaseMappedMemory(memory);
      assert(!error && "failed to deallocate read-only zero-filled memory");
      (void)error;
    }

    ZeroFilledMemoryBuffer(const ZeroFilledMemoryBuffer &) = delete;
    ZeroFilledMemoryBuffer(ZeroFilledMemoryBuffer &&) = delete;
    void operator=(const ZeroFilledMemoryBuffer &) = delete;
    void operator=(ZeroFilledMemoryBuffer &&) = delete;

    StringRef getBufferIdentifier() const override {
      return name;
    }
    BufferKind getBufferKind() const override {
      return MemoryBuffer_MMap;
    }
  };
} // end anonymous namespace

namespace {
class BridgingPPTracker : public clang::PPCallbacks {
  ClangImporter::Implementation &Impl;

public:
  BridgingPPTracker(ClangImporter::Implementation &Impl)
    : Impl(Impl) {}

private:
  static unsigned getNumModuleIdentifiers(const clang::Module *Mod) {
    unsigned Result = 1;
    while (Mod->Parent) {
      Mod = Mod->Parent;
      ++Result;
    }
    return Result;
  }

  void InclusionDirective(clang::SourceLocation HashLoc,
                          const clang::Token &IncludeTok,
                          StringRef FileName,
                          bool IsAngled,
                          clang::CharSourceRange FilenameRange,
                          const clang::FileEntry *File,
                          StringRef SearchPath,
                          StringRef RelativePath,
                          const clang::Module *Imported,
                          clang::SrcMgr::CharacteristicKind FileType) override{
    if (!Imported) {
      if (File)
        Impl.BridgeHeaderFiles.insert(File);
      return;
    }
    // Synthesize identifier locations.
    SmallVector<clang::SourceLocation, 4> IdLocs;
    for (unsigned I = 0, E = getNumModuleIdentifiers(Imported); I != E; ++I)
      IdLocs.push_back(HashLoc);
    handleImport(HashLoc, IdLocs, Imported);
  }

  void moduleImport(clang::SourceLocation ImportLoc,
                    clang::ModuleIdPath Path,
                    const clang::Module *Imported) override {
    if (!Imported)
      return;
    SmallVector<clang::SourceLocation, 4> IdLocs;
    for (auto &P : Path)
      IdLocs.push_back(P.second);
    handleImport(ImportLoc, IdLocs, Imported);
  }

  void handleImport(clang::SourceLocation ImportLoc,
                    ArrayRef<clang::SourceLocation> IdLocs,
                    const clang::Module *Imported) {
    clang::ASTContext &ClangCtx = Impl.getClangASTContext();
    clang::ImportDecl *ClangImport = clang::ImportDecl::Create(ClangCtx,
                                            ClangCtx.getTranslationUnitDecl(),
                                            ImportLoc,
                                           const_cast<clang::Module*>(Imported),
                                            IdLocs);
    Impl.BridgeHeaderTopLevelImports.push_back(ClangImport);
  }

  void MacroDefined(const clang::Token &MacroNameTok,
                    const clang::MacroDirective *MD) override {
    Impl.BridgeHeaderMacros.push_back(MacroNameTok.getIdentifierInfo());
  }
};

class ClangImporterDependencyCollector : public clang::DependencyCollector
{
  llvm::StringSet<> ExcludedPaths;
  /// The FileCollector is used by LLDB to generate reproducers. It's not used
  /// by Swift to track dependencies.
  std::shared_ptr<llvm::FileCollectorBase> FileCollector;
  const IntermoduleDepTrackingMode Mode;

public:
  ClangImporterDependencyCollector(
      IntermoduleDepTrackingMode Mode,
      std::shared_ptr<llvm::FileCollectorBase> FileCollector)
      : FileCollector(FileCollector), Mode(Mode) {}

  void excludePath(StringRef filename) {
    ExcludedPaths.insert(filename);
  }

  bool isClangImporterSpecialName(StringRef Filename) {
    using ImporterImpl = ClangImporter::Implementation;
    return (Filename == ImporterImpl::moduleImportBufferName
            || Filename == ImporterImpl::bridgingHeaderBufferName);
  }

  bool needSystemDependencies() override {
    return Mode == IntermoduleDepTrackingMode::IncludeSystem;
  }

  bool sawDependency(StringRef Filename, bool FromClangModule,
                     bool IsSystem, bool IsClangModuleFile,
                     bool IsMissing) override {
    if (!clang::DependencyCollector::sawDependency(Filename, FromClangModule,
                                                   IsSystem, IsClangModuleFile,
                                                   IsMissing))
      return false;
    // Currently preserving older ClangImporter behavior of ignoring .pcm
    // file dependencies, but possibly revisit?
    if (IsClangModuleFile
        || isClangImporterSpecialName(Filename)
        || ExcludedPaths.count(Filename))
      return false;
    return true;
  }

  void maybeAddDependency(StringRef Filename, bool FromModule, bool IsSystem,
                          bool IsModuleFile, bool IsMissing) override {
    if (FileCollector)
      FileCollector->addFile(Filename);
    clang::DependencyCollector::maybeAddDependency(
        Filename, FromModule, IsSystem, IsModuleFile, IsMissing);
  }
};
} // end anonymous namespace

std::shared_ptr<clang::DependencyCollector>
ClangImporter::createDependencyCollector(
    IntermoduleDepTrackingMode Mode,
    std::shared_ptr<llvm::FileCollectorBase> FileCollector) {
  return std::make_shared<ClangImporterDependencyCollector>(Mode,
                                                            FileCollector);
}

void ClangImporter::Implementation::addBridgeHeaderTopLevelDecls(
    clang::Decl *D) {
  if (shouldIgnoreBridgeHeaderTopLevelDecl(D))
    return;

  BridgeHeaderTopLevelDecls.push_back(D);
}

bool importer::isForwardDeclOfType(const clang::Decl *D) {
  if (auto *ID = dyn_cast<clang::ObjCInterfaceDecl>(D)) {
    if (!ID->isThisDeclarationADefinition())
      return true;
  } else if (auto PD = dyn_cast<clang::ObjCProtocolDecl>(D)) {
    if (!PD->isThisDeclarationADefinition())
      return true;
  } else if (auto TD = dyn_cast<clang::TagDecl>(D)) {
    if (!TD->isThisDeclarationADefinition())
      return true;
  }
  return false;
}

bool ClangImporter::Implementation::shouldIgnoreBridgeHeaderTopLevelDecl(
    clang::Decl *D) {
  return importer::isForwardDeclOfType(D);
}

ClangImporter::ClangImporter(ASTContext &ctx,
                             DependencyTracker *tracker,
                             DWARFImporterDelegate *dwarfImporterDelegate)
    : ClangModuleLoader(tracker),
      Impl(*new Implementation(ctx, dwarfImporterDelegate)) {
}

ClangImporter::~ClangImporter() {
  delete &Impl;
}

#pragma mark Module loading

/// Finds the glibc.modulemap file relative to the provided resource dir.
///
/// Note that the module map used for Glibc depends on the target we're
/// compiling for, and is not included in the resource directory with the other
/// implicit module maps. It's at {freebsd|linux}/{arch}/glibc.modulemap.
static Optional<StringRef>
getGlibcModuleMapPath(SearchPathOptions& Opts, llvm::Triple triple,
                      SmallVectorImpl<char> &buffer) {
  StringRef platform = swift::getPlatformNameForTriple(triple);
  StringRef arch = swift::getMajorArchitectureName(triple);

  if (!Opts.SDKPath.empty()) {
    buffer.clear();
    buffer.append(Opts.SDKPath.begin(), Opts.SDKPath.end());
    llvm::sys::path::append(buffer, "usr", "lib", "swift");
    llvm::sys::path::append(buffer, platform, arch, "glibc.modulemap");

    // Only specify the module map if that file actually exists.  It may not;
    // for example in the case that `swiftc -target x86_64-unknown-linux-gnu
    // -emit-ir` is invoked using a Swift compiler not built for Linux targets.
    if (llvm::sys::fs::exists(buffer))
      return StringRef(buffer.data(), buffer.size());
  }

  if (!Opts.RuntimeResourcePath.empty()) {
    buffer.clear();
    buffer.append(Opts.RuntimeResourcePath.begin(),
                  Opts.RuntimeResourcePath.end());
    llvm::sys::path::append(buffer, platform, arch, "glibc.modulemap");

    // Only specify the module map if that file actually exists.  It may not;
    // for example in the case that `swiftc -target x86_64-unknown-linux-gnu
    // -emit-ir` is invoked using a Swift compiler not built for Linux targets.
    if (llvm::sys::fs::exists(buffer))
      return StringRef(buffer.data(), buffer.size());
  }

  return None;
}

static Optional<StringRef>
getWasiLibcModuleMapPath(SearchPathOptions& Opts, llvm::Triple triple,
                         SmallVectorImpl<char> &buffer) {
  StringRef platform = swift::getPlatformNameForTriple(triple);
  StringRef arch = swift::getMajorArchitectureName(triple);

  if (!Opts.SDKPath.empty()) {
    buffer.clear();
    buffer.append(Opts.SDKPath.begin(), Opts.SDKPath.end());
    llvm::sys::path::append(buffer, "usr", "lib", "swift");
    llvm::sys::path::append(buffer, platform, arch, "wasi.modulemap");

    // Only specify the module map if that file actually exists.  It may not;
    // for example in the case that `swiftc -target x86_64-unknown-linux-gnu
    // -emit-ir` is invoked using a Swift compiler not built for Linux targets.
    if (llvm::sys::fs::exists(buffer))
      return StringRef(buffer.data(), buffer.size());
  }

  if (!Opts.RuntimeResourcePath.empty()) {
    buffer.clear();
    buffer.append(Opts.RuntimeResourcePath.begin(),
                  Opts.RuntimeResourcePath.end());
    llvm::sys::path::append(buffer, platform, arch, "wasi.modulemap");

    // Only specify the module map if that file actually exists.  It may not;
    // for example in the case that `swiftc -target x86_64-unknown-linux-gnu
    // -emit-ir` is invoked using a Swift compiler not built for Linux targets.
    if (llvm::sys::fs::exists(buffer))
      return StringRef(buffer.data(), buffer.size());
  }

  return None;
}

void
importer::getNormalInvocationArguments(
    std::vector<std::string> &invocationArgStrs,
    ASTContext &ctx) {
  const auto &LangOpts = ctx.LangOpts;
  const llvm::Triple &triple = LangOpts.Target;
  SearchPathOptions &searchPathOpts = ctx.SearchPathOpts;
  ClangImporterOptions &importerOpts = ctx.ClangImporterOpts;
  auto languageVersion = ctx.LangOpts.EffectiveLanguageVersion;

  if (llvm::sys::path::extension(importerOpts.BridgingHeader)
          .endswith(file_types::getExtension(file_types::TY_PCH))) {
    invocationArgStrs.insert(invocationArgStrs.end(), {
        "-include-pch", importerOpts.BridgingHeader
    });
  }

  // If there are no shims in the resource dir, add a search path in the SDK.
  SmallString<128> shimsPath(searchPathOpts.RuntimeResourcePath);
  llvm::sys::path::append(shimsPath, "shims");
  if (!llvm::sys::fs::exists(shimsPath)) {
    shimsPath = searchPathOpts.SDKPath;
    llvm::sys::path::append(shimsPath, "usr", "lib", "swift", "shims");
    invocationArgStrs.insert(invocationArgStrs.end(),
                             {"-isystem", std::string(shimsPath.str())});
  }

  // Construct the invocation arguments for the current target.
  // Add target-independent options first.
  invocationArgStrs.insert(invocationArgStrs.end(), {
      // Don't emit LLVM IR.
      "-fsyntax-only",

      // Enable block support.
      "-fblocks",

      languageVersion.preprocessorDefinition("__swift__", {10000, 100, 1}),

      "-fretain-comments-from-system-headers",

      "-isystem", searchPathOpts.RuntimeResourcePath,
  });

  // Enable Position Independence.  `-fPIC` is not supported on Windows, which
  // is implicitly position independent.
  if (!triple.isOSWindows())
    invocationArgStrs.insert(invocationArgStrs.end(), {"-fPIC"});

  // Enable modules.
  invocationArgStrs.insert(invocationArgStrs.end(), {
      "-fmodules",
      "-Xclang", "-fmodule-feature", "-Xclang", "swift"
  });
  // Don't enforce strict rules when inside the debugger to work around search
  // path problems caused by a module existing in both the build/install
  // directory and the source directory.
  if (!importerOpts.DebuggerSupport)
    invocationArgStrs.push_back(
        "-Werror=non-modular-include-in-framework-module");

  bool EnableCXXInterop = LangOpts.EnableCXXInterop;

  if (LangOpts.EnableObjCInterop) {
    invocationArgStrs.insert(invocationArgStrs.end(), {"-fobjc-arc"});
    // TODO: Investigate whether 7.0 is a suitable default version.
    if (!triple.isOSDarwin())
      invocationArgStrs.insert(invocationArgStrs.end(),
                               {"-fobjc-runtime=ios-7.0"});

    invocationArgStrs.insert(invocationArgStrs.end(), {
      "-x", EnableCXXInterop ? "objective-c++" : "objective-c",
    });
  } else {
    invocationArgStrs.insert(invocationArgStrs.end(), {
      "-x", EnableCXXInterop ? "c++" : "c",
    });
  }

  {
    const clang::LangStandard &stdcxx =
#if defined(CLANG_DEFAULT_STD_CXX)
        *clang::LangStandard::getLangStandardForName(CLANG_DEFAULT_STD_CXX);
#else
        clang::LangStandard::getLangStandardForKind(
            clang::LangStandard::lang_gnucxx14);
#endif

    const clang::LangStandard &stdc =
#if defined(CLANG_DEFAULT_STD_C)
        *clang::LangStandard::getLangStandardForName(CLANG_DEFAULT_STD_C);
#else
        clang::LangStandard::getLangStandardForKind(
            clang::LangStandard::lang_gnu11);
#endif

    invocationArgStrs.insert(invocationArgStrs.end(), {
      (Twine("-std=") + StringRef(EnableCXXInterop ? stdcxx.getName()
                                                   : stdc.getName())).str()
    });
  }

  // Set C language options.
  if (triple.isOSDarwin()) {
    invocationArgStrs.insert(invocationArgStrs.end(), {
      // Avoid including the iso646.h header because some headers from OS X
      // frameworks are broken by it.
      "-D_ISO646_H_", "-D__ISO646_H",

      // Request new APIs from AppKit.
      "-DSWIFT_SDK_OVERLAY_APPKIT_EPOCH=2",

      // Request new APIs from Foundation.
      "-DSWIFT_SDK_OVERLAY_FOUNDATION_EPOCH=8",

      // Request new APIs from SceneKit.
      "-DSWIFT_SDK_OVERLAY2_SCENEKIT_EPOCH=3",

      // Request new APIs from GameplayKit.
      "-DSWIFT_SDK_OVERLAY_GAMEPLAYKIT_EPOCH=1",

      // Request new APIs from SpriteKit.
      "-DSWIFT_SDK_OVERLAY_SPRITEKIT_EPOCH=1",

      // Request new APIs from CoreImage.
      "-DSWIFT_SDK_OVERLAY_COREIMAGE_EPOCH=2",

      // Request new APIs from libdispatch.
      "-DSWIFT_SDK_OVERLAY_DISPATCH_EPOCH=2",

      // Request new APIs from libpthread
      "-DSWIFT_SDK_OVERLAY_PTHREAD_EPOCH=1",

      // Request new APIs from CoreGraphics.
      "-DSWIFT_SDK_OVERLAY_COREGRAPHICS_EPOCH=0",

      // Request new APIs from UIKit.
      "-DSWIFT_SDK_OVERLAY_UIKIT_EPOCH=2",

      // Backwards compatibility for headers that were checking this instead of
      // '__swift__'.
      "-DSWIFT_CLASS_EXTRA=",
    });

    // Get the version of this compiler and pass it to C/Objective-C
    // declarations.
    auto V = version::Version::getCurrentCompilerVersion();
    if (!V.empty()) {
      invocationArgStrs.insert(invocationArgStrs.end(), {
        V.preprocessorDefinition("__SWIFT_COMPILER_VERSION",
                                 {1000000000, /*ignored*/ 0, 1000000, 1000, 1}),
      });
    }
  } else {
    // Ideally we should turn this on for all Glibc targets that are actually
    // using Glibc or a libc that respects that flag. This will cause some
    // source breakage however (specifically with strerror_r()) on Linux
    // without a workaround.
    if (triple.isOSFuchsia() || triple.isAndroid()) {
      // Many of the modern libc features are hidden behind feature macros like
      // _GNU_SOURCE or _XOPEN_SOURCE.
      invocationArgStrs.insert(invocationArgStrs.end(), {
        "-D_GNU_SOURCE",
      });
    }

    if (triple.isOSWASI()) {
      invocationArgStrs.insert(invocationArgStrs.end(), {"-D_WASI_EMULATED_MMAN"});
      SmallString<128> buffer;
      if (auto path = getWasiLibcModuleMapPath(searchPathOpts, triple, buffer)) {
        invocationArgStrs.push_back((Twine("-fmodule-map-file=") + *path).str());
      }
    }

    if (triple.isOSWindows()) {
      switch (triple.getArch()) {
      default: llvm_unreachable("unsupported Windows architecture");
      case llvm::Triple::arm:
      case llvm::Triple::thumb:
        invocationArgStrs.insert(invocationArgStrs.end(), {"-D_ARM_"});
        break;
      case llvm::Triple::aarch64:
      case llvm::Triple::aarch64_32:
        invocationArgStrs.insert(invocationArgStrs.end(), {"-D_ARM64_"});
        break;
      case llvm::Triple::x86:
        invocationArgStrs.insert(invocationArgStrs.end(), {"-D_X86_"});
        break;
      case llvm::Triple::x86_64:
        invocationArgStrs.insert(invocationArgStrs.end(), {"-D_AMD64_"});
        break;
      }
    }

    SmallString<128> buffer;
    if (auto path = getGlibcModuleMapPath(searchPathOpts, triple, buffer)) {
      invocationArgStrs.push_back((Twine("-fmodule-map-file=") + *path).str());
    } else {
      // FIXME: Emit a warning of some kind.
    }
  }

  if (searchPathOpts.SDKPath.empty()) {
    invocationArgStrs.push_back("-Xclang");
    invocationArgStrs.push_back("-nostdsysteminc");
  } else {
    if (triple.isWindowsMSVCEnvironment()) {
      llvm::SmallString<261> path; // MAX_PATH + 1
      path = searchPathOpts.SDKPath;
      llvm::sys::path::append(path, "usr", "include");
      llvm::sys::path::native(path);

      invocationArgStrs.push_back("-isystem");
      invocationArgStrs.push_back(std::string(path.str()));
    } else {
      // On Darwin, Clang uses -isysroot to specify the include
      // system root. On other targets, it seems to use --sysroot.
      invocationArgStrs.push_back(triple.isOSDarwin() ? "-isysroot"
                                                      : "--sysroot");
      invocationArgStrs.push_back(searchPathOpts.SDKPath);
    }
  }

  const std::string &moduleCachePath = importerOpts.ModuleCachePath;
  if (!moduleCachePath.empty()) {
    invocationArgStrs.push_back("-fmodules-cache-path=");
    invocationArgStrs.back().append(moduleCachePath);
  }

  if (ctx.SearchPathOpts.DisableModulesValidateSystemDependencies) {
    invocationArgStrs.push_back("-fno-modules-validate-system-headers");
  } else {
    invocationArgStrs.push_back("-fmodules-validate-system-headers");
  }

  if (importerOpts.DetailedPreprocessingRecord) {
    invocationArgStrs.insert(invocationArgStrs.end(), {
      "-Xclang", "-detailed-preprocessing-record",
      "-Xclang", "-fmodule-format=raw",
    });
  } else {
    invocationArgStrs.insert(invocationArgStrs.end(), {
      "-Xclang", "-fmodule-format=obj",
    });
  }

  // Enable API notes alongside headers/in frameworks.
  invocationArgStrs.push_back("-fapinotes-modules");
  invocationArgStrs.push_back("-fapinotes-swift-version=" +
                              languageVersion.asAPINotesVersionString());
  invocationArgStrs.push_back("-iapinotes-modules");
  invocationArgStrs.push_back((llvm::Twine(searchPathOpts.RuntimeResourcePath) +
                               llvm::sys::path::get_separator() +
                               "apinotes").str());
}

static void
getEmbedBitcodeInvocationArguments(std::vector<std::string> &invocationArgStrs,
                                   ASTContext &ctx) {
  invocationArgStrs.insert(invocationArgStrs.end(), {
    // Backend mode.
    "-fembed-bitcode",

    // ...but Clang isn't doing the emission.
    "-fsyntax-only",

    "-x", "ir",
  });
}

void
importer::addCommonInvocationArguments(
    std::vector<std::string> &invocationArgStrs,
    ASTContext &ctx) {
  using ImporterImpl = ClangImporter::Implementation;
  const llvm::Triple &triple = ctx.LangOpts.Target;
  SearchPathOptions &searchPathOpts = ctx.SearchPathOpts;
  const ClangImporterOptions &importerOpts = ctx.ClangImporterOpts;

  invocationArgStrs.push_back("-target");
  invocationArgStrs.push_back(triple.str());

  if (ctx.LangOpts.SDKVersion) {
    invocationArgStrs.push_back("-Xclang");
    invocationArgStrs.push_back(
        "-target-sdk-version=" + ctx.LangOpts.SDKVersion->getAsString());
  }

  invocationArgStrs.push_back(ImporterImpl::moduleImportBufferName);

  if (ctx.LangOpts.EnableAppExtensionRestrictions) {
    invocationArgStrs.push_back("-fapplication-extension");
  }

  if (!importerOpts.TargetCPU.empty()) {
    invocationArgStrs.push_back("-mcpu=" + importerOpts.TargetCPU);

  } else if (triple.isOSDarwin()) {
    // Special case CPU based on known deployments:
    //   - arm64 deploys to apple-a7
    //   - arm64 on macOS
    //   - arm64 for iOS/tvOS/watchOS simulators
    //   - arm64e deploys to apple-a12
    // and arm64e (everywhere) and arm64e macOS defaults to the "apple-a12" CPU
    // for Darwin, but Clang only detects this if we use -arch.
    if (triple.getArchName() == "arm64e")
      invocationArgStrs.push_back("-mcpu=apple-a12");
    else if (triple.isAArch64() && triple.isMacOSX())
      invocationArgStrs.push_back("-mcpu=apple-a12");
    else if (triple.isAArch64() && triple.isSimulatorEnvironment() &&
             (triple.isiOS() || triple.isWatchOS()))
      invocationArgStrs.push_back("-mcpu=apple-a12");
    else if (triple.getArch() == llvm::Triple::aarch64 ||
             triple.getArch() == llvm::Triple::aarch64_32 ||
             triple.getArch() == llvm::Triple::aarch64_be) {
      invocationArgStrs.push_back("-mcpu=apple-a7");
    }
  } else if (triple.getArch() == llvm::Triple::systemz) {
    invocationArgStrs.push_back("-march=z13");
  }

  if (!importerOpts.Optimization.empty()) {
    invocationArgStrs.push_back(importerOpts.Optimization);
  }

  const std::string &overrideResourceDir = importerOpts.OverrideResourceDir;
  if (overrideResourceDir.empty()) {
    llvm::SmallString<128> resourceDir(searchPathOpts.RuntimeResourcePath);

    // Adjust the path to refer to our copy of the Clang resource directory
    // under 'lib/swift/clang', which is either a real resource directory or a
    // symlink to one inside of a full Clang installation.
    //
    // The rationale for looking under the Swift resource directory and not
    // assuming that the Clang resource directory is located next to it is that
    // Swift, when installed separately, should not need to install files in
    // directories that are not "owned" by it.
    llvm::sys::path::append(resourceDir, "clang");

    // Set the Clang resource directory to the path we computed.
    invocationArgStrs.push_back("-resource-dir");
    invocationArgStrs.push_back(std::string(resourceDir.str()));
  } else {
    invocationArgStrs.push_back("-resource-dir");
    invocationArgStrs.push_back(overrideResourceDir);
  }

  if (!importerOpts.IndexStorePath.empty()) {
    invocationArgStrs.push_back("-index-store-path");
    invocationArgStrs.push_back(importerOpts.IndexStorePath);
  }

  invocationArgStrs.push_back("-fansi-escape-codes");

  for (auto extraArg : importerOpts.ExtraArgs) {
    invocationArgStrs.push_back(extraArg);
  }
}

bool ClangImporter::canReadPCH(StringRef PCHFilename) {
  if (!llvm::sys::fs::exists(PCHFilename))
    return false;

  // FIXME: The following attempts to do an initial ReadAST invocation to verify
  // the PCH, without causing trouble for the existing CompilerInstance.
  // Look into combining creating the ASTReader along with verification + update
  // if necessary, so that we can create and use one ASTReader in the common case
  // when there is no need for update.
  clang::CompilerInstance CI(Impl.Instance->getPCHContainerOperations(),
                             &Impl.Instance->getModuleCache());
  auto invocation =
      std::make_shared<clang::CompilerInvocation>(*Impl.Invocation);
  invocation->getPreprocessorOpts().DisablePCHOrModuleValidation =
      clang::DisableValidationForModuleKind::None;
  invocation->getPreprocessorOpts().AllowPCHWithCompilerErrors = false;
  invocation->getHeaderSearchOpts().ModulesValidateSystemHeaders = true;
  invocation->getLangOpts()->NeededByPCHOrCompilationUsesPCH = true;
  invocation->getLangOpts()->CacheGeneratedPCH = true;

  // ClangImporter::create adds a remapped MemoryBuffer that we don't need
  // here.  Moreover, it's a raw pointer owned by the preprocessor options; if
  // we don't clear the range then both the original and new CompilerInvocation
  // will try to free it.
  invocation->getPreprocessorOpts().RemappedFileBuffers.clear();

  CI.setInvocation(std::move(invocation));
  CI.setTarget(&Impl.Instance->getTarget());
  CI.setDiagnostics(
      &*clang::CompilerInstance::createDiagnostics(new clang::DiagnosticOptions()));

  // Note: Reusing the file manager is safe; this is a component that's already
  // reused when building PCM files for the module cache.
  CI.createSourceManager(Impl.Instance->getFileManager());
  auto &clangSrcMgr = CI.getSourceManager();
  auto FID = clangSrcMgr.createFileID(
                        std::make_unique<ZeroFilledMemoryBuffer>(1, "<main>"));
  clangSrcMgr.setMainFileID(FID);
  auto &diagConsumer = CI.getDiagnosticClient();
  diagConsumer.BeginSourceFile(CI.getLangOpts());
  SWIFT_DEFER {
    diagConsumer.EndSourceFile();
  };

  // Pass in TU_Complete, which is the default mode for the Preprocessor
  // constructor and the right one for reading a PCH.
  CI.createPreprocessor(clang::TU_Complete);
  CI.createASTContext();
  CI.createASTReader();
  clang::ASTReader &Reader = *CI.getASTReader();

  auto failureCapabilities =
    clang::ASTReader::ARR_Missing |
    clang::ASTReader::ARR_OutOfDate |
    clang::ASTReader::ARR_VersionMismatch;

  auto result = Reader.ReadAST(PCHFilename, clang::serialization::MK_PCH,
                               clang::SourceLocation(), failureCapabilities);
  switch (result) {
  case clang::ASTReader::Success:
    return true;
  case clang::ASTReader::Failure:
  case clang::ASTReader::Missing:
  case clang::ASTReader::OutOfDate:
  case clang::ASTReader::VersionMismatch:
    return false;
  case clang::ASTReader::ConfigurationMismatch:
  case clang::ASTReader::HadErrors:
    assert(0 && "unexpected ASTReader failure for PCH validation");
    return false;
  }
  llvm_unreachable("unhandled result");
}

Optional<std::string>
ClangImporter::getPCHFilename(const ClangImporterOptions &ImporterOptions,
                              StringRef SwiftPCHHash, bool &isExplicit) {
  if (llvm::sys::path::extension(ImporterOptions.BridgingHeader)
        .endswith(file_types::getExtension(file_types::TY_PCH))) {
    isExplicit = true;
    return ImporterOptions.BridgingHeader;
  }
  isExplicit = false;

  const auto &BridgingHeader = ImporterOptions.BridgingHeader;
  const auto &PCHOutputDir = ImporterOptions.PrecompiledHeaderOutputDir;
  if (SwiftPCHHash.empty() || BridgingHeader.empty() || PCHOutputDir.empty()) {
    return None;
  }

  SmallString<256> PCHBasename { llvm::sys::path::filename(BridgingHeader) };
  llvm::sys::path::replace_extension(PCHBasename, "");
  PCHBasename.append("-swift_");
  PCHBasename.append(SwiftPCHHash);
  PCHBasename.append("-clang_");
  PCHBasename.append(getClangModuleHash());
  PCHBasename.append(".pch");
  SmallString<256> PCHFilename { PCHOutputDir };
  llvm::sys::path::append(PCHFilename, PCHBasename);
  return PCHFilename.str().str();
}


Optional<std::string>
ClangImporter::getOrCreatePCH(const ClangImporterOptions &ImporterOptions,
                              StringRef SwiftPCHHash) {
  bool isExplicit;
  auto PCHFilename = getPCHFilename(ImporterOptions, SwiftPCHHash,
                                    isExplicit);
  if (!PCHFilename.hasValue()) {
    return None;
  }
  if (!isExplicit && !ImporterOptions.PCHDisableValidation &&
      !canReadPCH(PCHFilename.getValue())) {
    StringRef parentDir = llvm::sys::path::parent_path(PCHFilename.getValue());
    std::error_code EC = llvm::sys::fs::create_directories(parentDir);
    if (EC) {
      llvm::errs() << "failed to create directory '" << parentDir << "': "
        << EC.message();
      return None;
    }
    auto FailedToEmit = emitBridgingPCH(ImporterOptions.BridgingHeader,
                                        PCHFilename.getValue());
    if (FailedToEmit) {
      return None;
    }
  }

  return PCHFilename.getValue();
}

std::vector<std::string>
ClangImporter::getClangArguments(ASTContext &ctx) {
  if (ctx.ClangImporterOpts.ExtraArgsOnly) {
    return ctx.ClangImporterOpts.ExtraArgs;
  }
  std::vector<std::string> invocationArgStrs;
  // Clang expects this to be like an actual command line. So we need to pass in
  // "clang" for argv[0]
  invocationArgStrs.push_back("clang");
  switch (ctx.ClangImporterOpts.Mode) {
  case ClangImporterOptions::Modes::Normal:
  case ClangImporterOptions::Modes::PrecompiledModule:
    getNormalInvocationArguments(invocationArgStrs, ctx);
    break;
  case ClangImporterOptions::Modes::EmbedBitcode:
    getEmbedBitcodeInvocationArguments(invocationArgStrs, ctx);
    break;
  }
  addCommonInvocationArguments(invocationArgStrs, ctx);
  return invocationArgStrs;
}

std::unique_ptr<clang::CompilerInvocation>
ClangImporter::createClangInvocation(ClangImporter *importer,
                                     const ClangImporterOptions &importerOpts,
                                     ArrayRef<std::string> invocationArgStrs,
                                     std::vector<std::string> *CC1Args) {
  std::vector<const char *> invocationArgs;
  invocationArgs.reserve(invocationArgStrs.size());
  for (auto &argStr : invocationArgStrs)
    invocationArgs.push_back(argStr.c_str());
  // Set up a temporary diagnostic client to report errors from parsing the
  // command line, which may be important for Swift clients if, for example,
  // they're using -Xcc options. Unfortunately this diagnostic engine has to
  // use the default options because the /actual/ options haven't been parsed
  // yet.
  //
  // The long-term client for Clang diagnostics is set up below, after the
  // clang::CompilerInstance is created.
  llvm::IntrusiveRefCntPtr<clang::DiagnosticOptions> tempDiagOpts{
    new clang::DiagnosticOptions
  };

  ClangDiagnosticConsumer tempDiagClient{importer->Impl, *tempDiagOpts,
                                         importerOpts.DumpClangDiagnostics};
  llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine> tempClangDiags =
      clang::CompilerInstance::createDiagnostics(tempDiagOpts.get(),
                                                 &tempDiagClient,
                                                 /*owned*/false);

  auto CI = clang::createInvocationFromCommandLine(
      invocationArgs, tempClangDiags, nullptr, false, CC1Args);

  if (!CI) {
    return CI;
  }

  // FIXME: clang fails to generate a module if there is a `-fmodule-map-file`
  // argument pointing to a missing file.
  // Such missing module files occur frequently in SourceKit. If the files are
  // missing, SourceKit fails to build SwiftShims (which wouldn't have required
  // the missing module file), thus fails to load the stdlib and hence looses
  // all semantic functionality.
  // To work around this issue, drop all `-fmodule-map-file` arguments pointing
  // to missing files and report the error that clang would throw manually.
  // rdar://77516546 is tracking that the clang importer should be more
  // resilient and provide a module even if there were building it.
  auto VFS = clang::createVFSFromCompilerInvocation(
      *CI, *tempClangDiags,
      importer->Impl.SwiftContext.SourceMgr.getFileSystem());
  std::vector<std::string> FilteredModuleMapFiles;
  for (auto ModuleMapFile : CI->getFrontendOpts().ModuleMapFiles) {
    if (VFS->exists(ModuleMapFile)) {
      FilteredModuleMapFiles.push_back(ModuleMapFile);
    } else {
      importer->Impl.diagnose(SourceLoc(), diag::module_map_not_found,
                              ModuleMapFile);
    }
  }
  CI->getFrontendOpts().ModuleMapFiles = FilteredModuleMapFiles;

  return CI;
}

std::unique_ptr<ClangImporter>
ClangImporter::create(ASTContext &ctx,
                      std::string swiftPCHHash, DependencyTracker *tracker,
                      DWARFImporterDelegate *dwarfImporterDelegate) {
  std::unique_ptr<ClangImporter> importer{
      new ClangImporter(ctx, tracker, dwarfImporterDelegate)};
  auto &importerOpts = ctx.ClangImporterOpts;
  importer->Impl.ClangArgs = getClangArguments(ctx);
  ArrayRef<std::string> invocationArgStrs = importer->Impl.ClangArgs;
  if (importerOpts.DumpClangDiagnostics) {
    llvm::errs() << "'";
    llvm::interleave(
        invocationArgStrs, [](StringRef arg) { llvm::errs() << arg; },
        [] { llvm::errs() << "' '"; });
    llvm::errs() << "'\n";
  }



  if (llvm::sys::path::extension(importerOpts.BridgingHeader)
        .endswith(file_types::getExtension(file_types::TY_PCH))) {
    importer->Impl.setSinglePCHImport(importerOpts.BridgingHeader);
    importer->Impl.IsReadingBridgingPCH = true;
    if (tracker) {
      // Currently ignoring dependency on bridging .pch files because they are
      // temporaries; if and when they are no longer temporaries, this condition
      // should be removed.
      auto &coll = static_cast<ClangImporterDependencyCollector &>(
        *tracker->getClangCollector());
      coll.excludePath(importerOpts.BridgingHeader);
    }
  }

  // Create a new Clang compiler invocation.
  {
    importer->Impl.Invocation = createClangInvocation(importer.get(),
                                                      importerOpts,
                                                      invocationArgStrs);
    if (!importer->Impl.Invocation)
      return nullptr;
  }

  {
    // Create an almost-empty memory buffer.
    auto sourceBuffer = llvm::MemoryBuffer::getMemBuffer(
      "extern int __swift __attribute__((unavailable));",
      Implementation::moduleImportBufferName);
    clang::PreprocessorOptions &ppOpts =
        importer->Impl.Invocation->getPreprocessorOpts();
    ppOpts.addRemappedFile(Implementation::moduleImportBufferName,
                           sourceBuffer.release());
  }

  // Install a Clang module file extension to build Swift name lookup tables.
  importer->Impl.Invocation->getFrontendOpts().ModuleFileExtensions.push_back(
      std::make_shared<SwiftNameLookupExtension>(
          importer->Impl.BridgingHeaderLookupTable,
          importer->Impl.LookupTables, importer->Impl.SwiftContext,
          importer->Impl.getBufferImporterForDiagnostics(),
          importer->Impl.platformAvailability));

  // Create a compiler instance.
  {
    auto PCHContainerOperations =
      std::make_shared<clang::PCHContainerOperations>();
    PCHContainerOperations->registerWriter(
        std::make_unique<clang::ObjectFilePCHContainerWriter>());
    PCHContainerOperations->registerReader(
        std::make_unique<clang::ObjectFilePCHContainerReader>());
    importer->Impl.Instance.reset(
        new clang::CompilerInstance(std::move(PCHContainerOperations)));
  }
  auto &instance = *importer->Impl.Instance;
  instance.setInvocation(importer->Impl.Invocation);

  if (tracker)
    instance.addDependencyCollector(tracker->getClangCollector());

  {
    // Now set up the real client for Clang diagnostics---configured with proper
    // options---as opposed to the temporary one we made above.
    auto actualDiagClient = std::make_unique<ClangDiagnosticConsumer>(
        importer->Impl, instance.getDiagnosticOpts(),
        importerOpts.DumpClangDiagnostics);
    instance.createDiagnostics(actualDiagClient.release());
  }

  // Set up the file manager.
  {
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS =
        clang::createVFSFromCompilerInvocation(instance.getInvocation(),
                                               instance.getDiagnostics(),
                                               ctx.SourceMgr.getFileSystem());
    instance.createFileManager(std::move(VFS));
  }

  // Don't stop emitting messages if we ever can't load a module.
  // FIXME: This is actually a general problem: any "fatal" error could mess up
  // the CompilerInvocation when we're not in "show diagnostics after fatal
  // error" mode.
  clang::DiagnosticsEngine &clangDiags = instance.getDiagnostics();
  clangDiags.setSeverity(clang::diag::err_module_not_found,
                         clang::diag::Severity::Error,
                         clang::SourceLocation());
  clangDiags.setSeverity(clang::diag::err_module_not_built,
                         clang::diag::Severity::Error,
                         clang::SourceLocation());
  clangDiags.setFatalsAsError(ctx.Diags.getShowDiagnosticsAfterFatalError());


  // Create the associated action.
  importer->Impl.Action.reset(new ParsingAction(ctx, *importer,
                                                importer->Impl,
                                                importerOpts,
                                                swiftPCHHash));
  auto *action = importer->Impl.Action.get();

  // Execute the action. We effectively inline most of
  // CompilerInstance::ExecuteAction here, because we need to leave the AST
  // open for future module loading.
  // FIXME: This has to be cleaned up on the Clang side before we can improve
  // things here.

  // Create the target instance.
  instance.setTarget(
    clang::TargetInfo::CreateTargetInfo(clangDiags,
                                        instance.getInvocation().TargetOpts));
  if (!instance.hasTarget())
    return nullptr;

  // Inform the target of the language options.
  //
  // FIXME: We shouldn't need to do this, the target should be immutable once
  // created. This complexity should be lifted elsewhere.
  instance.getTarget().adjust(instance.getLangOpts());

  if (importerOpts.Mode == ClangImporterOptions::Modes::EmbedBitcode)
    return importer;

  // ClangImporter always sets this in Normal mode, so we need to make sure to
  // set it before bailing out early when configuring ClangImporter for
  // precompiled modules. This is not a benign langopt, so forgetting this (for
  // example, if we combined the early exit below with the one above) would make
  // the compiler instance used to emit PCMs incompatible with the one used to
  // read them later.
  instance.getLangOpts().NeededByPCHOrCompilationUsesPCH = true;

  if (importerOpts.Mode == ClangImporterOptions::Modes::PrecompiledModule)
    return importer;

  bool canBegin = action->BeginSourceFile(instance,
                                          instance.getFrontendOpts().Inputs[0]);
  if (!canBegin)
    return nullptr; // there was an error related to the compiler arguments.

  clang::Preprocessor &clangPP = instance.getPreprocessor();
  clangPP.enableIncrementalProcessing();

  // Setup Preprocessor callbacks before initialing the parser to make sure
  // we catch implicit includes.
  auto ppTracker = std::make_unique<BridgingPPTracker>(importer->Impl);
  clangPP.addPPCallbacks(std::move(ppTracker));

  instance.createASTReader();

  // Manually run the action, so that the TU stays open for additional parsing.
  instance.createSema(action->getTranslationUnitKind(), nullptr);
  importer->Impl.Parser.reset(new clang::Parser(clangPP, instance.getSema(),
                                                /*SkipFunctionBodies=*/false));

  clangPP.EnterMainSourceFile();
  importer->Impl.Parser->Initialize();

  importer->Impl.nameImporter.reset(new NameImporter(
      importer->Impl.SwiftContext, importer->Impl.platformAvailability,
      importer->Impl.getClangSema()));

  // FIXME: These decls are not being parsed correctly since (a) some of the
  // callbacks are still being added, and (b) the logic to parse them has
  // changed.
  clang::Parser::DeclGroupPtrTy parsed;
  while (!importer->Impl.Parser->ParseTopLevelDecl(parsed)) {
    for (auto *D : parsed.get()) {
      importer->Impl.addBridgeHeaderTopLevelDecls(D);

      if (auto named = dyn_cast<clang::NamedDecl>(D)) {
        addEntryToLookupTable(*importer->Impl.BridgingHeaderLookupTable, named,
                              *importer->Impl.nameImporter);
      }
    }
  }

  // FIXME: This is missing implicit includes.
  auto *CB = new HeaderImportCallbacks(importer->Impl);
  clangPP.addPPCallbacks(std::unique_ptr<clang::PPCallbacks>(CB));

  // Create the selectors we'll be looking for.
  auto &clangContext = importer->Impl.Instance->getASTContext();
  importer->Impl.objectAtIndexedSubscript
    = clangContext.Selectors.getUnarySelector(
        &clangContext.Idents.get("objectAtIndexedSubscript"));
  clang::IdentifierInfo *setObjectAtIndexedSubscriptIdents[2] = {
    &clangContext.Idents.get("setObject"),
    &clangContext.Idents.get("atIndexedSubscript")
  };
  importer->Impl.setObjectAtIndexedSubscript
    = clangContext.Selectors.getSelector(2, setObjectAtIndexedSubscriptIdents);
  importer->Impl.objectForKeyedSubscript
    = clangContext.Selectors.getUnarySelector(
        &clangContext.Idents.get("objectForKeyedSubscript"));
  clang::IdentifierInfo *setObjectForKeyedSubscriptIdents[2] = {
    &clangContext.Idents.get("setObject"),
    &clangContext.Idents.get("forKeyedSubscript")
  };
  importer->Impl.setObjectForKeyedSubscript
    = clangContext.Selectors.getSelector(2, setObjectForKeyedSubscriptIdents);

  // Set up the imported header module.
  auto *importedHeaderModule =
      ModuleDecl::create(ctx.getIdentifier(CLANG_HEADER_MODULE_NAME), ctx);
  importer->Impl.ImportedHeaderUnit =
    new (ctx) ClangModuleUnit(*importedHeaderModule, importer->Impl, nullptr);
  importedHeaderModule->addFile(*importer->Impl.ImportedHeaderUnit);
  importedHeaderModule->setHasResolvedImports();

  importer->Impl.IsReadingBridgingPCH = false;

  return importer;
}

bool ClangImporter::addSearchPath(StringRef newSearchPath, bool isFramework,
                                  bool isSystem) {
  clang::FileManager &fileMgr = Impl.Instance->getFileManager();
  auto optionalEntry = fileMgr.getOptionalDirectoryRef(newSearchPath);
  if (!optionalEntry)
    return true;
  auto entry = *optionalEntry;

  auto &headerSearchInfo = Impl.getClangPreprocessor().getHeaderSearchInfo();
  auto exists = std::any_of(headerSearchInfo.search_dir_begin(),
                            headerSearchInfo.search_dir_end(),
                            [&](const clang::DirectoryLookup &lookup) -> bool {
    if (isFramework)
      return lookup.getFrameworkDir() == &entry.getDirEntry();
    return lookup.getDir() == &entry.getDirEntry();
  });
  if (exists) {
    // Don't bother adding a search path that's already there. Clang would have
    // removed it via deduplication at the time the search path info gets built.
    return false;
  }

  auto kind = isSystem ? clang::SrcMgr::C_System : clang::SrcMgr::C_User;
  headerSearchInfo.AddSearchPath({entry, kind, isFramework},
                                 /*isAngled=*/true);

  // In addition to changing the current preprocessor directly, we still need
  // to change the options structure for future module-building.
  Impl.Instance->getHeaderSearchOpts().AddPath(newSearchPath,
                   isSystem ? clang::frontend::System : clang::frontend::Angled,
                                               isFramework,
                                               /*IgnoreSysRoot=*/true);
  return false;
}

clang::SourceLocation
ClangImporter::Implementation::getNextIncludeLoc() {
  clang::SourceManager &srcMgr = getClangInstance()->getSourceManager();

  if (!DummyIncludeBuffer.isValid()) {
    clang::SourceLocation includeLoc =
        srcMgr.getLocForStartOfFile(srcMgr.getMainFileID());
    // Picking the beginning of the main FileID as include location is also what
    // the clang PCH mechanism is doing (see
    // clang::ASTReader::getImportLocation()). Choose the next source location
    // here to avoid having the exact same import location as the clang PCH.
    // Otherwise, if we are using a PCH for bridging header, we'll have
    // problems with source order comparisons of clang source locations not
    // being deterministic.
    includeLoc = includeLoc.getLocWithOffset(1);
    DummyIncludeBuffer = srcMgr.createFileID(
        std::make_unique<ZeroFilledMemoryBuffer>(
          256*1024, StringRef(moduleImportBufferName)),
        clang::SrcMgr::C_User, /*LoadedID*/0, /*LoadedOffset*/0, includeLoc);
  }

  clang::SourceLocation clangImportLoc =
      srcMgr.getLocForStartOfFile(DummyIncludeBuffer)
            .getLocWithOffset(IncludeCounter++);
  assert(srcMgr.isInFileID(clangImportLoc, DummyIncludeBuffer) &&
         "confused Clang's source manager with our fake locations");
  return clangImportLoc;
}

bool ClangImporter::Implementation::importHeader(
    ModuleDecl *adapter, StringRef headerName, SourceLoc diagLoc,
    bool trackParsedSymbols,
    std::unique_ptr<llvm::MemoryBuffer> sourceBuffer,
    bool implicitImport) {

  // Don't even try to load the bridging header if the Clang AST is in a bad
  // state. It could cause a crash.
  auto &clangDiags = getClangASTContext().getDiagnostics();
  if (clangDiags.hasUnrecoverableErrorOccurred())
    return true;

  assert(adapter);
  ImportedHeaderOwners.push_back(adapter);

  bool hadError = clangDiags.hasErrorOccurred();

  clang::SourceManager &sourceMgr = getClangInstance()->getSourceManager();
  clang::FileID bufferID = sourceMgr.createFileID(std::move(sourceBuffer),
                                                  clang::SrcMgr::C_User,
                                                  /*LoadedID=*/0,
                                                  /*LoadedOffset=*/0,
                                                  getNextIncludeLoc());
  auto &consumer =
      static_cast<HeaderParsingASTConsumer &>(Instance->getASTConsumer());
  consumer.reset();

  clang::Preprocessor &pp = getClangPreprocessor();
  pp.EnterSourceFile(bufferID, /*Dir=*/nullptr, /*Loc=*/{});
  // Force the import to occur.
  pp.LookAhead(0);

  SmallVector<clang::DeclGroupRef, 16> allParsedDecls;
  auto handleParsed = [&](clang::DeclGroupRef parsed) {
    if (trackParsedSymbols) {
      for (auto *D : parsed) {
        addBridgeHeaderTopLevelDecls(D);
      }
    }

    allParsedDecls.push_back(parsed);
  };

  clang::Parser::DeclGroupPtrTy parsed;
  while (!Parser->ParseTopLevelDecl(parsed)) {
    if (parsed)
      handleParsed(parsed.get());
    for (auto additionalParsedGroup : consumer.getAdditionalParsedDecls())
      handleParsed(additionalParsedGroup);
    consumer.reset();
  }

  // We're trying to discourage (and eventually deprecate) the use of implicit
  // bridging-header imports triggered by IMPORTED_HEADER blocks in
  // modules. There are two sub-cases to consider:
  //
  //   #1 The implicit import actually occurred.
  //
  //   #2 The user explicitly -import-objc-header'ed some header or PCH that
  //      makes the implicit import redundant.
  //
  // It's not obvious how to exactly differentiate these cases given the
  // interface clang gives us, but we only want to warn on case #1, and the
  // non-emptiness of allParsedDecls is a _definite_ sign that we're in case
  // #1. So we treat that as an approximation of the condition we're after, and
  // accept that we might fail to warn in the odd case where "the import
  // occurred" but didn't introduce any new decls.
  //
  // We also want to limit (for now) the warning in case #1 to invocations that
  // requested an explicit bridging header, because otherwise the warning will
  // complain in a very common scenario (unit test w/o bridging header imports
  // application w/ bridging header) that we don't yet have Xcode automation
  // to correct. The fix would be explicitly importing on the command line.
  if (implicitImport && !allParsedDecls.empty() &&
    BridgingHeaderExplicitlyRequested) {
    diagnose(
      diagLoc, diag::implicit_bridging_header_imported_from_module,
      llvm::sys::path::filename(headerName), adapter->getName());
  }

  // We can't do this as we're parsing because we may want to resolve naming
  // conflicts between the things we've parsed.
  for (auto group : allParsedDecls)
    for (auto *D : group)
      if (auto named = dyn_cast<clang::NamedDecl>(D))
        addEntryToLookupTable(*BridgingHeaderLookupTable, named,
                              getNameImporter());

  pp.EndSourceFile();
  bumpGeneration();

  // Add any defined macros to the bridging header lookup table.
  addMacrosToLookupTable(*BridgingHeaderLookupTable, getNameImporter());

  // Finish loading any extra modules that were (transitively) imported.
  handleDeferredImports(diagLoc);

  // Wrap all Clang imports under a Swift import decl.
  for (auto &Import : BridgeHeaderTopLevelImports) {
    if (auto *ClangImport = Import.dyn_cast<clang::ImportDecl*>()) {
      Import = createImportDecl(SwiftContext, adapter, ClangImport, {});
    }
  }

  // Finalize the lookup table, which may fail.
  finalizeLookupTable(*BridgingHeaderLookupTable, getNameImporter(),
                      getBufferImporterForDiagnostics());

  // FIXME: What do we do if there was already an error?
  if (!hadError && clangDiags.hasErrorOccurred()) {
    diagnose(diagLoc, diag::bridging_header_error, headerName);
    return true;
  }

  return false;
}

bool ClangImporter::importHeader(StringRef header, ModuleDecl *adapter,
                                 off_t expectedSize, time_t expectedModTime,
                                 StringRef cachedContents, SourceLoc diagLoc) {
  clang::FileManager &fileManager = Impl.Instance->getFileManager();
  auto headerFile = fileManager.getFile(header, /*OpenFile=*/true);
  if (headerFile && (*headerFile)->getSize() == expectedSize &&
      (*headerFile)->getModificationTime() == expectedModTime) {
    return importBridgingHeader(header, adapter, diagLoc, false, true);
  }

  // If we've made it to here, this is some header other than the bridging
  // header, which means we can no longer rely on one file's modification time
  // to invalidate code completion caches. :-(
  Impl.setSinglePCHImport(None);

  if (!cachedContents.empty() && cachedContents.back() == '\0')
    cachedContents = cachedContents.drop_back();
  std::unique_ptr<llvm::MemoryBuffer> sourceBuffer{
    llvm::MemoryBuffer::getMemBuffer(cachedContents, header)
  };
  return Impl.importHeader(adapter, header, diagLoc, /*trackParsedSymbols=*/false,
                           std::move(sourceBuffer), true);
}

bool ClangImporter::importBridgingHeader(StringRef header, ModuleDecl *adapter,
                                         SourceLoc diagLoc,
                                         bool trackParsedSymbols,
                                         bool implicitImport) {
  if (llvm::sys::path::extension(header)
        .endswith(file_types::getExtension(file_types::TY_PCH))) {
    Impl.ImportedHeaderOwners.push_back(adapter);
    // We already imported this with -include-pch above, so we should have
    // collected a bunch of PCH-encoded module imports that we just need to
    // replay in handleDeferredImports.
    Impl.handleDeferredImports(diagLoc);
    return false;
  }

  clang::FileManager &fileManager = Impl.Instance->getFileManager();
  auto headerFile = fileManager.getFile(header, /*OpenFile=*/true);
  if (!headerFile) {
    Impl.diagnose(diagLoc, diag::bridging_header_missing, header);
    return true;
  }

  llvm::SmallString<128> importLine;
  if (Impl.SwiftContext.LangOpts.EnableObjCInterop)
    importLine = "#import \"";
  else
    importLine = "#include \"";

  importLine += header;
  importLine += "\"\n";

  std::unique_ptr<llvm::MemoryBuffer> sourceBuffer{
    llvm::MemoryBuffer::getMemBufferCopy(
      importLine, Implementation::bridgingHeaderBufferName)
  };
  return Impl.importHeader(adapter, header, diagLoc, trackParsedSymbols,
                           std::move(sourceBuffer), implicitImport);
}

std::string ClangImporter::getBridgingHeaderContents(StringRef headerPath,
                                                     off_t &fileSize,
                                                     time_t &fileModTime) {
  auto invocation =
      std::make_shared<clang::CompilerInvocation>(*Impl.Invocation);

  invocation->getFrontendOpts().DisableFree = false;
  invocation->getFrontendOpts().Inputs.clear();
  invocation->getFrontendOpts().Inputs.push_back(
      clang::FrontendInputFile(headerPath, clang::Language::ObjC));

  invocation->getPreprocessorOpts().resetNonModularOptions();

  clang::CompilerInstance rewriteInstance(
    Impl.Instance->getPCHContainerOperations(),
    &Impl.Instance->getModuleCache());
  rewriteInstance.setInvocation(invocation);
  rewriteInstance.createDiagnostics(new clang::IgnoringDiagConsumer);

  clang::FileManager &fileManager = Impl.Instance->getFileManager();
  rewriteInstance.setFileManager(&fileManager);
  rewriteInstance.createSourceManager(fileManager);
  rewriteInstance.setTarget(&Impl.Instance->getTarget());

  std::string result;
  bool success = llvm::CrashRecoveryContext().RunSafelyOnThread([&] {
    // A much simpler version of clang::RewriteIncludesAction that lets us
    // write to an in-memory buffer.
    class RewriteIncludesAction : public clang::PreprocessorFrontendAction {
      raw_ostream &OS;

      void ExecuteAction() override {
        clang::CompilerInstance &compiler = getCompilerInstance();
        clang::RewriteIncludesInInput(compiler.getPreprocessor(), &OS,
                                      compiler.getPreprocessorOutputOpts());
      }
    public:
      explicit RewriteIncludesAction(raw_ostream &os) : OS(os) {}
    };

    llvm::raw_string_ostream os(result);
    RewriteIncludesAction action(os);
    rewriteInstance.ExecuteAction(action);
  });

  success |= !rewriteInstance.getDiagnostics().hasErrorOccurred();
  if (!success) {
    Impl.diagnose({}, diag::could_not_rewrite_bridging_header);
    return "";
  }

  if (auto fileInfo = fileManager.getFile(headerPath)) {
    fileSize = (*fileInfo)->getSize();
    fileModTime = (*fileInfo)->getModificationTime();
  }
  return result;
}

/// Returns the appropriate source input language based on language options.
static clang::Language getLanguageFromOptions(
    const clang::LangOptions *LangOpts) {
  if (LangOpts->OpenCL)
    return clang::Language::OpenCL;
  if (LangOpts->CUDA)
    return clang::Language::CUDA;
  if (LangOpts->ObjC)
    return LangOpts->CPlusPlus ?
        clang::Language::ObjCXX : clang::Language::ObjC;
  return LangOpts->CPlusPlus ? clang::Language::CXX : clang::Language::C;
}

/// Wraps the given frontend action in an index data recording action if the
/// frontend options have an index store path specified.
static
std::unique_ptr<clang::FrontendAction> wrapActionForIndexingIfEnabled(
    const clang::FrontendOptions &FrontendOpts,
    std::unique_ptr<clang::FrontendAction> action) {
  if (!FrontendOpts.IndexStorePath.empty()) {
    return clang::index::createIndexDataRecordingAction(
        FrontendOpts, std::move(action));
  }
  return action;
}

std::unique_ptr<clang::CompilerInstance>
ClangImporter::cloneCompilerInstanceForPrecompiling() {
  auto invocation =
      std::make_shared<clang::CompilerInvocation>(*Impl.Invocation);

  auto &PPOpts = invocation->getPreprocessorOpts();
  PPOpts.resetNonModularOptions();

  auto &FrontendOpts = invocation->getFrontendOpts();
  FrontendOpts.DisableFree = false;
  FrontendOpts.Inputs.clear();

  auto clonedInstance = std::make_unique<clang::CompilerInstance>(
    Impl.Instance->getPCHContainerOperations(),
    &Impl.Instance->getModuleCache());
  clonedInstance->setInvocation(std::move(invocation));
  clonedInstance->createDiagnostics(&Impl.Instance->getDiagnosticClient(),
                                    /*ShouldOwnClient=*/false);

  clang::FileManager &fileManager = Impl.Instance->getFileManager();
  clonedInstance->setFileManager(&fileManager);
  clonedInstance->createSourceManager(fileManager);
  clonedInstance->setTarget(&Impl.Instance->getTarget());

  return clonedInstance;
}

bool
ClangImporter::emitBridgingPCH(StringRef headerPath,
                               StringRef outputPCHPath) {
  auto emitInstance = cloneCompilerInstanceForPrecompiling();
  auto &invocation = emitInstance->getInvocation();

  auto LangOpts = invocation.getLangOpts();
  LangOpts->NeededByPCHOrCompilationUsesPCH = true;
  LangOpts->CacheGeneratedPCH = true;

  auto language = getLanguageFromOptions(LangOpts);
  auto inputFile = clang::FrontendInputFile(headerPath, language);

  auto &FrontendOpts = invocation.getFrontendOpts();
  FrontendOpts.Inputs = {inputFile};
  FrontendOpts.OutputFile = outputPCHPath.str();
  FrontendOpts.ProgramAction = clang::frontend::GeneratePCH;

  auto action = wrapActionForIndexingIfEnabled(
      FrontendOpts, std::make_unique<clang::GeneratePCHAction>());
  emitInstance->ExecuteAction(*action);

  if (emitInstance->getDiagnostics().hasErrorOccurred()) {
    Impl.diagnose({}, diag::bridging_header_pch_error,
                  outputPCHPath, headerPath);
    return true;
  }
  return false;
}

bool ClangImporter::runPreprocessor(StringRef inputPath, StringRef outputPath) {
  auto emitInstance = cloneCompilerInstanceForPrecompiling();
  auto &invocation = emitInstance->getInvocation();
  auto LangOpts = invocation.getLangOpts();
  auto &OutputOpts = invocation.getPreprocessorOutputOpts();
  OutputOpts.ShowCPP = 1;
  OutputOpts.ShowComments = 0;
  OutputOpts.ShowLineMarkers = 0;
  OutputOpts.ShowMacros = 0;
  OutputOpts.ShowMacroComments = 0;
  auto language = getLanguageFromOptions(LangOpts);
  auto inputFile = clang::FrontendInputFile(inputPath, language);

  auto &FrontendOpts = invocation.getFrontendOpts();
  FrontendOpts.Inputs = {inputFile};
  FrontendOpts.OutputFile = outputPath.str();
  FrontendOpts.ProgramAction = clang::frontend::PrintPreprocessedInput;

  auto action = wrapActionForIndexingIfEnabled(
      FrontendOpts, std::make_unique<clang::PrintPreprocessedAction>());
  emitInstance->ExecuteAction(*action);
  return emitInstance->getDiagnostics().hasErrorOccurred();
}

bool ClangImporter::emitPrecompiledModule(StringRef moduleMapPath,
                                          StringRef moduleName,
                                          StringRef outputPath) {
  auto emitInstance = cloneCompilerInstanceForPrecompiling();
  auto &invocation = emitInstance->getInvocation();

  auto LangOpts = invocation.getLangOpts();
  LangOpts->setCompilingModule(clang::LangOptions::CMK_ModuleMap);
  LangOpts->ModuleName = moduleName.str();
  LangOpts->CurrentModule = LangOpts->ModuleName;

  auto language = getLanguageFromOptions(LangOpts);

  auto &FrontendOpts = invocation.getFrontendOpts();
  auto inputFile = clang::FrontendInputFile(
      moduleMapPath, clang::InputKind(
          language, clang::InputKind::ModuleMap, false),
      FrontendOpts.IsSystemModule);
  FrontendOpts.Inputs = {inputFile};
  FrontendOpts.OriginalModuleMap = moduleMapPath.str();
  FrontendOpts.OutputFile = outputPath.str();
  FrontendOpts.ProgramAction = clang::frontend::GenerateModule;

  auto action = wrapActionForIndexingIfEnabled(
      FrontendOpts,
      std::make_unique<clang::GenerateModuleFromModuleMapAction>());
  emitInstance->ExecuteAction(*action);

  if (emitInstance->getDiagnostics().hasErrorOccurred()) {
    Impl.diagnose({}, diag::emit_pcm_error, outputPath, moduleMapPath);
    return true;
  }
  return false;
}

bool ClangImporter::dumpPrecompiledModule(StringRef modulePath,
                                          StringRef outputPath) {
  auto dumpInstance = cloneCompilerInstanceForPrecompiling();
  auto &invocation = dumpInstance->getInvocation();

  auto inputFile = clang::FrontendInputFile(
      modulePath, clang::InputKind(
          clang::Language::Unknown, clang::InputKind::Precompiled, false));

  auto &FrontendOpts = invocation.getFrontendOpts();
  FrontendOpts.Inputs = {inputFile};
  FrontendOpts.OutputFile = outputPath.str();

  auto action = std::make_unique<clang::DumpModuleInfoAction>();
  dumpInstance->ExecuteAction(*action);

  if (dumpInstance->getDiagnostics().hasErrorOccurred()) {
    Impl.diagnose({}, diag::dump_pcm_error, modulePath);
    return true;
  }
  return false;
}

void ClangImporter::collectVisibleTopLevelModuleNames(
    SmallVectorImpl<Identifier> &names) const {
  SmallVector<clang::Module *, 32> Modules;
  Impl.getClangPreprocessor().getHeaderSearchInfo().collectAllModules(Modules);
  for (auto &M : Modules) {
    if (!M->isAvailable())
      continue;

    names.push_back(
        Impl.SwiftContext.getIdentifier(M->getTopLevelModuleName()));
  }
}

void ClangImporter::collectSubModuleNames(
    ImportPath::Module path,
    std::vector<std::string> &names) const {
  auto &clangHeaderSearch = Impl.getClangPreprocessor().getHeaderSearchInfo();

  // Look up the top-level module first.
  clang::Module *clangModule = clangHeaderSearch.lookupModule(
      path.front().Item.str(), /*AllowSearch=*/true,
      /*AllowExtraModuleMapSearch=*/true);
  if (!clangModule)
    return;
  clang::Module *submodule = clangModule;
  for (auto component : path.getSubmodulePath()) {
    submodule = submodule->findSubmodule(component.Item.str());
    if (!submodule)
      return;
  }
  for (auto sub : submodule->submodules())
    names.push_back(sub->Name);
}

bool ClangImporter::isModuleImported(const clang::Module *M) {
  return M->NameVisibility == clang::Module::NameVisibilityKind::AllVisible;
}

static std::string getScalaNodeText(llvm::yaml::Node *N) {
  SmallString<32> Buffer;
  return cast<llvm::yaml::ScalarNode>(N)->getValue(Buffer).str();
}

bool ClangImporter::canImportModule(ImportPath::Element moduleID,
                                    llvm::VersionTuple version,
                                    bool underlyingVersion) {
  // Look up the top-level module to see if it exists.
  // FIXME: This only works with top-level modules.
  auto &clangHeaderSearch = Impl.getClangPreprocessor().getHeaderSearchInfo();
  clang::Module *clangModule =
      clangHeaderSearch.lookupModule(moduleID.Item.str(), /*AllowSearch=*/true,
                                     /*AllowExtraModuleMapSearch=*/true);
  if (!clangModule) {
    return false;
  }

  clang::Module::Requirement r;
  clang::Module::UnresolvedHeaderDirective mh;
  clang::Module *m;
  auto &ctx = Impl.getClangASTContext();
  auto available = clangModule->isAvailable(ctx.getLangOpts(), getTargetInfo(),
                                            r, mh, m);
  if (!available)
    return false;
  if (version.empty())
    return true;
  assert(available);
  assert(!version.empty());
  llvm::VersionTuple currentVersion;
  StringRef path = getClangASTContext().getSourceManager()
    .getFilename(clangModule->DefinitionLoc);
  // Look for the .tbd file inside .framework dir to get the project version
  // number.
  std::string fwName = (llvm::Twine(moduleID.Item.str()) + ".framework").str();
  auto pos = path.find(fwName);
  while (pos != StringRef::npos) {
    llvm::SmallString<256> buffer(path.substr(0, pos + fwName.size()));
    llvm::sys::path::append(buffer, llvm::Twine(moduleID.Item.str()) + ".tbd");
    auto tbdPath = buffer.str();
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> tbdBufOrErr =
      llvm::MemoryBuffer::getFile(tbdPath);
    // .tbd file doesn't exist, break.
    if (!tbdBufOrErr) {
      break;
    }
    StringRef tbdBuffer = tbdBufOrErr->get()->getBuffer();

    // Use a new source manager instead of the one from ASTContext because we
    // don't want the Json file to be persistent.
    SourceManager SM;
    llvm::yaml::Stream Stream(llvm::MemoryBufferRef(tbdBuffer, tbdPath),
                              SM.getLLVMSourceMgr());
    auto DI = Stream.begin();
    assert(DI != Stream.end() && "Failed to read a document");
    llvm::yaml::Node *N = DI->getRoot();
    assert(N && "Failed to find a root");
    auto *pairs = dyn_cast_or_null<llvm::yaml::MappingNode>(N);
    if (!pairs)
      break;
    for (auto &keyValue: *pairs) {
      auto key = getScalaNodeText(keyValue.getKey());
      // Look for field "current-version" in the .tbd file.
      if (key == "current-version") {
        auto ver = getScalaNodeText(keyValue.getValue());
        currentVersion.tryParse(ver);
        break;
      }
    }
    break;
  }
  // Diagnose unable to checking the current version.
  if (currentVersion.empty()) {
    Impl.diagnose(moduleID.Loc, diag::cannot_find_project_version, "Clang",
                  moduleID.Item.str());
    return true;
  }
  assert(!currentVersion.empty());
  // Give a green light if the version on disk is greater or equal to the version
  // specified in the canImport condition.
  return currentVersion >= version;
}

ModuleDecl *ClangImporter::Implementation::loadModuleClang(
    SourceLoc importLoc, ImportPath::Module path) {
  auto &clangHeaderSearch = getClangPreprocessor().getHeaderSearchInfo();

  // Look up the top-level module first, to see if it exists at all.
  clang::Module *clangModule = clangHeaderSearch.lookupModule(
      path.front().Item.str(), /*AllowSearch=*/true,
      /*AllowExtraModuleMapSearch=*/true);
  if (!clangModule)
    return nullptr;

  // Convert the Swift import path over to a Clang import path.
  SmallVector<std::pair<clang::IdentifierInfo *, clang::SourceLocation>, 4>
      clangPath;
  for (auto component : path) {
    clangPath.emplace_back(
        getClangPreprocessor().getIdentifierInfo(component.Item.str()),
        exportSourceLoc(component.Loc));
  }

  auto &rawDiagClient = Instance->getDiagnosticClient();
  auto &diagClient = static_cast<ClangDiagnosticConsumer &>(rawDiagClient);

  auto loadModule = [&](clang::ModuleIdPath path,
                        clang::Module::NameVisibilityKind visibility)
      -> clang::ModuleLoadResult {
    auto importRAII =
        diagClient.handleImport(clangPath.front().first, importLoc);

    std::string preservedIndexStorePathOption;
    auto &clangFEOpts = Instance->getFrontendOpts();
    if (!clangFEOpts.IndexStorePath.empty()) {
      StringRef moduleName = path[0].first->getName();
      // Ignore the SwiftShims module for the index data.
      if (moduleName == SwiftContext.SwiftShimsModuleName.str()) {
        preservedIndexStorePathOption = clangFEOpts.IndexStorePath;
        clangFEOpts.IndexStorePath.clear();
      }
    }

    clang::SourceLocation clangImportLoc = getNextIncludeLoc();

    clang::ModuleLoadResult result =
        Instance->loadModule(clangImportLoc, path, visibility,
                             /*IsInclusionDirective=*/false);

    if (!preservedIndexStorePathOption.empty()) {
      // Restore the -index-store-path option.
      clangFEOpts.IndexStorePath = preservedIndexStorePathOption;
    }

    if (result && (visibility == clang::Module::AllVisible)) {
      getClangPreprocessor().makeModuleVisible(result, clangImportLoc);
    }
    return result;
  };

  // Now load the top-level module, so that we can check if the submodule
  // exists without triggering a fatal error.
  clangModule = loadModule(clangPath.front(), clang::Module::AllVisible);
  if (!clangModule)
    return nullptr;

  // If we're asked to import the top-level module then we're done here.
  auto *topSwiftModule = finishLoadingClangModule(clangModule, importLoc);
  if (path.size() == 1) {
    return topSwiftModule;
  }

  // Verify that the submodule exists.
  clang::Module *submodule = clangModule;
  for (auto &component : path.getSubmodulePath()) {
    submodule = submodule->findSubmodule(component.Item.str());

    // Special case: a submodule named "Foo.Private" can be moved to a top-level
    // module named "Foo_Private". Clang has special support for this.
    // We're limiting this to just submodules named "Private" because this will
    // put the Clang AST in a fatal error state if it /doesn't/ exist.
    if (!submodule && component.Item.str() == "Private" &&
        (&component) == (&path.getRaw()[1])) {
      submodule = loadModule(llvm::makeArrayRef(clangPath).slice(0, 2),
                             clang::Module::Hidden);
    }

    if (!submodule) {
      // FIXME: Specialize the error for a missing submodule?
      return nullptr;
    }
  }

  // Finally, load the submodule and make it visible.
  clangModule = loadModule(clangPath, clang::Module::AllVisible);
  if (!clangModule)
    return nullptr;

  return finishLoadingClangModule(clangModule, importLoc);
}

ModuleDecl *
ClangImporter::loadModule(SourceLoc importLoc,
                          ImportPath::Module path) {
  return Impl.loadModule(importLoc, path);
}

ModuleDecl *ClangImporter::Implementation::loadModule(
    SourceLoc importLoc, ImportPath::Module path) {
  ModuleDecl *MD = nullptr;
  if (!DisableSourceImport)
    MD = loadModuleClang(importLoc, path);
  if (!MD)
    MD = loadModuleDWARF(importLoc, path);
  return MD;
}

ModuleDecl *ClangImporter::Implementation::finishLoadingClangModule(
    const clang::Module *clangModule, SourceLoc importLoc) {
  assert(clangModule);

  // Bump the generation count.
  bumpGeneration();

  // Force load overlays for all imported modules.
  // FIXME: This forces the creation of wrapper modules for all imports as
  // well, and may do unnecessary work.
  ClangModuleUnit *wrapperUnit = getWrapperForModule(clangModule, importLoc);
  ModuleDecl *result = wrapperUnit->getParentModule();
  if (!ModuleWrappers[clangModule].getInt()) {
    ModuleWrappers[clangModule].setInt(true);
    (void) namelookup::getAllImports(result);
  }

  if (clangModule->isSubModule()) {
    finishLoadingClangModule(clangModule->getTopLevelModule(), importLoc);
  } else {

    if (!SwiftContext.getLoadedModule(result->getName()))
      SwiftContext.addLoadedModule(result);
  }

  return result;
}

// Run through the set of deferred imports -- either those referenced by
// submodule ID from a bridging PCH, or those already loaded as clang::Modules
// in response to an import directive in a bridging header -- and call
// finishLoadingClangModule on each.
void ClangImporter::Implementation::handleDeferredImports(SourceLoc diagLoc) {
  clang::ASTReader &R = *Instance->getASTReader();
  llvm::SmallSet<clang::serialization::SubmoduleID, 32> seenSubmodules;
  for (clang::serialization::SubmoduleID ID : PCHImportedSubmodules) {
    if (!seenSubmodules.insert(ID).second)
      continue;
    ImportedHeaderExports.push_back(R.getSubmodule(ID));
  }
  PCHImportedSubmodules.clear();

  // Avoid a for-in loop because in unusual situations we can end up pulling in
  // another bridging header while we finish loading the modules that are
  // already here. This is a brittle situation but it's outside what's
  // officially supported with bridging headers: app targets and unit tests
  // only. Unfortunately that's not enforced.
  for (size_t i = 0; i < ImportedHeaderExports.size(); ++i) {
    (void)finishLoadingClangModule(ImportedHeaderExports[i], diagLoc);
  }
}

ModuleDecl *ClangImporter::getImportedHeaderModule() const {
  return Impl.ImportedHeaderUnit->getParentModule();
}

ModuleDecl *
ClangImporter::getWrapperForModule(const clang::Module *mod,
                                   bool returnOverlayIfPossible) const {
  auto clangUnit = Impl.getWrapperForModule(mod);
  if (returnOverlayIfPossible && clangUnit->getOverlayModule())
    return clangUnit->getOverlayModule();
  return clangUnit->getParentModule();
}

PlatformAvailability::PlatformAvailability(const LangOptions &langOpts)
    : platformKind(targetPlatform(langOpts)) {
  switch (platformKind) {
  case PlatformKind::iOS:
  case PlatformKind::iOSApplicationExtension:
  case PlatformKind::macCatalyst:
  case PlatformKind::macCatalystApplicationExtension:
  case PlatformKind::tvOS:
  case PlatformKind::tvOSApplicationExtension:
    deprecatedAsUnavailableMessage =
        "APIs deprecated as of iOS 7 and earlier are unavailable in Swift";
    asyncDeprecatedAsUnavailableMessage =
      "APIs deprecated as of iOS 12 and earlier are not imported as 'async'";
    break;

  case PlatformKind::watchOS:
  case PlatformKind::watchOSApplicationExtension:
    deprecatedAsUnavailableMessage = "";
    asyncDeprecatedAsUnavailableMessage =
      "APIs deprecated as of watchOS 5 and earlier are not imported as "
      "'async'";
    break;

  case PlatformKind::macOS:
  case PlatformKind::macOSApplicationExtension:
    deprecatedAsUnavailableMessage =
        "APIs deprecated as of macOS 10.9 and earlier are unavailable in Swift";
    asyncDeprecatedAsUnavailableMessage =
      "APIs deprecated as of macOS 10.14 and earlier are not imported as "
      "'async'";
    break;

  case PlatformKind::OpenBSD:
    deprecatedAsUnavailableMessage = "";
    break;

  case PlatformKind::Windows:
    deprecatedAsUnavailableMessage = "";
    break;

  case PlatformKind::none:
    break;
  }
}

bool PlatformAvailability::isPlatformRelevant(StringRef name) const {
  switch (platformKind) {
  case PlatformKind::macOS:
    return name == "macos";
  case PlatformKind::macOSApplicationExtension:
    return name == "macos" || name == "macos_app_extension";

  case PlatformKind::iOS:
    return name == "ios";
  case PlatformKind::iOSApplicationExtension:
    return name == "ios" || name == "ios_app_extension";

  case PlatformKind::macCatalyst:
  case PlatformKind::macCatalystApplicationExtension:
    // ClangImporter does not yet support macCatalyst.
    return false;

  case PlatformKind::tvOS:
    return name == "tvos";
  case PlatformKind::tvOSApplicationExtension:
    return name == "tvos" || name == "tvos_app_extension";

  case PlatformKind::watchOS:
    return name == "watchos";
  case PlatformKind::watchOSApplicationExtension:
    return name == "watchos" || name == "watchos_app_extension";

  case PlatformKind::OpenBSD:
    return name == "openbsd";

  case PlatformKind::Windows:
    return name == "windows";

  case PlatformKind::none:
    return false;
  }

  llvm_unreachable("Unexpected platform");
}

bool PlatformAvailability::treatDeprecatedAsUnavailable(
    const clang::Decl *clangDecl, const llvm::VersionTuple &version,
    bool isAsync) const {
  assert(!version.empty() && "Must provide version when deprecated");
  unsigned major = version.getMajor();
  Optional<unsigned> minor = version.getMinor();

  switch (platformKind) {
  case PlatformKind::none:
    llvm_unreachable("version but no platform?");

  case PlatformKind::macOS:
  case PlatformKind::macOSApplicationExtension:
    // Anything deprecated by macOS 10.14 is unavailable for async import
    // in Swift.
    if (isAsync && !clangDecl->hasAttr<clang::SwiftAsyncAttr>()) {
      return major < 10 ||
          (major == 10 && (!minor.hasValue() || minor.getValue() <= 14));
    }

    // Anything deprecated in OSX 10.9.x and earlier is unavailable in Swift.
    return major < 10 ||
           (major == 10 && (!minor.hasValue() || minor.getValue() <= 9));

  case PlatformKind::iOS:
  case PlatformKind::iOSApplicationExtension:
  case PlatformKind::tvOS:
  case PlatformKind::tvOSApplicationExtension:
    // Anything deprecated by iOS 12 is unavailable for async import
    // in Swift.
    if (isAsync && !clangDecl->hasAttr<clang::SwiftAsyncAttr>()) {
      return major <= 12;
    }

    // Anything deprecated in iOS 7.x and earlier is unavailable in Swift.
    return major <= 7;

  case PlatformKind::macCatalyst:
  case PlatformKind::macCatalystApplicationExtension:
    // ClangImporter does not yet support macCatalyst.
    return false;

  case PlatformKind::watchOS:
  case PlatformKind::watchOSApplicationExtension:
    // Anything deprecated by watchOS 5.0 is unavailable for async import
    // in Swift.
    if (isAsync && !clangDecl->hasAttr<clang::SwiftAsyncAttr>()) {
      return major <= 5;
    }

    // No deprecation filter on watchOS
    return false;

  case PlatformKind::OpenBSD:
    // No deprecation filter on OpenBSD
    return false;

  case PlatformKind::Windows:
    // No deprecation filter on Windows
    return false;
  }

  llvm_unreachable("Unexpected platform");
}

ClangImporter::Implementation::Implementation(
    ASTContext &ctx,
    DWARFImporterDelegate *dwarfImporterDelegate)
    : SwiftContext(ctx),
      ImportForwardDeclarations(ctx.ClangImporterOpts.ImportForwardDeclarations),
      DisableSwiftBridgeAttr(ctx.ClangImporterOpts.DisableSwiftBridgeAttr),
      BridgingHeaderExplicitlyRequested(!ctx.ClangImporterOpts.BridgingHeader.empty()),
      DisableOverlayModules(ctx.ClangImporterOpts.DisableOverlayModules),
      IsReadingBridgingPCH(false),
      CurrentVersion(ImportNameVersion::fromOptions(ctx.LangOpts)),
      BridgingHeaderLookupTable(new SwiftLookupTable(nullptr)),
      BuffersForDiagnostics(ctx.SourceMgr),
      platformAvailability(ctx.LangOpts), nameImporter(),
      DisableSourceImport(ctx.ClangImporterOpts.DisableSourceImport),
      DWARFImporter(dwarfImporterDelegate) {}

ClangImporter::Implementation::~Implementation() {
#ifndef NDEBUG
  SwiftContext.SourceMgr.verifyAllBuffers();
#endif
}

ClangModuleUnit *ClangImporter::Implementation::getWrapperForModule(
    const clang::Module *underlying, SourceLoc diagLoc) {
  auto &cacheEntry = ModuleWrappers[underlying];
  if (ClangModuleUnit *cached = cacheEntry.getPointer())
    return cached;

  // FIXME: Handle hierarchical names better.
  Identifier name = SwiftContext.getIdentifier(underlying->Name);
  auto wrapper = ModuleDecl::create(name, SwiftContext);
  wrapper->setIsSystemModule(underlying->IsSystem);
  wrapper->setIsNonSwiftModule();
  wrapper->setHasResolvedImports();

  auto file = new (SwiftContext) ClangModuleUnit(*wrapper, *this,
                                                 underlying);
  wrapper->addFile(*file);
  SwiftContext.getClangModuleLoader()->findOverlayFiles(diagLoc, wrapper, file);
  cacheEntry.setPointer(file);

  return file;
}

ClangModuleUnit *ClangImporter::Implementation::getClangModuleForDecl(
    const clang::Decl *D,
    bool allowForwardDeclaration) {
  auto maybeModule = getClangSubmoduleForDecl(D, allowForwardDeclaration);
  if (!maybeModule)
    return nullptr;
  if (!maybeModule.getValue())
    return ImportedHeaderUnit;

  // Get the parent module because currently we don't represent submodules with
  // ClangModuleUnit.
  auto *M = maybeModule.getValue()->getTopLevelModule();

  return getWrapperForModule(M);
}

#pragma mark Source locations
clang::SourceLocation
ClangImporter::Implementation::exportSourceLoc(SourceLoc loc) {
  // FIXME: Implement!
  return clang::SourceLocation();
}

SourceLoc
ClangImporter::Implementation::importSourceLoc(clang::SourceLocation loc) {
  // FIXME: Implement!
  return SourceLoc();
}

SourceRange
ClangImporter::Implementation::importSourceRange(clang::SourceRange loc) {
  // FIXME: Implement!
  return SourceRange();
}

#pragma mark Importing names

clang::DeclarationName
ClangImporter::Implementation::exportName(Identifier name) {
  // FIXME: When we start dealing with C++, we can map over some operator
  // names.
  if (name.empty() || name.isOperator())
    return clang::DeclarationName();

  // Map the identifier. If it's some kind of keyword, it can't be mapped.
  auto ident = &Instance->getASTContext().Idents.get(name.str());
  if (ident->getTokenID() != clang::tok::identifier)
    return clang::DeclarationName();

  return ident;
}

Identifier
ClangImporter::Implementation::importIdentifier(
  const clang::IdentifierInfo *identifier,
  StringRef removePrefix)
{
  if (!identifier) return Identifier();

  StringRef name = identifier->getName();
  // Remove the prefix, if any.
  if (!removePrefix.empty()) {
    if (name.startswith(removePrefix)) {
      name = name.slice(removePrefix.size(), name.size());
    }
  }

  // Get the Swift identifier.
  return SwiftContext.getIdentifier(name);
}

ObjCSelector ClangImporter::Implementation::importSelector(
               clang::Selector selector) {
  auto &ctx = SwiftContext;

  // Handle zero-argument selectors directly.
  if (selector.isUnarySelector()) {
    Identifier name;
    if (auto id = selector.getIdentifierInfoForSlot(0))
      name = ctx.getIdentifier(id->getName());
    return ObjCSelector(ctx, 0, name);
  }

  SmallVector<Identifier, 2> pieces;
  for (auto i = 0u, n = selector.getNumArgs(); i != n; ++i) {
    Identifier piece;
    if (auto id = selector.getIdentifierInfoForSlot(i))
      piece = ctx.getIdentifier(id->getName());
    pieces.push_back(piece);
  }

  return ObjCSelector(ctx, pieces.size(), pieces);
}

clang::Selector
ClangImporter::Implementation::exportSelector(DeclName name,
                                              bool allowSimpleName) {
  if (!allowSimpleName && name.isSimpleName())
    return {};

  clang::ASTContext &ctx = getClangASTContext();

  SmallVector<clang::IdentifierInfo *, 8> pieces;
  pieces.push_back(exportName(name.getBaseIdentifier()).getAsIdentifierInfo());

  auto argNames = name.getArgumentNames();
  if (argNames.empty())
    return ctx.Selectors.getNullarySelector(pieces.front());

  if (!argNames.front().empty())
    return {};
  argNames = argNames.slice(1);

  for (Identifier argName : argNames)
    pieces.push_back(exportName(argName).getAsIdentifierInfo());

  return ctx.Selectors.getSelector(pieces.size(), pieces.data());
}

clang::Selector
ClangImporter::Implementation::exportSelector(ObjCSelector selector) {
  SmallVector<clang::IdentifierInfo *, 4> pieces;
  for (auto piece : selector.getSelectorPieces())
    pieces.push_back(exportName(piece).getAsIdentifierInfo());
  return getClangASTContext().Selectors.getSelector(selector.getNumArgs(),
                                                    pieces.data());
}

/// Determine whether the given method potentially conflicts with the
/// setter for a property in the given protocol.
static bool
isPotentiallyConflictingSetter(const clang::ObjCProtocolDecl *proto,
                               const clang::ObjCMethodDecl *method) {
  auto sel = method->getSelector();
  if (sel.getNumArgs() != 1)
    return false;

  clang::IdentifierInfo *setterID = sel.getIdentifierInfoForSlot(0);
  if (!setterID || !setterID->getName().startswith("set"))
    return false;

  for (auto *prop : proto->properties()) {
    if (prop->getSetterName() == sel)
      return true;
  }

  return false;
}

bool importer::shouldSuppressDeclImport(const clang::Decl *decl) {
  if (auto objcMethod = dyn_cast<clang::ObjCMethodDecl>(decl)) {
    // First check if we're actually in a Swift class.
    auto dc = decl->getDeclContext();
    if (hasNativeSwiftDecl(cast<clang::ObjCContainerDecl>(dc)))
      return true;

    // If this member is a method that is a getter or setter for a
    // property, don't add it into the table. property names and
    // getter names (by choosing to only have a property).
    //
    // Note that this is suppressed for certain accessibility declarations,
    // which are imported as getter/setter pairs and not properties.
    if (objcMethod->isPropertyAccessor()) {
      // Suppress the import of this method when the corresponding
      // property is not suppressed.
      return !shouldSuppressDeclImport(
               objcMethod->findPropertyDecl(/*CheckOverrides=*/false));
    }

    // If the method was declared within a protocol, check that it
    // does not conflict with the setter of a property.
    if (auto proto = dyn_cast<clang::ObjCProtocolDecl>(dc))
      return isPotentiallyConflictingSetter(proto, objcMethod);


    return false;
  }

  if (auto objcProperty = dyn_cast<clang::ObjCPropertyDecl>(decl)) {
    // First check if we're actually in a Swift class.
    auto dc = objcProperty->getDeclContext();
    if (hasNativeSwiftDecl(cast<clang::ObjCContainerDecl>(dc)))
      return true;

    // Suppress certain properties; import them as getter/setter pairs instead.
    if (shouldImportPropertyAsAccessors(objcProperty))
      return true;

    // Check whether there is a superclass method for the getter that
    // is *not* suppressed, in which case we will need to suppress
    // this property.
    auto objcClass = dyn_cast<clang::ObjCInterfaceDecl>(dc);
    if (!objcClass) {
      if (auto objcCategory = dyn_cast<clang::ObjCCategoryDecl>(dc)) {
        // If the enclosing category is invalid, suppress this declaration.
        if (objcCategory->isInvalidDecl()) return true;

        objcClass = objcCategory->getClassInterface();
      }
    }

    if (objcClass) {
      if (auto objcSuperclass = objcClass->getSuperClass()) {
        auto getterMethod =
            objcSuperclass->lookupMethod(objcProperty->getGetterName(),
                                         objcProperty->isInstanceProperty());
        if (getterMethod && !shouldSuppressDeclImport(getterMethod))
          return true;
      }
    }

    return false;
  }

  return false;
}

#pragma mark Name lookup
const clang::TypedefNameDecl *
ClangImporter::Implementation::lookupTypedef(clang::DeclarationName name) {
  clang::Sema &sema = Instance->getSema();
  clang::LookupResult lookupResult(sema, name,
                                   clang::SourceLocation(),
                                   clang::Sema::LookupOrdinaryName);

  if (sema.LookupName(lookupResult, /*scope=*/nullptr)) {
    for (auto decl : lookupResult) {
      if (auto typedefDecl =
          dyn_cast<clang::TypedefNameDecl>(decl->getUnderlyingDecl()))
        return typedefDecl;
    }
  }

  return nullptr;
}

static bool isDeclaredInModule(const ClangModuleUnit *ModuleFilter,
                               const Decl *VD) {
  auto ContainingUnit = VD->getDeclContext()->getModuleScopeContext();
  return ModuleFilter == ContainingUnit;
}

static const clang::Module *
getClangOwningModule(ClangNode Node, const clang::ASTContext &ClangCtx) {
  assert(!Node.getAsModule() && "not implemented for modules");

  if (const clang::Decl *D = Node.getAsDecl()) {
    auto ExtSource = ClangCtx.getExternalSource();
    assert(ExtSource);
    return ExtSource->getModule(D->getOwningModuleID());
  }

  if (const clang::ModuleMacro *M = Node.getAsModuleMacro())
    return M->getOwningModule();

  // A locally-defined MacroInfo does not have an owning module.
  assert(Node.getAsMacroInfo());
  return nullptr;
}

static const clang::Module *
getClangTopLevelOwningModule(ClangNode Node,
                             const clang::ASTContext &ClangCtx) {
  const clang::Module *OwningModule = getClangOwningModule(Node, ClangCtx);
  if (!OwningModule)
    return nullptr;
  return OwningModule->getTopLevelModule();
}

static bool isVisibleFromModule(const ClangModuleUnit *ModuleFilter,
                                ValueDecl *VD) {
  assert(ModuleFilter);

  auto ContainingUnit = VD->getDeclContext()->getModuleScopeContext();
  if (ModuleFilter == ContainingUnit)
    return true;

  // The rest of this function is looking to see if the Clang entity that
  // caused VD to be imported has redeclarations in the filter module.
  auto Wrapper = dyn_cast<ClangModuleUnit>(ContainingUnit);
  if (!Wrapper)
    return false;

  ASTContext &Ctx = ContainingUnit->getASTContext();
  auto *Importer = static_cast<ClangImporter *>(Ctx.getClangModuleLoader());
  auto ClangNode = Importer->getEffectiveClangNode(VD);

  // Macros can be "redeclared" by putting an equivalent definition in two
  // different modules. (We don't actually check the equivalence.)
  // FIXME: We're also not checking if the redeclaration is in /this/ module.
  if (ClangNode.getAsMacro())
    return true;

  const clang::Decl *D = ClangNode.castAsDecl();
  auto &ClangASTContext = ModuleFilter->getClangASTContext();

  // We don't handle Clang submodules; pop everything up to the top-level
  // module.
  auto OwningClangModule = getClangTopLevelOwningModule(ClangNode,
                                                        ClangASTContext);
  if (OwningClangModule == ModuleFilter->getClangModule())
    return true;

  // Handle redeclarable Clang decls by checking each redeclaration.
  bool IsTagDecl = isa<clang::TagDecl>(D);
  if (!(IsTagDecl || isa<clang::FunctionDecl>(D) || isa<clang::VarDecl>(D) ||
        isa<clang::TypedefNameDecl>(D))) {
    return false;
  }

  for (auto Redeclaration : D->redecls()) {
    if (Redeclaration == D)
      continue;

    // For enums, structs, and unions, only count definitions when looking to
    // see what other modules they appear in.
    if (IsTagDecl)
      if (!cast<clang::TagDecl>(Redeclaration)->isCompleteDefinition())
        continue;

    auto OwningClangModule = getClangTopLevelOwningModule(Redeclaration,
                                                          ClangASTContext);
    if (OwningClangModule == ModuleFilter->getClangModule())
      return true;
  }

  return false;
}


namespace {
class ClangVectorDeclConsumer : public clang::VisibleDeclConsumer {
  std::vector<clang::NamedDecl *> results;
public:
  ClangVectorDeclConsumer() = default;

  void FoundDecl(clang::NamedDecl *ND, clang::NamedDecl *Hiding,
                 clang::DeclContext *Ctx, bool InBaseClass) override {
    if (!ND->getIdentifier())
      return;

    if (ND->isModulePrivate())
      return;

    results.push_back(ND);
  }

  llvm::MutableArrayRef<clang::NamedDecl *> getResults() {
    return results;
  }
};

class FilteringVisibleDeclConsumer : public swift::VisibleDeclConsumer {
  swift::VisibleDeclConsumer &NextConsumer;
  const ClangModuleUnit *ModuleFilter;

public:
  FilteringVisibleDeclConsumer(swift::VisibleDeclConsumer &consumer,
                               const ClangModuleUnit *CMU)
      : NextConsumer(consumer), ModuleFilter(CMU) {
    assert(CMU);
  }

  void foundDecl(ValueDecl *VD, DeclVisibilityKind Reason,
                 DynamicLookupInfo dynamicLookupInfo) override {
    if (isVisibleFromModule(ModuleFilter, VD))
      NextConsumer.foundDecl(VD, Reason, dynamicLookupInfo);
  }
};

class FilteringDeclaredDeclConsumer : public swift::VisibleDeclConsumer {
  swift::VisibleDeclConsumer &NextConsumer;
  const ClangModuleUnit *ModuleFilter;

public:
  FilteringDeclaredDeclConsumer(swift::VisibleDeclConsumer &consumer,
                                const ClangModuleUnit *CMU)
      : NextConsumer(consumer), ModuleFilter(CMU) {
    assert(CMU && CMU->isTopLevel() && "Only top-level modules supported");
  }

  void foundDecl(ValueDecl *VD, DeclVisibilityKind Reason,
                 DynamicLookupInfo dynamicLookupInfo) override {
    if (isDeclaredInModule(ModuleFilter, VD))
      NextConsumer.foundDecl(VD, Reason, dynamicLookupInfo);
  }
};

/// A hack to hide particular types in the "Darwin" module on Apple platforms.
class DarwinLegacyFilterDeclConsumer : public swift::VisibleDeclConsumer {
  swift::VisibleDeclConsumer &NextConsumer;
  clang::ASTContext &ClangASTContext;

  bool shouldDiscard(ValueDecl *VD) {
    if (!VD->hasClangNode())
      return false;

    const clang::Module *clangModule = getClangOwningModule(VD->getClangNode(),
                                                            ClangASTContext);
    if (!clangModule)
      return false;

    if (clangModule->Name == "MacTypes") {
      if (!VD->hasName() || VD->getBaseName().isSpecial())
        return true;
      return llvm::StringSwitch<bool>(VD->getBaseIdentifier().str())
          .Cases("OSErr", "OSStatus", "OptionBits", false)
          .Cases("FourCharCode", "OSType", false)
          .Case("Boolean", false)
          .Case("kUnknownType", false)
          .Cases("UTF32Char", "UniChar", "UTF16Char", "UTF8Char", false)
          .Case("ProcessSerialNumber", false)
          .Default(true);
    }

    if (clangModule->Parent &&
        clangModule->Parent->Name == "CarbonCore") {
      return llvm::StringSwitch<bool>(clangModule->Name)
          .Cases("BackupCore", "DiskSpaceRecovery", "MacErrors", false)
          .Case("UnicodeUtilities", false)
          .Default(true);
    }

    if (clangModule->Parent &&
        clangModule->Parent->Name == "OSServices") {
      // Note that this is a list of things to /drop/ rather than to /keep/.
      // We're more likely to see new, modern headers added to OSServices.
      return llvm::StringSwitch<bool>(clangModule->Name)
          .Cases("IconStorage", "KeychainCore", "Power", true)
          .Cases("SecurityCore", "SystemSound", true)
          .Cases("WSMethodInvocation", "WSProtocolHandler", "WSTypes", true)
          .Default(false);
    }

    return false;
  }

public:
  DarwinLegacyFilterDeclConsumer(swift::VisibleDeclConsumer &consumer,
                                 clang::ASTContext &clangASTContext)
      : NextConsumer(consumer), ClangASTContext(clangASTContext) {}

  static bool needsFiltering(const clang::Module *topLevelModule) {
    return topLevelModule && (topLevelModule->Name == "Darwin" ||
                              topLevelModule->Name == "CoreServices");
  }

  void foundDecl(ValueDecl *VD, DeclVisibilityKind Reason,
                 DynamicLookupInfo dynamicLookupInfo) override {
    if (!shouldDiscard(VD))
      NextConsumer.foundDecl(VD, Reason, dynamicLookupInfo);
  }
};

} // unnamed namespace

/// Translate a MacroDefinition to a ClangNode, either a ModuleMacro for
/// a definition imported from a module or a MacroInfo for a macro defined
/// locally.
ClangNode getClangNodeForMacroDefinition(clang::MacroDefinition &M) {
  if (!M.getModuleMacros().empty())
    return ClangNode(M.getModuleMacros().back()->getMacroInfo());
  if (auto *MD = M.getLocalDirective())
    return ClangNode(MD->getMacroInfo());
  return ClangNode();
}

void ClangImporter::lookupBridgingHeaderDecls(
                              llvm::function_ref<bool(ClangNode)> filter,
                              llvm::function_ref<void(Decl*)> receiver) const {
  for (auto &Import : Impl.BridgeHeaderTopLevelImports) {
    auto ImportD = Import.get<ImportDecl*>();
    if (filter(ImportD->getClangDecl()))
      receiver(ImportD);
  }
  for (auto *ClangD : Impl.BridgeHeaderTopLevelDecls) {
    if (filter(ClangD)) {
      if (auto *ND = dyn_cast<clang::NamedDecl>(ClangD)) {
        if (Decl *imported = Impl.importDeclReal(ND, Impl.CurrentVersion))
          receiver(imported);
      }
    }
  }

  auto &ClangPP = Impl.getClangPreprocessor();
  for (clang::IdentifierInfo *II : Impl.BridgeHeaderMacros) {
    auto MD = ClangPP.getMacroDefinition(II);
    if (auto macroNode = getClangNodeForMacroDefinition(MD)) {
      if (filter(macroNode)) {
        auto MI = macroNode.getAsMacro();
        Identifier Name = Impl.getNameImporter().importMacroName(II, MI);
        if (Decl *imported = Impl.importMacro(Name, macroNode))
          receiver(imported);
      }
    }
  }
}

bool ClangImporter::lookupDeclsFromHeader(StringRef Filename,
                              llvm::function_ref<bool(ClangNode)> filter,
                              llvm::function_ref<void(Decl*)> receiver) const {
  auto File = getClangPreprocessor().getFileManager().getFile(Filename);
  if (!File)
    return true;

  auto &ClangCtx = getClangASTContext();
  auto &ClangSM = ClangCtx.getSourceManager();
  auto &ClangPP = getClangPreprocessor();

  // Look up the header in the includes of the bridging header.
  if (Impl.BridgeHeaderFiles.count(*File)) {
    auto headerFilter = [&](ClangNode ClangN) -> bool {
      if (ClangN.isNull())
        return false;

      auto ClangLoc = ClangSM.getFileLoc(ClangN.getLocation());
      if (ClangLoc.isInvalid())
        return false;

      if (ClangSM.getFileEntryForID(ClangSM.getFileID(ClangLoc)) != *File)
        return false;

      return filter(ClangN);
    };

    lookupBridgingHeaderDecls(headerFilter, receiver);
    return false;
  }

  clang::FileID FID = ClangSM.translateFile(*File);
  if (FID.isInvalid())
    return false;

  // Look up the header in the ASTReader.
  if (ClangSM.isLoadedFileID(FID)) {
    // Decls.
    SmallVector<clang::Decl *, 32> Decls;
    unsigned Length = ClangSM.getFileIDSize(FID);
    ClangCtx.getExternalSource()->FindFileRegionDecls(FID, 0, Length, Decls);
    for (auto *ClangD : Decls) {
      if (Impl.shouldIgnoreBridgeHeaderTopLevelDecl(ClangD))
        continue;
      if (filter(ClangD)) {
        if (auto *ND = dyn_cast<clang::NamedDecl>(ClangD)) {
          if (Decl *imported = Impl.importDeclReal(ND, Impl.CurrentVersion))
            receiver(imported);
        }
      }
    }

    // Macros.
    if (auto *ppRec = ClangPP.getPreprocessingRecord()) {
      clang::SourceLocation B = ClangSM.getLocForStartOfFile(FID);
      clang::SourceLocation E = ClangSM.getLocForEndOfFile(FID);
      clang::SourceRange R(B, E);
      const auto &Entities = ppRec->getPreprocessedEntitiesInRange(R);
      for (auto I = Entities.begin(), E = Entities.end(); I != E; ++I) {
        if (!ppRec->isEntityInFileID(I, FID))
          continue;
        clang::PreprocessedEntity *PPE = *I;
        if (!PPE)
          continue;
        if (auto *MDR = dyn_cast<clang::MacroDefinitionRecord>(PPE)) {
          auto *II = const_cast<clang::IdentifierInfo*>(MDR->getName());
          auto MD = ClangPP.getMacroDefinition(II);
          if (auto macroNode = getClangNodeForMacroDefinition(MD)) {
            if (filter(macroNode)) {
              auto MI = macroNode.getAsMacro();
              Identifier Name = Impl.getNameImporter().importMacroName(II, MI);
              if (Decl *imported = Impl.importMacro(Name, macroNode))
                receiver(imported);
            }
          }
        }
      }

      // FIXME: Module imports inside that header.
    }
    return false;
  }

  return true; // no info found about that header.
}

void ClangImporter::lookupValue(DeclName name, VisibleDeclConsumer &consumer) {
  Impl.forEachLookupTable([&](SwiftLookupTable &table) -> bool {
    Impl.lookupValue(table, name, consumer);
    return false;
  });
}

ClangNode ClangImporter::getEffectiveClangNode(const Decl *decl) const {
  // Directly...
  if (auto clangNode = decl->getClangNode())
    return clangNode;

  // Or via the nested "Code" enum.
  if (auto *errorWrapper = dyn_cast<StructDecl>(decl)) {
    if (auto *code = Impl.lookupErrorCodeEnum(errorWrapper))
      if (auto clangNode = code->getClangNode())
        return clangNode;
  }

  return ClangNode();
}

void ClangImporter::lookupTypeDecl(
    StringRef rawName, ClangTypeKind kind,
    llvm::function_ref<void(TypeDecl *)> receiver) {
  clang::DeclarationName clangName(
      &Impl.Instance->getASTContext().Idents.get(rawName));

  SmallVector<clang::Sema::LookupNameKind, 1> lookupKinds;
  switch (kind) {
  case ClangTypeKind::Typedef:
    lookupKinds.push_back(clang::Sema::LookupOrdinaryName);
    break;
  case ClangTypeKind::Tag:
    lookupKinds.push_back(clang::Sema::LookupTagName);
    lookupKinds.push_back(clang::Sema::LookupNamespaceName);
    break;
  case ClangTypeKind::ObjCProtocol:
    lookupKinds.push_back(clang::Sema::LookupObjCProtocolName);
    break;
  }

  // Perform name lookup into the global scope.
  auto &sema = Impl.Instance->getSema();
  bool foundViaClang = false;

  for (auto lookupKind : lookupKinds) {
    clang::LookupResult lookupResult(sema, clangName, clang::SourceLocation(),
                                     lookupKind);
    if (!Impl.DisableSourceImport &&
        sema.LookupName(lookupResult, /*Scope=*/ sema.TUScope)) {
      for (auto clangDecl : lookupResult) {
        if (!isa<clang::TypeDecl>(clangDecl) &&
            !isa<clang::NamespaceDecl>(clangDecl) &&
            !isa<clang::ObjCContainerDecl>(clangDecl) &&
            !isa<clang::ObjCCompatibleAliasDecl>(clangDecl)) {
          continue;
        }
        auto *imported = Impl.importDecl(clangDecl, Impl.CurrentVersion);
        if (auto *importedType = dyn_cast_or_null<TypeDecl>(imported)) {
          foundViaClang = true;
          receiver(importedType);
        }
      }
    }
  }

  // If Clang couldn't find the type, query the DWARFImporterDelegate.
  if (!foundViaClang)
    Impl.lookupTypeDeclDWARF(rawName, kind, receiver);
}

void ClangImporter::lookupRelatedEntity(
    StringRef rawName, ClangTypeKind kind, StringRef relatedEntityKind,
    llvm::function_ref<void(TypeDecl *)> receiver) {
  using CISTAttr = ClangImporterSynthesizedTypeAttr;
  if (relatedEntityKind ==
        CISTAttr::manglingNameForKind(CISTAttr::Kind::NSErrorWrapper) ||
      relatedEntityKind ==
        CISTAttr::manglingNameForKind(CISTAttr::Kind::NSErrorWrapperAnon)) {
    auto underlyingKind = ClangTypeKind::Tag;
    if (relatedEntityKind ==
          CISTAttr::manglingNameForKind(CISTAttr::Kind::NSErrorWrapperAnon)) {
      underlyingKind = ClangTypeKind::Typedef;
    }
    lookupTypeDecl(rawName, underlyingKind,
                   [this, receiver] (const TypeDecl *foundType) {
      auto *enumDecl =
          dyn_cast_or_null<clang::EnumDecl>(foundType->getClangDecl());
      if (!enumDecl)
        return;
      if (!Impl.getEnumInfo(enumDecl).isErrorEnum())
        return;
      auto *enclosingType =
          dyn_cast<NominalTypeDecl>(foundType->getDeclContext());
      if (!enclosingType)
        return;
      receiver(enclosingType);
    });
  }
}

void ClangModuleUnit::lookupVisibleDecls(ImportPath::Access accessPath,
                                         VisibleDeclConsumer &consumer,
                                         NLKind lookupKind) const {
  // FIXME: Ignore submodules, which are empty for now.
  if (clangModule && clangModule->isSubModule())
    return;

  // FIXME: Respect the access path.
  FilteringVisibleDeclConsumer filterConsumer(consumer, this);

  DarwinLegacyFilterDeclConsumer darwinFilterConsumer(filterConsumer,
                                                      getClangASTContext());

  swift::VisibleDeclConsumer *actualConsumer = &filterConsumer;
  if (lookupKind == NLKind::UnqualifiedLookup &&
      DarwinLegacyFilterDeclConsumer::needsFiltering(clangModule)) {
    actualConsumer = &darwinFilterConsumer;
  }

  // Find the corresponding lookup table.
  if (auto lookupTable = owner.findLookupTable(clangModule)) {
    // Search it.
    owner.lookupVisibleDecls(*lookupTable, *actualConsumer);
  }
}

namespace {
class VectorDeclPtrConsumer : public swift::VisibleDeclConsumer {
public:
  SmallVectorImpl<Decl *> &Results;
  explicit VectorDeclPtrConsumer(SmallVectorImpl<Decl *> &Decls)
    : Results(Decls) {}

  void foundDecl(ValueDecl *VD, DeclVisibilityKind Reason,
                 DynamicLookupInfo) override {
    Results.push_back(VD);
  }
};
} // unnamed namespace

void ClangModuleUnit::getTopLevelDecls(SmallVectorImpl<Decl*> &results) const {
  VectorDeclPtrConsumer consumer(results);
  FilteringDeclaredDeclConsumer filterConsumer(consumer, this);
  DarwinLegacyFilterDeclConsumer darwinFilterConsumer(filterConsumer,
                                                      getClangASTContext());

  const clang::Module *topLevelModule =
    clangModule ? clangModule->getTopLevelModule() : nullptr;

  swift::VisibleDeclConsumer *actualConsumer = &filterConsumer;
  if (DarwinLegacyFilterDeclConsumer::needsFiltering(topLevelModule))
    actualConsumer = &darwinFilterConsumer;

  // Find the corresponding lookup table.
  if (auto lookupTable = owner.findLookupTable(topLevelModule)) {
    // Search it.
    owner.lookupVisibleDecls(*lookupTable, *actualConsumer);

    // Add the extensions produced by importing categories.
    for (auto category : lookupTable->categories()) {
      if (auto extension = cast_or_null<ExtensionDecl>(
              owner.importDecl(category, owner.CurrentVersion,
                               /*UseCanonical*/false))) {
        results.push_back(extension);
      }
    }

    auto findEnclosingExtension = [](Decl *importedDecl) -> ExtensionDecl * {
      for (auto importedDC = importedDecl->getDeclContext();
           !importedDC->isModuleContext();
           importedDC = importedDC->getParent()) {
        if (auto ext = dyn_cast<ExtensionDecl>(importedDC))
          return ext;
      }
      return nullptr;
    };
    // Retrieve all of the globals that will be mapped to members.

    // FIXME: Since we don't represent Clang submodules as Swift
    // modules, we're getting everything.
    llvm::SmallPtrSet<ExtensionDecl *, 8> knownExtensions;
    for (auto entry : lookupTable->allGlobalsAsMembers()) {
      auto decl = entry.get<clang::NamedDecl *>();
      auto importedDecl = owner.importDecl(decl, owner.CurrentVersion);
      if (!importedDecl) continue;

      // Find the enclosing extension, if there is one.
      ExtensionDecl *ext = findEnclosingExtension(importedDecl);
      if (ext && knownExtensions.insert(ext).second)
        results.push_back(ext);

      // If this is a compatibility typealias, the canonical type declaration
      // may exist in another extension.
      auto alias = dyn_cast<TypeAliasDecl>(importedDecl);
      if (!alias || !alias->isCompatibilityAlias()) continue;

      auto aliasedTy = alias->getUnderlyingType();
      ext = nullptr;
      importedDecl = nullptr;

      // Note: We can't use getAnyGeneric() here because `aliasedTy`
      // might be typealias.
      if (auto Ty = dyn_cast<TypeAliasType>(aliasedTy.getPointer()))
        importedDecl = Ty->getDecl();
      else if (auto Ty = dyn_cast<AnyGenericType>(aliasedTy.getPointer()))
        importedDecl = Ty->getDecl();
      if (!importedDecl) continue;

      ext = findEnclosingExtension(importedDecl);
      if (ext && knownExtensions.insert(ext).second)
        results.push_back(ext);
    }
  }
}

ImportDecl *swift::createImportDecl(ASTContext &Ctx,
                                    DeclContext *DC,
                                    ClangNode ClangN,
                                    ArrayRef<clang::Module *> Exported) {
  auto *ImportedMod = ClangN.getClangModule();
  assert(ImportedMod);

  ImportPath::Builder importPath;
  auto *TmpMod = ImportedMod;
  while (TmpMod) {
    importPath.push_back(Ctx.getIdentifier(TmpMod->Name));
    TmpMod = TmpMod->Parent;
  }
  std::reverse(importPath.begin(), importPath.end());

  bool IsExported = false;
  for (auto *ExportedMod : Exported) {
    if (ImportedMod == ExportedMod) {
      IsExported = true;
      break;
    }
  }

  auto *ID = ImportDecl::create(Ctx, DC, SourceLoc(),
                                ImportKind::Module, SourceLoc(),
                                importPath.get(), ClangN);
  if (IsExported)
    ID->getAttrs().add(new (Ctx) ExportedAttr(/*IsImplicit=*/false));
  return ID;
}

static void getImportDecls(ClangModuleUnit *ClangUnit, const clang::Module *M,
                           SmallVectorImpl<Decl *> &Results) {
  assert(M);
  SmallVector<clang::Module *, 1> Exported;
  M->getExportedModules(Exported);

  ASTContext &Ctx = ClangUnit->getASTContext();

  for (auto *ImportedMod : M->Imports) {
    auto *ID = createImportDecl(Ctx, ClangUnit, ImportedMod, Exported);
    Results.push_back(ID);
  }
}

void ClangModuleUnit::getDisplayDecls(SmallVectorImpl<Decl*> &results) const {
  if (clangModule)
    getImportDecls(const_cast<ClangModuleUnit *>(this), clangModule, results);
  getTopLevelDecls(results);
}

void ClangModuleUnit::lookupValue(DeclName name, NLKind lookupKind,
                                  SmallVectorImpl<ValueDecl*> &results) const {
  // FIXME: Ignore submodules, which are empty for now.
  if (clangModule && clangModule->isSubModule())
    return;

  VectorDeclConsumer vectorWriter(results);
  FilteringVisibleDeclConsumer filteringConsumer(vectorWriter, this);

  DarwinLegacyFilterDeclConsumer darwinFilterConsumer(filteringConsumer,
                                                      getClangASTContext());

  swift::VisibleDeclConsumer *consumer = &filteringConsumer;
  if (lookupKind == NLKind::UnqualifiedLookup &&
      DarwinLegacyFilterDeclConsumer::needsFiltering(clangModule)) {
    consumer = &darwinFilterConsumer;
  }

  // Find the corresponding lookup table.
  if (auto lookupTable = owner.findLookupTable(clangModule)) {
    // Search it.
    owner.lookupValue(*lookupTable, name, *consumer);
  }
}

bool ClangImporter::Implementation::isVisibleClangEntry(
    const clang::NamedDecl *clangDecl) {
  // For a declaration, check whether the declaration is hidden.
  clang::Sema &clangSema = getClangSema();
  if (clangSema.isVisible(clangDecl)) return true;

  // Is any redeclaration visible?
  for (auto redecl : clangDecl->redecls()) {
    if (clangSema.isVisible(cast<clang::NamedDecl>(redecl))) return true;
  }

  return false;
}

bool ClangImporter::Implementation::isVisibleClangEntry(
  SwiftLookupTable::SingleEntry entry) {
  if (auto clangDecl = entry.dyn_cast<clang::NamedDecl *>()) {
    return isVisibleClangEntry(clangDecl);
  }

  // If it's a macro from a module, check whether the module has been imported.
  if (auto moduleMacro = entry.dyn_cast<clang::ModuleMacro *>()) {
    clang::Module *module = moduleMacro->getOwningModule();
    return module->NameVisibility == clang::Module::AllVisible;
  }

  return true;
}

TypeDecl *
ClangModuleUnit::lookupNestedType(Identifier name,
                                  const NominalTypeDecl *baseType) const {
  // Special case for error code enums: try looking directly into the struct
  // first. But only if it looks like a synthesized error wrapped struct.
  if (name == getASTContext().Id_Code &&
      !baseType->hasClangNode() &&
      isa<StructDecl>(baseType)) {
    auto *wrapperStruct = cast<StructDecl>(baseType);
    if (auto *codeEnum = owner.lookupErrorCodeEnum(wrapperStruct))
      return codeEnum;

    // Otherwise, fall back and try via lookup table.
  }

  auto lookupTable = owner.findLookupTable(clangModule);
  if (!lookupTable)
    return nullptr;

  auto baseTypeContext = owner.getEffectiveClangContext(baseType);
  if (!baseTypeContext)
    return nullptr;

  // FIXME: This is very similar to what's in Implementation::lookupValue and
  // Implementation::loadAllMembers.
  SmallVector<TypeDecl *, 2> results;
  for (auto entry : lookupTable->lookup(SerializedSwiftName(name.str()),
                                        baseTypeContext)) {
    // If the entry is not visible, skip it.
    if (!owner.isVisibleClangEntry(entry)) continue;

    auto *clangDecl = entry.dyn_cast<clang::NamedDecl *>();
    if (!clangDecl)
      continue;

    const auto *clangTypeDecl = clangDecl->getMostRecentDecl();

    bool anyMatching = false;
    TypeDecl *originalDecl = nullptr;
    owner.forEachDistinctName(clangTypeDecl,
                              [&](ImportedName newName,
                                  ImportNameVersion nameVersion) -> bool {
      if (anyMatching)
        return true;
      if (!newName.getDeclName().isSimpleName(name))
        return true;

      auto decl = dyn_cast_or_null<TypeDecl>(
          owner.importDeclReal(clangTypeDecl, nameVersion));
      if (!decl)
        return false;

      if (!originalDecl)
        originalDecl = decl;
      else if (originalDecl == decl)
        return true;

      auto *importedContext = decl->getDeclContext()->getSelfNominalTypeDecl();
      if (importedContext != baseType)
        return true;

      assert(decl->getName() == name &&
             "importFullName behaved differently from importDecl");
      results.push_back(decl);
      anyMatching = true;
      return true;
    });
  }

  if (results.size() != 1) {
    // It's possible that two types were import-as-member'd onto the same base
    // type with the same name. In this case, fall back to regular lookup.
    return nullptr;
  }

  return results.front();
}

void ClangImporter::loadExtensions(NominalTypeDecl *nominal,
                                   unsigned previousGeneration) {
  // Determine the effective Clang context for this Swift nominal type.
  auto effectiveClangContext = Impl.getEffectiveClangContext(nominal);
  if (!effectiveClangContext) return;

  // For an Objective-C class, import all of the visible categories.
  if (auto objcClass = dyn_cast_or_null<clang::ObjCInterfaceDecl>(
                         effectiveClangContext.getAsDeclContext())) {
    SmallVector<clang::NamedDecl *, 4> DelayedCategories;

    // Simply importing the categories adds them to the list of extensions.
    for (const auto *Cat : objcClass->known_categories()) {
      if (getClangSema().isVisible(Cat)) {
        Impl.importDeclReal(Cat, Impl.CurrentVersion);
      }
    }
  }

  // Dig through each of the Swift lookup tables, creating extensions
  // where needed.
  (void)Impl.forEachLookupTable([&](SwiftLookupTable &table) -> bool {
      // FIXME: If we already looked at this for this generation,
      // skip.

      for (auto entry : table.allGlobalsAsMembersInContext(effectiveClangContext)) {
        // If the entry is not visible, skip it.
        if (!Impl.isVisibleClangEntry(entry)) continue;

        if (auto decl = entry.dyn_cast<clang::NamedDecl *>()) {
          // Import the context of this declaration, which has the
          // side effect of creating instantiations.
          (void)Impl.importDeclContextOf(decl, effectiveClangContext);
        } else {
          llvm_unreachable("Macros cannot be imported as members.");
        }
      }

      return false;
    });
}

void ClangImporter::loadObjCMethods(
       ClassDecl *classDecl,
       ObjCSelector selector,
       bool isInstanceMethod,
       unsigned previousGeneration,
       llvm::TinyPtrVector<AbstractFunctionDecl *> &methods) {
  const auto *objcClass =
      dyn_cast_or_null<clang::ObjCInterfaceDecl>(classDecl->getClangDecl());
  if (!objcClass)
    return;

  // Collect the set of visible Objective-C methods with this selector.
  clang::Selector clangSelector = Impl.exportSelector(selector);

  AbstractFunctionDecl *method = nullptr;
  auto *objcMethod = objcClass->lookupMethod(
      clangSelector, isInstanceMethod,
      /*shallowCategoryLookup=*/false,
      /*followSuper=*/false);

  if (objcMethod) {
    // If we found a property accessor, import the property.
    if (objcMethod->isPropertyAccessor())
      (void)Impl.importDecl(objcMethod->findPropertyDecl(true),
                            Impl.CurrentVersion);

    method = dyn_cast_or_null<AbstractFunctionDecl>(
        Impl.importDecl(objcMethod, Impl.CurrentVersion));
  }

  // If we didn't find anything, we're done.
  if (method == nullptr)
    return;

  // If we did find something, it might be a duplicate of something we found
  // earlier, because we aren't tracking generation counts for Clang modules.
  // Filter out the duplicates.
  // FIXME: We shouldn't need to do this.
  if (!llvm::is_contained(methods, method))
    methods.push_back(method);
}

void
ClangModuleUnit::lookupClassMember(ImportPath::Access accessPath,
                                   DeclName name,
                                   SmallVectorImpl<ValueDecl*> &results) const {
  // FIXME: Ignore submodules, which are empty for now.
  if (clangModule && clangModule->isSubModule())
    return;

  VectorDeclConsumer consumer(results);

  // Find the corresponding lookup table.
  if (auto lookupTable = owner.findLookupTable(clangModule)) {
    // Search it.
    owner.lookupObjCMembers(*lookupTable, name, consumer);
  }
}

void ClangModuleUnit::lookupClassMembers(ImportPath::Access accessPath,
                                         VisibleDeclConsumer &consumer) const {
  // FIXME: Ignore submodules, which are empty for now.
  if (clangModule && clangModule->isSubModule())
    return;

  // Find the corresponding lookup table.
  if (auto lookupTable = owner.findLookupTable(clangModule)) {
    // Search it.
    owner.lookupAllObjCMembers(*lookupTable, consumer);
  }
}

void ClangModuleUnit::lookupObjCMethods(
       ObjCSelector selector,
       SmallVectorImpl<AbstractFunctionDecl *> &results) const {
  // FIXME: Ignore submodules, which are empty for now.
  if (clangModule && clangModule->isSubModule())
    return;

  // Map the selector into a Clang selector.
  auto clangSelector = owner.exportSelector(selector);
  if (clangSelector.isNull()) return;

  // Collect all of the Objective-C methods with this selector.
  SmallVector<clang::ObjCMethodDecl *, 8> objcMethods;
  auto &clangSema = owner.getClangSema();
  clangSema.CollectMultipleMethodsInGlobalPool(clangSelector,
                                               objcMethods,
                                               /*InstanceFirst=*/true,
                                               /*CheckTheOther=*/false);
  clangSema.CollectMultipleMethodsInGlobalPool(clangSelector,
                                               objcMethods,
                                               /*InstanceFirst=*/false,
                                               /*CheckTheOther=*/false);

  // Import the methods.
  auto &clangCtx = clangSema.getASTContext();
  for (auto objcMethod : objcMethods) {
    // Verify that this method came from this module.
    auto owningClangModule = getClangTopLevelOwningModule(objcMethod, clangCtx);
    if (owningClangModule != clangModule) continue;

    if (shouldSuppressDeclImport(objcMethod))
      continue;

    // If we found a property accessor, import the property.
    if (objcMethod->isPropertyAccessor())
      (void)owner.importDecl(objcMethod->findPropertyDecl(true),
                             owner.CurrentVersion);

    auto imported =
        owner.importDecl(objcMethod, owner.CurrentVersion);
    if (!imported) continue;

    if (auto func = dyn_cast<AbstractFunctionDecl>(imported))
      results.push_back(func);

    // If there is an alternate declaration, also look at it.
    for (auto alternate : owner.getAlternateDecls(imported)) {
      if (auto func = dyn_cast<AbstractFunctionDecl>(alternate))
        results.push_back(func);
    }
  }
}

void ClangModuleUnit::collectLinkLibraries(
    ModuleDecl::LinkLibraryCallback callback) const {
  if (!clangModule)
    return;

  // Skip this lib name in favor of export_as name.
  if (clangModule->UseExportAsModuleLinkName)
    return;

  for (auto clangLinkLib : clangModule->LinkLibraries) {
    LibraryKind kind;
    if (clangLinkLib.IsFramework)
      kind = LibraryKind::Framework;
    else
      kind = LibraryKind::Library;

    callback(LinkLibrary(clangLinkLib.Library, kind));
  }
}

StringRef ClangModuleUnit::getFilename() const {
  if (!clangModule) {
    StringRef SinglePCH = owner.getSinglePCHImport();
    if (SinglePCH.empty())
      return "<imports>";
    else
      return SinglePCH;
  }
  if (const clang::FileEntry *F = clangModule->getASTFile())
    if (!F->getName().empty())
      return F->getName();
  return StringRef();
}

clang::TargetInfo &ClangImporter::getTargetInfo() const {
  return Impl.Instance->getTarget();
}

clang::ASTContext &ClangImporter::getClangASTContext() const {
  return Impl.getClangASTContext();
}

clang::Preprocessor &ClangImporter::getClangPreprocessor() const {
  return Impl.getClangPreprocessor();
}

const clang::CompilerInstance &ClangImporter::getClangInstance() const {
  return *Impl.Instance;
}

const clang::Module *ClangImporter::getClangOwningModule(ClangNode Node) const {
  return Impl.getClangOwningModule(Node);
}

const clang::Module *
ClangImporter::Implementation::getClangOwningModule(ClangNode Node) const {
  return ::getClangOwningModule(Node, getClangASTContext());
}

bool ClangImporter::hasTypedef(const clang::Decl *typeDecl) const {
  return Impl.DeclsWithSuperfluousTypedefs.count(typeDecl);
}

clang::Sema &ClangImporter::getClangSema() const {
  return Impl.getClangSema();
}

clang::CodeGenOptions &ClangImporter::getClangCodeGenOpts() const {
  return Impl.getClangCodeGenOpts();
}

std::string ClangImporter::getClangModuleHash() const {
  return Impl.Invocation->getModuleHash(Impl.Instance->getDiagnostics());
}

Decl *ClangImporter::importDeclCached(const clang::NamedDecl *ClangDecl) {
  return Impl.importDeclCached(ClangDecl, Impl.CurrentVersion);
}

void ClangImporter::printStatistics() const {
  Impl.Instance->getASTReader()->PrintStats();
}

void ClangImporter::verifyAllModules() {
#ifndef NDEBUG
  if (Impl.VerifiedDeclsCounter == Impl.ImportedDecls.size())
    return;

  // Collect the Decls before verifying them; the act of verifying may cause
  // more decls to be imported and modify the map while we are iterating it.
  size_t verifiedCounter = Impl.ImportedDecls.size();
  SmallVector<Decl *, 8> Decls;
  for (auto &I : Impl.ImportedDecls)
    if (I.first.second == Impl.CurrentVersion)
      if (Decl *D = I.second)
        Decls.push_back(D);

  for (auto D : Decls)
    verify(D);

  Impl.VerifiedDeclsCounter = verifiedCounter;
#endif
}

const clang::Type *
ClangImporter::parseClangFunctionType(StringRef typeStr,
                                      SourceLoc loc) const {
  auto &sema = Impl.getClangSema();
  StringRef filename = Impl.SwiftContext.SourceMgr.getDisplayNameForLoc(loc);
  // TODO: Obtain a clang::SourceLocation from the swift::SourceLoc we have
  auto parsedType = sema.ParseTypeFromStringCallback(typeStr, filename, {});
  if (!parsedType.isUsable())
    return nullptr;
  clang::QualType resultType = clang::Sema::GetTypeFromParser(parsedType.get());
  auto *typePtr = resultType.getTypePtrOrNull();
  if (typePtr && (typePtr->isFunctionPointerType()
                  || typePtr->isBlockPointerType()))
      return typePtr;
  return nullptr;
}

void ClangImporter::printClangType(const clang::Type *type,
                                   llvm::raw_ostream &os) const {
  auto policy = clang::PrintingPolicy(getClangASTContext().getLangOpts());
  clang::QualType(type, 0).print(os, policy);
}

//===----------------------------------------------------------------------===//
// ClangModule Implementation
//===----------------------------------------------------------------------===//

static_assert(IsTriviallyDestructible<ClangModuleUnit>::value,
              "ClangModuleUnits are BumpPtrAllocated; the d'tor is not called");

ClangModuleUnit::ClangModuleUnit(ModuleDecl &M,
                                 ClangImporter::Implementation &owner,
                                 const clang::Module *clangModule)
  : LoadedFile(FileUnitKind::ClangModule, M), owner(owner),
    clangModule(clangModule) {
  // Capture the file metadata before it goes away.
  if (clangModule)
    ASTSourceDescriptor = {*const_cast<clang::Module *>(clangModule)};
}

StringRef ClangModuleUnit::getModuleDefiningPath() const {
  if (!clangModule || clangModule->DefinitionLoc.isInvalid())
    return "";

  auto &clangSourceMgr = owner.getClangASTContext().getSourceManager();
  return clangSourceMgr.getFilename(clangModule->DefinitionLoc);
}

Optional<clang::ASTSourceDescriptor>
ClangModuleUnit::getASTSourceDescriptor() const {
  if (clangModule) {
    assert(ASTSourceDescriptor.getModuleOrNull() == clangModule);
    return ASTSourceDescriptor;
  }
  return None;
}

bool ClangModuleUnit::hasClangModule(ModuleDecl *M) {
  for (auto F : M->getFiles()) {
    if (isa<ClangModuleUnit>(F))
      return true;
  }
  return false;
}

bool ClangModuleUnit::isTopLevel() const {
  return !clangModule || !clangModule->isSubModule();
}

bool ClangModuleUnit::isSystemModule() const {
  return clangModule && clangModule->IsSystem;
}

clang::ASTContext &ClangModuleUnit::getClangASTContext() const {
  return owner.getClangASTContext();
}

StringRef ClangModuleUnit::getExportedModuleName() const {
  if (clangModule && !clangModule->ExportAsModule.empty())
    return clangModule->ExportAsModule;

  return getParentModule()->getName().str();
}

ModuleDecl *ClangModuleUnit::getOverlayModule() const {
  if (!clangModule)
    return nullptr;

  if (owner.DisableOverlayModules)
    return nullptr;

  if (!isTopLevel()) {
    // FIXME: Is this correct for submodules?
    auto topLevel = clangModule->getTopLevelModule();
    auto wrapper = owner.getWrapperForModule(topLevel);
    return wrapper->getOverlayModule();
  }

  if (!overlayModule.getInt()) {
    // FIXME: Include proper source location.
    ModuleDecl *M = getParentModule();
    ASTContext &Ctx = M->getASTContext();
    auto overlay = Ctx.getOverlayModule(this);
    if (overlay) {
      Ctx.addLoadedModule(overlay);
    } else {
      // FIXME: This is the awful legacy of the old implementation of overlay
      // loading laid bare. Because the previous implementation used
      // ASTContext::getModuleByIdentifier, it consulted the clang importer
      // recursively which forced the current module, its dependencies, and
      // the overlays of those dependencies to load and
      // become visible in the current context. All of the callers of
      // ClangModuleUnit::getOverlayModule are relying on this behavior, and
      // untangling them is going to take a heroic amount of effort.
      // Clang module loading should *never* *ever* be allowed to load unrelated
      // Swift modules.
      ImportPath::Module::Builder builder(M->getName());
      (void) owner.loadModule(SourceLoc(), std::move(builder).get());
    }
    auto mutableThis = const_cast<ClangModuleUnit *>(this);
    mutableThis->overlayModule.setPointerAndInt(overlay, true);
  }

  return overlayModule.getPointer();
}

void ClangModuleUnit::getImportedModules(
    SmallVectorImpl<ImportedModule> &imports,
    ModuleDecl::ImportFilter filter) const {
  // Bail out if we /only/ want ImplementationOnly imports; Clang modules never
  // have any of these.
  if (filter.containsOnly(ModuleDecl::ImportFilterKind::ImplementationOnly))
    return;

  // [NOTE: Pure-Clang-modules-privately-import-stdlib]:
  // Needed for implicitly synthesized conformances.
  if (filter.contains(ModuleDecl::ImportFilterKind::Default))
    if (auto stdlib = owner.getStdlibModule())
      imports.push_back({ImportPath::Access(), stdlib});

  SmallVector<clang::Module *, 8> imported;
  if (!clangModule) {
    // This is the special "imported headers" module.
    if (filter.contains(ModuleDecl::ImportFilterKind::Exported)) {
      imported.append(owner.ImportedHeaderExports.begin(),
                      owner.ImportedHeaderExports.end());
    }

  } else {
    clangModule->getExportedModules(imported);

    if (filter.contains(ModuleDecl::ImportFilterKind::Default)) {
      // Copy in any modules that are imported but not exported.
      llvm::SmallPtrSet<clang::Module *, 8> knownModules(imported.begin(),
                                                         imported.end());
      if (!filter.contains(ModuleDecl::ImportFilterKind::Exported)) {
        // Remove the exported ones now that we're done with them.
        imported.clear();
      }
      llvm::copy_if(clangModule->Imports, std::back_inserter(imported),
                    [&](clang::Module *mod) {
                     return !knownModules.insert(mod).second;
                    });

      // FIXME: The parent module isn't exactly a private import, but it is
      // needed for link dependencies.
      if (clangModule->Parent)
        imported.push_back(clangModule->Parent);
    }
  }

  auto topLevelOverlay = getOverlayModule();
  for (auto importMod : imported) {
    auto wrapper = owner.getWrapperForModule(importMod);

    auto actualMod = wrapper->getOverlayModule();
    if (!actualMod) {
      // HACK: Deal with imports of submodules by importing the top-level module
      // as well.
      auto importTopLevel = importMod->getTopLevelModule();
      if (importTopLevel != importMod) {
        if (!clangModule || importTopLevel != clangModule->getTopLevelModule()){
          auto topLevelWrapper = owner.getWrapperForModule(importTopLevel);
          imports.push_back({ ImportPath::Access(),
                              topLevelWrapper->getParentModule() });
        }
      }
      actualMod = wrapper->getParentModule();
    } else if (actualMod == topLevelOverlay) {
      actualMod = wrapper->getParentModule();
    }

    assert(actualMod && "Missing imported overlay");
    imports.push_back({ImportPath::Access(), actualMod});
  }
}

void ClangModuleUnit::getImportedModulesForLookup(
    SmallVectorImpl<ImportedModule> &imports) const {

  // Reuse our cached list of imports if we have one.
  if (importedModulesForLookup.hasValue()) {
    imports.append(importedModulesForLookup->begin(),
                   importedModulesForLookup->end());
    return;
  }

  size_t firstImport = imports.size();

  SmallVector<clang::Module *, 8> imported;
  const clang::Module *topLevel;
  ModuleDecl *topLevelOverlay = getOverlayModule();
  if (!clangModule) {
    // This is the special "imported headers" module.
    imported.append(owner.ImportedHeaderExports.begin(),
                    owner.ImportedHeaderExports.end());
    topLevel = nullptr;
  } else {
    clangModule->getExportedModules(imported);
    topLevel = clangModule->getTopLevelModule();
  }

  if (imported.empty()) {
    importedModulesForLookup = ArrayRef<ImportedModule>();
    return;
  }

  SmallPtrSet<clang::Module *, 32> seen{imported.begin(), imported.end()};
  SmallVector<clang::Module *, 8> tmpBuf;
  llvm::SmallSetVector<clang::Module *, 8> topLevelImported;

  // Get the transitive set of top-level imports. That is, if a particular
  // import is a top-level import, add it. Otherwise, keep searching.
  while (!imported.empty()) {
    clang::Module *next = imported.pop_back_val();

    // HACK: Deal with imports of submodules by importing the top-level module
    // as well, unless it's the top-level module we're currently in.
    clang::Module *nextTopLevel = next->getTopLevelModule();
    if (nextTopLevel != topLevel) {
      topLevelImported.insert(nextTopLevel);

      // Don't continue looking through submodules of modules that have
      // overlays. The overlay might shadow things.
      auto wrapper = owner.getWrapperForModule(nextTopLevel);
      if (wrapper->getOverlayModule())
        continue;
    }

    // Only look through the current module if it's not top-level.
    if (nextTopLevel == next)
      continue;

    next->getExportedModules(tmpBuf);
    for (clang::Module *nextImported : tmpBuf) {
      if (seen.insert(nextImported).second)
        imported.push_back(nextImported);
    }
    tmpBuf.clear();
  }

  for (auto importMod : topLevelImported) {
    auto wrapper = owner.getWrapperForModule(importMod);

    auto actualMod = wrapper->getOverlayModule();
    if (!actualMod || actualMod == topLevelOverlay)
      actualMod = wrapper->getParentModule();

    assert(actualMod && "Missing imported overlay");
    imports.push_back({ImportPath::Access(), actualMod});
  }

  // Cache our results for use next time.
  auto importsToCache = llvm::makeArrayRef(imports).slice(firstImport);
  importedModulesForLookup = getASTContext().AllocateCopy(importsToCache);
}

void ClangImporter::getMangledName(raw_ostream &os,
                                   const clang::NamedDecl *clangDecl) const {
  if (!Impl.Mangler)
    Impl.Mangler.reset(Impl.getClangASTContext().createMangleContext());

  if (auto ctor = dyn_cast<clang::CXXConstructorDecl>(clangDecl)) {
    auto ctorGlobalDecl =
        clang::GlobalDecl(ctor, clang::CXXCtorType::Ctor_Complete);
    Impl.Mangler->mangleCXXName(ctorGlobalDecl, os);
  } else {
    Impl.Mangler->mangleName(clangDecl, os);
  }
}

// ---------------------------------------------------------------------------
// Swift lookup tables
// ---------------------------------------------------------------------------

SwiftLookupTable *ClangImporter::Implementation::findLookupTable(
                    const clang::Module *clangModule) {
  // If the Clang module is null, use the bridging header lookup table.
  if (!clangModule)
    return BridgingHeaderLookupTable.get();

  // Submodules share lookup tables with their parents.
  if (clangModule->isSubModule())
    return findLookupTable(clangModule->getTopLevelModule());

  // Look for a Clang module with this name.
  auto known = LookupTables.find(clangModule->Name);
  if (known == LookupTables.end()) return nullptr;

  return known->second.get();
}

bool ClangImporter::Implementation::forEachLookupTable(
       llvm::function_ref<bool(SwiftLookupTable &table)> fn) {
  // Visit the bridging header's lookup table.
  if (fn(*BridgingHeaderLookupTable)) return true;

  // Collect and sort the set of module names.
  SmallVector<StringRef, 4> moduleNames;
  for (const auto &entry : LookupTables) {
    moduleNames.push_back(entry.first);
  }
  llvm::array_pod_sort(moduleNames.begin(), moduleNames.end());

  // Visit the lookup tables.
  for (auto moduleName : moduleNames) {
    if (fn(*LookupTables[moduleName])) return true;
  }

  return false;
}

void ClangImporter::Implementation::lookupValue(
       SwiftLookupTable &table, DeclName name,
       VisibleDeclConsumer &consumer) {
  auto &clangCtx = getClangASTContext();
  auto clangTU = clangCtx.getTranslationUnitDecl();

  // For operators we have to look up static member functions in addition to the
  // top-level function lookup below.
  if (name.isOperator()) {
    for (auto entry : table.lookupMemberOperators(name.getBaseName())) {
      if (isVisibleClangEntry(entry)) {
        if (auto decl = dyn_cast_or_null<ValueDecl>(
                importDeclReal(entry->getMostRecentDecl(), CurrentVersion)))
          consumer.foundDecl(decl, DeclVisibilityKind::VisibleAtTopLevel);
      }
    }
  }

  for (auto entry : table.lookup(name.getBaseName(), clangTU)) {
    // If the entry is not visible, skip it.
    if (!isVisibleClangEntry(entry)) continue;

    ValueDecl *decl;
    // If it's a Clang declaration, try to import it.
    if (auto clangDecl = entry.dyn_cast<clang::NamedDecl *>()) {
      decl = cast_or_null<ValueDecl>(
          importDeclReal(clangDecl->getMostRecentDecl(), CurrentVersion));
      if (!decl) continue;
    } else if (!name.isSpecial()) {
      // Try to import a macro.
      if (auto modMacro = entry.dyn_cast<clang::ModuleMacro *>())
        decl = importMacro(name.getBaseIdentifier(), modMacro);
      else if (auto clangMacro = entry.dyn_cast<clang::MacroInfo *>())
        decl = importMacro(name.getBaseIdentifier(), clangMacro);
      else
        llvm_unreachable("new kind of lookup table entry");
      if (!decl) continue;
    } else {
      continue;
    }

    // If we found a declaration from the standard library, make sure
    // it does not show up in the lookup results for the imported
    // module.
    if (decl->getDeclContext()->isModuleScopeContext() &&
        decl->getModuleContext() == getStdlibModule())
      continue;

    // If the name matched, report this result.
    bool anyMatching = false;
    if (decl->getName().matchesRef(name) &&
        decl->getDeclContext()->isModuleScopeContext()) {
      consumer.foundDecl(decl, DeclVisibilityKind::VisibleAtTopLevel);
      anyMatching = true;
    }

    // If there is an alternate declaration and the name matches,
    // report this result.
    for (auto alternate : getAlternateDecls(decl)) {
      if (alternate->getName().matchesRef(name) &&
          alternate->getDeclContext()->isModuleScopeContext()) {
        consumer.foundDecl(alternate, DeclVisibilityKind::VisibleAtTopLevel);
        anyMatching = true;
      }
    }

    // If we have a declaration and nothing matched so far, try the names used
    // in other versions of Swift.
    if (auto clangDecl = entry.dyn_cast<clang::NamedDecl *>()) {
      const clang::NamedDecl *recentClangDecl =
          clangDecl->getMostRecentDecl();

      CurrentVersion.forEachOtherImportNameVersion(
          [&](ImportNameVersion nameVersion) {
        if (anyMatching)
          return;

        // Check to see if the name and context match what we expect.
        ImportedName newName = importFullName(recentClangDecl, nameVersion);
        if (!newName.getDeclName().matchesRef(name))
          return;

        // If we asked for an async import and didn't find one, skip this.
        // This filters out duplicates.
        if (nameVersion.supportsConcurrency() &&
            !newName.getAsyncInfo())
          return;

        const clang::DeclContext *clangDC =
            newName.getEffectiveContext().getAsDeclContext();
        if (!clangDC || !clangDC->isFileContext())
          return;

        // Then try to import the decl under the alternate name.
        auto alternateNamedDecl =
            cast_or_null<ValueDecl>(importDeclReal(recentClangDecl,
                                                   nameVersion));
        if (!alternateNamedDecl || alternateNamedDecl == decl)
          return;
        assert(alternateNamedDecl->getName().matchesRef(name) &&
               "importFullName behaved differently from importDecl");
        if (alternateNamedDecl->getDeclContext()->isModuleScopeContext()) {
          consumer.foundDecl(alternateNamedDecl,
                             DeclVisibilityKind::VisibleAtTopLevel);
          anyMatching = true;
        }
      });
    }
  }
}

void ClangImporter::Implementation::lookupVisibleDecls(
       SwiftLookupTable &table,
       VisibleDeclConsumer &consumer) {
  // Retrieve and sort all of the base names in this particular table.
  auto baseNames = table.allBaseNames();
  llvm::array_pod_sort(baseNames.begin(), baseNames.end());

  // Look for namespace-scope entities with each base name.
  for (auto baseName : baseNames) {
    lookupValue(table, baseName.toDeclBaseName(SwiftContext), consumer);
  }
}

void ClangImporter::Implementation::lookupObjCMembers(
       SwiftLookupTable &table,
       DeclName name,
       VisibleDeclConsumer &consumer) {
  for (auto clangDecl : table.lookupObjCMembers(name.getBaseName())) {
    // If the entry is not visible, skip it.
    if (!isVisibleClangEntry(clangDecl)) continue;

    forEachDistinctName(clangDecl,
                        [&](ImportedName importedName,
                            ImportNameVersion nameVersion) -> bool {
      // Import the declaration.
      auto decl =
          cast_or_null<ValueDecl>(importDeclReal(clangDecl, nameVersion));
      if (!decl)
        return false;

      // If the name we found matches, report the declaration.
      // FIXME: If we didn't need to check alternate decls here, we could avoid
      // importing the member at all by checking importedName ahead of time.
      if (decl->getName().matchesRef(name)) {
        consumer.foundDecl(decl, DeclVisibilityKind::DynamicLookup,
                           DynamicLookupInfo::AnyObject);
      }

      // Check for an alternate declaration; if its name matches,
      // report it.
      for (auto alternate : getAlternateDecls(decl)) {
        if (alternate->getName().matchesRef(name)) {
          consumer.foundDecl(alternate, DeclVisibilityKind::DynamicLookup,
                             DynamicLookupInfo::AnyObject);
        }
      }
      return true;
    });
  }
}

void ClangImporter::Implementation::lookupAllObjCMembers(
       SwiftLookupTable &table,
       VisibleDeclConsumer &consumer) {
  // Retrieve and sort all of the base names in this particular table.
  auto baseNames = table.allBaseNames();
  llvm::array_pod_sort(baseNames.begin(), baseNames.end());

  // Look for Objective-C members with each base name.
  for (auto baseName : baseNames) {
    lookupObjCMembers(table, baseName.toDeclBaseName(SwiftContext), consumer);
  }
}

TinyPtrVector<ValueDecl *>
ClangImporter::Implementation::loadNamedMembers(
    const IterableDeclContext *IDC, DeclBaseName N, uint64_t contextData) {

  auto *D = IDC->getDecl();
  auto *DC = D->getInnermostDeclContext();
  auto *CD = D->getClangDecl();
  auto *CDC = cast<clang::DeclContext>(CD);
  assert(CD && "loadNamedMembers on a Decl without a clangDecl");

  auto *nominal = DC->getSelfNominalTypeDecl();
  auto effectiveClangContext = getEffectiveClangContext(nominal);

  // There are 3 cases:
  //
  //  - The decl is from a bridging header, CMO is Some(nullptr)
  //    which denotes the __ObjC Swift module and its associated
  //    BridgingHeaderLookupTable.
  //
  //  - The decl is from a clang module, CMO is Some(M) for non-null
  //    M and we can use the table for that module.
  //
  //  - The decl is a forward declaration, CMO is None, which should
  //    never be the case if we got here (someone is asking for members).
  //
  // findLookupTable, below, handles the first two cases; we assert on the
  // third.

  auto CMO = getClangSubmoduleForDecl(CD);
  assert(CMO && "loadNamedMembers on a forward-declared Decl");

  auto table = findLookupTable(*CMO);
  assert(table && "clang module without lookup table");

  assert(isa<clang::ObjCContainerDecl>(CD) || isa<clang::NamespaceDecl>(CD));

  // Force the members of the entire inheritance hierarchy to be loaded and
  // deserialized before loading the named member of a class. This warms up
  // ClangImporter::Implementation::MembersForNominal, used for computing
  // property overrides.
  //
  // FIXME: If getOverriddenDecl() kicked off a request for imported decls,
  // we could postpone this until overrides are actually requested.
  if (auto *classDecl = dyn_cast<ClassDecl>(D))
    if (auto *superclassDecl = classDecl->getSuperclassDecl())
      (void) const_cast<ClassDecl *>(superclassDecl)->lookupDirect(N);

  TinyPtrVector<ValueDecl *> Members;
  for (auto entry : table->lookup(SerializedSwiftName(N),
                                  effectiveClangContext)) {
    if (!entry.is<clang::NamedDecl *>()) continue;
    auto member = entry.get<clang::NamedDecl *>();
    if (!isVisibleClangEntry(member)) continue;

    // Skip Decls from different clang::DeclContexts
    if (member->getDeclContext() != CDC) continue;

    SmallVector<Decl*, 4> tmp;
    insertMembersAndAlternates(member, tmp);
    for (auto *TD : tmp) {
      if (auto *V = dyn_cast<ValueDecl>(TD)) {
        // Skip ValueDecls if they import under different names.
        if (V->getBaseName() == N) {
          Members.push_back(V);
        }
      }

      // If the property's accessors have alternate decls, we might have
      // to import those too.
      if (auto *ASD = dyn_cast<AbstractStorageDecl>(TD)) {
        for (auto *AD : ASD->getAllAccessors()) {
          for (auto *D : getAlternateDecls(AD)) {
            if (D->getBaseName() == N)
              Members.push_back(D);
          }
        }
      }
    }
  }

  for (auto entry : table->lookupGlobalsAsMembers(SerializedSwiftName(N),
                                                  effectiveClangContext)) {
    if (!entry.is<clang::NamedDecl *>()) continue;
    auto member = entry.get<clang::NamedDecl *>();
    if (!isVisibleClangEntry(member)) continue;

    // Skip Decls from different clang::DeclContexts
    if (member->getDeclContext() != CDC) continue;

    SmallVector<Decl*, 4> tmp;
    insertMembersAndAlternates(member, tmp);
    for (auto *TD : tmp) {
      if (auto *V = dyn_cast<ValueDecl>(TD)) {
        // Skip ValueDecls if they import under different names.
        if (V->getBaseName() == N) {
          Members.push_back(V);
        }
      }
    }
  }

  if (N == DeclBaseName::createConstructor()) {
    if (auto *classDecl = dyn_cast<ClassDecl>(D)) {
      SmallVector<Decl *, 4> ctors;
      importInheritedConstructors(cast<clang::ObjCInterfaceDecl>(CD),
                                  classDecl, ctors);
      for (auto ctor : ctors)
        Members.push_back(cast<ValueDecl>(ctor));
    }
  }

  if (!isa<ProtocolDecl>(D)) {
    if (auto *OCD = dyn_cast<clang::ObjCContainerDecl>(CD)) {
      SmallVector<Decl *, 1> newMembers;
      importMirroredProtocolMembers(OCD, DC, N, newMembers);
      for (auto member : newMembers)
          Members.push_back(cast<ValueDecl>(member));
    }
  }

  return Members;
}


EffectiveClangContext ClangImporter::Implementation::getEffectiveClangContext(
    const NominalTypeDecl *nominal) {
  // If we have a Clang declaration, look at it to determine the
  // effective Clang context.
  if (auto constClangDecl = nominal->getClangDecl()) {
    auto clangDecl = const_cast<clang::Decl *>(constClangDecl);
    if (auto dc = dyn_cast<clang::DeclContext>(clangDecl))
      return EffectiveClangContext(dc);
    if (auto typedefName = dyn_cast<clang::TypedefNameDecl>(clangDecl))
      return EffectiveClangContext(typedefName);

    return EffectiveClangContext();
  }

  // If it's an @objc entity, go look for it.
  // Note that we're stepping lightly here to avoid computing isObjC()
  // too early.
  if (isa<ClassDecl>(nominal) &&
      (nominal->getAttrs().hasAttribute<ObjCAttr>() ||
       (!nominal->getParentSourceFile() && nominal->isObjC()))) {
    // Map the name. If we can't represent the Swift name in Clang.
    Identifier name = nominal->getName();
    if (auto objcAttr = nominal->getAttrs().getAttribute<ObjCAttr>()) {
      if (auto objcName = objcAttr->getName()) {
        if (objcName->getNumArgs() == 0) {
          // This is an error if not 0, but it should be caught later.
          name = objcName->getSimpleName();
        }
      }
    }
    auto clangName = exportName(name);
    if (!clangName)
      return EffectiveClangContext();

    // Perform name lookup into the global scope.
    auto &sema = Instance->getSema();
    clang::LookupResult lookupResult(sema, clangName,
                                     clang::SourceLocation(),
                                     clang::Sema::LookupOrdinaryName);
    if (sema.LookupName(lookupResult, /*Scope=*/nullptr)) {
      // FIXME: Filter based on access path? C++ access control?
      for (auto clangDecl : lookupResult) {
        if (auto objcClass = dyn_cast<clang::ObjCInterfaceDecl>(clangDecl))
          return EffectiveClangContext(objcClass);

        /// FIXME: Other type declarations should also be okay?
      }
    }

    // For source compatibility reasons, fall back to the Swift name.
    //
    // This is how people worked around not being able to import-as-member onto
    // Swift types by their ObjC name before the above code to handle ObjCAttr
    // was added.
    if (name != nominal->getName())
      clangName = exportName(nominal->getName());

    lookupResult.clear();
    lookupResult.setLookupName(clangName);
    // FIXME: This loop is duplicated from above, but doesn't obviously factor
    // out in a nice way.
    if (sema.LookupName(lookupResult, /*Scope=*/nullptr)) {
      // FIXME: Filter based on access path? C++ access control?
      for (auto clangDecl : lookupResult) {
        if (auto objcClass = dyn_cast<clang::ObjCInterfaceDecl>(clangDecl))
          return EffectiveClangContext(objcClass);

        /// FIXME: Other type declarations should also be okay?
      }
    }
  }

  return EffectiveClangContext();
}

void ClangImporter::dumpSwiftLookupTables() {
  Impl.dumpSwiftLookupTables();
}

void ClangImporter::Implementation::dumpSwiftLookupTables() {
  // Sort the module names so we can print in a deterministic order.
  SmallVector<StringRef, 4> moduleNames;
  for (const auto &lookupTable : LookupTables) {
    moduleNames.push_back(lookupTable.first);
  }
  array_pod_sort(moduleNames.begin(), moduleNames.end());

  // Print out the lookup tables for the various modules.
  for (auto moduleName : moduleNames) {
    llvm::errs() << "<<" << moduleName << " lookup table>>\n";
    LookupTables[moduleName]->deserializeAll();
    LookupTables[moduleName]->dump(llvm::errs());
  }

  llvm::errs() << "<<Bridging header lookup table>>\n";
  BridgingHeaderLookupTable->dump(llvm::errs());
}

DeclName ClangImporter::
importName(const clang::NamedDecl *D,
           clang::DeclarationName preferredName) {
  return Impl.importFullName(D, Impl.CurrentVersion, preferredName).
    getDeclName();
}

bool ClangImporter::isInOverlayModuleForImportedModule(
                                               const DeclContext *overlayDC,
                                               const DeclContext *importedDC) {
  overlayDC = overlayDC->getModuleScopeContext();
  importedDC = importedDC->getModuleScopeContext();

  auto importedClangModuleUnit = dyn_cast<ClangModuleUnit>(importedDC);
  if (!importedClangModuleUnit || !importedClangModuleUnit->getClangModule())
    return false;

  auto overlayModule = overlayDC->getParentModule();
  if (overlayModule == importedClangModuleUnit->getOverlayModule())
    return true;

  // Is this a private module that's re-exported to the public (overlay) name?
  auto clangModule =
  importedClangModuleUnit->getClangModule()->getTopLevelModule();
  return !clangModule->ExportAsModule.empty() &&
    clangModule->ExportAsModule == overlayModule->getName().str();
}

/// Extract the specified-or-defaulted -module-cache-path that winds up in
/// the clang importer, for reuse as the .swiftmodule cache path when
/// building a ModuleInterfaceLoader.
std::string
swift::getModuleCachePathFromClang(const clang::CompilerInstance &Clang) {
  if (!Clang.hasPreprocessor())
    return "";
  std::string SpecificModuleCachePath =
      Clang.getPreprocessor().getHeaderSearchInfo().getModuleCachePath().str();

  // The returned-from-clang module cache path includes a suffix directory
  // that is specific to the clang version and invocation; we want the
  // directory above that.
  return llvm::sys::path::parent_path(SpecificModuleCachePath).str();
}

clang::FunctionDecl *ClangImporter::instantiateCXXFunctionTemplate(
    ASTContext &ctx, clang::FunctionTemplateDecl *func, SubstitutionMap subst) {
  SmallVector<clang::TemplateArgument, 4> templateSubst;
  std::unique_ptr<TemplateInstantiationError> error =
      ctx.getClangTemplateArguments(func->getTemplateParameters(),
                                    subst.getReplacementTypes(), templateSubst);
  if (error) {
    std::string failedTypesStr;
    llvm::raw_string_ostream failedTypesStrStream(failedTypesStr);
    llvm::interleaveComma(error->failedTypes, failedTypesStrStream);
    // TODO: Use the location of the apply here.
    // TODO: This error message should not reference implementation details.
    // See: https://github.com/apple/swift/pull/33053#discussion_r477003350
    ctx.Diags.diagnose(SourceLoc(),
                       diag::unable_to_convert_generic_swift_types.ID,
                       {func->getName(), StringRef(failedTypesStr)});
    // Return a valid FunctionDecl but, we'll never use it.
    return func->getAsFunction();
  }

  // Instanciate a specialization of this template using the substitution map.
  auto *templateArgList = clang::TemplateArgumentList::CreateCopy(
      func->getASTContext(), templateSubst);
  auto &sema = getClangInstance().getSema();
  auto *spec = sema.InstantiateFunctionDeclaration(func, templateArgList,
                                                   clang::SourceLocation());
  sema.InstantiateFunctionDefinition(clang::SourceLocation(), spec);
  return spec;
}

StructDecl *
ClangImporter::instantiateCXXClassTemplate(
    clang::ClassTemplateDecl *decl,
    ArrayRef<clang::TemplateArgument> arguments) {
  void *InsertPos = nullptr;
  auto *ctsd = decl->findSpecialization(arguments, InsertPos);
  if (!ctsd) {
    ctsd = clang::ClassTemplateSpecializationDecl::Create(
        decl->getASTContext(), decl->getTemplatedDecl()->getTagKind(),
        decl->getDeclContext(), decl->getTemplatedDecl()->getBeginLoc(),
        decl->getLocation(), decl, arguments, nullptr);
    decl->AddSpecialization(ctsd, InsertPos);
  }

  auto CanonType = decl->getASTContext().getTypeDeclType(ctsd);
  assert(isa<clang::RecordType>(CanonType) &&
          "type of non-dependent specialization is not a RecordType");

  return dyn_cast_or_null<StructDecl>(
      Impl.importDecl(ctsd, Impl.CurrentVersion));
}
