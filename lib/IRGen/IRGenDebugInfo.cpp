//===--- IRGenDebugInfo.cpp - Debug Info Support --------------------------===//
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
//  This file implements IR debug info generation for Swift.
//
//===----------------------------------------------------------------------===//

#include "IRGenDebugInfo.h"
#include "GenOpaque.h"
#include "GenStruct.h"
#include "GenType.h"
#include "IRBuilder.h"
#include "swift/AST/ASTDemangler.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/Module.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/TypeDifferenceVisitor.h"
#include "swift/Basic/Dwarf.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Version.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Demangling/ManglingMacros.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILModule.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/ExternalASTSource.h"
#include "clang/Basic/Module.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Serialization/ASTReader.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Config/config.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Local.h"

#define DEBUG_TYPE "debug-info"

using namespace swift;
using namespace irgen;

llvm::cl::opt<bool> VerifyLineTable(
    "verify-linetable", llvm::cl::init(false),
    llvm::cl::desc(
        "Verify that the debug locations within one scope are contiguous."));

namespace {
using TrackingDIRefMap =
    llvm::DenseMap<const llvm::MDString *, llvm::TrackingMDNodeRef>;

class EqualUpToClangTypes
    : public CanTypeDifferenceVisitor<EqualUpToClangTypes> {
public:
  bool visitDifferentTypeStructure(CanType t1, CanType t2) {
#define COMPARE_UPTO_CLANG_TYPE(CLASS)                                         \
  if (auto f1 = dyn_cast<CLASS>(t1)) {                                         \
    auto f2 = cast<CLASS>(t2);                                                 \
    return !f1->getExtInfo().isEqualTo(f2->getExtInfo(),                       \
                                       /*useClangTypes*/ false);               \
  }
    COMPARE_UPTO_CLANG_TYPE(FunctionType);
    COMPARE_UPTO_CLANG_TYPE(SILFunctionType);
#undef COMPARE_UPTO_CLANG_TYPE
    return true;
  }
  bool check(Type t1, Type t2) {
    return !visit(t1->getCanonicalType(), t2->getCanonicalType());
  };
};

static bool equalWithoutExistentialTypes(Type t1, Type t2) {
  static Type (*withoutExistentialTypes)(Type) = [](Type type) -> Type {
    return type.transform([](Type type) -> Type {
      if (auto existential = type->getAs<ExistentialType>()) {
        return withoutExistentialTypes(existential->getConstraintType());
      }
      return type;
    });
  };

  return withoutExistentialTypes(t1)
      ->isEqual(withoutExistentialTypes(t2));
}

class IRGenDebugInfoImpl : public IRGenDebugInfo {
  const IRGenOptions &Opts;
  ClangImporter &CI;
  SourceManager &SM;
  llvm::Module &M;
  llvm::DIBuilder DBuilder;
  IRGenModule &IGM;
  const PathRemapper &DebugPrefixMap;

  /// Various caches.
  /// \{
  llvm::StringSet<> VarNames;
  using VarID = std::tuple<llvm::MDNode *, llvm::StringRef, unsigned, uint16_t>;
  llvm::DenseMap<VarID, llvm::TrackingMDNodeRef> LocalVarCache;
  llvm::DenseMap<const SILDebugScope *, llvm::TrackingMDNodeRef> ScopeCache;
  llvm::DenseMap<const SILDebugScope *, llvm::TrackingMDNodeRef> InlinedAtCache;
  llvm::DenseMap<const void *, SILLocation::FilenameAndLocation>
     FilenameAndLocationCache;
  llvm::DenseMap<TypeBase *, llvm::TrackingMDNodeRef> DITypeCache;
  llvm::DenseMap<const void *, llvm::TrackingMDNodeRef> DIModuleCache;
  llvm::StringMap<llvm::TrackingMDNodeRef> DIFileCache;
  llvm::StringMap<llvm::TrackingMDNodeRef> RuntimeErrorFnCache;
  TrackingDIRefMap DIRefMap;
  TrackingDIRefMap InnerTypeCache;
  /// \}

  /// A list of replaceable fwddecls that need to be RAUWed at the end.
  std::vector<std::pair<StringRef, llvm::TrackingMDRef>> FwdDeclTypes;
  /// The set of imported modules.
  llvm::DenseSet<ModuleDecl *> ImportedModules;

  llvm::BumpPtrAllocator DebugInfoNames;
  /// The current working directory.
  StringRef CWDName;
  /// User-provided -D macro definitions.
  SmallString<0> ConfigMacros;
  /// The current compilation unit.
  llvm::DICompileUnit *TheCU = nullptr;
  /// The main file.
  llvm::DIFile *MainFile = nullptr;
  /// The current module.
  llvm::DIModule *MainModule = nullptr;
  /// Scope of entry point function (main by default).
  llvm::DIScope *EntryPointFn = nullptr;
  /// The artificial type decls for named archetypes.
  llvm::StringMap<TypeAliasDecl *> MetadataTypeDeclCache;
  /// Catch-all type for opaque internal types.
  llvm::DIType *InternalType = nullptr;

  /// The last location that was emitted.
  SILLocation::FilenameAndLocation LastFilenameAndLocation;
  /// The scope of that last location.
  const SILDebugScope *LastScope = nullptr;

  /// Used by pushLoc.
  SmallVector<std::pair<SILLocation::FilenameAndLocation, const SILDebugScope *>, 8>
      LocationStack;

#ifndef NDEBUG
  using UUSTuple = std::pair<std::pair<unsigned, unsigned>, StringRef>;
  struct FilenameAndLocationKey : public UUSTuple {
    FilenameAndLocationKey(SILLocation::FilenameAndLocation DL)
        : UUSTuple({{DL.line, DL.column}, DL.filename}) {}
    inline bool operator==(const SILLocation::FilenameAndLocation &DL) const {
      return first.first == DL.line && first.second == DL.column &&
             second.equals(DL.filename);
    }
  };
  llvm::DenseSet<UUSTuple> PreviousLineEntries;
  SILLocation::FilenameAndLocation PreviousFilenameAndLocation;
#endif

public:
  IRGenDebugInfoImpl(const IRGenOptions &Opts, ClangImporter &CI,
                     IRGenModule &IGM, llvm::Module &M,
                     StringRef MainOutputFilenameForDebugInfo,
                     StringRef PrivateDiscriminator);
  void finalize();

  void setCurrentLoc(IRBuilder &Builder, const SILDebugScope *DS,
                     SILLocation Loc);

  void addFailureMessageToCurrentLoc(IRBuilder &Builder, StringRef failureMsg);

  void clearLoc(IRBuilder &Builder);
  void pushLoc();
  void popLoc();
  void setInlinedTrapLocation(IRBuilder &Builder, const SILDebugScope *Scope);
  void setEntryPointLoc(IRBuilder &Builder);
  llvm::DIScope *getEntryPointFn();
  llvm::DIScope *getOrCreateScope(const SILDebugScope *DS);
  void emitImport(ImportDecl *D);
  llvm::DISubprogram *emitFunction(const SILDebugScope *DS, llvm::Function *Fn,
                                   SILFunctionTypeRepresentation Rep,
                                   SILType Ty, DeclContext *DeclCtx = nullptr,
                                   StringRef outlinedFromName = StringRef());
  llvm::DISubprogram *emitFunction(SILFunction &SILFn, llvm::Function *Fn);
  void emitArtificialFunction(IRBuilder &Builder, llvm::Function *Fn,
                              SILType SILTy);
  void emitOutlinedFunction(IRBuilder &Builder,
                            llvm::Function *Fn,
                            StringRef outlinedFromName);

  /// Return false if we fail to create the right DW_OP_LLVM_fragment operand.
  bool handleFragmentDIExpr(const SILDIExprOperand &CurDIExprOp,
                            llvm::DIExpression::FragmentInfo &Fragment);
  /// Return false if we fail to create the desired !DIExpression.
  bool buildDebugInfoExpression(const SILDebugVariable &VarInfo,
                                SmallVectorImpl<uint64_t> &Operands,
                                llvm::DIExpression::FragmentInfo &Fragment);

  /// Emit a dbg.declare at the current insertion point in Builder.
  void emitVariableDeclaration(IRBuilder &Builder,
                               ArrayRef<llvm::Value *> Storage,
                               DebugTypeInfo Ty, const SILDebugScope *DS,
                               llvm::Optional<SILLocation> VarLoc,
                               SILDebugVariable VarInfo,
                               IndirectionKind = DirectValue,
                               ArtificialKind = RealValue,
                               AddrDbgInstrKind = AddrDbgInstrKind::DbgDeclare);

  void emitDbgIntrinsic(IRBuilder &Builder, llvm::Value *Storage,
                        llvm::DILocalVariable *Var, llvm::DIExpression *Expr,
                        unsigned Line, unsigned Col, llvm::DILocalScope *Scope,
                        const SILDebugScope *DS, bool InCoroContext,
                        AddrDbgInstrKind = AddrDbgInstrKind::DbgDeclare);

  void emitGlobalVariableDeclaration(llvm::GlobalVariable *Storage,
                                     StringRef Name, StringRef LinkageName,
                                     DebugTypeInfo DebugType,
                                     bool IsLocalToUnit,
                                     llvm::Optional<SILLocation> Loc);
  void emitTypeMetadata(IRGenFunction &IGF, llvm::Value *Metadata,
                        unsigned Depth, unsigned Index, StringRef Name);
  void emitPackCountParameter(IRGenFunction &IGF, llvm::Value *Metadata,
                              SILDebugVariable VarInfo);

  /// Return the DIBuilder.
  llvm::DIBuilder &getBuilder() { return DBuilder; }

  /// Decode (and cache) a SourceLoc.
  SILLocation::FilenameAndLocation decodeSourceLoc(SourceLoc SL);

  IRGenDebugInfoFormat getDebugInfoFormat() { return Opts.DebugInfoFormat; }

private:
  static StringRef getFilenameFromDC(const DeclContext *DC) {
    if (auto *LF = dyn_cast<LoadedFile>(DC))
      return LF->getFilename();
    if (auto *SF = dyn_cast<SourceFile>(DC))
      return SF->getFilename();
    if (auto *M = dyn_cast<ModuleDecl>(DC))
      return M->getModuleFilename();
    return {};
  }

  using FilenameAndLocation = SILLocation::FilenameAndLocation;
  FilenameAndLocation getDeserializedLoc(Pattern *) { return {}; }
  FilenameAndLocation getDeserializedLoc(Expr *) { return {}; }
  FilenameAndLocation getDeserializedLoc(Decl *D) {
    FilenameAndLocation L;
    const DeclContext *DC = D->getDeclContext()->getModuleScopeContext();
    StringRef Filename = getFilenameFromDC(DC);
    if (!Filename.empty())
      L.filename = Filename;
    return L;
  }

  /// Use the Swift SM to figure out the actual line/column of a SourceLoc.
  template <typename WithLoc>
  FilenameAndLocation getSwiftFilenameAndLocation(IRGenDebugInfo &DI,
                                                  WithLoc *ASTNode, bool End) {
    if (!ASTNode)
      return {};

    SourceLoc Loc = End ? ASTNode->getEndLoc() : ASTNode->getStartLoc();
    if (Loc.isInvalid())
      // This may be a deserialized or clang-imported decl. And modules
      // don't come with SourceLocs right now. Get at least the name of
      // the module.
      return getDeserializedLoc(ASTNode);

    return DI.decodeSourceLoc(Loc);
  }

  FilenameAndLocation getFilenameAndLocation(IRGenDebugInfo &DI, Pattern *P,
                                             bool End = false) {
    return getSwiftFilenameAndLocation(DI, P, End);
  }
  FilenameAndLocation getFilenameAndLocation(IRGenDebugInfo &DI, Expr *E,
                                             bool End = false) {
    return getSwiftFilenameAndLocation(DI, E, End);
  }
  FilenameAndLocation getFilenameAndLocation(IRGenDebugInfo &DI, Decl *D,
                                             bool End = false) {
    FilenameAndLocation L;
    if (!D)
      return L;

    if (auto *ClangDecl = D->getClangDecl()) {
      clang::SourceLocation ClangSrcLoc = ClangDecl->getBeginLoc();
      clang::SourceManager &ClangSM =
          CI.getClangASTContext().getSourceManager();
      clang::PresumedLoc PresumedLoc = ClangSM.getPresumedLoc(ClangSrcLoc);
      if (!PresumedLoc.isValid())
        return L;
      L.line = PresumedLoc.getLine();
      L.column = PresumedLoc.getColumn();
      L.filename = PresumedLoc.getFilename();
      return L;
    }
    return getSwiftFilenameAndLocation(DI, D, End);
  }

  FilenameAndLocation getStartLocation(llvm::Optional<SILLocation> OptLoc) {
    if (!OptLoc)
      return {};
    if (OptLoc->isFilenameAndLocation())
      return sanitizeCodeViewFilenameAndLocation(*OptLoc->getFilenameAndLocation());
    return decodeSourceLoc(OptLoc->getStartSourceLoc());
  }

  FilenameAndLocation sanitizeCodeViewFilenameAndLocation(FilenameAndLocation DLoc) {
    if (Opts.DebugInfoFormat == IRGenDebugInfoFormat::CodeView)
      // When WinDbg finds two locations with the same line but different
      // columns, the user must select an address when they break on that
      // line. Also, clang does not emit column locations in CodeView for C++.
      DLoc.column = 0;
    return DLoc;
  }

  FilenameAndLocation decodeFilenameAndLocation(SILLocation Loc) {
    if (Loc.isFilenameAndLocation())
      return sanitizeCodeViewFilenameAndLocation(*Loc.getFilenameAndLocation());
    return decodeSourceLoc(Loc.getSourceLocForDebugging());
  }

  /// Strdup a raw char array using the bump pointer.
  StringRef BumpAllocatedString(const char *Data, size_t Length) {
    char *Ptr = DebugInfoNames.Allocate<char>(Length + 1);
    memcpy(Ptr, Data, Length);
    *(Ptr + Length) = 0;
    return StringRef(Ptr, Length);
  }

  /// Strdup S using the bump pointer.
  StringRef BumpAllocatedString(std::string S) {
    return BumpAllocatedString(S.c_str(), S.length());
  }

  /// Strdup StringRef S using the bump pointer.
  StringRef BumpAllocatedString(StringRef S) {
    return BumpAllocatedString(S.data(), S.size());
  }

  /// Return the size reported by a type.
  static unsigned getSizeInBits(llvm::DIType *Ty) {
    // Follow derived types until we reach a type that
    // reports back a size.
    while (isa<llvm::DIDerivedType>(Ty) && !Ty->getSizeInBits()) {
      auto *DT = cast<llvm::DIDerivedType>(Ty);
      Ty = DT->getBaseType();
      if (!Ty)
        return 0;
    }
    return Ty->getSizeInBits();
  }

#ifndef NDEBUG

  /// Return the size reported by the variable's type.
  static unsigned getSizeInBits(const llvm::DILocalVariable *Var) {
    llvm::DIType *Ty = Var->getType();
    return getSizeInBits(Ty);
  }

#endif

  /// Determine whether this debug scope belongs to an explicit closure.
  static bool isExplicitClosure(const SILFunction *SILFn) {
    if (SILFn && SILFn->hasLocation())
      if (Expr *E = SILFn->getLocation().getAsASTNode<Expr>())
        if (isa<ClosureExpr>(E))
          return true;
    return false;
  }

public:
  llvm::MDNode *createInlinedAt(const SILDebugScope *DS) {
    auto *CS = DS->InlinedCallSite;
    if (!CS)
      return nullptr;
 
    auto CachedInlinedAt = InlinedAtCache.find(CS);
    if (CachedInlinedAt != InlinedAtCache.end())
      return cast<llvm::MDNode>(CachedInlinedAt->second);

    auto L = decodeFilenameAndLocation(CS->Loc);
    auto Scope = getOrCreateScope(CS->Parent.dyn_cast<const SILDebugScope *>());
    if (auto *Fn = CS->Parent.dyn_cast<SILFunction *>())
      Scope = getOrCreateScope(Fn->getDebugScope());
    // Pretend transparent functions don't exist.
    if (!Scope)
      return createInlinedAt(CS);
    auto InlinedAt = llvm::DILocation::get(
        IGM.getLLVMContext(), L.line, L.column, Scope, createInlinedAt(CS));
    InlinedAtCache.insert({CS, llvm::TrackingMDNodeRef(InlinedAt)});
    return InlinedAt;
  }

private:

#ifndef NDEBUG
  /// Perform a couple of sanity checks on scopes.
  static bool parentScopesAreSane(const SILDebugScope *DS) {
    auto *Parent = DS;
    while ((Parent = Parent->Parent.dyn_cast<const SILDebugScope *>())) {
      if (!DS->InlinedCallSite)
        assert(!Parent->InlinedCallSite &&
               "non-inlined scope has an inlined parent");
    }
    return true;
  }

  /// Assert that within one lexical block, each location is only visited once.
  bool lineEntryIsSane(FilenameAndLocation DL, const SILDebugScope *DS);

#endif

  llvm::DIFile *getOrCreateFile(StringRef Filename) {
    if (Filename.empty())
      Filename = SILLocation::getCompilerGeneratedLoc()->filename;

    // Look in the cache first.
    auto CachedFile = DIFileCache.find(Filename);
    if (CachedFile != DIFileCache.end()) {
      // Verify that the information still exists.
      if (llvm::Metadata *V = CachedFile->second)
        return cast<llvm::DIFile>(V);
    }

    // Detect the main file.
    StringRef MainFileName = MainFile->getFilename();
    if (MainFile && Filename.endswith(MainFileName)) {
      SmallString<256> AbsThisFile, AbsMainFile;
      AbsThisFile = Filename;
      llvm::sys::fs::make_absolute(AbsThisFile);
      if (llvm::sys::path::is_absolute(MainFileName))
        AbsMainFile = MainFileName;
      else
        llvm::sys::path::append(AbsMainFile, MainFile->getDirectory(),
                                MainFileName);
      if (AbsThisFile == DebugPrefixMap.remapPath(AbsMainFile)) {
        DIFileCache[Filename] = llvm::TrackingMDNodeRef(MainFile);
        return MainFile;
      }
    }

    return createFile(Filename, llvm::None, llvm::None);
  }

  /// This is effectively \p clang::CGDebugInfo::createFile().
  llvm::DIFile *
  createFile(StringRef FileName,
             llvm::Optional<llvm::DIFile::ChecksumInfo<StringRef>> CSInfo,
             llvm::Optional<StringRef> Source) {
    StringRef File, Dir;
    StringRef CurDir = Opts.DebugCompilationDir;
    SmallString<128> NormalizedFile(FileName);
    SmallString<128> FileBuf, DirBuf;
    llvm::sys::path::remove_dots(NormalizedFile);
    if (llvm::sys::path::is_absolute(NormalizedFile) &&
        llvm::sys::path::is_absolute(CurDir)) {
      // Strip the common prefix (if it is more than just "/") from current
      // directory and FileName for a more space-efficient encoding.
      auto FileIt = llvm::sys::path::begin(NormalizedFile);
      auto FileE = llvm::sys::path::end(NormalizedFile);
      auto CurDirIt = llvm::sys::path::begin(CurDir);
      auto CurDirE = llvm::sys::path::end(CurDir);
      for (; CurDirIt != CurDirE && *CurDirIt == *FileIt; ++CurDirIt, ++FileIt)
        llvm::sys::path::append(DirBuf, *CurDirIt);
      if (std::distance(llvm::sys::path::begin(CurDir), CurDirIt) == 1) {
        // Don't strip the common prefix if it is only the root "/"
        // since that would make LLVM diagnostic locations confusing.
        Dir = {};
        File = NormalizedFile;
      } else {
        for (; FileIt != FileE; ++FileIt)
          llvm::sys::path::append(FileBuf, *FileIt);
        Dir = DirBuf;
        File = FileBuf;
      }
    } else {
      File = NormalizedFile;
      // Leave <compiler-generated> & friends as is, without directory.
      if (!(File.startswith("<") && File.endswith(">")))
        Dir = CurDir;
    }
    llvm::DIFile *F =
        DBuilder.createFile(DebugPrefixMap.remapPath(File),
                            DebugPrefixMap.remapPath(Dir), CSInfo, Source);
    DIFileCache[FileName].reset(F);
    return F;
  }

  StringRef getName(const FuncDecl &FD) {
    // Getters and Setters are anonymous functions, so we forge a name
    // using its parent declaration.
    if (auto accessor = dyn_cast<AccessorDecl>(&FD))
      if (ValueDecl *VD = accessor->getStorage()) {
        const char *Kind;
        switch (accessor->getAccessorKind()) {
        case AccessorKind::Get:
          Kind = ".get";
          break;
        case AccessorKind::Set:
          Kind = ".set";
          break;
        case AccessorKind::WillSet:
          Kind = ".willset";
          break;
        case AccessorKind::DidSet:
          Kind = ".didset";
          break;
        case AccessorKind::Address:
          Kind = ".addressor";
          break;
        case AccessorKind::MutableAddress:
          Kind = ".mutableAddressor";
          break;
        case AccessorKind::Read:
          Kind = ".read";
          break;
        case AccessorKind::Modify:
          Kind = ".modify";
          break;
        case AccessorKind::Init:
          Kind = ".init";
          break;
        }

        SmallVector<char, 64> Buf;
        StringRef Name =
            (VD->getBaseName().userFacingName() + Twine(Kind)).toStringRef(Buf);
        return BumpAllocatedString(Name);
      }

    if (FD.hasName())
      return FD.getBaseIdentifier().str();

    return StringRef();
  }

  StringRef getName(SILLocation L) {
    if (L.isNull())
      return StringRef();

    if (FuncDecl *FD = L.getAsASTNode<FuncDecl>())
      return getName(*FD);

    if (ValueDecl *D = L.getAsASTNode<ValueDecl>())
      return D->getBaseName().userFacingName();

    if (auto *D = L.getAsASTNode<MacroExpansionDecl>())
      return D->getMacroName().getBaseIdentifier().str();

    if (auto *E = L.getAsASTNode<MacroExpansionExpr>())
      return E->getMacroName().getBaseIdentifier().str();

    return StringRef();
  }

  static CanSILFunctionType getFunctionType(SILType SILTy) {
    if (!SILTy)
      return CanSILFunctionType();

    auto FnTy = SILTy.getAs<SILFunctionType>();
    if (!FnTy) {
      LLVM_DEBUG(llvm::dbgs() << "Unexpected function type: ";
                 SILTy.print(llvm::dbgs()); llvm::dbgs() << "\n");
      return CanSILFunctionType();
    }

    return FnTy;
  }

  llvm::DIScope *getOrCreateContext(DeclContext *DC) {
    if (!DC)
      return TheCU;

    if (isa<FuncDecl>(DC))
      if (auto *Decl = IGM.getSILModule().lookUpFunction(SILDeclRef(
              cast<AbstractFunctionDecl>(DC), SILDeclRef::Kind::Func)))
        return getOrCreateScope(Decl->getDebugScope());

    switch (DC->getContextKind()) {
    // The interesting cases are already handled above.
    case DeclContextKind::AbstractFunctionDecl:
    case DeclContextKind::AbstractClosureExpr:

    // We don't model these in DWARF.
    case DeclContextKind::SerializedLocal:
    case DeclContextKind::Initializer:
    case DeclContextKind::ExtensionDecl:
    case DeclContextKind::SubscriptDecl:
    case DeclContextKind::EnumElementDecl:
    case DeclContextKind::TopLevelCodeDecl:
      return getOrCreateContext(DC->getParent());

    case DeclContextKind::Package: {
      auto *pkg = cast<PackageUnit>(DC);
      return getOrCreateContext(pkg);
    }
    case DeclContextKind::Module:
      return getOrCreateModule(
          {ImportPath::Access(), cast<ModuleDecl>(DC)});
    case DeclContextKind::FileUnit:
      // A module may contain multiple files.
      return getOrCreateContext(DC->getParent());
    case DeclContextKind::MacroDecl:
      return getOrCreateContext(DC->getParent());
    case DeclContextKind::GenericTypeDecl: {
      // The generic signature of this nominal type has no relation to the current
      // function's generic signature.
      auto *NTD = cast<NominalTypeDecl>(DC);
      GenericContextScope scope(IGM, NTD->getGenericSignature().getCanonicalSignature());

      auto Ty = NTD->getDeclaredInterfaceType();
      // Create a Forward-declared type.
      auto DbgTy = DebugTypeInfo::getForwardDecl(Ty);
      return getOrCreateType(DbgTy);
    }
    }
    return TheCU;
  }

  void createParameterType(llvm::SmallVectorImpl<llvm::Metadata *> &Parameters,
                           SILType type) {
    auto RealType = type.getASTType();
    auto DbgTy = DebugTypeInfo::getForwardDecl(RealType);
    Parameters.push_back(getOrCreateType(DbgTy));
  }

  // This is different from SILFunctionType::getAllResultsType() in some subtle
  // ways.
  static SILType getResultTypeForDebugInfo(IRGenModule &IGM,
                                           CanSILFunctionType fnTy) {
    if (fnTy->getNumResults() == 1) {
      return fnTy->getResults()[0].getSILStorageType(
          IGM.getSILModule(), fnTy, IGM.getMaximalTypeExpansionContext());
    } else if (!fnTy->getNumIndirectFormalResults()) {
      return fnTy->getDirectFormalResultsType(
          IGM.getSILModule(), IGM.getMaximalTypeExpansionContext());
    } else {
      SmallVector<TupleTypeElt, 4> eltTys;
      for (auto &result : fnTy->getResults()) {
        eltTys.push_back(result.getReturnValueType(
            IGM.getSILModule(), fnTy, IGM.getMaximalTypeExpansionContext()));
      }
      return SILType::getPrimitiveAddressType(
          CanType(TupleType::get(eltTys, fnTy->getASTContext())));
    }
  }

  llvm::DITypeRefArray createParameterTypes(SILType SILTy) {
    if (!SILTy)
      return nullptr;
    return createParameterTypes(SILTy.castTo<SILFunctionType>());
  }

  llvm::DITypeRefArray createParameterTypes(CanSILFunctionType FnTy) {
    SmallVector<llvm::Metadata *, 16> Parameters;

    GenericContextScope scope(IGM, FnTy->getInvocationGenericSignature());

    // The function return type is the first element in the list.
    createParameterType(Parameters, getResultTypeForDebugInfo(IGM, FnTy));

    for (auto Param : FnTy->getParameters())
      createParameterType(
          Parameters, IGM.silConv.getSILType(
                          Param, FnTy, IGM.getMaximalTypeExpansionContext()));

    return DBuilder.getOrCreateTypeArray(Parameters);
  }

  /// FIXME: replace this condition with something more sane.
  static bool isAllocatingConstructor(SILFunctionTypeRepresentation Rep,
                                      DeclContext *DeclCtx) {
    return Rep != SILFunctionTypeRepresentation::Method && DeclCtx &&
           isa<ConstructorDecl>(DeclCtx);
  }

  void createImportedModule(llvm::DIScope *Context,
                            ImportedModule M, llvm::DIFile *File,
                            unsigned Line) {
    // For overlays of Clang modules also emit an import of the underlying Clang
    // module. The helps the debugger resolve types that are present only in the
    // underlying module.
    if (const clang::Module *UnderlyingClangModule =
            M.importedModule->findUnderlyingClangModule()) {
      DBuilder.createImportedModule(
          Context,
          getOrCreateModule(
              {*const_cast<clang::Module *>(UnderlyingClangModule)},
              UnderlyingClangModule),
          File, 0);
    }
    DBuilder.createImportedModule(Context, getOrCreateModule(M), File, Line);
  }

  llvm::DIModule *getOrCreateModule(const void *Key, llvm::DIScope *Parent,
                                    StringRef Name, StringRef IncludePath,
                                    uint64_t Signature = ~1ULL,
                                    StringRef ASTFile = StringRef()) {
    // Look in the cache first.
    auto Val = DIModuleCache.find(Key);
    if (Val != DIModuleCache.end())
      return cast<llvm::DIModule>(Val->second);

    std::string RemappedIncludePath = DebugPrefixMap.remapPath(IncludePath);
    std::string RemappedASTFile = DebugPrefixMap.remapPath(ASTFile);

    // For Clang modules / PCH, create a Skeleton CU pointing to the PCM/PCH.
    if (!Opts.DisableClangModuleSkeletonCUs) {
      bool CreateSkeletonCU = !ASTFile.empty();
      bool IsRootModule = !Parent;
      if (CreateSkeletonCU && IsRootModule) {
        llvm::DIBuilder DIB(M);
        DIB.createCompileUnit(IGM.ObjCInterop ? llvm::dwarf::DW_LANG_ObjC
                                              : llvm::dwarf::DW_LANG_C99,
                              DIB.createFile(Name, RemappedIncludePath),
                              TheCU->getProducer(), true, StringRef(), 0,
                              RemappedASTFile, llvm::DICompileUnit::FullDebug,
                              Signature);
        DIB.finalize();
      }
    }

    llvm::DIModule *M =
        DBuilder.createModule(Parent, Name, ConfigMacros, RemappedIncludePath);
    DIModuleCache.insert({Key, llvm::TrackingMDNodeRef(M)});
    return M;
  }

  using ASTSourceDescriptor = clang::ASTSourceDescriptor;
  /// Create a DIModule from a clang module or PCH.
  /// The clang::Module pointer is passed separately because the recursive case
  /// needs to fudge the AST descriptor.
  llvm::DIModule *getOrCreateModule(ASTSourceDescriptor Desc,
                                    const clang::Module *ClangModule) {
    // PCH files don't have a signature field in the control block,
    // but LLVM detects skeleton CUs by looking for a non-zero DWO id.
    // We use the lower 64 bits for debug info.
    uint64_t Signature =
      Desc.getSignature() ? Desc.getSignature().truncatedValue() : ~1ULL;

    // Handle Clang modules.
    if (ClangModule) {
      llvm::DIModule *Parent = nullptr;
      if (ClangModule->Parent) {
        // The loading of additional modules by Sema may trigger an out-of-date
        // PCM rebuild in the Clang module dependencies of the additional
        // module. A PCM rebuild causes the ModuleManager to unload previously
        // loaded ASTFiles. For this reason we must use the cached ASTFile
        // information here instead of the potentially dangling pointer to the
        // ASTFile that is stored in the clang::Module object.
        //
        // Note: The implementation here assumes that all clang submodules
        //       belong to the same PCM file.
        ASTSourceDescriptor ParentDescriptor(*ClangModule->Parent);
        Parent = getOrCreateModule({ParentDescriptor.getModuleName(),
                                    ParentDescriptor.getPath(),
                                    Desc.getASTFile(), Desc.getSignature()},
                                   ClangModule->Parent);
      }
      return getOrCreateModule(ClangModule, Parent, Desc.getModuleName(),
                               Desc.getPath(), Signature, Desc.getASTFile());
    }
    // Handle PCH.
    return getOrCreateModule(Desc.getASTFile().bytes_begin(), nullptr,
                             Desc.getModuleName(), Desc.getPath(), Signature,
                             Desc.getASTFile());
  };

  static llvm::Optional<ASTSourceDescriptor>
  getClangModule(const ModuleDecl &M) {
    for (auto *FU : M.getFiles())
      if (auto *CMU = dyn_cast_or_null<ClangModuleUnit>(FU))
        if (auto Desc = CMU->getASTSourceDescriptor())
          return Desc;
    return llvm::None;
  }

  llvm::DIModule *getOrCreateModule(ImportedModule IM) {
    ModuleDecl *M = IM.importedModule;
    if (llvm::Optional<ASTSourceDescriptor> ModuleDesc = getClangModule(*M))
      return getOrCreateModule(*ModuleDesc, ModuleDesc->getModuleOrNull());
    StringRef Path = getFilenameFromDC(M);
    // Use the module 'real' name, which can be different from the name if module
    // aliasing was used (swift modules only). For example, if a source file has
    // 'import Foo', and '-module-alias Foo=Bar' was passed in, the real name of
    // the module on disk is Bar (.swiftmodule or .swiftinterface), and is used
    // for loading and mangling.
    StringRef Name = M->getRealName().str();
    return getOrCreateModule(M, TheCU, Name, Path);
  }

  TypeAliasDecl *getMetadataType(StringRef ArchetypeName) {
    TypeAliasDecl *&Entry = MetadataTypeDeclCache[ArchetypeName];
    if (Entry)
      return Entry;

    SourceLoc NoLoc;
    Entry = new (IGM.Context) TypeAliasDecl(
        NoLoc, NoLoc, IGM.Context.getIdentifier(ArchetypeName), NoLoc,
        /*genericparams*/ nullptr, IGM.Context.TheBuiltinModule);
    Entry->setUnderlyingType(IGM.Context.TheRawPointerType);
    return Entry;
  }

  /// Return the DIFile that is the ancestor of Scope.
  llvm::DIFile *getFile(llvm::DIScope *Scope) {
    while (!isa<llvm::DIFile>(Scope)) {
      switch (Scope->getTag()) {
      case llvm::dwarf::DW_TAG_lexical_block:
        Scope = cast<llvm::DILexicalBlock>(Scope)->getScope();
        break;
      case llvm::dwarf::DW_TAG_subprogram:
        Scope = cast<llvm::DISubprogram>(Scope)->getFile();
        break;
      default:
        return MainFile;
      }
      if (Scope)
        return MainFile;
    }
    return cast<llvm::DIFile>(Scope);
  }

  static unsigned getStorageSizeInBits(const llvm::DataLayout &DL,
                                       ArrayRef<llvm::Value *> Storage) {
    unsigned SizeInBits = 0;
    for (llvm::Value *Piece : Storage)
      SizeInBits += DL.getTypeSizeInBits(Piece->getType());
    return SizeInBits;
  }

  StringRef getMangledName(DebugTypeInfo DbgTy) {
    if (DbgTy.isMetadataType())
      return MetadataTypeDeclCache.find(DbgTy.getDecl()->getName().str())
          ->getKey();

    // This is a bit of a hack. We need a generic signature to use for mangling.
    // If we started with an interface type, just use IGM.getCurGenericContext(),
    // since callers that use interface types typically push a signature that way.
    //
    // Otherwise, if we have a contextual type, find an archetype and ask it for
    // it's generic signature. The context generic signature from the IRGenModule
    // is unlikely to be useful here.
    GenericSignature Sig;
    Type Ty = DbgTy.getType();
    if (Ty->hasArchetype()) {
      Ty.findIf([&](Type t) -> bool {
        if (auto *archetypeTy = t->getAs<PrimaryArchetypeType>()) {
          Sig = archetypeTy->getGenericEnvironment()->getGenericSignature();
          return true;
        }

        if (auto *archetypeTy = t->getAs<PackArchetypeType>()) {
          Sig = archetypeTy->getGenericEnvironment()->getGenericSignature();
          return true;
        }

        return false;
      });

      Ty = Ty->mapTypeOutOfContext();
    } else {
      Sig = IGM.getCurGenericContext();
    }

    // Strip off top level of type sugar (except for type aliases).
    // We don't want Optional<T> and T? to get different debug types.
    while (true) {
      if (auto *ParenTy = dyn_cast<ParenType>(Ty.getPointer())) {
        Ty = ParenTy->getUnderlyingType();
        continue;
      }

      if (auto *SugarTy = dyn_cast<SyntaxSugarType>(Ty.getPointer())) {
        Ty = SugarTy->getSinglyDesugaredType();
        continue;
      }

      break;
    }

    // TODO: Eliminate substitutions in SILFunctionTypes for now.
    // On platforms where the substitutions affect representation, we will need
    // to preserve this info and teach type reconstruction about it.
    Ty = Ty->replaceSubstitutedSILFunctionTypesWithUnsubstituted(
        IGM.getSILModule());

    Mangle::ASTMangler Mangler;
    std::string Result = Mangler.mangleTypeForDebugger(Ty, Sig);

    // TODO(https://github.com/apple/swift/issues/57699): We currently cannot round trip some C++ types.
    if (!Opts.DisableRoundTripDebugTypes &&
        !Ty->getASTContext().LangOpts.EnableCXXInterop) {
      // Make sure we can reconstruct mangled types for the debugger.
      auto &Ctx = Ty->getASTContext();
      Type Reconstructed = Demangle::getTypeForMangling(Ctx, Result, Sig);
      if (!Reconstructed) {
        llvm::errs() << "Failed to reconstruct type for " << Result << "\n";
        llvm::errs() << "Original type:\n";
        Ty->dump(llvm::errs());
        if (Sig)
          llvm::errs() << "Generic signature: " << Sig << "\n";
        llvm::errs() << SWIFT_CRASH_BUG_REPORT_MESSAGE << "\n"
          << "Pass '-Xfrontend -disable-round-trip-debug-types' to disable "
             "this assertion.\n";
        abort();
      } else if (!Reconstructed->isEqual(Ty) &&
                 // FIXME: Some existential types are reconstructed without
                 // an explicit ExistentialType wrapping the constraint.
                 !equalWithoutExistentialTypes(Reconstructed, Ty) &&
                 !EqualUpToClangTypes().check(Reconstructed, Ty)) {
        // [FIXME: Include-Clang-type-in-mangling] Remove second check
        llvm::errs() << "Incorrect reconstructed type for " << Result << "\n";
        llvm::errs() << "Original type:\n";
        Ty->dump(llvm::errs());
        llvm::errs() << "Reconstructed type:\n";
        Reconstructed->dump(llvm::errs());
        if (Sig)
          llvm::errs() << "Generic signature: " << Sig << "\n";
        llvm::errs() << SWIFT_CRASH_BUG_REPORT_MESSAGE << "\n"
          << "Pass '-Xfrontend -disable-round-trip-debug-types' to disable "
             "this assertion.\n";
        abort();
      }
    }

    return BumpAllocatedString(Result);
  }

  llvm::DIDerivedType *createMemberType(CompletedDebugTypeInfo DbgTy,
                                        StringRef Name, unsigned &OffsetInBits,
                                        llvm::DIScope *Scope,
                                        llvm::DIFile *File,
                                        llvm::DINode::DIFlags Flags) {
    unsigned SizeOfByte = CI.getTargetInfo().getCharWidth();
    auto *Ty = getOrCreateType(DbgTy);
    auto *DITy =
        DBuilder.createMemberType(Scope, Name, File, 0, DbgTy.getSizeInBits(),
                                  0, OffsetInBits, Flags, Ty);
    OffsetInBits += getSizeInBits(Ty);
    OffsetInBits = llvm::alignTo(OffsetInBits,
                                 SizeOfByte * DbgTy.getAlignment().getValue());
    return DITy;
  }

  llvm::DICompositeType *
  createStructType(DebugTypeInfo DbgTy, NominalTypeDecl *Decl, Type BaseTy,
                   llvm::DIScope *Scope, llvm::DIFile *File, unsigned Line,
                   unsigned SizeInBits, unsigned AlignInBits,
                   llvm::DINode::DIFlags Flags, llvm::DIType *DerivedFrom,
                   unsigned RuntimeLang, StringRef UniqueID) {
    StringRef Name = Decl->getName().str();

    // Forward declare this first because types may be recursive.
    auto FwdDecl = llvm::TempDIType(DBuilder.createReplaceableCompositeType(
        llvm::dwarf::DW_TAG_structure_type, Name, Scope, File, Line,
        llvm::dwarf::DW_LANG_Swift, SizeInBits, 0, Flags, UniqueID));

#ifndef NDEBUG
    if (UniqueID.empty())
      assert(!Name.empty() &&
             "no mangled name and no human readable name given");
    else
      assert((UniqueID.startswith("_T") ||
              UniqueID.startswith(MANGLING_PREFIX_STR)) &&
             "UID is not a mangled name");
#endif

    auto TH = llvm::TrackingMDNodeRef(FwdDecl.get());
    DITypeCache[DbgTy.getType()] = TH;
    // Collect the members.
    SmallVector<llvm::Metadata *, 16> Elements;
    unsigned OffsetInBits = 0;
    for (VarDecl *VD : Decl->getStoredProperties()) {
      auto memberTy = BaseTy->getTypeOfMember(IGM.getSwiftModule(), VD);

      if (auto DbgTy = CompletedDebugTypeInfo::getFromTypeInfo(
              VD->getInterfaceType(),
              IGM.getTypeInfoForUnlowered(
                  IGM.getSILTypes().getAbstractionPattern(VD), memberTy),
              IGM))
        Elements.push_back(createMemberType(*DbgTy, VD->getName().str(),
                                            OffsetInBits, Scope, File, Flags));
      else
        // Without complete type info we can only create a forward decl.
        return DBuilder.createForwardDecl(
            llvm::dwarf::DW_TAG_structure_type, UniqueID, Scope, File, Line,
            llvm::dwarf::DW_LANG_Swift, SizeInBits, 0);
    }
    if (OffsetInBits > SizeInBits)
      SizeInBits = OffsetInBits;

    auto DITy = DBuilder.createStructType(
        Scope, Name, File, Line, SizeInBits, AlignInBits, Flags, DerivedFrom,
        DBuilder.getOrCreateArray(Elements), RuntimeLang, nullptr, UniqueID);
    DBuilder.replaceTemporary(std::move(FwdDecl), DITy);
    return DITy;
  }

  llvm::DICompositeType *createEnumType(CompletedDebugTypeInfo DbgTy,
                                        EnumDecl *Decl, StringRef MangledName,
                                        llvm::DIScope *Scope,
                                        llvm::DIFile *File, unsigned Line,
                                        llvm::DINode::DIFlags Flags) {
    StringRef Name = Decl->getName().str();
    unsigned SizeInBits = DbgTy.getSizeInBits();
    // Default, since Swift doesn't allow specifying a custom alignment.
    unsigned AlignInBits = 0;

    // FIXME: Is DW_TAG_union_type the right thing here?
    // Consider using a DW_TAG_variant_type instead.
    auto FwdDecl = llvm::TempDIType(DBuilder.createReplaceableCompositeType(
        llvm::dwarf::DW_TAG_union_type, MangledName, Scope, File, Line,
        llvm::dwarf::DW_LANG_Swift, SizeInBits, AlignInBits, Flags,
        MangledName));

    auto TH = llvm::TrackingMDNodeRef(FwdDecl.get());
    DITypeCache[DbgTy.getType()] = TH;

    SmallVector<llvm::Metadata *, 16> Elements;

    for (auto *ElemDecl : Decl->getAllElements()) {
      // FIXME <rdar://problem/14845818> Support enums.
      // Swift Enums can be both like DWARF enums and discriminated unions.
      // LLVM now supports variant types in debug metadata, which may be a
      // better fit.
      llvm::Optional<CompletedDebugTypeInfo> ElemDbgTy;
      if (Decl->hasRawType()) {
        // An enum with a raw type (enum E : Int {}), similar to a
        // DWARF enum.
        //
        // The storage occupied by the enum may be smaller than the
        // one of the raw type as long as it is large enough to hold
        // all enum values. Use the raw type for the debug type, but
        // the storage size from the enum.
        auto RawType = Decl->getRawType();
        auto &TI = IGM.getTypeInfoForUnlowered(RawType);
        ElemDbgTy = CompletedDebugTypeInfo::getFromTypeInfo(RawType, TI, IGM);
      } else if (auto ArgTy = ElemDecl->getArgumentInterfaceType()) {
        // A discriminated union. This should really be described as a
        // DW_TAG_variant_type. For now only describing the data.
        ArgTy = ElemDecl->getParentEnum()->mapTypeIntoContext(ArgTy);
        auto &TI = IGM.getTypeInfoForUnlowered(ArgTy);
        ElemDbgTy = CompletedDebugTypeInfo::getFromTypeInfo(ArgTy, TI, IGM);
      } else {
        // Discriminated union case without argument. Fallback to Int
        // as the element type; there is no storage here.
        Type IntTy = IGM.Context.getIntType();
        auto &TI = IGM.getTypeInfoForUnlowered(IntTy);
        ElemDbgTy = CompletedDebugTypeInfo::getFromTypeInfo(IntTy, TI, IGM);
      }
      if (!ElemDbgTy) {
        // Without complete type info we can only create a forward decl.
        return DBuilder.createForwardDecl(
            llvm::dwarf::DW_TAG_union_type, Name, Scope, File, Line,
            llvm::dwarf::DW_LANG_Swift, SizeInBits, 0, MangledName);
      }
      unsigned Offset = 0;
      auto MTy =
          createMemberType(*ElemDbgTy, ElemDecl->getBaseIdentifier().str(),
                           Offset, Scope, File, Flags);
      Elements.push_back(MTy);
    }
    auto DITy = DBuilder.createUnionType(
        Scope, Name, File, Line, SizeInBits, AlignInBits,
        Flags, DBuilder.getOrCreateArray(Elements),
        llvm::dwarf::DW_LANG_Swift, MangledName);

    DBuilder.replaceTemporary(std::move(FwdDecl), DITy);
    return DITy;
  }

  llvm::DIType *getOrCreateDesugaredType(Type Ty, DebugTypeInfo DbgTy) {
    DebugTypeInfo BlandDbgTy(Ty, DbgTy.getFragmentStorageType(),
                             DbgTy.getRawSizeInBits(), DbgTy.getAlignment(),
                             DbgTy.hasDefaultAlignment(),
                             DbgTy.isMetadataType(), DbgTy.isSizeFragmentSize(),
                             DbgTy.isFixedBuffer());
    return getOrCreateType(BlandDbgTy);
  }

  uint64_t getSizeOfBasicType(CompletedDebugTypeInfo DbgTy) {
    uint64_t BitWidth = DbgTy.getSizeInBits();
    llvm::Type *StorageType = DbgTy.getFragmentStorageType()
                                  ? DbgTy.getFragmentStorageType()
                                  : IGM.DataLayout.getSmallestLegalIntType(
                                        IGM.getLLVMContext(), BitWidth);

    if (StorageType)
      return IGM.DataLayout.getTypeSizeInBits(StorageType);

    // This type is too large to fit in a register.
    assert(BitWidth > IGM.DataLayout.getLargestLegalIntTypeSizeInBits());
    return BitWidth;
  }

  /// Collect the type parameters of a bound generic type. This is needed to
  /// anchor any typedefs that may appear in parameters so they can be
  /// resolved in the debugger without needing to query the Swift module.
  llvm::DINodeArray collectGenericParams(BoundGenericType *BGT) {
    SmallVector<llvm::Metadata *, 16> TemplateParams;
    for (auto Param : BGT->getGenericArgs()) {
      TemplateParams.push_back(DBuilder.createTemplateTypeParameter(
          TheCU, "", getOrCreateType(DebugTypeInfo::getForwardDecl(Param)),
          false));
    }
    return DBuilder.getOrCreateArray(TemplateParams);
  }

  /// Create a sized container for a sizeless type. Used to represent
  /// BoundGenericEnums that may have different sizes depending on what they are
  /// bound to, but still share a mangled name.
  llvm::DIType *createOpaqueStructWithSizedContainer(
      llvm::DIScope *Scope, StringRef Name, llvm::DIFile *File, unsigned Line,
      unsigned SizeInBits, unsigned AlignInBits, llvm::DINode::DIFlags Flags,
      StringRef MangledName, llvm::DINodeArray BoundParams) {
    // This uses a separate cache and not DIRefMap for the inner type to avoid
    // associating the anonymous container (which is specific to the
    // variable/storage and not the type) with the MangledName.
    llvm::DICompositeType *UniqueType = nullptr;
    auto *UID = llvm::MDString::get(IGM.getLLVMContext(), MangledName);
    if (llvm::Metadata *V = InnerTypeCache.lookup(UID))
      UniqueType = cast<llvm::DICompositeType>(V);
    else {
      UniqueType = DBuilder.createForwardDecl(
          llvm::dwarf::DW_TAG_structure_type, MangledName, Scope, File, Line,
          llvm::dwarf::DW_LANG_Swift, 0, 0);
      if (BoundParams)
        DBuilder.replaceArrays(UniqueType, nullptr, BoundParams);
      InnerTypeCache[UID] = llvm::TrackingMDNodeRef(UniqueType);
    }

    llvm::Metadata *Elements[] = {DBuilder.createMemberType(
        Scope, "", File, 0, SizeInBits, AlignInBits, 0, Flags, UniqueType)};
    return DBuilder.createStructType(
        Scope, "", File, Line, SizeInBits, AlignInBits, Flags,
        /* DerivedFrom */ nullptr, DBuilder.getOrCreateArray(Elements),
        llvm::dwarf::DW_LANG_Swift);
  }

  llvm::DIType *createPointerSizedStruct(llvm::DIScope *Scope, StringRef Name,
                                         llvm::DIFile *File, unsigned Line,
                                         llvm::DINode::DIFlags Flags,
                                         StringRef MangledName) {
    if (Opts.DebugInfoLevel > IRGenDebugInfoLevel::ASTTypes) {
      auto FwdDecl = DBuilder.createForwardDecl(
          llvm::dwarf::DW_TAG_structure_type, Name, Scope, File, Line,
          llvm::dwarf::DW_LANG_Swift, 0, 0);
      return createPointerSizedStruct(Scope, Name, FwdDecl, File, Line, Flags,
                                      MangledName);
    } else {
      unsigned SizeInBits = CI.getTargetInfo().getPointerWidth(0);
      return createOpaqueStruct(Scope, Name, File, Line, SizeInBits, 0, Flags,
                                MangledName);
    }
  }

  llvm::DIType *createPointerSizedStruct(llvm::DIScope *Scope, StringRef Name,
                                         llvm::DIType *PointeeTy,
                                         llvm::DIFile *File, unsigned Line,
                                         llvm::DINode::DIFlags Flags,
                                         StringRef MangledName) {
    unsigned PtrSize = CI.getTargetInfo().getPointerWidth(0);
    auto PtrTy = DBuilder.createPointerType(PointeeTy, PtrSize, 0);
    llvm::Metadata *Elements[] = {DBuilder.createMemberType(
        Scope, "ptr", File, 0, PtrSize, 0, 0, Flags, PtrTy)};
    return DBuilder.createStructType(
        Scope, Name, File, Line, PtrSize, 0, Flags,
        /* DerivedFrom */ nullptr, DBuilder.getOrCreateArray(Elements),
        llvm::dwarf::DW_LANG_Swift, nullptr, MangledName);
  }

  llvm::DIType *
  createDoublePointerSizedStruct(llvm::DIScope *Scope, StringRef Name,
                                 llvm::DIType *PointeeTy, llvm::DIFile *File,
                                 unsigned Line, llvm::DINode::DIFlags Flags,
                                 StringRef MangledName) {
    unsigned PtrSize = CI.getTargetInfo().getPointerWidth(0);
    llvm::Metadata *Elements[] = {
        DBuilder.createMemberType(
            Scope, "ptr", File, 0, PtrSize, 0, 0, Flags,
            DBuilder.createPointerType(PointeeTy, PtrSize, 0)),
        DBuilder.createMemberType(
            Scope, "_", File, 0, PtrSize, 0, 0, Flags,
            DBuilder.createPointerType(nullptr, PtrSize, 0))};
    return DBuilder.createStructType(
        Scope, Name, File, Line, 2 * PtrSize, 0, Flags,
        /* DerivedFrom */ nullptr, DBuilder.getOrCreateArray(Elements),
        llvm::dwarf::DW_LANG_Swift, nullptr, MangledName);
  }

  llvm::DIType *createFixedValueBufferStruct(llvm::DIType *PointeeTy) {
    unsigned Line = 0;
    unsigned PtrSize = CI.getTargetInfo().getPointerWidth(0);
    llvm::DINode::DIFlags Flags = llvm::DINode::FlagArtificial;
    llvm::DIFile *File = MainFile;
    llvm::DIScope *Scope = TheCU;
    llvm::Metadata *Elements[] = {DBuilder.createMemberType(
        Scope, "contents", File, 0, PtrSize, 0, 0, Flags, PointeeTy)};
    return DBuilder.createStructType(
        Scope, "$swift.fixedbuffer", File, Line, 3 * PtrSize, 0, Flags,
        /* DerivedFrom */ nullptr, DBuilder.getOrCreateArray(Elements),
        llvm::dwarf::DW_LANG_Swift, nullptr);
  }

  llvm::DIType *createFunctionPointer(DebugTypeInfo DbgTy, llvm::DIScope *Scope,
                                      unsigned SizeInBits, unsigned AlignInBits,
                                      llvm::DINode::DIFlags Flags,
                                      StringRef MangledName) {
    auto FwdDecl = llvm::TempDINode(DBuilder.createReplaceableCompositeType(
        llvm::dwarf::DW_TAG_subroutine_type, MangledName, Scope, MainFile, 0,
        llvm::dwarf::DW_LANG_Swift, SizeInBits, AlignInBits, Flags,
        MangledName));

    auto TH = llvm::TrackingMDNodeRef(FwdDecl.get());
    DITypeCache[DbgTy.getType()] = TH;

    CanSILFunctionType FunTy;
    TypeBase *BaseTy = DbgTy.getType();
    if (auto *SILFnTy = dyn_cast<SILFunctionType>(BaseTy))
      FunTy = CanSILFunctionType(SILFnTy);
    // FIXME: Handling of generic parameters in SIL type lowering is in flux.
    // DebugInfo doesn't appear to care about the generic context, so just
    // throw it away before lowering.
    else if (isa<GenericFunctionType>(BaseTy)) {
      auto *fTy = cast<AnyFunctionType>(BaseTy);
      auto *nongenericTy = FunctionType::get(fTy->getParams(), fTy->getResult(),
                                             fTy->getExtInfo());

      FunTy = IGM.getLoweredType(nongenericTy).castTo<SILFunctionType>();
    } else
      FunTy = IGM.getLoweredType(BaseTy).castTo<SILFunctionType>();
    auto Params = createParameterTypes(FunTy);

    auto FnTy = DBuilder.createSubroutineType(Params, Flags);
    llvm::DIType *DITy;
    if (FunTy->getRepresentation() == SILFunctionType::Representation::Thick) {
      if (SizeInBits == 2 * CI.getTargetInfo().getPointerWidth(0))
        // This is a FunctionPairTy: { i8*, %swift.refcounted* }.
        DITy = createDoublePointerSizedStruct(Scope, MangledName, FnTy,
                                              MainFile, 0, Flags, MangledName);
      else
        // This is a generic function as noted above.
        DITy = createOpaqueStruct(Scope, MangledName, MainFile, 0, SizeInBits,
                                  AlignInBits, Flags, MangledName);
    } else {
      assert(SizeInBits == CI.getTargetInfo().getPointerWidth(0));
      DITy = createPointerSizedStruct(Scope, MangledName, FnTy, MainFile, 0,
                                      Flags, MangledName);
    }
    DBuilder.replaceTemporary(std::move(FwdDecl), DITy);
    return DITy;
  }

  llvm::DIType *createTuple(DebugTypeInfo DbgTy, llvm::DIScope *Scope,
                            unsigned SizeInBits, unsigned AlignInBits,
                            llvm::DINode::DIFlags Flags,
                            StringRef MangledName) {
    TypeBase *BaseTy = DbgTy.getType();
    auto *TupleTy = BaseTy->castTo<TupleType>();

    SmallVector<llvm::Metadata *, 16> Elements;
    unsigned OffsetInBits = 0;
    auto genericSig = IGM.getCurGenericContext();
    for (auto ElemTy : TupleTy->getElementTypes()) {
      auto &elemTI = IGM.getTypeInfoForUnlowered(
          AbstractionPattern(genericSig, ElemTy->getCanonicalType()), ElemTy);
      if (auto DbgTy =
              CompletedDebugTypeInfo::getFromTypeInfo(ElemTy, elemTI, IGM))
        Elements.push_back(
            createMemberType(*DbgTy, "", OffsetInBits, Scope, MainFile, Flags));
      else
        // We can only create a forward declaration without complete size info.
        return DBuilder.createReplaceableCompositeType(
            llvm::dwarf::DW_TAG_structure_type, MangledName, Scope, MainFile, 0,
            llvm::dwarf::DW_LANG_Swift, 0, AlignInBits, Flags, MangledName);
    }
    // FIXME: assert that SizeInBits == OffsetInBits.
    SizeInBits = OffsetInBits;
   
    auto FwdDecl = llvm::TempDINode(DBuilder.createReplaceableCompositeType(
        llvm::dwarf::DW_TAG_structure_type, MangledName, Scope, MainFile, 0,
        llvm::dwarf::DW_LANG_Swift, SizeInBits, AlignInBits, Flags,
        MangledName));

    DITypeCache[DbgTy.getType()] = llvm::TrackingMDNodeRef(FwdDecl.get());

    auto DITy = DBuilder.createStructType(
        Scope, MangledName, MainFile, 0, SizeInBits, AlignInBits, Flags,
        nullptr, // DerivedFrom
        DBuilder.getOrCreateArray(Elements), llvm::dwarf::DW_LANG_Swift,
        nullptr, MangledName);

    DBuilder.replaceTemporary(std::move(FwdDecl), DITy);
    return DITy;
  }

  llvm::DICompositeType *
  createOpaqueStruct(llvm::DIScope *Scope, StringRef Name, llvm::DIFile *File,
                     unsigned Line, unsigned SizeInBits, unsigned AlignInBits,
                     llvm::DINode::DIFlags Flags, StringRef MangledName) {
    return DBuilder.createStructType(
        Scope, Name, File, Line, SizeInBits, AlignInBits, Flags,
        /* DerivedFrom */ nullptr,
        DBuilder.getOrCreateArray(ArrayRef<llvm::Metadata *>()),
        llvm::dwarf::DW_LANG_Swift, nullptr, MangledName);
  }

  llvm::DIType *createType(DebugTypeInfo DbgTy, StringRef MangledName,
                           llvm::DIScope *Scope, llvm::DIFile *File) {
    // FIXME: For SizeInBits, clang uses the actual size of the type on
    // the target machine instead of the storage size that is alloca'd
    // in the LLVM IR. For all types that are boxed in a struct, we are
    // emitting the storage size of the struct, but it may be necessary
    // to emit the (target!) size of the underlying basic type.
    uint64_t SizeOfByte = CI.getTargetInfo().getCharWidth();
    // FIXME: SizeInBits is redundant with DbgTy, remove it.
    uint64_t SizeInBits = 0;
    if (DbgTy.getTypeSizeInBits())
      SizeInBits = *DbgTy.getTypeSizeInBits();
    unsigned AlignInBits = DbgTy.hasDefaultAlignment()
                               ? 0
                               : DbgTy.getAlignment().getValue() * SizeOfByte;
    unsigned Encoding = 0;
    llvm::DINode::DIFlags Flags = llvm::DINode::FlagZero;

    TypeBase *BaseTy = DbgTy.getType();
    if (!BaseTy) {
      LLVM_DEBUG(llvm::dbgs() << "Type without TypeBase: ";
                 DbgTy.getType()->dump(llvm::dbgs()); llvm::dbgs() << "\n");
      if (!InternalType) {
        StringRef Name = "<internal>";
        InternalType = DBuilder.createForwardDecl(
            llvm::dwarf::DW_TAG_structure_type, Name, Scope, File,
            /*Line*/ 0, llvm::dwarf::DW_LANG_Swift, SizeInBits, AlignInBits,
            MangledName);
      }
      return InternalType;
    }

    // Here goes!
    switch (BaseTy->getKind()) {
    case TypeKind::BuiltinPackIndex:
    case TypeKind::BuiltinInteger: {
      Encoding = llvm::dwarf::DW_ATE_unsigned;
      if (auto CompletedDbgTy = CompletedDebugTypeInfo::get(DbgTy))
        SizeInBits = getSizeOfBasicType(*CompletedDbgTy);
      break;
    }

    case TypeKind::BuiltinIntegerLiteral: {
      Encoding = llvm::dwarf::DW_ATE_unsigned; // ?
      if (auto CompletedDbgTy = CompletedDebugTypeInfo::get(DbgTy))
        SizeInBits = getSizeOfBasicType(*CompletedDbgTy);
      break;
    }

    case TypeKind::BuiltinFloat: {
      auto *FloatTy = BaseTy->castTo<BuiltinFloatType>();
      // Assuming that the bitwidth and FloatTy->getFPKind() are identical.
      SizeInBits = FloatTy->getBitWidth();
      Encoding = llvm::dwarf::DW_ATE_float;
      break;
    }

    case TypeKind::BuiltinNativeObject: {
      unsigned PtrSize = CI.getTargetInfo().getPointerWidth(0);
      auto PTy = DBuilder.createPointerType(nullptr, PtrSize, 0,
                                            /* DWARFAddressSpace */ llvm::None,
                                            MangledName);
      return DBuilder.createObjectPointerType(PTy);
    }

    case TypeKind::BuiltinBridgeObject: {
      unsigned PtrSize = CI.getTargetInfo().getPointerWidth(0);
      auto PTy = DBuilder.createPointerType(nullptr, PtrSize, 0,
                                            /* DWARFAddressSpace */ llvm::None,
                                            MangledName);
      return DBuilder.createObjectPointerType(PTy);
    }

    case TypeKind::BuiltinRawPointer: {
      unsigned PtrSize = CI.getTargetInfo().getPointerWidth(0);
      return DBuilder.createPointerType(nullptr, PtrSize, 0,
                                        /* DWARFAddressSpace */ llvm::None,
                                        MangledName);
    }

    case TypeKind::BuiltinRawUnsafeContinuation: {
      unsigned PtrSize = CI.getTargetInfo().getPointerWidth(0);
      return DBuilder.createPointerType(nullptr, PtrSize, 0,
                                        /* DWARFAddressSpace */ llvm::None,
                                        MangledName);
    }

    case TypeKind::BuiltinJob: {
      unsigned PtrSize = CI.getTargetInfo().getPointerWidth(0);
      return DBuilder.createPointerType(nullptr, PtrSize, 0,
                                        /* DWARFAddressSpace */ llvm::None,
                                        MangledName);
    }

    case TypeKind::BuiltinExecutor: {
      return createDoublePointerSizedStruct(
          Scope, "Builtin.Executor", nullptr, MainFile, 0,
          llvm::DINode::FlagArtificial, MangledName);
    }

    case TypeKind::DynamicSelf: {
      // Self. We don't have a way to represent instancetype in DWARF,
      // so we emit the static type instead. This is similar to what we
      // do with instancetype in Objective-C.
      auto *DynamicSelfTy = BaseTy->castTo<DynamicSelfType>();
      auto SelfTy =
          getOrCreateDesugaredType(DynamicSelfTy->getSelfType(), DbgTy);
      return DBuilder.createTypedef(SelfTy, MangledName, File, 0, File);
    }

    // Even builtin swift types usually come boxed in a struct.
    case TypeKind::Struct: {
      auto *StructTy = BaseTy->castTo<StructType>();
      auto *Decl = StructTy->getDecl();
      auto L = getFilenameAndLocation(*this, Decl);
      auto *File = getOrCreateFile(L.filename);
      // No line numbers are attached to type forward declarations.  This is
      // intentional: It interferes with the efficacy of incremental builds. We
      // don't want a whitespace change to an secondary file trigger a
      // recompilation of the debug info of a primary source file.
      unsigned FwdDeclLine = 0;
      if (Opts.DebugInfoLevel > IRGenDebugInfoLevel::ASTTypes)
        return createStructType(DbgTy, Decl, StructTy, Scope, File, L.line,
                                SizeInBits, AlignInBits, Flags, nullptr,
                                llvm::dwarf::DW_LANG_Swift, MangledName);
      StringRef Name = Decl->getName().str();
      if (!DbgTy.getTypeSizeInBits())
        return DBuilder.createForwardDecl(
            llvm::dwarf::DW_TAG_structure_type, MangledName, Scope, File,
            FwdDeclLine, llvm::dwarf::DW_LANG_Swift, 0, AlignInBits);
      if (DbgTy.isFixedBuffer())
        return DBuilder.createForwardDecl(
            llvm::dwarf::DW_TAG_structure_type, MangledName, Scope, File,
            FwdDeclLine, llvm::dwarf::DW_LANG_Swift, 0, AlignInBits);
      return createOpaqueStruct(Scope, Name, File, FwdDeclLine, SizeInBits,
                                AlignInBits, Flags, MangledName);
    }

    case TypeKind::Class: {
      // Classes are represented as DW_TAG_structure_type. This way the
      // DW_AT_APPLE_runtime_class(DW_LANG_Swift) attribute can be
      // used to differentiate them from C++ and ObjC classes.
      auto *ClassTy = BaseTy->castTo<ClassType>();
      auto *Decl = ClassTy->getDecl();
      auto L = getFilenameAndLocation(*this, Decl);
      auto *File = getOrCreateFile(L.filename);
      unsigned FwdDeclLine = 0;
      assert(SizeInBits == CI.getTargetInfo().getPointerWidth(0));
      return createPointerSizedStruct(Scope, Decl->getNameStr(), File,
                                      FwdDeclLine, Flags, MangledName);
    }

    case TypeKind::Protocol: {
      auto *ProtocolTy = BaseTy->castTo<ProtocolType>();
      auto *Decl = ProtocolTy->getDecl();
      // FIXME: (LLVM branch) This should probably be a DW_TAG_interface_type.
      auto L = getFilenameAndLocation(*this, Decl);
      auto *File = getOrCreateFile(L.filename);
      unsigned FwdDeclLine = 0;
      return createOpaqueStruct(Scope, Decl ? Decl->getNameStr() : MangledName,
                                File, FwdDeclLine, SizeInBits, AlignInBits,
                                Flags, MangledName);
    }

    case TypeKind::Existential:
    case TypeKind::ProtocolComposition:
    case TypeKind::ParameterizedProtocol: {
      auto *Decl = DbgTy.getDecl();
      auto L = getFilenameAndLocation(*this, Decl);
      auto *File = getOrCreateFile(L.filename);
      unsigned FwdDeclLine = 0;
      return createOpaqueStruct(Scope, Decl ? Decl->getNameStr() : MangledName,
                                File, FwdDeclLine, SizeInBits, AlignInBits,
                                Flags, MangledName);
    }

    case TypeKind::UnboundGeneric: {
      auto *UnboundTy = BaseTy->castTo<UnboundGenericType>();
      auto *Decl = UnboundTy->getDecl();
      auto L = getFilenameAndLocation(*this, Decl);
      auto *File = getOrCreateFile(L.filename);
      unsigned FwdDeclLine = 0;
      assert(SizeInBits == CI.getTargetInfo().getPointerWidth(0));
      return createPointerSizedStruct(Scope,
                                      Decl ? Decl->getNameStr() : MangledName,
                                      File, FwdDeclLine, Flags, MangledName);
    }

    case TypeKind::BoundGenericStruct: {
      auto *StructTy = BaseTy->castTo<BoundGenericStructType>();
      auto *Decl = StructTy->getDecl();
      auto L = getFilenameAndLocation(*this, Decl);
      auto *File = getOrCreateFile(L.filename);
      unsigned FwdDeclLine = 0;
      return createOpaqueStructWithSizedContainer(
          Scope, Decl ? Decl->getNameStr() : "", File, FwdDeclLine, SizeInBits,
          AlignInBits, Flags, MangledName, collectGenericParams(StructTy));
    }

    case TypeKind::BoundGenericClass: {
      auto *ClassTy = BaseTy->castTo<BoundGenericClassType>();
      auto *Decl = ClassTy->getDecl();
      auto L = getFilenameAndLocation(*this, Decl);
      auto *File = getOrCreateFile(L.filename);
      unsigned FwdDeclLine = 0;

      // TODO: We may want to peek at Decl->isObjC() and set this
      // attribute accordingly.
      assert(SizeInBits == CI.getTargetInfo().getPointerWidth(0));
      return createPointerSizedStruct(Scope,
                                      Decl ? Decl->getNameStr() : MangledName,
                                      File, FwdDeclLine, Flags, MangledName);
    }

    case TypeKind::Pack:
    case TypeKind::PackElement:
      llvm_unreachable("Unimplemented!");

    case TypeKind::SILPack:
    case TypeKind::PackExpansion:
      //assert(SizeInBits == CI.getTargetInfo().getPointerWidth(0));
      return createPointerSizedStruct(Scope,
                                      MangledName,
                                      MainFile, 0, Flags, MangledName);
      
    case TypeKind::BuiltinTuple:
      llvm_unreachable("BuiltinTupleType should not show up here");

    case TypeKind::Tuple: {
      // Tuples are also represented as structs.  Since tuples are ephemeral
      // (not nominal) they don't have a source location.
      if (Opts.DebugInfoLevel > IRGenDebugInfoLevel::ASTTypes)
        return createTuple(DbgTy, Scope, SizeInBits, AlignInBits, Flags,
                           MangledName);
      else
        return createOpaqueStruct(Scope, MangledName, MainFile, 0, SizeInBits,
                                  AlignInBits, Flags, MangledName);
    }

    case TypeKind::InOut:
      break;

    case TypeKind::OpaqueTypeArchetype:
    case TypeKind::PrimaryArchetype:
    case TypeKind::OpenedArchetype:
    case TypeKind::ElementArchetype:
    case TypeKind::PackArchetype: {
      auto *Archetype = BaseTy->castTo<ArchetypeType>();
      AssociatedTypeDecl *assocType = nullptr;
      if (auto depMemTy = Archetype->getInterfaceType()
              ->getAs<DependentMemberType>())
        assocType = depMemTy->getAssocType();
      auto L = getFilenameAndLocation(*this, assocType);
      auto *File = getOrCreateFile(L.filename);
      unsigned FwdDeclLine = 0;
      auto Superclass = Archetype->getSuperclass();
      auto DerivedFrom = Superclass.isNull()
                             ? nullptr
                             : getOrCreateDesugaredType(Superclass, DbgTy);
      auto FwdDecl = llvm::TempDIType(DBuilder.createReplaceableCompositeType(
          llvm::dwarf::DW_TAG_structure_type, MangledName, Scope, File,
          FwdDeclLine, llvm::dwarf::DW_LANG_Swift, SizeInBits, AlignInBits,
          Flags));

      // Emit the protocols the archetypes conform to.
      SmallVector<llvm::Metadata *, 4> Protocols;
      for (auto *ProtocolDecl : Archetype->getConformsTo()) {
        auto PTy =
            IGM.getLoweredType(ProtocolDecl->getInterfaceType()).getASTType();
        auto PDbgTy = DebugTypeInfo::getFromTypeInfo(
            ProtocolDecl->getInterfaceType(), IGM.getTypeInfoForLowered(PTy),
            IGM, false);
        auto PDITy = getOrCreateType(PDbgTy);
        Protocols.push_back(
            DBuilder.createInheritance(FwdDecl.get(), PDITy, 0, 0, Flags));
      }
      auto DITy = DBuilder.createStructType(
          Scope, MangledName, File, FwdDeclLine, SizeInBits, AlignInBits, Flags,
          DerivedFrom, DBuilder.getOrCreateArray(Protocols),
          llvm::dwarf::DW_LANG_Swift, nullptr);

      DBuilder.replaceTemporary(std::move(FwdDecl), DITy);
      return DITy;
    }

    case TypeKind::ExistentialMetatype:
    case TypeKind::Metatype: {
      // Metatypes are (mostly) singleton type descriptors, often without
      // storage.
      Flags |= llvm::DINode::FlagArtificial;
      auto L = getFilenameAndLocation(*this, DbgTy.getDecl());
      auto *File = getOrCreateFile(L.filename);
      unsigned FwdDeclLine = 0;

      return DBuilder.createStructType(
          Scope, MangledName, File, FwdDeclLine, SizeInBits, AlignInBits, Flags,
          nullptr, nullptr, llvm::dwarf::DW_LANG_Swift, nullptr, MangledName);
    }

    case TypeKind::SILFunction:
    case TypeKind::Function:
    case TypeKind::GenericFunction: {
      if (Opts.DebugInfoLevel > IRGenDebugInfoLevel::ASTTypes)
        return createFunctionPointer(DbgTy, Scope, SizeInBits, AlignInBits,
                                     Flags, MangledName);
      else
        return createOpaqueStruct(Scope, MangledName, MainFile, 0, SizeInBits,
                                  AlignInBits, Flags, MangledName);
    }

    case TypeKind::Enum: {
      auto *EnumTy = BaseTy->castTo<EnumType>();
      auto *Decl = EnumTy->getDecl();
      auto L = getFilenameAndLocation(*this, Decl);
      auto *File = getOrCreateFile(L.filename);
      unsigned FwdDeclLine = 0;
      if (Opts.DebugInfoLevel > IRGenDebugInfoLevel::ASTTypes)
        if (auto CompletedDbgTy = CompletedDebugTypeInfo::get(DbgTy))
          return createEnumType(*CompletedDbgTy, Decl, MangledName, Scope, File,
                                L.line, Flags);
      return createOpaqueStruct(Scope, Decl->getName().str(), File, FwdDeclLine,
                                SizeInBits, AlignInBits, Flags, MangledName);
    }

    case TypeKind::BoundGenericEnum: {
      auto *EnumTy = BaseTy->castTo<BoundGenericEnumType>();
      auto *Decl = EnumTy->getDecl();
      auto L = getFilenameAndLocation(*this, Decl);
      auto *File = getOrCreateFile(L.filename);
      unsigned FwdDeclLine = 0;

      return createOpaqueStructWithSizedContainer(
          Scope, Decl->getName().str(), File, FwdDeclLine, SizeInBits,
          AlignInBits, Flags, MangledName, collectGenericParams(EnumTy));
    }

    case TypeKind::BuiltinVector: {
      // FIXME: Emit the name somewhere.
      (void)MangledName;
      auto *BuiltinVectorTy = BaseTy->castTo<BuiltinVectorType>();
      auto ElemTy = BuiltinVectorTy->getElementType();
      auto ElemDbgTy = DebugTypeInfo::getFromTypeInfo(
          ElemTy, IGM.getTypeInfoForUnlowered(ElemTy), IGM, false);
      unsigned Count = BuiltinVectorTy->getNumElements();
      auto Subscript = DBuilder.getOrCreateSubrange(0, Count ? Count : -1);
      return DBuilder.createVectorType(SizeInBits, AlignInBits,
                                       getOrCreateType(ElemDbgTy),
                                       DBuilder.getOrCreateArray(Subscript));
    }

    // Reference storage types.
#define REF_STORAGE(Name, ...) case TypeKind::Name##Storage:
#include "swift/AST/ReferenceStorage.def"
      {
        auto *ReferenceTy = cast<ReferenceStorageType>(BaseTy);
        auto CanTy = ReferenceTy->getReferentType();
        auto L = getFilenameAndLocation(*this, DbgTy.getDecl());
        auto *File = getOrCreateFile(L.filename);
        unsigned CompilerGeneratedLine = 0;

        return DBuilder.createTypedef(getOrCreateDesugaredType(CanTy, DbgTy),
                                      MangledName, File, CompilerGeneratedLine,
                                      File);
      }

      // Sugared types.

    case TypeKind::TypeAlias: {
      auto *TypeAliasTy = cast<TypeAliasType>(BaseTy);
      auto *Decl = TypeAliasTy->getDecl();
      auto L = getFilenameAndLocation(*this, Decl);
      auto AliasedTy = TypeAliasTy->getSinglyDesugaredType();
      auto *File = getOrCreateFile(L.filename);

      // For TypeAlias types, the DeclContext for the aliased type is
      // in the decl of the alias type.
      DebugTypeInfo AliasedDbgTy(
          AliasedTy, DbgTy.getFragmentStorageType(), DbgTy.getRawSizeInBits(),
          DbgTy.getAlignment(), DbgTy.hasDefaultAlignment(), false,
          DbgTy.isSizeFragmentSize(), DbgTy.isFixedBuffer());
      return DBuilder.createTypedef(getOrCreateType(AliasedDbgTy), MangledName,
                                    File, 0, Scope);
    }

    case TypeKind::Paren: {
      auto Ty = cast<ParenType>(BaseTy)->getUnderlyingType();
      return getOrCreateDesugaredType(Ty, DbgTy);
    }

    // SyntaxSugarType derivations.
    case TypeKind::Dictionary:
    case TypeKind::ArraySlice:
    case TypeKind::Optional:
    case TypeKind::VariadicSequence: {
      auto *SyntaxSugarTy = cast<SyntaxSugarType>(BaseTy);
      auto *CanTy = SyntaxSugarTy->getSinglyDesugaredType();
      return getOrCreateDesugaredType(CanTy, DbgTy);
    }

    // SILBox should appear only inside of coroutine contexts.
    case TypeKind::SILBox:
    case TypeKind::DependentMember:
    case TypeKind::GenericTypeParam: {
      // FIXME: Provide a more meaningful debug type.
      return DBuilder.createStructType(
          Scope, MangledName, File, 0, SizeInBits, AlignInBits, Flags, nullptr,
          nullptr, llvm::dwarf::DW_LANG_Swift, nullptr, MangledName);
    }

    // The following types exist primarily for internal use by the type
    // checker.
    case TypeKind::Error:
    case TypeKind::Unresolved:
    case TypeKind::LValue:
    case TypeKind::TypeVariable:
    case TypeKind::Placeholder:
    case TypeKind::Module:
    case TypeKind::SILBlockStorage:
    case TypeKind::SILToken:
    case TypeKind::BuiltinUnsafeValueBuffer:
    case TypeKind::BuiltinDefaultActorStorage:
    case TypeKind::BuiltinNonDefaultDistributedActorStorage:
    case TypeKind::SILMoveOnlyWrapped:
      LLVM_DEBUG(llvm::dbgs() << "Unhandled type: ";
                 DbgTy.getType()->dump(llvm::dbgs()); llvm::dbgs() << "\n");
      MangledName = "<unknown>";
    }
    return DBuilder.createBasicType(MangledName, SizeInBits, Encoding);
  }

  /// Determine if there exists a name mangling for the given type.
  static bool canMangle(TypeBase *Ty) {
    switch (Ty->getKind()) {
    case TypeKind::GenericFunction: // Not yet supported.
    case TypeKind::SILBlockStorage: // Not supported at all.
      return false;
    default:
      return true;
    }
  }

  llvm::DIType *getTypeOrNull(TypeBase *Ty) {
    auto CachedType = DITypeCache.find(Ty);
    if (CachedType != DITypeCache.end()) {
      // Verify that the information still exists.
      if (llvm::Metadata *Val = CachedType->second) {
        auto DITy = cast<llvm::DIType>(Val);
        return DITy;
      }
    }
    return nullptr;
  }

  /// The private discriminator is represented as an inline namespace.
  llvm::DIScope *getFilePrivateScope(llvm::DIScope *Parent, TypeDecl *Decl) {
    // Retrieve the private discriminator.
    auto *MSC = Decl->getDeclContext()->getModuleScopeContext();
    auto *FU = cast<FileUnit>(MSC);
    Identifier PD = FU->getDiscriminatorForPrivateDecl(Decl);
    bool ExportSymbols = true;
    return DBuilder.createNameSpace(Parent, PD.str(), ExportSymbols);
  }

#ifndef NDEBUG
  /// Verify that the size of this type matches the one of the cached type.
  bool sanityCheckCachedType(DebugTypeInfo DbgTy, llvm::DIType *CachedType) {
    if (DbgTy.isForwardDecl())
      return true;
    auto SizeInBits = DbgTy.getTypeSizeInBits();
    unsigned CachedSizeInBits = getSizeInBits(CachedType);
    if ((SizeInBits && CachedSizeInBits != *SizeInBits) ||
        (!SizeInBits && CachedSizeInBits)) {
      CachedType->dump();
      DbgTy.dump();
      llvm::errs() << "SizeInBits = " << SizeInBits << "\n";
      llvm::errs() << "CachedSizeInBits = " << CachedSizeInBits << "\n";
      return false;
    }
    return true;
  }
#endif
  
  llvm::DIType *getOrCreateType(DebugTypeInfo DbgTy) {
    // Is this an empty type?
    if (DbgTy.isNull())
      // We can't use the empty type as an index into DenseMap.
      return createType(DbgTy, "", TheCU, MainFile);

    // Look in the cache first.
    if (auto *DITy = getTypeOrNull(DbgTy.getType())) {
      assert(sanityCheckCachedType(DbgTy, DITy));
      return DITy;
    }

    // Second line of defense: Look up the mangled name. TypeBase*'s are
    // not necessarily unique, but name mangling is too expensive to do
    // every time.
    StringRef MangledName;
    llvm::MDString *UID = nullptr;
    if (canMangle(DbgTy.getType())) {
      MangledName = getMangledName(DbgTy);
      UID = llvm::MDString::get(IGM.getLLVMContext(), MangledName);
      if (llvm::Metadata *CachedTy = DIRefMap.lookup(UID)) {
        auto DITy = cast<llvm::DIType>(CachedTy);
        assert(sanityCheckCachedType(DbgTy, DITy));
        return DITy;
      }
    }

    // Retrieve the context of the type, as opposed to the DeclContext
    // of the variable.
    //
    // FIXME: Builtin and qualified types in LLVM have no parent
    // scope. TODO: This can be fixed by extending DIBuilder.
    llvm::DIScope *Scope = nullptr;
    // Make sure to retrieve the context of the type alias, not the pointee.
    DeclContext *Context = nullptr;
    const Decl *TypeDecl = nullptr;
    const clang::Decl *ClangDecl = nullptr;
    if (auto Alias = dyn_cast<TypeAliasType>(DbgTy.getType())) {
      TypeAliasDecl *AliasDecl = Alias->getDecl();
      TypeDecl = AliasDecl;
      Context = AliasDecl->getParent();
      ClangDecl = AliasDecl->getClangDecl();
    } else if (auto *ND = DbgTy.getType()->getNominalOrBoundGenericNominal()) {
      TypeDecl = ND;
      Context = ND->getParent();
      ClangDecl = ND->getClangDecl();
    }
    if (ClangDecl) {
      clang::ASTReader &Reader = *CI.getClangInstance().getASTReader();
      auto Idx = ClangDecl->getOwningModuleID();
      auto SubModuleDesc = Reader.getSourceDescriptor(Idx);
      auto TopLevelModuleDesc = getClangModule(*TypeDecl->getModuleContext());
      if (SubModuleDesc) {
        if (TopLevelModuleDesc)
          // Describe the submodule, but substitute the cached ASTFile from
          // the toplevel module. The ASTFile pointer in SubModule may be
          // dangling and cant be trusted.
          Scope = getOrCreateModule({SubModuleDesc->getModuleName(),
                                     SubModuleDesc->getPath(),
                                     TopLevelModuleDesc->getASTFile(),
                                     TopLevelModuleDesc->getSignature()},
                                    SubModuleDesc->getModuleOrNull());
        else if (SubModuleDesc->getModuleOrNull() == nullptr)
          // This is (bridging header) PCH.
          Scope = getOrCreateModule(*SubModuleDesc, nullptr);
      }
    }
    if (!Scope)
      Scope = getOrCreateContext(Context);

    // Scope outermost fileprivate decls in an inline private discriminator
    // namespace.
    StringRef Name = MangledName;
    if (auto *Decl = DbgTy.getDecl()) {
      Name = Decl->getName().str();
      if (Decl->isOutermostPrivateOrFilePrivateScope())
        Scope = getFilePrivateScope(Scope, Decl);
    }

    // If this is a forward decl, create one for this mangled name and don't
    // cache it.
    if (DbgTy.isForwardDecl() && !isa<TypeAliasType>(DbgTy.getType())) {
      // In LTO type uniquing is performed based on the UID. Forward
      // declarations may not have a unique ID to avoid a forward declaration
      // winning over a full definition.
      auto *FwdDecl = DBuilder.createReplaceableCompositeType(
          llvm::dwarf::DW_TAG_structure_type, MangledName, Scope, 0, 0,
          llvm::dwarf::DW_LANG_Swift, 0, 0, llvm::DINode::FlagFwdDecl);
      FwdDeclTypes.emplace_back(
          std::piecewise_construct, std::make_tuple(MangledName),
          std::make_tuple(static_cast<llvm::Metadata *>(FwdDecl)));
      return FwdDecl;
    }
    llvm::DIType *DITy = createType(DbgTy, MangledName, Scope, getFile(Scope));

    // Don't cache a type alias to a forward declaration either.
    if (DbgTy.isForwardDecl() || DbgTy.isFixedBuffer() ||
        DITy->isForwardDecl())
      return DITy;

    if (auto Ty = DbgTy.getType())
      switch (Ty->getKind()) {
      case TypeKind::PrimaryArchetype:
        // FIXME: Primary archetypes carry all sorts of auxiliary information
        // that isn't contained in their mangled name.  See also
        // getMangledName().
        return DITy;
      case TypeKind::BoundGenericEnum:
      case TypeKind::BoundGenericStruct:
        // FIXME: These are emitted in sized anonymous containers, and their
        // size is not consistent when resilient.
        return DITy;
      default:
        break;
      }

    // Incrementally build the DIRefMap.
    if (auto *CTy = dyn_cast<llvm::DICompositeType>(DITy)) {
#ifndef NDEBUG
      // Sanity check.
      if (llvm::Metadata *V = DIRefMap.lookup(UID)) {
        auto *CachedTy = cast<llvm::DIType>(V);
        assert(CachedTy == DITy && "conflicting types for one UID");
      }
#endif
      // If this type supports a UID, enter it to the cache.
      if (auto UID = CTy->getRawIdentifier()) {
        assert(UID->getString() == MangledName &&
               "Unique identifier is different from mangled name ");
        DIRefMap[UID] = llvm::TrackingMDNodeRef(DITy);
      }
    }

    // Store it in the cache.
    DITypeCache.insert({DbgTy.getType(), llvm::TrackingMDNodeRef(DITy)});

    return DITy;
  }
};

IRGenDebugInfoImpl::IRGenDebugInfoImpl(const IRGenOptions &Opts,
                                       ClangImporter &CI, IRGenModule &IGM,
                                       llvm::Module &M,
                                       StringRef MainOutputFilenameForDebugInfo,
                                       StringRef PD)
    : Opts(Opts), CI(CI), SM(IGM.Context.SourceMgr), M(M), DBuilder(M),
      IGM(IGM), DebugPrefixMap(Opts.DebugPrefixMap) {
  assert(Opts.DebugInfoLevel > IRGenDebugInfoLevel::None &&
         "no debug info should be generated");
  llvm::SmallString<256> SourcePath;
  if (MainOutputFilenameForDebugInfo.empty())
    SourcePath = "<unknown>";
  else
    SourcePath = MainOutputFilenameForDebugInfo;

  unsigned Lang = llvm::dwarf::DW_LANG_Swift;
  std::string Producer = version::getSwiftFullVersion(
      IGM.Context.LangOpts.EffectiveLanguageVersion);
  unsigned Major, Minor;
  std::tie(Major, Minor) = version::getSwiftNumericVersion();
  unsigned MajorRuntimeVersion = Major;

  // No split DWARF on Darwin.
  StringRef SplitName = StringRef();
  // Note that File + Dir need not result in a valid path.
  // The directory part of the main file is the current working directory.
  std::string RemappedFile = DebugPrefixMap.remapPath(SourcePath);
  std::string RemappedDir = DebugPrefixMap.remapPath(Opts.DebugCompilationDir);
  bool RelFile = llvm::sys::path::is_relative(RemappedFile);
  bool RelDir = llvm::sys::path::is_relative(RemappedDir);
  MainFile = (RelFile && RelDir)
                 ? createFile(SourcePath, {}, {})
                 : DBuilder.createFile(RemappedFile, RemappedDir);

  StringRef Sysroot = IGM.Context.SearchPathOpts.getSDKPath();
  StringRef SDK;
  {
    auto B = llvm::sys::path::rbegin(Sysroot);
    auto E = llvm::sys::path::rend(Sysroot);
    auto It = std::find_if(B, E, [](auto SDK) { return SDK.endswith(".sdk"); });
    if (It != E)
      SDK = *It;
  }

  bool EnableCXXInterop =
      IGM.getSILModule().getASTContext().LangOpts.EnableCXXInterop;
  TheCU = DBuilder.createCompileUnit(
      Lang, MainFile, Producer, Opts.shouldOptimize(),
      Opts.getDebugFlags(PD, EnableCXXInterop), MajorRuntimeVersion, SplitName,
      Opts.DebugInfoLevel > IRGenDebugInfoLevel::LineTables
          ? llvm::DICompileUnit::FullDebug
          : llvm::DICompileUnit::LineTablesOnly,
      /* DWOId */ 0, /* SplitDebugInlining */ true,
      /* DebugInfoForProfiling */ false,
      llvm::DICompileUnit::DebugNameTableKind::Default,
      /* RangesBaseAddress */ false, DebugPrefixMap.remapPath(Sysroot), SDK);

  // Because the swift compiler relies on Clang to setup the Module,
  // the clang CU is always created first.  Several dwarf-reading
  // tools (older versions of ld64, and lldb) can get confused if the
  // first CU in an object is empty, so ensure that the Swift CU comes
  // first by rearranging the list of CUs in the LLVM module.
  llvm::NamedMDNode *CU_Nodes = M.getNamedMetadata("llvm.dbg.cu");
  SmallVector<llvm::DICompileUnit *, 2> CUs;
  for (auto *N : CU_Nodes->operands())
    CUs.push_back(cast<llvm::DICompileUnit>(N));
  CU_Nodes->dropAllReferences();
  for (auto CU = CUs.rbegin(), CE = CUs.rend(); CU != CE; ++CU)
    CU_Nodes->addOperand(*CU);

  // Create a module for the current compile unit.
  auto *MDecl = IGM.getSwiftModule();
  llvm::sys::path::remove_filename(SourcePath);
  MainModule = getOrCreateModule(MDecl, TheCU, Opts.ModuleName, SourcePath);
  DBuilder.createImportedModule(MainFile, MainModule, MainFile, 0);

  // Macro definitions that were defined by the user with "-Xcc -D" on the
  // command line. This does not include any macros defined by ClangImporter.
  llvm::raw_svector_ostream OS(ConfigMacros);
  unsigned I = 0;
  // Translate the macro definitions back into a command line.
  for (auto &Macro : Opts.ClangDefines) {
    if (++I > 1)
      OS << ' ';
    OS << '"';
    for (char c : Macro)
      switch (c) {
      case '\\':
        OS << "\\\\";
        break;
      case '"':
        OS << "\\\"";
        break;
      default:
        OS << c;
      }
    OS << '"';
  }
}

void IRGenDebugInfoImpl::finalize() {
  assert(LocationStack.empty() && "Mismatch of pushLoc() and popLoc().");

  // Get the list of imported modules (which may actually be different
  // from all ImportDecls).
  SmallVector<ImportedModule, 8> ModuleWideImports;
  IGM.getSwiftModule()->getImportedModules(ModuleWideImports,
                                           ModuleDecl::getImportFilterLocal());
  for (auto M : ModuleWideImports)
    if (!ImportedModules.count(M.importedModule))
      createImportedModule(MainFile, M, MainFile, 0);

  // Finalize all replaceable forward declarations.
  auto finalize = [&](llvm::MDNode *FwdDeclType, llvm::MDNode *FullType,
                      llvm::MDString *UID = nullptr) {
    llvm::TempMDNode FwdDecl(cast<llvm::MDNode>(FwdDeclType));
    llvm::Metadata *Replacement = FullType ? FullType : FwdDeclType;
    llvm::Metadata *Replaced = DBuilder.replaceTemporary(
        std::move(FwdDecl), cast<llvm::MDNode>(Replacement));

    // Unique all identical forward declarations.
    if (UID && !FullType)
      DIRefMap[UID] = llvm::TrackingMDNodeRef(cast<llvm::MDNode>(Replaced));
  };

  for (auto &Ty : FwdDeclTypes) {
    auto *UID = llvm::MDString::get(IGM.getLLVMContext(), Ty.first);
    finalize(cast<llvm::MDNode>(Ty.second),
             llvm::cast_or_null<llvm::DIType>(DIRefMap.lookup(UID)), UID);
  }

  // Finalize the DIBuilder.
  DBuilder.finalize();
}

#ifndef NDEBUG
bool IRGenDebugInfoImpl::lineEntryIsSane(SILLocation::FilenameAndLocation DL,
                                         const SILDebugScope *DS) {
  // All bets are off for optimized code.
  if (!VerifyLineTable || Opts.shouldOptimize())
    return true;
  // We entered a new lexical block.
  if (DS != LastScope)
    PreviousLineEntries.clear();
  if (DL.line == 0 || DL == PreviousFilenameAndLocation)
    return true;
  // Save the last non-zero line entry.
  PreviousFilenameAndLocation = DL;
  auto ItNew = PreviousLineEntries.insert(FilenameAndLocationKey(DL));
  // Return true iff DL was not yet in PreviousLineEntries.
  return ItNew.second;
}
#endif

void IRGenDebugInfoImpl::setCurrentLoc(IRBuilder &Builder,
                                       const SILDebugScope *DS,
                                       SILLocation Loc) {
  assert(DS && "empty scope");
  auto *Scope = getOrCreateScope(DS);
  if (!Scope)
    return;

  // NOTE: In CodeView, zero is not an artificial line location. We try to
  //       avoid those line locations near user code to reduce the number
  //       of breaks in the linetables.
  SILLocation::FilenameAndLocation L;
  SILFunction *Fn = DS->getInlinedFunction();
  if (Fn && (Fn->isThunk() || Fn->isTransparent())) {
    L = *SILLocation::getCompilerGeneratedLoc();
  } else if (DS == LastScope && Loc.isHiddenFromDebugInfo()) {
    // Reuse the last source location if we are still in the same
    // scope to get a more contiguous line table.
    L = LastFilenameAndLocation;
  } else if (DS == LastScope &&
             (Loc.is<ArtificialUnreachableLocation>() || Loc.isLineZero(SM)) &&
             Opts.DebugInfoFormat == IRGenDebugInfoFormat::CodeView) {
    // If the scope has not changed and the line number is either zero or
    // artificial, we want to keep the most recent debug location.
    L = LastFilenameAndLocation;
  } else {
    // Decode the location.
    if (!Loc.isInPrologue() ||
        Opts.DebugInfoFormat == IRGenDebugInfoFormat::CodeView)
      L = decodeFilenameAndLocation(Loc);

    // Otherwise use a line 0 artificial location, but the file from the
    // location. If we are emitting CodeView, we do not want to use line zero
    // since it does not represent an artificial line location.
    if (Loc.isHiddenFromDebugInfo() &&
        Opts.DebugInfoFormat != IRGenDebugInfoFormat::CodeView) {
      L.line = 0;
      L.column = 0;
    }
  }

  auto *File = getOrCreateFile(L.filename);
  if (File->getFilename() != Scope->getFilename()) {
    // We changed files in the middle of a scope. This happens, for
    // example, when constructors are inlined. Create a new scope to
    // reflect this.
    auto File = getOrCreateFile(L.filename);
    Scope = DBuilder.createLexicalBlockFile(Scope, File);
  }

  assert(lineEntryIsSane(L, DS) &&
         "non-contiguous debug location in same scope at -Onone");
  LastFilenameAndLocation = L;
  LastScope = DS;

  auto *InlinedAt = createInlinedAt(DS);
  assert(((!InlinedAt) || (InlinedAt && Scope)) && "inlined w/o scope");
  assert(parentScopesAreSane(DS) && "parent scope sanity check failed");
  auto DL = llvm::DILocation::get(IGM.getLLVMContext(), L.line, L.column, Scope,
                                  InlinedAt);
#ifndef NDEBUG
  {
    llvm::DILocalScope *Scope = DL->getInlinedAtScope();
    llvm::DISubprogram *SP = Scope->getSubprogram();
    llvm::Function *F = Builder.GetInsertBlock()->getParent();
    assert((!F || SP->describes(F)) && "location points to different function");
  }
#endif
  Builder.SetCurrentDebugLocation(DL);
}

void IRGenDebugInfoImpl::addFailureMessageToCurrentLoc(IRBuilder &Builder,
                                                       StringRef failureMsg) {
  auto TrapLoc = Builder.getCurrentDebugLocation();

  // Create a function in the debug info which has failureMsg as name.
  // TrapSc is the SIL debug scope which corresponds to TrapSP in the LLVM debug
  // info.
  RegularLocation ALoc = RegularLocation::getAutoGeneratedLocation();
  const SILDebugScope *TrapSc = new (IGM.getSILModule()) SILDebugScope(ALoc);

  llvm::DISubroutineType *DIFnTy = DBuilder.createSubroutineType(nullptr);

  llvm::DISubprogram *TrapSP;
  auto It = RuntimeErrorFnCache.find(failureMsg);
  if (It != RuntimeErrorFnCache.end())
    TrapSP = llvm::cast<llvm::DISubprogram>(It->second);
  else {
    std::string FuncName = "Swift runtime failure: ";
    FuncName += failureMsg;
    llvm::DIFile *File = getOrCreateFile({});
    TrapSP = DBuilder.createFunction(
        File, FuncName, StringRef(), File, 0,
        DIFnTy, 0, llvm::DINode::FlagArtificial,
        llvm::DISubprogram::SPFlagDefinition, nullptr, nullptr, nullptr);
    RuntimeErrorFnCache.insert({failureMsg, llvm::TrackingMDNodeRef(TrapSP)});
  }

  ScopeCache[TrapSc] = llvm::TrackingMDNodeRef(TrapSP);
  LastScope = TrapSc;

  assert(parentScopesAreSane(TrapSc) && "parent scope sanity check failed");

  // Wrap the existing TrapLoc into the failure function.
  auto DL = llvm::DILocation::get(IGM.getLLVMContext(), 0, 0, TrapSP, TrapLoc);
  Builder.SetCurrentDebugLocation(DL);
}

void IRGenDebugInfoImpl::clearLoc(IRBuilder &Builder) {
  LastFilenameAndLocation = {};
  LastScope = nullptr;
  Builder.SetCurrentDebugLocation(llvm::DebugLoc());
}

/// Push the current debug location onto a stack and initialize the
/// IRBuilder to an empty location.
void IRGenDebugInfoImpl::pushLoc() {
  LocationStack.push_back(std::make_pair(LastFilenameAndLocation, LastScope));
  LastFilenameAndLocation = {};
  LastScope = nullptr;
}

/// Restore the current debug location from the stack.
void IRGenDebugInfoImpl::popLoc() {
  std::tie(LastFilenameAndLocation, LastScope) = LocationStack.pop_back_val();
}

/// This is done for WinDbg to avoid having two non-contiguous sets of
/// instructions because the ``@llvm.trap`` instruction gets placed at the end
/// of the function.
void IRGenDebugInfoImpl::setInlinedTrapLocation(IRBuilder &Builder,
                                                const SILDebugScope *Scope) {
  if (Opts.DebugInfoFormat != IRGenDebugInfoFormat::CodeView)
    return;

  // The @llvm.trap could be inlined into a chunk of code that was also inlined.
  // If this is the case then simply using the LastScope's location would
  // generate debug info that claimed Function A owned Block X and Block X
  // thought it was owned by Function B. Therefore, we need to find the last
  // inlined scope to point to.
  const SILDebugScope *TheLastScope = LastScope;
  while (TheLastScope->InlinedCallSite &&
         TheLastScope->InlinedCallSite != TheLastScope) {
    TheLastScope = TheLastScope->InlinedCallSite;
  }
  auto LastLocation = llvm::DILocation::get(
      IGM.getLLVMContext(), LastFilenameAndLocation.line,
      LastFilenameAndLocation.column, getOrCreateScope(TheLastScope));
  // FIXME: This location should point to stdlib instead of being artificial.
  auto DL = llvm::DILocation::get(IGM.getLLVMContext(), 0, 0,
                                  getOrCreateScope(Scope), LastLocation);
  Builder.SetCurrentDebugLocation(DL);
}

void IRGenDebugInfoImpl::setEntryPointLoc(IRBuilder &Builder) {
  auto DL = llvm::DILocation::get(IGM.getLLVMContext(), 0, 0, getEntryPointFn(),
                                  nullptr);
  Builder.SetCurrentDebugLocation(DL);
}

llvm::DIScope *IRGenDebugInfoImpl::getEntryPointFn() {
  // Lazily create EntryPointFn.
  if (!EntryPointFn) {
    EntryPointFn = DBuilder.createReplaceableCompositeType(
        llvm::dwarf::DW_TAG_subroutine_type,
        IGM.getSILModule().getASTContext().getEntryPointFunctionName(),
        MainFile, MainFile, 0);
  }
  return EntryPointFn;
}

llvm::DIScope *IRGenDebugInfoImpl::getOrCreateScope(const SILDebugScope *DS) {
  if (DS == nullptr)
    return MainFile;

  // Try to find it in the cache first.
  auto CachedScope = ScopeCache.find(DS);
  if (CachedScope != ScopeCache.end())
    return cast<llvm::DIScope>(CachedScope->second);

  // If this is an (inlined) function scope, the function may
  // not have been created yet.
  if (auto *SILFn = DS->Parent.dyn_cast<SILFunction *>()) {
    auto *FnScope = SILFn->getDebugScope();
    // FIXME: This is a bug in the SIL deserialization.
    if (!FnScope)
      SILFn->setDebugScope(DS);

    auto CachedScope = ScopeCache.find(FnScope);
    if (CachedScope != ScopeCache.end())
      return cast<llvm::DIScope>(CachedScope->second);

    // Force the debug info for the function to be emitted, even if it
    // is external or has been inlined.
    llvm::Function *Fn = nullptr;
    if (!SILFn->getName().empty() && !SILFn->isZombie())
      Fn = IGM.getAddrOfSILFunction(SILFn, NotForDefinition);
    auto *SP = emitFunction(*SILFn, Fn);

    // Cache it.
    ScopeCache[DS] = llvm::TrackingMDNodeRef(SP);
    return SP;
  }

  auto *ParentScope = DS->Parent.get<const SILDebugScope *>();
  llvm::DIScope *Parent = getOrCreateScope(ParentScope);
  assert(isa<llvm::DILocalScope>(Parent) && "not a local scope");

  if (Opts.DebugInfoLevel <= IRGenDebugInfoLevel::LineTables)
    return Parent;

  assert(DS->Parent && "lexical block must have a parent subprogram");
  auto L = getStartLocation(DS->Loc);
  llvm::DIFile *File = getOrCreateFile(L.filename);
  auto *DScope = DBuilder.createLexicalBlock(Parent, File, L.line, L.column);

  // Cache it.
  ScopeCache[DS] = llvm::TrackingMDNodeRef(DScope);
  return DScope;
}

void IRGenDebugInfoImpl::emitImport(ImportDecl *D) {
  if (Opts.DebugInfoLevel <= IRGenDebugInfoLevel::LineTables)
    return;

  assert(D->getModule() && "compiler-synthesized ImportDecl is incomplete");
  ImportedModule Imported = { D->getAccessPath(), D->getModule() };
  auto L = getFilenameAndLocation(*this, D);
  auto *File = getOrCreateFile(L.filename);
  createImportedModule(File, Imported, File, L.line);
  ImportedModules.insert(Imported.importedModule);
}

llvm::DISubprogram *IRGenDebugInfoImpl::emitFunction(SILFunction &SILFn,
                                                     llvm::Function *Fn) {
  auto *DS = SILFn.getDebugScope();
  assert(DS && "SIL function has no debug scope");
  (void)DS;
  return emitFunction(SILFn.getDebugScope(), Fn, SILFn.getRepresentation(),
                      SILFn.getLoweredType(), SILFn.getDeclContext());
}

llvm::DISubprogram *
IRGenDebugInfoImpl::emitFunction(const SILDebugScope *DS, llvm::Function *Fn,
                                 SILFunctionTypeRepresentation Rep,
                                 SILType SILTy, DeclContext *DeclCtx,
                                 StringRef outlinedFromName) {
  auto Cached = ScopeCache.find(DS);
  if (Cached != ScopeCache.end()) {
    auto SP = cast<llvm::DISubprogram>(Cached->second);
    // If we created the DISubprogram for a forward declaration,
    // attach it to the function now.
    if (!Fn->getSubprogram() && !Fn->isDeclaration())
      Fn->setSubprogram(SP);
    return SP;
  }

  // Some IRGen-generated helper functions don't have a corresponding
  // SIL function, hence the dyn_cast.
  auto *SILFn = DS ? DS->Parent.dyn_cast<SILFunction *>() : nullptr;

  StringRef LinkageName;
  if (!outlinedFromName.empty())
    LinkageName = outlinedFromName;
  else if (Fn)
    LinkageName = Fn->getName();
  else if (DS)
    LinkageName = SILFn->getName();
  else
    llvm_unreachable("function has no mangled name");

  StringRef Name;
  if (DS) {
    if (DS->Loc.isSILFile())
      Name = SILFn->getName();
    else
      Name = getName(DS->Loc);
  }

  /// The source line used for the function prologue.
  unsigned ScopeLine = 0;
  SILLocation::FilenameAndLocation L;
  if (!DS || (SILFn && (SILFn->isBare() || SILFn->isThunk() ||
                        SILFn->isTransparent()))) {
    // Bare functions and thunks should not have any line numbers. This
    // is especially important for shared functions like reabstraction
    // thunk helpers, where DS->Loc is an arbitrary location of whichever use
    // was emitted first.
    L = *SILLocation::getCompilerGeneratedLoc();
  } else {
    L = decodeFilenameAndLocation(DS->Loc);
    ScopeLine = L.line;
  }

  auto Line = L.line;
  auto File = getOrCreateFile(L.filename);
  llvm::DIScope *Scope = MainModule;
  if (SILFn && SILFn->getDeclContext())
    Scope = getOrCreateContext(SILFn->getDeclContext()->getParent());

  // We know that main always comes from MainFile.
  if (LinkageName ==
      IGM.getSILModule().getASTContext().getEntryPointFunctionName()) {
    File = MainFile;
    Line = 1;
    Name = LinkageName;
  }

  CanSILFunctionType FnTy = getFunctionType(SILTy);
  auto Params = Opts.DebugInfoLevel > IRGenDebugInfoLevel::LineTables
                    ? createParameterTypes(SILTy)
                    : nullptr;
  llvm::DISubroutineType *DIFnTy = DBuilder.createSubroutineType(Params);
  llvm::DITemplateParameterArray TemplateParameters = nullptr;
  llvm::DISubprogram *Decl = nullptr;

  // Various flags.
  llvm::DINode::DIFlags Flags = llvm::DINode::FlagZero;
  // Mark everything that is not visible from the source code (i.e.,
  // does not have a Swift name) as artificial, so the debugger can
  // ignore it. Explicit closures are exempt from this rule. We also
  // make an exception for toplevel code, which, although it does not
  // have a Swift name, does appear prominently in the source code.
  // ObjC thunks should also not show up in the linetable, because we
  // never want to set a breakpoint there.
  if ((Name.empty() &&
       LinkageName !=
           IGM.getSILModule().getASTContext().getEntryPointFunctionName() &&
       !isExplicitClosure(SILFn)) ||
      (Rep == SILFunctionTypeRepresentation::ObjCMethod) ||
      isAllocatingConstructor(Rep, DeclCtx)) {
    Flags |= llvm::DINode::FlagArtificial;
    ScopeLine = 0;
  }

  if (FnTy &&
      FnTy->getRepresentation() == SILFunctionType::Representation::Block)
    Flags |= llvm::DINode::FlagAppleBlock;

  // Get the throws information.
  llvm::DITypeArray Error = nullptr;
  if (FnTy && (Opts.DebugInfoLevel > IRGenDebugInfoLevel::LineTables))
    if (auto ErrorInfo = FnTy->getOptionalErrorResult()) {
      SILType SILTy = IGM.silConv.getSILType(
          *ErrorInfo, FnTy, IGM.getMaximalTypeExpansionContext());
      auto DTI = DebugTypeInfo::getFromTypeInfo(
          ErrorInfo->getReturnValueType(IGM.getSILModule(), FnTy,
                                        IGM.getMaximalTypeExpansionContext()),
          IGM.getTypeInfo(SILTy), IGM, false);
      Error = DBuilder.getOrCreateArray({getOrCreateType(DTI)}).get();
    }

  llvm::DISubprogram::DISPFlags SPFlags = llvm::DISubprogram::toSPFlags(
      /*IsLocalToUnit=*/Fn ? Fn->hasInternalLinkage() : true,
      /*IsDefinition=*/true, /*IsOptimized=*/Opts.shouldOptimize());

  // Construct the DISubprogram.
  llvm::DISubprogram *SP = DBuilder.createFunction(
      Scope, Name, LinkageName, File, Line, DIFnTy, ScopeLine, Flags, SPFlags,
      TemplateParameters, Decl, Error);

  if (Fn && !Fn->isDeclaration())
    Fn->setSubprogram(SP);

  // RAUW the entry point function forward declaration with the real thing.
  if (LinkageName ==
      IGM.getSILModule().getASTContext().getEntryPointFunctionName()) {
    if (EntryPointFn) {
      assert(EntryPointFn->isTemporary() &&
             "more than one entry point function");
      EntryPointFn->replaceAllUsesWith(SP);
      llvm::MDNode::deleteTemporary(EntryPointFn);
    }
    EntryPointFn = SP;
  }

  if (!DS)
    return nullptr;

  ScopeCache[DS] = llvm::TrackingMDNodeRef(SP);
  return SP;
}

void IRGenDebugInfoImpl::emitArtificialFunction(IRBuilder &Builder,
                                                llvm::Function *Fn,
                                                SILType SILTy) {
  RegularLocation ALoc = RegularLocation::getAutoGeneratedLocation();
  const SILDebugScope *Scope = new (IGM.getSILModule()) SILDebugScope(ALoc);
  emitFunction(Scope, Fn, SILFunctionTypeRepresentation::Thin, SILTy);
  /// Reusing the current file would be wrong: An objc thunk, for example, could
  /// be triggered from any random location. Use a placeholder name instead.
  setCurrentLoc(Builder, Scope, ALoc);
}

void IRGenDebugInfoImpl::emitOutlinedFunction(IRBuilder &Builder,
                                              llvm::Function *Fn,
                                              StringRef outlinedFromName) {
  RegularLocation ALoc = RegularLocation::getAutoGeneratedLocation();
  const SILDebugScope *Scope = new (IGM.getSILModule()) SILDebugScope(ALoc);
  emitFunction(Scope, Fn, SILFunctionTypeRepresentation::Thin, SILType(),
               nullptr, outlinedFromName);
  /// Reusing the current file would be wrong: An objc thunk, for example, could
  /// be triggered from any random location. Use a placeholder name instead.
  setCurrentLoc(Builder, Scope, ALoc);
}

bool IRGenDebugInfoImpl::handleFragmentDIExpr(
    const SILDIExprOperand &CurDIExprOp,
    llvm::DIExpression::FragmentInfo &Fragment) {
  assert(CurDIExprOp.getOperator() == SILDIExprOperator::Fragment);
  // Expecting a VarDecl that points to a field in an struct
  auto DIExprArgs = CurDIExprOp.args();
  auto *VD = dyn_cast_or_null<VarDecl>(DIExprArgs.size()?
                                       DIExprArgs[0].getAsDecl() : nullptr);
  assert(VD && "Expecting a VarDecl as the operand for "
               "DIExprOperator::Fragment");
  // Translate the based type
  DeclContext *ParentDecl = VD->getDeclContext();
  assert(ParentDecl && "VarDecl has no parent context?");
  SILType ParentSILType =
      IGM.getLoweredType(ParentDecl->getDeclaredTypeInContext());
  // Retrieve the offset & size of the field
  llvm::Constant *Offset =
      emitPhysicalStructMemberFixedOffset(IGM, ParentSILType, VD);
  auto *FieldTypeInfo = getPhysicalStructFieldTypeInfo(IGM, ParentSILType, VD);
  // FIXME: This will only happen if IRGen hasn't processed ParentSILType
  // (into its own representation) but we probably should ask IRGen to process
  // it right now.
  if (!FieldTypeInfo)
    return false;
  llvm::Type *FieldTy = FieldTypeInfo->getStorageType();
  // Doesn't support non-fixed type right now
  if (!Offset || !FieldTy)
    return false;

  uint64_t SizeOfByte = CI.getTargetInfo().getCharWidth();
  uint64_t SizeInBits = IGM.DataLayout.getTypeSizeInBits(FieldTy);
  uint64_t OffsetInBits =
      Offset->getUniqueInteger().getLimitedValue() * SizeOfByte;

  // Translate to DW_OP_LLVM_fragment operands
  Fragment = {SizeInBits, OffsetInBits};

  return true;
}

bool IRGenDebugInfoImpl::buildDebugInfoExpression(
    const SILDebugVariable &VarInfo, SmallVectorImpl<uint64_t> &Operands,
    llvm::DIExpression::FragmentInfo &Fragment) {
  assert(VarInfo.DIExpr && "SIL debug info expression not found");

#ifndef NDEBUG
  bool HasFragment = VarInfo.DIExpr.hasFragment();
#endif

  const auto &DIExpr = VarInfo.DIExpr;
  for (const SILDIExprOperand &ExprOperand : DIExpr.operands()) {
    switch (ExprOperand.getOperator()) {
    case SILDIExprOperator::Fragment:
      assert(HasFragment && "Fragment must be the last part of a DIExpr");
#ifndef NDEBUG
      // Trigger the assert above if we have more than one fragment expression.
      HasFragment = false;
#endif
      if (!handleFragmentDIExpr(ExprOperand, Fragment))
        return false;
      break;
    case SILDIExprOperator::Dereference:
      Operands.push_back(llvm::dwarf::DW_OP_deref);
      break;
    case SILDIExprOperator::Plus:
      Operands.push_back(llvm::dwarf::DW_OP_plus);
      break;
    case SILDIExprOperator::Minus:
      Operands.push_back(llvm::dwarf::DW_OP_minus);
      break;
    case SILDIExprOperator::ConstUInt:
      Operands.push_back(llvm::dwarf::DW_OP_constu);
      Operands.push_back(*ExprOperand[1].getAsConstInt());
      break;
    case SILDIExprOperator::ConstSInt:
      Operands.push_back(llvm::dwarf::DW_OP_consts);
      Operands.push_back(*ExprOperand[1].getAsConstInt());
      break;
    case SILDIExprOperator::INVALID:
      return false;
    }
  }
  return true;
}

void IRGenDebugInfoImpl::emitVariableDeclaration(
    IRBuilder &Builder, ArrayRef<llvm::Value *> Storage, DebugTypeInfo DbgTy,
    const SILDebugScope *DS, llvm::Optional<SILLocation> DbgInstLoc,
    SILDebugVariable VarInfo, IndirectionKind Indirection,
    ArtificialKind Artificial, AddrDbgInstrKind AddrDInstrKind) {
  assert(DS && "variable has no scope");

  if (Opts.DebugInfoLevel <= IRGenDebugInfoLevel::LineTables)
    return;

  // We cannot yet represent local archetypes.
  if (DbgTy.getType()->hasLocalArchetype())
    return;

  auto *Scope = dyn_cast_or_null<llvm::DILocalScope>(getOrCreateScope(DS));
  assert(Scope && "variable has no local scope");
  auto DInstLoc = getStartLocation(DbgInstLoc);

  // FIXME: this should be the scope of the type's declaration.
  // If this is an argument, attach it to the current function scope.
  uint16_t ArgNo = VarInfo.ArgNo;
  if (ArgNo > 0) {
    while (isa<llvm::DILexicalBlock>(Scope))
      Scope = cast<llvm::DILexicalBlock>(Scope)->getScope();
  }
  assert(isa_and_nonnull<llvm::DIScope>(Scope) && "variable has no scope");
  llvm::DIFile *Unit = getFile(Scope);
  llvm::DIType *DITy = getOrCreateType(DbgTy);
  assert(DITy && "could not determine debug type of variable");
  if (VarInfo.Constant)
    DITy = DBuilder.createQualifiedType(llvm::dwarf::DW_TAG_const_type, DITy);

  unsigned DInstLine = DInstLoc.line;

  // Self is always an artificial argument, so are variables without location.
  if (!DInstLine || (ArgNo > 0 && VarInfo.Name == IGM.Context.Id_self.str()))
    Artificial = ArtificialValue;

  llvm::DINode::DIFlags Flags = llvm::DINode::FlagZero;
  if (Artificial || DITy->isArtificial() || DITy == InternalType)
    Flags |= llvm::DINode::FlagArtificial;

  // Create the descriptor for the variable.
  unsigned DVarLine = DInstLine;
  uint16_t DVarCol = 0;
  if (VarInfo.Loc) {
    auto DVarLoc = getStartLocation(VarInfo.Loc);
    DVarLine = DVarLoc.line;
    DVarCol = DVarLoc.column;
  }
  llvm::DIScope *VarScope = Scope;
  if (ArgNo == 0 && VarInfo.Scope) {
    if (auto *VS = dyn_cast_or_null<llvm::DILocalScope>(
            getOrCreateScope(VarInfo.Scope))) {
      VarScope = VS;
    }
  }

  // Get or create the DILocalVariable.
  llvm::DILocalVariable *Var;
  // VarInfo.Name points into tail-allocated storage in debug_value insns.
  llvm::StringRef UniqueName = VarNames.insert(VarInfo.Name).first->getKey();
  VarID Key(VarScope, UniqueName, DVarLine, DVarCol);
  auto CachedVar = LocalVarCache.find(Key);
  if (CachedVar != LocalVarCache.end()) {
    Var = cast<llvm::DILocalVariable>(CachedVar->second);
  } else {
    // The llvm.dbg.value(undef) emitted for zero-sized variables get filtered
    // out by DwarfDebug::collectEntityInfo(), so all variables need to be
    // preserved even at -Onone.
    bool Preserve = true;
    if (ArgNo > 0)
      Var = DBuilder.createParameterVariable(
          VarScope, VarInfo.Name, ArgNo, Unit, DVarLine, DITy, Preserve, Flags);
    else
      Var = DBuilder.createAutoVariable(VarScope, VarInfo.Name, Unit, DVarLine,
                                        DITy, Preserve, Flags);
    LocalVarCache.insert({Key, llvm::TrackingMDNodeRef(Var)});
  }

  // Running variables for the current/previous piece.
  bool IsPiece = Storage.size() > 1;
  uint64_t SizeOfByte = CI.getTargetInfo().getCharWidth();
  unsigned AlignInBits = SizeOfByte;
  unsigned OffsetInBits = 0;
  unsigned SizeInBits = 0;
  llvm::DIExpression::FragmentInfo Fragment = {0, 0};

  auto appendDIExpression =
      [&VarInfo, this](llvm::DIExpression *DIExpr,
                       llvm::DIExpression::FragmentInfo PieceFragment)
      -> llvm::DIExpression * {
    if (!VarInfo.DIExpr) {
      if (!PieceFragment.SizeInBits)
        return DIExpr;

      return llvm::DIExpression::createFragmentExpression(
                 DIExpr, PieceFragment.OffsetInBits, PieceFragment.SizeInBits)
          .value_or(nullptr);
    }

    llvm::SmallVector<uint64_t, 2> Operands;
    llvm::DIExpression::FragmentInfo VarFragment = {0, 0};
    if (!buildDebugInfoExpression(VarInfo, Operands, VarFragment))
      return nullptr;

    if (!Operands.empty())
      DIExpr = llvm::DIExpression::append(DIExpr, Operands);

    // Add the fragment of the SIL variable.
    if (VarFragment.SizeInBits)
      DIExpr = llvm::DIExpression::createFragmentExpression(
                   DIExpr, VarFragment.OffsetInBits, VarFragment.SizeInBits)
                   .value_or(nullptr);

    if (!DIExpr)
      return nullptr;

    // When the fragment of the SIL variable is further split into other
    // fragments (PieceFragment), merge them into one DW_OP_LLVM_Fragment
    // expression.
    if (PieceFragment.SizeInBits)
      return llvm::DIExpression::createFragmentExpression(
                 DIExpr, PieceFragment.OffsetInBits, PieceFragment.SizeInBits)
          .value_or(nullptr);

    return DIExpr;
  };

  for (llvm::Value *Piece : Storage) {
    SmallVector<uint64_t, 3> Operands;

    if (DbgTy.getType()->isForeignReferenceType())
      Operands.push_back(llvm::dwarf::DW_OP_deref);

    if (Indirection == IndirectValue || Indirection == CoroIndirectValue)
      Operands.push_back(llvm::dwarf::DW_OP_deref);

    if (IsPiece) {
      // Advance the offset for the next piece.
      OffsetInBits += SizeInBits;
      SizeInBits = IGM.DataLayout.getTypeSizeInBits(Piece->getType());
      AlignInBits = IGM.DataLayout.getABITypeAlignment(Piece->getType());
      if (!AlignInBits)
        AlignInBits = SizeOfByte;

      // Sanity checks.
#ifndef NDEBUG
      assert(SizeInBits && "zero-sized piece");
      if (getSizeInBits(Var)) {
        assert(SizeInBits < getSizeInBits(Var) && "piece covers entire var");
        assert(OffsetInBits + SizeInBits <= getSizeInBits(Var) &&
               "pars > totum");
      }
#endif

      // Add the piece DW_OP_LLVM_fragment operands
      Fragment.OffsetInBits = OffsetInBits;
      Fragment.SizeInBits = SizeInBits;
    }
    llvm::DIExpression *DIExpr = DBuilder.createExpression(Operands);
    DIExpr = appendDIExpression(DIExpr, Fragment);
    if (DIExpr)
      emitDbgIntrinsic(
          Builder, Piece, Var, DIExpr, DInstLine, DInstLoc.column, Scope, DS,
          Indirection == CoroDirectValue || Indirection == CoroIndirectValue,
          AddrDInstrKind);
  }

  // Emit locationless intrinsic for variables that were optimized away.
  if (Storage.empty()) {
    llvm::DIExpression::FragmentInfo NoFragment = {0, 0};
    if (auto *DIExpr =
            appendDIExpression(DBuilder.createExpression(), NoFragment))
      emitDbgIntrinsic(Builder, llvm::ConstantInt::get(IGM.Int64Ty, 0), Var,
                       DIExpr, DInstLine, DInstLoc.column, Scope, DS,
                       Indirection == CoroDirectValue ||
                           Indirection == CoroIndirectValue,
                       AddrDInstrKind);
  }
}

namespace {

/// A helper struct that is used by emitDbgIntrinsic to factor redundant code.
struct DbgIntrinsicEmitter {
  PointerUnion<llvm::BasicBlock *, llvm::Instruction *> InsertPt;
  irgen::IRBuilder &IRBuilder;
  llvm::DIBuilder &DIBuilder;
  AddrDbgInstrKind ForceDbgDeclare;

  /// Initialize the emitter and initialize the emitter to assume that it is
  /// going to insert an llvm.dbg.declare or an llvm.dbg.addr either at the
  /// current "generalized insertion point" of the IRBuilder. The "generalized
  /// insertion point" is
  DbgIntrinsicEmitter(irgen::IRBuilder &IRBuilder, llvm::DIBuilder &DIBuilder,
                      AddrDbgInstrKind ForceDebugDeclare)
      : InsertPt(), IRBuilder(IRBuilder), DIBuilder(DIBuilder),
        ForceDbgDeclare(ForceDebugDeclare) {
    auto *ParentBB = IRBuilder.GetInsertBlock();
    auto InsertBefore = IRBuilder.GetInsertPoint();

    if (InsertBefore != ParentBB->end())
      InsertPt = &*InsertBefore;
    else
      InsertPt = ParentBB;
  }

  ///

  llvm::Instruction *insert(llvm::Value *Addr, llvm::DILocalVariable *VarInfo,
                            llvm::DIExpression *Expr,
                            const llvm::DILocation *DL) {
    if (auto *Inst = InsertPt.dyn_cast<llvm::Instruction *>()) {
      return insert(Addr, VarInfo, Expr, DL, Inst);
    } else {
      return insert(Addr, VarInfo, Expr, DL,
                    InsertPt.get<llvm::BasicBlock *>());
    }
  }

  llvm::Instruction *insert(llvm::Value *Addr, llvm::DILocalVariable *VarInfo,
                            llvm::DIExpression *Expr,
                            const llvm::DILocation *DL,
                            llvm::Instruction *InsertBefore) {
    if (ForceDbgDeclare == AddrDbgInstrKind::DbgDeclare)
      return DIBuilder.insertDeclare(Addr, VarInfo, Expr, DL, InsertBefore);
    return DIBuilder.insertDbgAddrIntrinsic(Addr, VarInfo, Expr, DL,
                                            InsertBefore);
  }

  llvm::Instruction *insert(llvm::Value *Addr, llvm::DILocalVariable *VarInfo,
                            llvm::DIExpression *Expr,
                            const llvm::DILocation *DL,
                            llvm::BasicBlock *Block) {
    if (ForceDbgDeclare == AddrDbgInstrKind::DbgDeclare)
      return DIBuilder.insertDeclare(Addr, VarInfo, Expr, DL, Block);
    return DIBuilder.insertDbgAddrIntrinsic(Addr, VarInfo, Expr, DL, Block);
  }
};

} // namespace

void IRGenDebugInfoImpl::emitDbgIntrinsic(
    IRBuilder &Builder, llvm::Value *Storage, llvm::DILocalVariable *Var,
    llvm::DIExpression *Expr, unsigned Line, unsigned Col,
    llvm::DILocalScope *Scope, const SILDebugScope *DS, bool InCoroContext,
    AddrDbgInstrKind AddrDInstKind) {
  Storage = Storage->stripPointerCasts();
  // Set the location/scope of the intrinsic.
  auto *InlinedAt = createInlinedAt(DS);
  auto DL =
      llvm::DILocation::get(IGM.getLLVMContext(), Line, Col, Scope, InlinedAt);

  // An alloca may only be described by exactly one dbg.declare.
  if (isa<llvm::AllocaInst>(Storage) &&
      !llvm::FindDbgDeclareUses(Storage).empty())
    return;

  // Fragment DIExpression cannot cover the whole variable
  // or going out-of-bound.
  if (auto Fragment = Expr->getFragmentInfo()) {
    if (auto VarSize = Var->getSizeInBits()) {
      unsigned FragSize = Fragment->SizeInBits;
      unsigned FragOffset = Fragment->OffsetInBits;
      if (FragOffset + FragSize > *VarSize || FragSize == *VarSize) {
        // Drop the fragment part
        assert(Expr->isValid());
        // Since this expression is valid, DW_OP_LLVM_fragment
        // and its arguments must be the last 3 elements.
        auto OrigElements = Expr->getElements();
        Expr = DBuilder.createExpression(OrigElements.drop_back(3));
      }
    }
  }

  auto *ParentBlock = Builder.GetInsertBlock();

  // First before we do anything, check if we have an Undef. In this case, we
  // /always/ emit an llvm.dbg.value of undef.
  // If we have undef, always emit a llvm.dbg.value in the current position.
  if (isa<llvm::UndefValue>(Storage)) {
    DBuilder.insertDbgValueIntrinsic(Storage, Var, Expr, DL, ParentBlock);
    return;
  }

  DbgIntrinsicEmitter inserter{Builder, DBuilder, AddrDInstKind};

  // If we have a single alloca...
  if (auto *Alloca = dyn_cast<llvm::AllocaInst>(Storage)) {
    auto InsertBefore = Builder.GetInsertPoint();

    if (AddrDInstKind == AddrDbgInstrKind::DbgDeclare) {
      ParentBlock = Alloca->getParent();
      InsertBefore = std::next(Alloca->getIterator());
    }

    if (InsertBefore != ParentBlock->end()) {
      inserter.insert(Alloca, Var, Expr, DL, &*InsertBefore);
    } else {
      inserter.insert(Alloca, Var, Expr, DL, ParentBlock);
    }
    return;
  }

  if ((isa<llvm::IntrinsicInst>(Storage) &&
       cast<llvm::IntrinsicInst>(Storage)->getIntrinsicID() ==
           llvm::Intrinsic::coro_alloca_get)) {
    inserter.insert(Storage, Var, Expr, DL, ParentBlock);
    return;
  }

  if (InCoroContext) {
    PointerUnion<llvm::BasicBlock *, llvm::Instruction *> InsertPt;

    // If we have a dbg.declare, we are relying on a contract with the coroutine
    // splitter that in split coroutines we always create debug info for values
    // in the coroutine context by creating a llvm.dbg.declare for the variable
    // in the entry block of each funclet.
    if (AddrDInstKind == AddrDbgInstrKind::DbgDeclare) {
      // Function arguments in async functions are emitted without a shadow copy
      // (that would interfere with coroutine splitting) but with a
      // llvm.dbg.declare to give CoroSplit.cpp license to emit a shadow copy
      // for them pointing inside the Swift Context argument that is valid
      // throughout the function.
      auto &EntryBlock = ParentBlock->getParent()->getEntryBlock();
      if (auto *InsertBefore = &*EntryBlock.getFirstInsertionPt()) {
        InsertPt = InsertBefore;
      } else {
        InsertPt = &EntryBlock;
      }
    } else {
      // For llvm.dbg.addr, we just want to insert the intrinsic at the current
      // insertion point. This is because our contract with the coroutine
      // splitter is that the coroutine splitter just needs to emit the
      // llvm.dbg.addr where we placed them. It shouldn't move them or do
      // anything special with it. Instead, we have previously inserted extra
      // debug_value clones previously after each instruction at the SIL level
      // that corresponds with a funclet edge. This operation effectively sets
      // up the rest of the pipeline to be stupid and just emit the
      // llvm.dbg.addr in the correct places. This is done by the SILOptimizer
      // pass DebugInfoCanonicalizer.
      auto InsertBefore = Builder.GetInsertPoint();
      if (InsertBefore != ParentBlock->end()) {
        InsertPt = &*InsertBefore;
      } else {
        InsertPt = ParentBlock;
      }
    }

    // Ok, we now have our insert pt. Call the appropriate operations.
    assert(InsertPt);
    if (auto *InsertBefore = InsertPt.dyn_cast<llvm::Instruction *>()) {
      inserter.insert(Storage, Var, Expr, DL, InsertBefore);
    } else {
      inserter.insert(Storage, Var, Expr, DL,
                      InsertPt.get<llvm::BasicBlock *>());
    }
    return;
  }

  // Insert a dbg.value at the current insertion point.
  if (isa<llvm::Argument>(Storage) && !Var->getArg() &&
      ParentBlock->getFirstNonPHIOrDbg())
    // SelectionDAGISel only generates debug info for a dbg.value
    // that is associated with a llvm::Argument if either its !DIVariable
    // is marked as argument or there is no non-debug intrinsic instruction
    // before it. So In the case of associating a llvm::Argument with a
    // non-argument debug variable -- usually via a !DIExpression -- we
    // need to make sure that dbg.value is before any non-phi / no-dbg
    // instruction.
    DBuilder.insertDbgValueIntrinsic(Storage, Var, Expr, DL,
                                     ParentBlock->getFirstNonPHIOrDbg());
  else
    DBuilder.insertDbgValueIntrinsic(Storage, Var, Expr, DL, ParentBlock);
}

void IRGenDebugInfoImpl::emitGlobalVariableDeclaration(
    llvm::GlobalVariable *Var, StringRef Name, StringRef LinkageName,
    DebugTypeInfo DbgTy, bool IsLocalToUnit, llvm::Optional<SILLocation> Loc) {
  if (Opts.DebugInfoLevel <= IRGenDebugInfoLevel::LineTables)
    return;

  if (swift::TypeBase *ty = DbgTy.getType()) {
    if (MetatypeType *metaTy = dyn_cast<MetatypeType>(ty))
      ty = metaTy->getInstanceType().getPointer();
  }

  llvm::DIType *DITy = getOrCreateType(DbgTy);
  VarDecl *VD = nullptr;
  if (Loc)
    VD = dyn_cast_or_null<VarDecl>(Loc->getAsASTNode<Decl>());
  if (!VD || VD->isLet())
    DITy = DBuilder.createQualifiedType(llvm::dwarf::DW_TAG_const_type, DITy);

  if (DITy->isArtificial() || DITy == InternalType || !Loc)
    // FIXME: Really these should be marked as artificial, but LLVM
    // currently has no support for flags to be put on global
    // variables. In the mean time, elide these variables, they
    // would confuse both the user and LLDB.
    return;

  if (DbgTy.isFixedBuffer())
    DITy = createFixedValueBufferStruct(DITy);

  auto L = getStartLocation(Loc);
  auto File = getOrCreateFile(L.filename);

  // Emit it as global variable of the current module.
  llvm::DIExpression *Expr = nullptr;
  if (!Var)
    Expr = DBuilder.createConstantValueExpression(0);
  auto *GV = DBuilder.createGlobalVariableExpression(
      MainModule, Name, LinkageName, File, L.line, DITy, IsLocalToUnit, true,
      Expr);
  if (Var)
    Var->addDebugInfo(GV);
}

void IRGenDebugInfoImpl::emitTypeMetadata(IRGenFunction &IGF,
                                          llvm::Value *Metadata, unsigned Depth,
                                          unsigned Index, StringRef Name) {
  if (Opts.DebugInfoLevel <= IRGenDebugInfoLevel::LineTables)
    return;

  // Don't emit debug info in transparent functions.
  auto *DS = IGF.getDebugScope();
  if (!DS || DS->getInlinedFunction()->isTransparent())
    return;

  llvm::SmallString<8> Buf;
  static const char *Tau = u8"\u03C4";
  llvm::raw_svector_ostream OS(Buf);
  OS << '$' << Tau << '_' << Depth << '_' << Index;
  uint64_t PtrWidthInBits = CI.getTargetInfo().getPointerWidth(0);
  assert(PtrWidthInBits % 8 == 0);
  auto DbgTy = DebugTypeInfo::getTypeMetadata(
      getMetadataType(Name)->getDeclaredInterfaceType().getPointer(),
      Metadata->getType(), Size(PtrWidthInBits / 8),
      Alignment(CI.getTargetInfo().getPointerAlign(0)));
  emitVariableDeclaration(IGF.Builder, Metadata, DbgTy, IGF.getDebugScope(),
                          {}, {OS.str().str(), 0, false},
                          // swift.type is already a pointer type,
                          // having a shadow copy doesn't add another
                          // layer of indirection.
                          IGF.isAsync() ? CoroDirectValue : DirectValue,
                          ArtificialValue);
}

void IRGenDebugInfoImpl::emitPackCountParameter(IRGenFunction &IGF,
                                                llvm::Value *Metadata,
                                                SILDebugVariable VarInfo) {
  if (Opts.DebugInfoLevel <= IRGenDebugInfoLevel::LineTables)
    return;

  // Don't emit debug info in transparent functions.
  auto *DS = IGF.getDebugScope();
  if (!DS || DS->getInlinedFunction()->isTransparent())
    return;

  Type IntTy = BuiltinIntegerType::get(CI.getTargetInfo().getPointerWidth(0),
                                       IGM.getSwiftModule()->getASTContext());
  auto &TI = IGM.getTypeInfoForUnlowered(IntTy);
  auto DbgTy = *CompletedDebugTypeInfo::getFromTypeInfo(IntTy, TI, IGM);
  emitVariableDeclaration(
      IGF.Builder, Metadata, DbgTy, IGF.getDebugScope(), {}, VarInfo,
      IGF.isAsync() ? CoroDirectValue : DirectValue, ArtificialValue);
}

SILLocation::FilenameAndLocation
IRGenDebugInfoImpl::decodeSourceLoc(SourceLoc SL) {
  auto &Cached = FilenameAndLocationCache[SL.getOpaquePointerValue()];
  if (Cached.filename.empty()) {
    Cached = sanitizeCodeViewFilenameAndLocation(
        SILLocation::decode(SL, SM, /*ForceGeneratedSourceToDisk=*/true));
  }
  return Cached;
}

} // anonymous namespace

std::unique_ptr<IRGenDebugInfo> IRGenDebugInfo::createIRGenDebugInfo(
    const IRGenOptions &Opts, ClangImporter &CI, IRGenModule &IGM,
    llvm::Module &M, StringRef MainOutputFilenameForDebugInfo,
    StringRef PrivateDiscriminator) {
  return std::make_unique<IRGenDebugInfoImpl>(
      Opts, CI, IGM, M, MainOutputFilenameForDebugInfo, PrivateDiscriminator);
}

IRGenDebugInfo::~IRGenDebugInfo() {}

// Forwarding to the private implementation.
void IRGenDebugInfo::finalize() {
  static_cast<IRGenDebugInfoImpl *>(this)->finalize();
}

void IRGenDebugInfo::setCurrentLoc(IRBuilder &Builder, const SILDebugScope *DS,
                                   SILLocation Loc) {
  static_cast<IRGenDebugInfoImpl *>(this)->setCurrentLoc(Builder, DS, Loc);
}

void IRGenDebugInfo::addFailureMessageToCurrentLoc(IRBuilder &Builder,
                                                   StringRef failureMsg) {
  static_cast<IRGenDebugInfoImpl *>(this)->addFailureMessageToCurrentLoc(
      Builder, failureMsg);
}

void IRGenDebugInfo::clearLoc(IRBuilder &Builder) {
  static_cast<IRGenDebugInfoImpl *>(this)->clearLoc(Builder);
}

void IRGenDebugInfo::pushLoc() {
  static_cast<IRGenDebugInfoImpl *>(this)->pushLoc();
}

void IRGenDebugInfo::popLoc() {
  static_cast<IRGenDebugInfoImpl *>(this)->popLoc();
}

void IRGenDebugInfo::setInlinedTrapLocation(IRBuilder &Builder,
                                            const SILDebugScope *Scope) {
  static_cast<IRGenDebugInfoImpl *>(this)->setInlinedTrapLocation(Builder,
                                                                  Scope);
}

void IRGenDebugInfo::setEntryPointLoc(IRBuilder &Builder) {
  static_cast<IRGenDebugInfoImpl *>(this)->setEntryPointLoc(Builder);
}

llvm::DIScope *IRGenDebugInfo::getEntryPointFn() {
  return static_cast<IRGenDebugInfoImpl *>(this)->getEntryPointFn();
}

llvm::DIScope *IRGenDebugInfo::getOrCreateScope(const SILDebugScope *DS) {
  return static_cast<IRGenDebugInfoImpl *>(this)->getOrCreateScope(DS);
}

void IRGenDebugInfo::emitImport(ImportDecl *D) {
  static_cast<IRGenDebugInfoImpl *>(this)->emitImport(D);
}

llvm::DISubprogram *
IRGenDebugInfo::emitFunction(const SILDebugScope *DS, llvm::Function *Fn,
                             SILFunctionTypeRepresentation Rep, SILType Ty,
                             DeclContext *DeclCtx, GenericEnvironment *GE) {
  return static_cast<IRGenDebugInfoImpl *>(this)->emitFunction(DS, Fn, Rep, Ty,
                                                               DeclCtx);
}

llvm::DISubprogram *IRGenDebugInfo::emitFunction(SILFunction &SILFn,
                                                 llvm::Function *Fn) {
  return static_cast<IRGenDebugInfoImpl *>(this)->emitFunction(SILFn, Fn);
}

void IRGenDebugInfo::emitArtificialFunction(IRBuilder &Builder,
                                            llvm::Function *Fn, SILType SILTy) {
  static_cast<IRGenDebugInfoImpl *>(this)->emitArtificialFunction(Builder, Fn,
                                                                  SILTy);
}
void IRGenDebugInfo::emitOutlinedFunction(IRBuilder &Builder,
                                          llvm::Function *Fn, StringRef name) {
  static_cast<IRGenDebugInfoImpl *>(this)->emitOutlinedFunction(Builder, Fn,
                                                                name);
}
void IRGenDebugInfo::emitVariableDeclaration(
    IRBuilder &Builder, ArrayRef<llvm::Value *> Storage, DebugTypeInfo Ty,
    const SILDebugScope *DS, llvm::Optional<SILLocation> VarLoc,
    SILDebugVariable VarInfo, IndirectionKind Indirection,
    ArtificialKind Artificial, AddrDbgInstrKind AddrDInstKind) {
  static_cast<IRGenDebugInfoImpl *>(this)->emitVariableDeclaration(
      Builder, Storage, Ty, DS, VarLoc, VarInfo, Indirection, Artificial,
      AddrDInstKind);
}

void IRGenDebugInfo::emitDbgIntrinsic(IRBuilder &Builder, llvm::Value *Storage,
                                      llvm::DILocalVariable *Var,
                                      llvm::DIExpression *Expr, unsigned Line,
                                      unsigned Col, llvm::DILocalScope *Scope,
                                      const SILDebugScope *DS,
                                      bool InCoroContext,
                                      AddrDbgInstrKind AddrDInstKind) {
  static_cast<IRGenDebugInfoImpl *>(this)->emitDbgIntrinsic(
      Builder, Storage, Var, Expr, Line, Col, Scope, DS, InCoroContext,
      AddrDInstKind);
}

void IRGenDebugInfo::emitGlobalVariableDeclaration(
    llvm::GlobalVariable *Storage, StringRef Name, StringRef LinkageName,
    DebugTypeInfo DebugType, bool IsLocalToUnit,
    llvm::Optional<SILLocation> Loc) {
  static_cast<IRGenDebugInfoImpl *>(this)->emitGlobalVariableDeclaration(
      Storage, Name, LinkageName, DebugType, IsLocalToUnit, Loc);
}

void IRGenDebugInfo::emitTypeMetadata(IRGenFunction &IGF, llvm::Value *Metadata,
                                      unsigned Depth, unsigned Index,
                                      StringRef Name) {
  static_cast<IRGenDebugInfoImpl *>(this)->emitTypeMetadata(IGF, Metadata,
                                                            Depth, Index, Name);
}

void IRGenDebugInfo::emitPackCountParameter(IRGenFunction &IGF,
                                            llvm::Value *Metadata,
                                            SILDebugVariable VarInfo) {
  static_cast<IRGenDebugInfoImpl *>(this)->emitPackCountParameter(IGF, Metadata,
                                                                  VarInfo);
}

llvm::DIBuilder &IRGenDebugInfo::getBuilder() {
  return static_cast<IRGenDebugInfoImpl *>(this)->getBuilder();
}

SILLocation::FilenameAndLocation IRGenDebugInfo::decodeSourceLoc(SourceLoc SL) {
  return static_cast<IRGenDebugInfoImpl *>(this)->decodeSourceLoc(SL);
}

AutoRestoreLocation::AutoRestoreLocation(IRGenDebugInfo *DI, IRBuilder &Builder)
    : DI(DI), Builder(Builder) {
  if (DI)
    SavedLocation = Builder.getCurrentDebugLocation();
}

/// Autorestore everything back to normal.
AutoRestoreLocation::~AutoRestoreLocation() {
  if (DI)
    Builder.SetCurrentDebugLocation(SavedLocation);
}

ArtificialLocation::ArtificialLocation(const SILDebugScope *DS,
                                       IRGenDebugInfo *DI, IRBuilder &Builder)
    : AutoRestoreLocation(DI, Builder) {
  if (DI) {
    unsigned Line = 0;
    auto *Scope = DI->getOrCreateScope(DS);
    auto DII = static_cast<IRGenDebugInfoImpl *>(DI);
    if (DII->getDebugInfoFormat() == IRGenDebugInfoFormat::CodeView) {
      // In CodeView, line zero is not an artificial line location and so we
      // try to use the location of the scope.
      if (auto *LB = dyn_cast<llvm::DILexicalBlock>(Scope))
        Line = LB->getLine();
      else if (auto *SP = dyn_cast<llvm::DISubprogram>(Scope))
        Line = SP->getLine();
    }
    auto DL = llvm::DILocation::get(Scope->getContext(), Line, 0, Scope,
                                    DII->createInlinedAt(DS));
    Builder.SetCurrentDebugLocation(DL);
  }
}

PrologueLocation::PrologueLocation(IRGenDebugInfo *DI, IRBuilder &Builder)
    : AutoRestoreLocation(DI, Builder) {
  if (DI)
    DI->clearLoc(Builder);
}
