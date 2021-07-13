//===--- Module.cpp - Swift Language Module Implementation ----------------===//
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
//  This file implements the Module class and subclasses.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Module.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/AccessScope.h"
#include "swift/AST/Builtins.h"
#include "swift/AST/ClangModuleLoader.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/FileUnit.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/ImportCache.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/LinkLibrary.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/ParseRequests.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/PrintOptions.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/SynthesizedFileUnit.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Compiler.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Statistic.h"
#include "swift/Demangling/ManglingMacros.h"
#include "swift/Parse/Token.h"
#include "swift/Strings.h"
#include "swift/Syntax/SyntaxNodes.h"
#include "clang/Basic/Module.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;

static_assert(IsTriviallyDestructible<FileUnit>::value,
              "FileUnits are BumpPtrAllocated; the d'tor may not be called");
static_assert(IsTriviallyDestructible<LoadedFile>::value,
              "LoadedFiles are BumpPtrAllocated; the d'tor may not be called");

//===----------------------------------------------------------------------===//
// Builtin Module Name lookup
//===----------------------------------------------------------------------===//

class BuiltinUnit::LookupCache {
  /// The cache of identifiers we've already looked up.  We use a
  /// single hashtable for both types and values as a minor
  /// optimization; this prevents us from having both a builtin type
  /// and a builtin value with the same name, but that's okay.
  llvm::DenseMap<Identifier, ValueDecl*> Cache;

public:
  void lookupValue(Identifier Name, NLKind LookupKind, const BuiltinUnit &M,
                   SmallVectorImpl<ValueDecl*> &Result);
};

BuiltinUnit::LookupCache &BuiltinUnit::getCache() const {
  // FIXME: This leaks. Sticking this into ASTContext isn't enough because then
  // the DenseMap will leak.
  if (!Cache)
    const_cast<BuiltinUnit *>(this)->Cache = std::make_unique<LookupCache>();
  return *Cache;
}

void BuiltinUnit::LookupCache::lookupValue(
       Identifier Name, NLKind LookupKind, const BuiltinUnit &M,
       SmallVectorImpl<ValueDecl*> &Result) {
  // Only qualified lookup ever finds anything in the builtin module.
  if (LookupKind != NLKind::QualifiedLookup) return;

  ValueDecl *&Entry = Cache[Name];
  ASTContext &Ctx = M.getParentModule()->getASTContext();
  if (!Entry) {
    if (Type Ty = getBuiltinType(Ctx, Name.str())) {
      auto *TAD = new (Ctx) TypeAliasDecl(SourceLoc(), SourceLoc(),
                                          Name, SourceLoc(),
                                          /*genericparams*/nullptr,
                                          const_cast<BuiltinUnit*>(&M));
      TAD->setUnderlyingType(Ty);
      TAD->setAccess(AccessLevel::Public);
      Entry = TAD;
    }
  }

  if (!Entry)
    Entry = getBuiltinValueDecl(Ctx, Name);

  if (Entry)
    Result.push_back(Entry);
}

// Out-of-line because std::unique_ptr wants LookupCache to be complete.
BuiltinUnit::BuiltinUnit(ModuleDecl &M)
   : FileUnit(FileUnitKind::Builtin, M) {
  M.getASTContext().addDestructorCleanup(*this);
}

//===----------------------------------------------------------------------===//
// Normal Module Name Lookup
//===----------------------------------------------------------------------===//

SourceFile::~SourceFile() = default;

/// A utility for caching global lookups into SourceFiles and modules of
/// SourceFiles. This is used for lookup of top-level declarations, as well
/// as operator lookup (which looks into types) and AnyObject dynamic lookup
/// (which looks at all class members).
class swift::SourceLookupCache {
  /// A lookup map for value decls. When declarations are added they are added
  /// under all variants of the name they can be found under.
  class ValueDeclMap {
    llvm::DenseMap<DeclName, TinyPtrVector<ValueDecl *>> Members;

  public:
    void add(ValueDecl *VD) {
      if (!VD->hasName()) return;
      VD->getName().addToLookupTable(Members, VD);
    }

    void clear() {
      Members.shrink_and_clear();
    }

    decltype(Members)::const_iterator begin() const { return Members.begin(); }
    decltype(Members)::const_iterator end() const { return Members.end(); }
    decltype(Members)::const_iterator find(DeclName Name) const {
      return Members.find(Name);
    }
  };

  ValueDeclMap TopLevelValues;
  ValueDeclMap ClassMembers;
  bool MemberCachePopulated = false;

  template<typename T>
  using OperatorMap = llvm::DenseMap<Identifier, TinyPtrVector<T *>>;
  OperatorMap<OperatorDecl> Operators;
  OperatorMap<PrecedenceGroupDecl> PrecedenceGroups;

  template<typename Range>
  void addToUnqualifiedLookupCache(Range decls, bool onlyOperators);
  template<typename Range>
  void addToMemberCache(Range decls);
public:
  SourceLookupCache(const SourceFile &SF);
  SourceLookupCache(const ModuleDecl &Mod);

  /// Throw away as much memory as possible.
  void invalidate();

  void lookupValue(DeclName Name, NLKind LookupKind,
                   SmallVectorImpl<ValueDecl*> &Result);

  /// Retrieves all the operator decls. The order of the results is not
  /// guaranteed to be meaningful.
  void getOperatorDecls(SmallVectorImpl<OperatorDecl *> &results);

  /// Retrieves all the precedence groups. The order of the results is not
  /// guaranteed to be meaningful.
  void getPrecedenceGroups(SmallVectorImpl<PrecedenceGroupDecl *> &results);

  /// Look up an operator declaration.
  ///
  /// \param name The operator name ("+", ">>", etc.)
  /// \param fixity The fixity of the operator (infix, prefix or postfix).
  void lookupOperator(Identifier name, OperatorFixity fixity,
                      TinyPtrVector<OperatorDecl *> &results);

  /// Look up a precedence group.
  ///
  /// \param name The operator name ("+", ">>", etc.)
  void lookupPrecedenceGroup(Identifier name,
                             TinyPtrVector<PrecedenceGroupDecl *> &results);

  void lookupVisibleDecls(ImportPath::Access AccessPath,
                          VisibleDeclConsumer &Consumer,
                          NLKind LookupKind);

  void populateMemberCache(const SourceFile &SF);
  void populateMemberCache(const ModuleDecl &Mod);

  void lookupClassMembers(ImportPath::Access AccessPath,
                          VisibleDeclConsumer &consumer);

  void lookupClassMember(ImportPath::Access accessPath,
                         DeclName name,
                         SmallVectorImpl<ValueDecl*> &results);

  SmallVector<ValueDecl *, 0> AllVisibleValues;
};

SourceLookupCache &SourceFile::getCache() const {
  if (!Cache) {
    const_cast<SourceFile *>(this)->Cache =
        std::make_unique<SourceLookupCache>(*this);
  }
  return *Cache;
}

template<typename Range>
void SourceLookupCache::addToUnqualifiedLookupCache(Range decls,
                                                    bool onlyOperators) {
  for (Decl *D : decls) {
    if (auto *VD = dyn_cast<ValueDecl>(D)) {
      if (onlyOperators ? VD->isOperator() : VD->hasName()) {
        // Cache the value under both its compound name and its full name.
        TopLevelValues.add(VD);
      }
    }

    if (auto *NTD = dyn_cast<NominalTypeDecl>(D))
      if (!NTD->hasUnparsedMembers() || NTD->maybeHasOperatorDeclarations())
        addToUnqualifiedLookupCache(NTD->getMembers(), true);

    if (auto *ED = dyn_cast<ExtensionDecl>(D)) {
      // Avoid populating the cache with the members of invalid extension
      // declarations.  These members can be used to point validation inside of
      // a malformed context.
      if (ED->isInvalid()) continue;

      if (!ED->hasUnparsedMembers() || ED->maybeHasOperatorDeclarations())
        addToUnqualifiedLookupCache(ED->getMembers(), true);
    }

    if (auto *OD = dyn_cast<OperatorDecl>(D))
      Operators[OD->getName()].push_back(OD);

    if (auto *PG = dyn_cast<PrecedenceGroupDecl>(D))
      PrecedenceGroups[PG->getName()].push_back(PG);
  }
}

void SourceLookupCache::populateMemberCache(const SourceFile &SF) {
  if (MemberCachePopulated)
    return;

  FrontendStatsTracer tracer(SF.getASTContext().Stats,
                             "populate-source-file-class-member-cache");
  addToMemberCache(SF.getTopLevelDecls());
  MemberCachePopulated = true;
}

void SourceLookupCache::populateMemberCache(const ModuleDecl &Mod) {
  if (MemberCachePopulated)
    return;

  FrontendStatsTracer tracer(Mod.getASTContext().Stats,
                             "populate-module-class-member-cache");

  for (const FileUnit *file : Mod.getFiles()) {
    auto &SF = *cast<SourceFile>(file);
    addToMemberCache(SF.getTopLevelDecls());
  }

  MemberCachePopulated = true;
}

template <typename Range>
void SourceLookupCache::addToMemberCache(Range decls) {
  for (Decl *D : decls) {
    if (auto *NTD = dyn_cast<NominalTypeDecl>(D)) {
      if (!NTD->hasUnparsedMembers() ||
          NTD->maybeHasNestedClassDeclarations() ||
          NTD->mayContainMembersAccessedByDynamicLookup())
        addToMemberCache(NTD->getMembers());

    } else if (auto *ED = dyn_cast<ExtensionDecl>(D)) {
      if (!ED->hasUnparsedMembers() ||
          ED->maybeHasNestedClassDeclarations() ||
          ED->mayContainMembersAccessedByDynamicLookup())
        addToMemberCache(ED->getMembers());

    } else if (auto *VD = dyn_cast<ValueDecl>(D)) {
      if (VD->canBeAccessedByDynamicLookup())
        ClassMembers.add(VD);
    }
  }
}

/// Populate our cache on the first name lookup.
SourceLookupCache::SourceLookupCache(const SourceFile &SF) {
  FrontendStatsTracer tracer(SF.getASTContext().Stats,
                             "source-file-populate-cache");
  addToUnqualifiedLookupCache(SF.getTopLevelDecls(), false);
  addToUnqualifiedLookupCache(SF.getHoistedDecls(), false);
}

SourceLookupCache::SourceLookupCache(const ModuleDecl &M) {
  FrontendStatsTracer tracer(M.getASTContext().Stats,
                             "module-populate-cache");
  for (const FileUnit *file : M.getFiles()) {
    if (auto *SFU = dyn_cast<SynthesizedFileUnit>(file)) {
      addToUnqualifiedLookupCache(SFU->getTopLevelDecls(), false);
      continue;
    }
    auto *SF = cast<SourceFile>(file);
    addToUnqualifiedLookupCache(SF->getTopLevelDecls(), false);
    addToUnqualifiedLookupCache(SF->getHoistedDecls(), false);
  }
}

void SourceLookupCache::lookupValue(DeclName Name, NLKind LookupKind,
                                    SmallVectorImpl<ValueDecl*> &Result) {
  auto I = TopLevelValues.find(Name);
  if (I == TopLevelValues.end()) return;

  Result.reserve(I->second.size());
  for (ValueDecl *Elt : I->second)
    Result.push_back(Elt);
}

void SourceLookupCache::getPrecedenceGroups(
    SmallVectorImpl<PrecedenceGroupDecl *> &results) {
  for (auto &groups : PrecedenceGroups)
    results.append(groups.second.begin(), groups.second.end());
}

void SourceLookupCache::getOperatorDecls(
    SmallVectorImpl<OperatorDecl *> &results) {
  for (auto &ops : Operators)
    results.append(ops.second.begin(), ops.second.end());
}

void SourceLookupCache::lookupOperator(Identifier name, OperatorFixity fixity,
                                       TinyPtrVector<OperatorDecl *> &results) {
  auto ops = Operators.find(name);
  if (ops == Operators.end())
    return;

  for (auto *op : ops->second)
    if (op->getFixity() == fixity)
      results.push_back(op);
}

void SourceLookupCache::lookupPrecedenceGroup(
    Identifier name, TinyPtrVector<PrecedenceGroupDecl *> &results) {
  auto groups = PrecedenceGroups.find(name);
  if (groups == PrecedenceGroups.end())
    return;

  for (auto *group : groups->second)
    results.push_back(group);
}

void SourceLookupCache::lookupVisibleDecls(ImportPath::Access AccessPath,
                                           VisibleDeclConsumer &Consumer,
                                           NLKind LookupKind) {
  assert(AccessPath.size() <= 1 && "can only refer to top-level decls");

  if (!AccessPath.empty()) {
    auto I = TopLevelValues.find(AccessPath.front().Item);
    if (I == TopLevelValues.end()) return;

    for (auto vd : I->second)
      Consumer.foundDecl(vd, DeclVisibilityKind::VisibleAtTopLevel);
    return;
  }

  for (auto &tlv : TopLevelValues) {
    for (ValueDecl *vd : tlv.second) {
      // Declarations are added under their full and simple names.  Skip the
      // entry for the simple name so that we report each declaration once.
      if (tlv.first.isSimpleName() && !vd->getName().isSimpleName())
        continue;
      Consumer.foundDecl(vd, DeclVisibilityKind::VisibleAtTopLevel);
    }
  }
}

void SourceLookupCache::lookupClassMembers(ImportPath::Access accessPath,
                                           VisibleDeclConsumer &consumer) {
  assert(accessPath.size() <= 1 && "can only refer to top-level decls");

  if (!accessPath.empty()) {
    for (auto &member : ClassMembers) {
      // Non-simple names are also stored under their simple name, so make
      // sure to only report them once.
      if (!member.first.isSimpleName())
        continue;

      for (ValueDecl *vd : member.second) {
        auto *nominal = vd->getDeclContext()->getSelfNominalTypeDecl();
        if (nominal && nominal->getName() == accessPath.front().Item)
          consumer.foundDecl(vd, DeclVisibilityKind::DynamicLookup,
                             DynamicLookupInfo::AnyObject);
      }
    }
    return;
  }

  for (auto &member : ClassMembers) {
    // Non-simple names are also stored under their simple name, so make sure to
    // only report them once.
    if (!member.first.isSimpleName())
      continue;

    for (ValueDecl *vd : member.second)
      consumer.foundDecl(vd, DeclVisibilityKind::DynamicLookup,
                         DynamicLookupInfo::AnyObject);
  }
}

void SourceLookupCache::lookupClassMember(ImportPath::Access accessPath,
                                          DeclName name,
                                          SmallVectorImpl<ValueDecl*> &results) {
  assert(accessPath.size() <= 1 && "can only refer to top-level decls");

  auto iter = ClassMembers.find(name);
  if (iter == ClassMembers.end())
    return;

  if (!accessPath.empty()) {
    for (ValueDecl *vd : iter->second) {
      auto *nominal = vd->getDeclContext()->getSelfNominalTypeDecl();
      if (nominal && nominal->getName() == accessPath.front().Item)
        results.push_back(vd);
    }
    return;
  }

  results.append(iter->second.begin(), iter->second.end());
}

void SourceLookupCache::invalidate() {
  TopLevelValues.clear();
  ClassMembers.clear();
  MemberCachePopulated = false;

  // std::move AllVisibleValues into a temporary to destroy its contents.
  using SameSizeSmallVector = decltype(AllVisibleValues);
  (void)SameSizeSmallVector{std::move(AllVisibleValues)};
}

//===----------------------------------------------------------------------===//
// Module Implementation
//===----------------------------------------------------------------------===//

ModuleDecl::ModuleDecl(Identifier name, ASTContext &ctx,
                       ImplicitImportInfo importInfo)
    : DeclContext(DeclContextKind::Module, nullptr),
      TypeDecl(DeclKind::Module, &ctx, name, SourceLoc(), {}),
      ImportInfo(importInfo) {

  ctx.addDestructorCleanup(*this);
  setImplicit();
  setInterfaceType(ModuleType::get(this));

  setAccess(AccessLevel::Public);

  Bits.ModuleDecl.StaticLibrary = 0;
  Bits.ModuleDecl.TestingEnabled = 0;
  Bits.ModuleDecl.FailedToLoad = 0;
  Bits.ModuleDecl.RawResilienceStrategy = 0;
  Bits.ModuleDecl.HasResolvedImports = 0;
  Bits.ModuleDecl.PrivateImportsEnabled = 0;
  Bits.ModuleDecl.ImplicitDynamicEnabled = 0;
  Bits.ModuleDecl.IsSystemModule = 0;
  Bits.ModuleDecl.IsNonSwiftModule = 0;
  Bits.ModuleDecl.IsMainModule = 0;
  Bits.ModuleDecl.HasIncrementalInfo = 0;
}

ImplicitImportList ModuleDecl::getImplicitImports() const {
  auto &evaluator = getASTContext().evaluator;
  auto *mutableThis = const_cast<ModuleDecl *>(this);
  return evaluateOrDefault(evaluator, ModuleImplicitImportsRequest{mutableThis},
                           {});
}

void ModuleDecl::addFile(FileUnit &newFile) {
  // If this is a LoadedFile, make sure it loaded without error.
  assert(!(isa<LoadedFile>(newFile) &&
           cast<LoadedFile>(newFile).hadLoadError()));

  // Require Main and REPL files to be the first file added.
  assert(Files.empty() ||
         !isa<SourceFile>(newFile) ||
         cast<SourceFile>(newFile).Kind == SourceFileKind::Library ||
         cast<SourceFile>(newFile).Kind == SourceFileKind::SIL);
  Files.push_back(&newFile);
  clearLookupCache();
}

ArrayRef<SourceFile *>
PrimarySourceFilesRequest::evaluate(Evaluator &evaluator,
                                    ModuleDecl *mod) const {
  assert(mod->isMainModule() && "Only the main module can have primaries");

  SmallVector<SourceFile *, 8> primaries;
  for (auto *file : mod->getFiles()) {
    if (auto *SF = dyn_cast<SourceFile>(file)) {
      if (SF->isPrimary())
        primaries.push_back(SF);
    }
  }
  return mod->getASTContext().AllocateCopy(primaries);
}

ArrayRef<SourceFile *> ModuleDecl::getPrimarySourceFiles() const {
  auto &eval = getASTContext().evaluator;
  auto *mutableThis = const_cast<ModuleDecl *>(this);
  return evaluateOrDefault(eval, PrimarySourceFilesRequest{mutableThis}, {});
}

SourceFile *CodeCompletionFileRequest::evaluate(Evaluator &evaluator,
                                                ModuleDecl *mod) const {
  const auto &SM = mod->getASTContext().SourceMgr;
  assert(mod->isMainModule() && "Can only do completion in the main module");
  assert(SM.hasCodeCompletionBuffer() && "Not performing code completion?");

  for (auto *file : mod->getFiles()) {
    auto *SF = dyn_cast<SourceFile>(file);
    if (SF && SF->getBufferID() == SM.getCodeCompletionBufferID())
      return SF;
  }
  llvm_unreachable("Couldn't find the completion file?");
}

#define FORWARD(name, args) \
  for (const FileUnit *file : getFiles()) \
    file->name args;

SourceLookupCache &ModuleDecl::getSourceLookupCache() const {
  if (!Cache) {
    const_cast<ModuleDecl *>(this)->Cache =
        std::make_unique<SourceLookupCache>(*this);
  }
  return *Cache;
}

ModuleDecl *ModuleDecl::getTopLevelModule(bool overlay) {
  // If this is a Clang module, ask the Clang importer for the top-level module.
  // We need to check isNonSwiftModule() to ensure we don't look through
  // overlays.
  if (isNonSwiftModule()) {
    if (auto *underlying = findUnderlyingClangModule()) {
      auto &ctx = getASTContext();
      auto *clangLoader = ctx.getClangModuleLoader();
      return clangLoader->getWrapperForModule(underlying->getTopLevelModule(),
                                              overlay);
    }
  }
  // Swift modules don't currently support submodules.
  return this;
}

static bool isParsedModule(const ModuleDecl *mod) {
  // FIXME: If we ever get mixed modules that contain both SourceFiles and other
  // kinds of file units, this will break; there all callers of this function should
  // themselves assert that all file units in the module are SourceFiles when this
  // function returns true.
  auto files = mod->getFiles();
  return (files.size() > 0 &&
          isa<SourceFile>(files[0]) &&
          cast<SourceFile>(files[0])->Kind != SourceFileKind::SIL);
}

void ModuleDecl::lookupValue(DeclName Name, NLKind LookupKind,
                             SmallVectorImpl<ValueDecl*> &Result) const {
  auto *stats = getASTContext().Stats;
  if (stats)
    ++stats->getFrontendCounters().NumModuleLookupValue;

  if (isParsedModule(this)) {
    getSourceLookupCache().lookupValue(Name, LookupKind, Result);
    return;
  }

  FORWARD(lookupValue, (Name, LookupKind, Result));
}

TypeDecl * ModuleDecl::lookupLocalType(StringRef MangledName) const {
  for (auto file : getFiles()) {
    auto TD = file->lookupLocalType(MangledName);
    if (TD)
      return TD;
  }
  return nullptr;
}

OpaqueTypeDecl *
ModuleDecl::lookupOpaqueResultType(StringRef MangledName) {
  for (auto file : getFiles()) {
    auto OTD = file->lookupOpaqueResultType(MangledName);
    if (OTD)
      return OTD;
  }
  return nullptr;
}

void ModuleDecl::lookupMember(SmallVectorImpl<ValueDecl*> &results,
                              DeclContext *container, DeclName name,
                              Identifier privateDiscriminator) const {
  size_t oldSize = results.size();
  bool alreadyInPrivateContext = false;

  auto containerDecl = container->getAsDecl();
  // If FileUnit, then use FileUnit::lookupValue instead.
  assert(containerDecl != nullptr && "This context does not support lookup.");

  if (auto nominal = dyn_cast<NominalTypeDecl>(containerDecl)) {
    auto lookupResults = nominal->lookupDirect(name);

    // Filter out declarations from other modules.
    llvm::copy_if(lookupResults,
                  std::back_inserter(results),
                  [this](const ValueDecl *VD) -> bool {
      return VD->getModuleContext() == this;
    });

    auto AS = nominal->getFormalAccessScope();
    if (AS.isPrivate() || AS.isFileScope())
      alreadyInPrivateContext = true;
  } else if (isa<ModuleDecl>(containerDecl)) {
    assert(container == this);
    this->lookupValue(name, NLKind::QualifiedLookup, results);
  } else if (!isa<GenericTypeDecl>(containerDecl)) {
    // If ExtensionDecl, then use ExtensionDecl::lookupDirect instead.
    llvm_unreachable("This context does not support lookup.");
  }

  // Filter by private-discriminator, or filter out private decls if there isn't
  // one...unless we're already in a private context, in which case everything
  // is private and a discriminator is unnecessary.
  if (alreadyInPrivateContext) {
    assert(privateDiscriminator.empty() && "unnecessary private discriminator");
    // Don't remove anything; everything here is private anyway.

  } else if (privateDiscriminator.empty()) {
    auto newEnd = std::remove_if(results.begin()+oldSize, results.end(),
                                 [](const ValueDecl *VD) -> bool {
      return VD->getFormalAccess() <= AccessLevel::FilePrivate;
    });
    results.erase(newEnd, results.end());

  } else {
    auto newEnd = std::remove_if(results.begin()+oldSize, results.end(),
                                 [=](const ValueDecl *VD) -> bool {
      if (VD->getFormalAccess() > AccessLevel::FilePrivate)
        return true;
      auto enclosingFile =
        cast<FileUnit>(VD->getDeclContext()->getModuleScopeContext());
      auto discriminator = enclosingFile->getDiscriminatorForPrivateValue(VD);
      return discriminator != privateDiscriminator;
    });
    results.erase(newEnd, results.end());
  }
}

void ModuleDecl::lookupObjCMethods(
       ObjCSelector selector,
       SmallVectorImpl<AbstractFunctionDecl *> &results) const {
  FORWARD(lookupObjCMethods, (selector, results));
}

void ModuleDecl::lookupImportedSPIGroups(
                        const ModuleDecl *importedModule,
                        llvm::SmallSetVector<Identifier, 4> &spiGroups) const {
  FORWARD(lookupImportedSPIGroups, (importedModule, spiGroups));
}

void BuiltinUnit::lookupValue(DeclName name, NLKind lookupKind,
                              SmallVectorImpl<ValueDecl*> &result) const {
  getCache().lookupValue(name.getBaseIdentifier(), lookupKind, *this, result);
}

void BuiltinUnit::lookupObjCMethods(
       ObjCSelector selector,
       SmallVectorImpl<AbstractFunctionDecl *> &results) const {
  // No @objc methods in the Builtin module.
}

void SourceFile::lookupValue(DeclName name, NLKind lookupKind,
                             SmallVectorImpl<ValueDecl*> &result) const {
  getCache().lookupValue(name, lookupKind, result);
}

void ModuleDecl::lookupVisibleDecls(ImportPath::Access AccessPath,
                                    VisibleDeclConsumer &Consumer,
                                    NLKind LookupKind) const {
  if (isParsedModule(this))
    return getSourceLookupCache().lookupVisibleDecls(
      AccessPath, Consumer, LookupKind);

  FORWARD(lookupVisibleDecls, (AccessPath, Consumer, LookupKind));
}

void SourceFile::lookupVisibleDecls(ImportPath::Access AccessPath,
                                    VisibleDeclConsumer &Consumer,
                                    NLKind LookupKind) const {
  getCache().lookupVisibleDecls(AccessPath, Consumer, LookupKind);
}

void ModuleDecl::lookupClassMembers(ImportPath::Access accessPath,
                                    VisibleDeclConsumer &consumer) const {
  if (isParsedModule(this)) {
    auto &cache = getSourceLookupCache();
    cache.populateMemberCache(*this);
    cache.lookupClassMembers(accessPath, consumer);
    return;
  }

  FORWARD(lookupClassMembers, (accessPath, consumer));
}

void SourceFile::lookupClassMembers(ImportPath::Access accessPath,
                                    VisibleDeclConsumer &consumer) const {
  auto &cache = getCache();
  cache.populateMemberCache(*this);
  cache.lookupClassMembers(accessPath, consumer);
}

void ModuleDecl::lookupClassMember(ImportPath::Access accessPath,
                                   DeclName name,
                                   SmallVectorImpl<ValueDecl*> &results) const {
  auto *stats = getASTContext().Stats;
  if (stats)
    ++stats->getFrontendCounters().NumModuleLookupClassMember;

  if (isParsedModule(this)) {
    FrontendStatsTracer tracer(getASTContext().Stats,
                               "source-file-lookup-class-member");
    auto &cache = getSourceLookupCache();
    cache.populateMemberCache(*this);
    cache.lookupClassMember(accessPath, name, results);
    return;
  }

  FORWARD(lookupClassMember, (accessPath, name, results));
}

void SourceFile::lookupClassMember(ImportPath::Access accessPath,
                                   DeclName name,
                                   SmallVectorImpl<ValueDecl*> &results) const {
  FrontendStatsTracer tracer(getASTContext().Stats,
                             "source-file-lookup-class-member");
  auto &cache = getCache();
  cache.populateMemberCache(*this);
  cache.lookupClassMember(accessPath, name, results);
}

void SourceFile::lookupObjCMethods(
       ObjCSelector selector,
       SmallVectorImpl<AbstractFunctionDecl *> &results) const {
  // FIXME: Make sure this table is complete, somehow.
  auto known = ObjCMethods.find(selector);
  if (known == ObjCMethods.end()) return;
  results.append(known->second.begin(), known->second.end());
}

void ModuleDecl::getLocalTypeDecls(SmallVectorImpl<TypeDecl*> &Results) const {
  FORWARD(getLocalTypeDecls, (Results));
}

void ModuleDecl::getTopLevelDecls(SmallVectorImpl<Decl*> &Results) const {
  FORWARD(getTopLevelDecls, (Results));
}

void ModuleDecl::getExportedPrespecializations(
    SmallVectorImpl<Decl *> &Results) const {
  FORWARD(getExportedPrespecializations, (Results));
}

void ModuleDecl::getTopLevelDeclsWhereAttributesMatch(
              SmallVectorImpl<Decl*> &Results,
              llvm::function_ref<bool(DeclAttributes)> matchAttributes) const {
  FORWARD(getTopLevelDeclsWhereAttributesMatch, (Results, matchAttributes));
}

void SourceFile::getTopLevelDecls(SmallVectorImpl<Decl*> &Results) const {
  auto decls = getTopLevelDecls();
  Results.append(decls.begin(), decls.end());
}

void ModuleDecl::getOperatorDecls(
    SmallVectorImpl<OperatorDecl *> &results) const {
  // For a parsed module, we can check the source cache on the module rather
  // than doing an O(N) search over the source files.
  if (isParsedModule(this)) {
    getSourceLookupCache().getOperatorDecls(results);
    return;
  }
  FORWARD(getOperatorDecls, (results));
}

void SourceFile::getOperatorDecls(
       SmallVectorImpl<OperatorDecl*> &results) const {
  getCache().getOperatorDecls(results);
}

void ModuleDecl::getPrecedenceGroups(
       SmallVectorImpl<PrecedenceGroupDecl*> &results) const {
  // For a parsed module, we can check the source cache on the module rather
  // than doing an O(N) search over the source files.
  if (isParsedModule(this)) {
    getSourceLookupCache().getPrecedenceGroups(results);
    return;
  }
  FORWARD(getPrecedenceGroups, (results));
}

void SourceFile::getPrecedenceGroups(
       SmallVectorImpl<PrecedenceGroupDecl*> &results) const {
  getCache().getPrecedenceGroups(results);
}

void SourceFile::getLocalTypeDecls(SmallVectorImpl<TypeDecl*> &Results) const {
  Results.append(LocalTypeDecls.begin(), LocalTypeDecls.end());
}

void
SourceFile::getOpaqueReturnTypeDecls(SmallVectorImpl<OpaqueTypeDecl*> &Results)
const {
  auto result = const_cast<SourceFile *>(this)->getOpaqueReturnTypeDecls();
  llvm::copy(result, std::back_inserter(Results));
}

TypeDecl *SourceFile::lookupLocalType(llvm::StringRef mangledName) const {
  ASTContext &ctx = getASTContext();
  for (auto typeDecl : LocalTypeDecls) {
    auto typeMangledName = evaluateOrDefault(ctx.evaluator,
                                             MangleLocalTypeDeclRequest { typeDecl },
                                             std::string());
    if (mangledName == typeMangledName)
      return typeDecl;
  }

  return nullptr;
}

Optional<ExternalSourceLocs::RawLocs>
SourceFile::getExternalRawLocsForDecl(const Decl *D) const {
  auto *FileCtx = D->getDeclContext()->getModuleScopeContext();
  assert(FileCtx == this && "D doesn't belong to this source file");
  if (FileCtx != this) {
    // D doesn't belong to this file. This shouldn't happen in practice.
    return None;
  }

  SourceLoc Loc = D->getLoc(/*SerializedOK=*/false);
  if (Loc.isInvalid())
    return None;

  SourceManager &SM = getASTContext().SourceMgr;
  auto BufferID = SM.findBufferContainingLoc(Loc);

  ExternalSourceLocs::RawLocs Result;
  auto setLoc = [&](ExternalSourceLocs::RawLoc &RawLoc, SourceLoc Loc) {
    if (!Loc.isValid())
      return;

    RawLoc.Offset = SM.getLocOffsetInBuffer(Loc, BufferID);
    std::tie(RawLoc.Line, RawLoc.Column) = SM.getLineAndColumnInBuffer(Loc);

    auto *VF = SM.getVirtualFile(Loc);
    if (!VF)
      return;

    RawLoc.Directive.Offset =
        SM.getLocOffsetInBuffer(VF->Range.getStart(), BufferID);
    RawLoc.Directive.LineOffset = VF->LineOffset;
    RawLoc.Directive.Length = VF->Range.getByteLength();
    RawLoc.Directive.Name = StringRef(VF->Name);
  };

  Result.SourceFilePath = SM.getIdentifierForBuffer(BufferID);
  for (const auto &SRC : D->getRawComment(/*SerializedOK=*/false).Comments) {
    Result.DocRanges.emplace_back(ExternalSourceLocs::RawLoc(),
                                  SRC.Range.getByteLength());
    setLoc(Result.DocRanges.back().first, SRC.Range.getStart());
  }
  setLoc(Result.Loc, D->getLoc(/*SerializedOK=*/false));
  setLoc(Result.StartLoc, D->getStartLoc());
  setLoc(Result.EndLoc, D->getEndLoc());
  return Result;
}

void ModuleDecl::getDisplayDecls(SmallVectorImpl<Decl*> &Results) const {
  // FIXME: Should this do extra access control filtering?
  FORWARD(getDisplayDecls, (Results));
}

ProtocolConformanceRef
ModuleDecl::lookupExistentialConformance(Type type, ProtocolDecl *protocol) {
  ASTContext &ctx = getASTContext();

  assert(type->isExistentialType());

  // If the existential type cannot be represented or the protocol does not
  // conform to itself, there's no point in looking further.
  if (!protocol->existentialConformsToSelf())
    return ProtocolConformanceRef::forInvalid();

  auto layout = type->getExistentialLayout();

  // Due to an IRGen limitation, witness tables cannot be passed from an
  // existential to an archetype parameter, so for now we restrict this to
  // @objc protocols and marker protocols.
  if (!layout.isObjC() && !protocol->isMarkerProtocol()) {
    // There's a specific exception for protocols with self-conforming
    // witness tables, but the existential has to be *exactly* that type.
    // TODO: synthesize witness tables on-demand for protocol compositions
    // that can satisfy the requirement.
    if (protocol->requiresSelfConformanceWitnessTable() &&
        type->is<ProtocolType>() &&
        type->castTo<ProtocolType>()->getDecl() == protocol)
      return ProtocolConformanceRef(ctx.getSelfConformance(protocol));

    return ProtocolConformanceRef::forInvalid();
  }

  // If the existential is class-constrained, the class might conform
  // concretely.
  if (auto superclass = layout.explicitSuperclass) {
    if (auto result = lookupConformance(superclass, protocol))
      return result;
  }

  // Otherwise, the existential might conform abstractly.
  for (auto proto : layout.getProtocols()) {
    auto *protoDecl = proto->getDecl();

    // If we found the protocol we're looking for, return an abstract
    // conformance to it.
    if (protoDecl == protocol)
      return ProtocolConformanceRef(ctx.getSelfConformance(protocol));

    // If the protocol has a superclass constraint, we might conform
    // concretely.
    if (auto superclass = protoDecl->getSuperclass()) {
      if (auto result = lookupConformance(superclass, protocol))
        return result;
    }

    // Now check refined protocols.
    if (protoDecl->inheritsFrom(protocol))
      return ProtocolConformanceRef(ctx.getSelfConformance(protocol));
  }

  // We didn't find our protocol in the existential's list; it doesn't
  // conform.
  return ProtocolConformanceRef::forInvalid();
}

ProtocolConformanceRef ModuleDecl::lookupConformance(Type type,
                                                     ProtocolDecl *protocol) {
  // If we are recursively checking for implicit conformance of a nominal
  // type to Sendable, fail without evaluating this request. This
  // squashes cycles.
  LookupConformanceInModuleRequest request{{this, type, protocol}};
  if (protocol->isSpecificProtocol(KnownProtocolKind::Sendable)) {
    if (auto nominal = type->getAnyNominal()) {
      GetImplicitSendableRequest icvRequest{nominal};
      if (getASTContext().evaluator.hasActiveRequest(icvRequest) ||
          getASTContext().evaluator.hasActiveRequest(request))
        return ProtocolConformanceRef::forInvalid();
    }
  }

  return evaluateOrDefault(
      getASTContext().evaluator, request, ProtocolConformanceRef::forInvalid());
}

ProtocolConformanceRef
LookupConformanceInModuleRequest::evaluate(
    Evaluator &evaluator, LookupConformanceDescriptor desc) const {
  auto *mod = desc.Mod;
  auto type = desc.Ty;
  auto protocol = desc.PD;
  ASTContext &ctx = mod->getASTContext();

  // A dynamic Self type conforms to whatever its underlying type
  // conforms to.
  if (auto selfType = type->getAs<DynamicSelfType>())
    type = selfType->getSelfType();

  // An archetype conforms to a protocol if the protocol is listed in the
  // archetype's list of conformances, or if the archetype has a superclass
  // constraint and the superclass conforms to the protocol.
  if (auto archetype = type->getAs<ArchetypeType>()) {

    // The generic signature builder drops conformance requirements that are made
    // redundant by a superclass requirement, so check for a concrete
    // conformance first, since an abstract conformance might not be
    // able to be resolved by a substitution that makes the archetype
    // concrete.
    if (auto super = archetype->getSuperclass()) {
      if (auto inheritedConformance = mod->lookupConformance(super, protocol)) {
        return ProtocolConformanceRef(ctx.getInheritedConformance(
            type, inheritedConformance.getConcrete()));
      }
    }

    for (auto ap : archetype->getConformsTo()) {
      if (ap == protocol || ap->inheritsFrom(protocol))
        return ProtocolConformanceRef(protocol);
    }

    return ProtocolConformanceRef::forInvalid();
  }

  // An existential conforms to a protocol if the protocol is listed in the
  // existential's list of conformances and the existential conforms to
  // itself.
  if (type->isExistentialType())
    return mod->lookupExistentialConformance(type, protocol);

  // Type variables have trivial conformances.
  if (type->isTypeVariableOrMember())
    return ProtocolConformanceRef(protocol);

  // UnresolvedType is a placeholder for an unknown type used when generating
  // diagnostics.  We consider it to conform to all protocols, since the
  // intended type might have. Same goes for PlaceholderType.
  if (type->is<UnresolvedType>() || type->is<PlaceholderType>())
    return ProtocolConformanceRef(protocol);

  auto nominal = type->getAnyNominal();

  // If we don't have a nominal type, there are no conformances.
  if (!nominal || isa<ProtocolDecl>(nominal))
    return ProtocolConformanceRef::forInvalid();

  // Find the (unspecialized) conformance.
  SmallVector<ProtocolConformance *, 2> conformances;
  if (!nominal->lookupConformance(mod, protocol, conformances)) {
    if (!protocol->isSpecificProtocol(KnownProtocolKind::Sendable))
      return ProtocolConformanceRef::forInvalid();

    // Try to infer Sendable conformance.
    GetImplicitSendableRequest cvRequest{nominal};
    if (auto conformance = evaluateOrDefault(
            ctx.evaluator, cvRequest, nullptr)) {
      conformances.clear();
      conformances.push_back(conformance);
    } else {
      return ProtocolConformanceRef::forInvalid();
    }
  }

  assert(!conformances.empty());

  // If we have multiple conformances, first try to filter out any that are
  // unavailable on the current deployment target.
  //
  // FIXME: Conformance lookup should really depend on source location for
  // this to be 100% correct.
  if (conformances.size() > 1) {
    SmallVector<ProtocolConformance *, 2> availableConformances;

    for (auto *conformance : conformances) {
      if (conformance->getDeclContext()->isAlwaysAvailableConformanceContext())
        availableConformances.push_back(conformance);
    }

    // Don't filter anything out if all conformances are unavailable.
    if (!availableConformances.empty())
      std::swap(availableConformances, conformances);
  }

  // If we still have multiple conformances, just pick the first one.
  auto conformance = conformances.front();

  // Rebuild inherited conformances based on the root normal conformance.
  // FIXME: This is a hack to work around our inability to handle multiple
  // levels of substitution through inherited conformances elsewhere in the
  // compiler.
  if (auto inherited = dyn_cast<InheritedProtocolConformance>(conformance)) {
    // Dig out the conforming nominal type.
    auto rootConformance = inherited->getRootConformance();
    auto conformingClass
      = rootConformance->getType()->getClassOrBoundGenericClass();

    // Map up to our superclass's type.
    auto superclassTy = type->getSuperclassForDecl(conformingClass);

    // Compute the conformance for the inherited type.
    auto inheritedConformance = mod->lookupConformance(superclassTy, protocol);
    assert(inheritedConformance &&
           "We already found the inherited conformance");

    // Create the inherited conformance entry.
    conformance =
        ctx.getInheritedConformance(type, inheritedConformance.getConcrete());
    return ProtocolConformanceRef(conformance);
  }

  // If the type is specialized, find the conformance for the generic type.
  if (type->isSpecialized()) {
    // Figure out the type that's explicitly conforming to this protocol.
    Type explicitConformanceType = conformance->getType();
    DeclContext *explicitConformanceDC = conformance->getDeclContext();

    // If the explicit conformance is associated with a type that is different
    // from the type we're checking, retrieve generic conformance.
    if (!explicitConformanceType->isEqual(type)) {
      // Gather the substitutions we need to map the generic conformance to
      // the specialized conformance.
      auto subMap = type->getContextSubstitutionMap(mod, explicitConformanceDC);

      // Create the specialized conformance entry.
      auto result = ctx.getSpecializedConformance(type, conformance, subMap);
      return ProtocolConformanceRef(result);
    }
  }

  // Record and return the simple conformance.
  return ProtocolConformanceRef(conformance);
}

Fingerprint SourceFile::getInterfaceHash() const {
  assert(hasInterfaceHash() && "Interface hash not enabled");
  auto &eval = getASTContext().evaluator;
  auto *mutableThis = const_cast<SourceFile *>(this);
  Optional<StableHasher> interfaceHasher =
      evaluateOrDefault(eval, ParseSourceFileRequest{mutableThis}, {})
              .InterfaceHasher;
  return Fingerprint{StableHasher{interfaceHasher.getValue()}.finalize()};
}

Fingerprint SourceFile::getInterfaceHashIncludingTypeMembers() const {
  /// FIXME: Gross. Hashing multiple "hash" values.
  auto hash = StableHasher::defaultHasher();
  hash.combine(getInterfaceHash());

  std::function<void(IterableDeclContext *)> hashTypeBodyFingerprints =
      [&](IterableDeclContext *IDC) {
        if (auto fp = IDC->getBodyFingerprint())
          hash.combine(*fp);
        for (auto *member : IDC->getParsedMembers())
          if (auto *childIDC = dyn_cast<IterableDeclContext>(member))
            hashTypeBodyFingerprints(childIDC);
      };

  for (auto *D : getTopLevelDecls()) {
    if (auto IDC = dyn_cast<IterableDeclContext>(D))
      hashTypeBodyFingerprints(IDC);
  }

  return Fingerprint{std::move(hash)};
}

syntax::SourceFileSyntax SourceFile::getSyntaxRoot() const {
  assert(shouldBuildSyntaxTree() && "Syntax tree disabled");
  auto &eval = getASTContext().evaluator;
  auto *mutableThis = const_cast<SourceFile *>(this);
  return *evaluateOrDefault(eval, ParseSourceFileRequest{mutableThis}, {})
              .SyntaxRoot;
}

void DirectOperatorLookupRequest::writeDependencySink(
    evaluator::DependencyCollector &reqTracker,
    const TinyPtrVector<OperatorDecl *> &ops) const {
  auto &desc = std::get<0>(getStorage());
  reqTracker.addTopLevelName(desc.name);
}

TinyPtrVector<OperatorDecl *>
DirectOperatorLookupRequest::evaluate(Evaluator &evaluator,
                                      OperatorLookupDescriptor descriptor,
                                      OperatorFixity fixity) const {
  // For a parsed module, we can check the source cache on the module rather
  // than doing an O(N) search over the source files.
  TinyPtrVector<OperatorDecl *> results;
  if (auto module = descriptor.getModule()) {
    if (isParsedModule(module)) {
      module->getSourceLookupCache().lookupOperator(descriptor.name, fixity,
                                                    results);
      return results;
    }
  }

  // Otherwise query each file.
  for (auto *file : descriptor.getFiles())
    file->lookupOperatorDirect(descriptor.name, fixity, results);

  return results;
}

void SourceFile::lookupOperatorDirect(
    Identifier name, OperatorFixity fixity,
    TinyPtrVector<OperatorDecl *> &results) const {
  getCache().lookupOperator(name, fixity, results);
}

void DirectPrecedenceGroupLookupRequest::writeDependencySink(
    evaluator::DependencyCollector &reqTracker,
    const TinyPtrVector<PrecedenceGroupDecl *> &groups) const {
  auto &desc = std::get<0>(getStorage());
  reqTracker.addTopLevelName(desc.name);
}

TinyPtrVector<PrecedenceGroupDecl *>
DirectPrecedenceGroupLookupRequest::evaluate(
    Evaluator &evaluator, OperatorLookupDescriptor descriptor) const {
  // For a parsed module, we can check the source cache on the module rather
  // than doing an O(N) search over the source files.
  TinyPtrVector<PrecedenceGroupDecl *> results;
  if (auto module = descriptor.getModule()) {
    if (isParsedModule(module)) {
      module->getSourceLookupCache().lookupPrecedenceGroup(descriptor.name,
                                                           results);
      return results;
    }
  }

  // Otherwise query each file.
  for (auto *file : descriptor.getFiles())
    file->lookupPrecedenceGroupDirect(descriptor.name, results);

  return results;
}

void SourceFile::lookupPrecedenceGroupDirect(
    Identifier name, TinyPtrVector<PrecedenceGroupDecl *> &results) const {
  getCache().lookupPrecedenceGroup(name, results);
}

void ModuleDecl::getImportedModules(SmallVectorImpl<ImportedModule> &modules,
                                    ModuleDecl::ImportFilter filter) const {
  FORWARD(getImportedModules, (modules, filter));
}

void
SourceFile::getImportedModules(SmallVectorImpl<ImportedModule> &modules,
                               ModuleDecl::ImportFilter filter) const {
  // FIXME: Ideally we should assert that the file has had its imports resolved
  // before calling this function. However unfortunately that can cause issues
  // for overlays which can depend on a Clang submodule for the underlying
  // framework they are overlaying, which causes us to attempt to load the
  // overlay again. We need to find a way to ensure that an overlay dependency
  // with the same name as the overlay always loads the underlying Clang module.
  // We currently handle this for a direct import from the overlay, but not when
  // it happens through other imports.
  assert(filter && "no imports requested?");
  if (!Imports)
    return;

  for (auto desc : *Imports) {
    ModuleDecl::ImportFilter requiredFilter;
    if (desc.options.contains(ImportFlags::Exported))
      requiredFilter |= ModuleDecl::ImportFilterKind::Exported;
    else if (desc.options.contains(ImportFlags::ImplementationOnly))
      requiredFilter |= ModuleDecl::ImportFilterKind::ImplementationOnly;
    else if (desc.options.contains(ImportFlags::SPIAccessControl))
      requiredFilter |= ModuleDecl::ImportFilterKind::SPIAccessControl;
    else
      requiredFilter |= ModuleDecl::ImportFilterKind::Default;

    if (!separatelyImportedOverlays.lookup(desc.module.importedModule).empty())
      requiredFilter |= ModuleDecl::ImportFilterKind::ShadowedByCrossImportOverlay;

    if (filter.contains(requiredFilter))
      modules.push_back(desc.module);
  }
}

void SourceFile::dumpSeparatelyImportedOverlays() const {
  for (auto &pair : separatelyImportedOverlays) {
    auto &underlying = std::get<0>(pair);
    auto &overlays = std::get<1>(pair);

    llvm::errs() << (void*)underlying << " ";
    underlying->dump(llvm::errs());

    for (auto overlay : overlays) {
      llvm::errs() << "- ";
      llvm::errs() << (void*)overlay << " ";
      overlay->dump(llvm::errs());
    }
  }
}

void ModuleDecl::getImportedModulesForLookup(
    SmallVectorImpl<ImportedModule> &modules) const {
  FORWARD(getImportedModulesForLookup, (modules));
}

ModuleDecl::ReverseFullNameIterator::ReverseFullNameIterator(
    const ModuleDecl *M) {
  assert(M);
  // Note: This will look through overlays as well, but that's fine for name
  // generation purposes. The point of an overlay is to
  if (auto *clangModule = M->findUnderlyingClangModule())
    current = clangModule;
  else
    current = M;
}

StringRef ModuleDecl::ReverseFullNameIterator::operator*() const {
  assert(current && "all name components exhausted");

  if (auto *swiftModule = current.dyn_cast<const ModuleDecl *>())
    return swiftModule->getName().str();

  auto *clangModule =
      static_cast<const clang::Module *>(current.get<const void *>());
  return clangModule->Name;
}

ModuleDecl::ReverseFullNameIterator &
ModuleDecl::ReverseFullNameIterator::operator++() {
  if (!current)
    return *this;

  if (current.is<const ModuleDecl *>()) {
    current = nullptr;
    return *this;
  }

  auto *clangModule =
      static_cast<const clang::Module *>(current.get<const void *>());
  if (clangModule->Parent)
    current = clangModule->Parent;
  else
    current = nullptr;
  return *this;
}

void
ModuleDecl::ReverseFullNameIterator::printForward(raw_ostream &out,
                                                  StringRef delim) const {
  SmallVector<StringRef, 8> elements(*this, {});
  llvm::interleave(
      llvm::reverse(elements), [&out](StringRef next) { out << next; },
      [&out, delim] { out << delim; });
}

void
ImportedModule::removeDuplicates(SmallVectorImpl<ImportedModule> &imports) {
  std::sort(imports.begin(), imports.end(),
            [](const ImportedModule &lhs, const ImportedModule &rhs) -> bool {
    // Arbitrarily sort by name to get a deterministic order.
    if (lhs.importedModule != rhs.importedModule) {
      return std::lexicographical_compare(
          lhs.importedModule->getReverseFullModuleName(), {},
          rhs.importedModule->getReverseFullModuleName(), {});
    }
    return std::lexicographical_compare(
        lhs.accessPath.begin(), lhs.accessPath.end(), rhs.accessPath.begin(),
        rhs.accessPath.end(),
        [](const ImportPath::Element &lElem, const ImportPath::Element &rElem) {
          return lElem.Item.str() < rElem.Item.str();
        });
  });
  auto last = std::unique(
      imports.begin(), imports.end(),
      [](const ImportedModule &lhs, const ImportedModule &rhs) -> bool {
        if (lhs.importedModule != rhs.importedModule)
          return false;
        return lhs.accessPath.isSameAs(rhs.accessPath);
      });
  imports.erase(last, imports.end());
}

Identifier ModuleDecl::getABIName() const {
  if (!ModuleABIName.empty())
    return ModuleABIName;

  // Hard code that the _Concurrency module has Swift as its ABI name.
  // FIXME: This works around a backward-compatibility issue where
  // -module-abi-name is not supported on existing Swift compilers. Remove
  // this hack later and pass -module-abi-name when building the _Concurrency
  // module.
  if (getName().str() == SWIFT_CONCURRENCY_NAME) {
    ModuleABIName = getASTContext().getIdentifier(STDLIB_NAME);
    return ModuleABIName;
  }

  return getName();
}

StringRef ModuleDecl::getModuleFilename() const {
  // FIXME: Audit uses of this function and figure out how to migrate them to
  // per-file names. Modules can consist of more than one file.
  StringRef Result;
  for (auto F : getFiles()) {
    if (auto SF = dyn_cast<SourceFile>(F)) {
      if (!Result.empty())
        return StringRef();
      Result = SF->getFilename();
      continue;
    }
    if (auto LF = dyn_cast<LoadedFile>(F)) {
      if (!Result.empty())
        return StringRef();
      Result = LF->getFilename();
      continue;
    }
    // Skip synthesized files.
    if (auto *SFU = dyn_cast<SynthesizedFileUnit>(F))
      continue;
    return StringRef();
  }
  return Result;
}

bool ModuleDecl::isStdlibModule() const {
  return !getParent() && getName() == getASTContext().StdlibModuleName;
}

bool ModuleDecl::hasStandardSubstitutions() const {
  return !getParent() &&
      (getName() == getASTContext().StdlibModuleName ||
       getName() == getASTContext().Id_Concurrency);
}

bool ModuleDecl::isSwiftShimsModule() const {
  return !getParent() && getName() == getASTContext().SwiftShimsModuleName;
}

bool ModuleDecl::isOnoneSupportModule() const {
  return !getParent() && getName().str() == SWIFT_ONONE_SUPPORT;
}

bool ModuleDecl::isFoundationModule() const {
  return !getParent() && getName() == getASTContext().Id_Foundation;
}

bool ModuleDecl::isBuiltinModule() const {
  return this == getASTContext().TheBuiltinModule;
}

bool SourceFile::registerMainDecl(ValueDecl *mainDecl, SourceLoc diagLoc) {
  assert(mainDecl);
  if (mainDecl == MainDecl)
    return false;

  ArtificialMainKind kind = mainDecl->getArtificialMainKind();
  if (getParentModule()->registerEntryPointFile(this, diagLoc, kind))
    return true;

  MainDecl = mainDecl;
  MainDeclDiagLoc = diagLoc;

  return false;
}

bool ModuleDecl::registerEntryPointFile(FileUnit *file, SourceLoc diagLoc,
                                        Optional<ArtificialMainKind> kind) {
  if (!EntryPointInfo.hasEntryPoint()) {
    EntryPointInfo.setEntryPointFile(file);
    return false;
  }

  if (diagLoc.isInvalid())
    return true;

  assert(kind.hasValue() && "multiple entry points without attributes");

  // %select indices for UI/NSApplication-related diagnostics.
  enum : unsigned {
    UIApplicationMainClass = 0,
    NSApplicationMainClass = 1,
    MainType = 2,
  } mainTypeDiagKind;

  switch (kind.getValue()) {
  case ArtificialMainKind::UIApplicationMain:
    mainTypeDiagKind = UIApplicationMainClass;
    break;
  case ArtificialMainKind::NSApplicationMain:
    mainTypeDiagKind = NSApplicationMainClass;
    break;
  case ArtificialMainKind::TypeMain:
    mainTypeDiagKind = MainType;
    break;
  }

  FileUnit *existingFile = EntryPointInfo.getEntryPointFile();
  const Decl *existingDecl = existingFile->getMainDecl();
  SourceLoc existingDiagLoc;

  if (auto *sourceFile = dyn_cast<SourceFile>(existingFile)) {
    if (existingDecl) {
      existingDiagLoc = sourceFile->getMainDeclDiagLoc();
    } else {
      if (auto bufID = sourceFile->getBufferID())
        existingDiagLoc = getASTContext().SourceMgr.getLocForBufferStart(*bufID);
    }
  }

  if (existingDecl) {
    if (EntryPointInfo.markDiagnosedMultipleMainClasses()) {
      // If we already have a main type, and we haven't diagnosed it,
      // do so now.
      if (existingDiagLoc.isValid()) {
        getASTContext().Diags.diagnose(existingDiagLoc,
                                       diag::attr_ApplicationMain_multiple,
                                       mainTypeDiagKind);
      } else {
        getASTContext().Diags.diagnose(existingDecl,
                                       diag::attr_ApplicationMain_multiple,
                                       mainTypeDiagKind);
      }
    }

    // Always diagnose the new class.
    getASTContext().Diags.diagnose(diagLoc, diag::attr_ApplicationMain_multiple,
                                   mainTypeDiagKind);

  } else {
    // We don't have an existing class, but we /do/ have a file in script mode.
    // Diagnose that.
    if (EntryPointInfo.markDiagnosedMainClassWithScript()) {
      getASTContext().Diags.diagnose(
          diagLoc, diag::attr_ApplicationMain_with_script, mainTypeDiagKind);

      if (existingDiagLoc.isValid()) {
        getASTContext().Diags.diagnose(existingDiagLoc,
                           diag::attr_ApplicationMain_script_here);
      }
    }
  }

  return true;
}

void ModuleDecl::collectLinkLibraries(LinkLibraryCallback callback) const {
  // FIXME: The proper way to do this depends on the decls used.
  FORWARD(collectLinkLibraries, (callback));
}

void
SourceFile::collectLinkLibraries(ModuleDecl::LinkLibraryCallback callback) const {
  llvm::SmallDenseSet<ModuleDecl *, 32> visited;
  SmallVector<ImportedModule, 32> stack;

  ModuleDecl::ImportFilter filter = {
      ModuleDecl::ImportFilterKind::Exported,
      ModuleDecl::ImportFilterKind::Default,
      ModuleDecl::ImportFilterKind::SPIAccessControl};

  auto *topLevel = getParentModule();

  ModuleDecl::ImportFilter topLevelFilter = filter;
  topLevelFilter |= ModuleDecl::ImportFilterKind::ImplementationOnly;
  topLevel->getImportedModules(stack, topLevelFilter);

  // Make sure the top-level module is first; we want pre-order-ish traversal.
  stack.emplace_back(ImportPath::Access(), topLevel);

  while (!stack.empty()) {
    auto next = stack.pop_back_val().importedModule;

    if (!visited.insert(next).second)
      continue;

    if (next->getName() != getParentModule()->getName()) {
      next->collectLinkLibraries(callback);
    }

    next->getImportedModules(stack, filter);
  }
}

bool ModuleDecl::walk(ASTWalker &Walker) {
  llvm::SaveAndRestore<ASTWalker::ParentTy> SAR(Walker.Parent, this);
  for (auto SF : getFiles())
    if (SF->walk(Walker))
      return true;
  return false;
}

ModuleDecl *ModuleDecl::getUnderlyingModuleIfOverlay() const {
  for (auto *FU : getFiles()) {
    if (auto *Mod = FU->getUnderlyingModuleIfOverlay())
      return Mod;
  }
  return nullptr;
}

const clang::Module *ModuleDecl::findUnderlyingClangModule() const {
  for (auto *FU : getFiles()) {
    if (auto *Mod = FU->getUnderlyingClangModule())
      return Mod;
  }
  return nullptr;
}

void ModuleDecl::collectBasicSourceFileInfo(
    llvm::function_ref<void(const BasicSourceFileInfo &)> callback) const {
  for (const FileUnit *fileUnit : getFiles()) {
    if (const auto *SF = dyn_cast<SourceFile>(fileUnit)) {
      callback(BasicSourceFileInfo(SF));
    } else if (auto *serialized = dyn_cast<LoadedFile>(fileUnit)) {
      serialized->collectBasicSourceFileInfo(callback);
    }
  }
}

Fingerprint ModuleDecl::getFingerprint() const {
  StableHasher hasher = StableHasher::defaultHasher();
  SmallVector<Fingerprint, 16> FPs;
  collectBasicSourceFileInfo([&](const BasicSourceFileInfo &bsfi) {
    // For incremental imports, the hash must be insensitive to type-body
    // changes, so use the one without type members.
    FPs.emplace_back(bsfi.getInterfaceHashExcludingTypeMembers());
  });
  
  // Sort the fingerprints lexicographically so we have a stable hash despite
  // an unstable ordering of files across rebuilds.
  // FIXME: If we used a commutative hash combine (say, if we could take an
  // XOR here) we could avoid this sort.
  std::sort(FPs.begin(), FPs.end(), std::less<Fingerprint>());
  for (const auto &FP : FPs) {
    hasher.combine(FP);
  }

  return Fingerprint{std::move(hasher)};
}

//===----------------------------------------------------------------------===//
// Cross-Import Overlays
//===----------------------------------------------------------------------===//

namespace swift {
/// Represents a file containing information about cross-module overlays.
class OverlayFile {
  friend class ModuleDecl;
  /// The file that data should be loaded from.
  StringRef filePath;

  /// The list of module names; empty if loading failed.
  llvm::TinyPtrVector<Identifier> overlayModuleNames;

  enum class State { Pending, Loaded, Failed };
  State state = State::Pending;

  /// Actually loads the overlay module name list. This should mutate
  /// \c overlayModuleNames, but not \c filePath.
  ///
  /// \returns \c true on success, \c false on failure. Diagnoses any failures
  ///          before returning.
  bool loadOverlayModuleNames(const ModuleDecl *M, SourceLoc diagLoc,
                              Identifier bystandingModule);
  bool loadOverlayModuleNames(ASTContext &ctx,
                              StringRef module,
                              StringRef bystandingModule,
                              SourceLoc diagLoc);
public:
  // Only allocate in ASTContexts.
  void *operator new(size_t bytes) = delete;
  void *operator new(size_t bytes, const ASTContext &ctx,
                     unsigned alignment = alignof(OverlayFile)) {
    return ctx.Allocate(bytes, alignment);
  }

  OverlayFile(StringRef filePath)
      : filePath(filePath) {
    assert(!filePath.empty());
  }

  /// Returns the list of additional modules that should be imported if both
  /// the primary and secondary modules have been imported. This may load a
  /// file; if so, it will diagnose any errors itself and arrange for the file
  /// to not be loaded again.
  ///
  /// The result can be empty, either because of an error or because the file
  /// didn't contain any overlay module names.
  ArrayRef<Identifier> getOverlayModuleNames(const ModuleDecl *M,
                                             SourceLoc diagLoc,
                                             Identifier bystandingModule) {
    if (state == State::Pending) {
      state = loadOverlayModuleNames(M, diagLoc, bystandingModule)
            ? State::Loaded : State::Failed;
    }
    return overlayModuleNames;
  }
};
}

void ModuleDecl::addCrossImportOverlayFile(StringRef file) {
  auto &ctx = getASTContext();

  Identifier secondaryModule = ctx.getIdentifier(llvm::sys::path::stem(file));
  declaredCrossImports[secondaryModule]
      .push_back(new (ctx) OverlayFile(ctx.AllocateCopy(file)));
}

llvm::SmallSetVector<Identifier, 4>
ModuleDecl::collectCrossImportOverlay(ASTContext &ctx,
                                      StringRef file,
                                      StringRef moduleName,
                                      StringRef &bystandingModule) {
  OverlayFile ovFile(file);
  bystandingModule = llvm::sys::path::stem(file);
  ovFile.loadOverlayModuleNames(ctx, moduleName, bystandingModule, SourceLoc());
  llvm::SmallSetVector<Identifier, 4> result;
  for (auto Id: ovFile.overlayModuleNames) {
    result.insert(Id);
  }
  return result;
}

bool ModuleDecl::mightDeclareCrossImportOverlays() const {
  return !declaredCrossImports.empty();
}

void ModuleDecl::
findDeclaredCrossImportOverlays(Identifier bystanderName,
                                SmallVectorImpl<Identifier> &overlayNames,
                                SourceLoc diagLoc) const {
  if (getName() == bystanderName)
    // We don't currently support self-cross-imports.
    return;

  for (auto &crossImportFile : declaredCrossImports.lookup(bystanderName))
    llvm::copy(crossImportFile->getOverlayModuleNames(this, diagLoc,
                                                      bystanderName),
               std::back_inserter(overlayNames));
}

void ModuleDecl::getDeclaredCrossImportBystanders(
    SmallVectorImpl<Identifier> &otherModules) {
  for (auto &pair : declaredCrossImports)
    otherModules.push_back(std::get<0>(pair));
}

void ModuleDecl::findDeclaredCrossImportOverlaysTransitive(
    SmallVectorImpl<ModuleDecl *> &overlayModules) {
  SmallVector<ModuleDecl *, 1> worklist;
  SmallPtrSet<ModuleDecl *, 1> seen;
  SourceLoc unused;

  worklist.push_back(this);
  if (auto *clangModule = getUnderlyingModuleIfOverlay())
    worklist.push_back(clangModule);

  while (!worklist.empty()) {
    ModuleDecl *current = worklist.back();
    worklist.pop_back();
    for (auto &pair: current->declaredCrossImports) {
      Identifier &bystander = std::get<0>(pair);
      for (auto *file: std::get<1>(pair)) {
        auto overlays = file->getOverlayModuleNames(current, unused, bystander);
        for (Identifier overlay: overlays) {
          // We don't present non-underscored overlays as part of the underlying
          // module, so ignore them.
          if (!overlay.str().startswith("_"))
            continue;
          ModuleDecl *overlayMod =
              getASTContext().getModuleByName(overlay.str());
          if (!overlayMod)
            continue;
          if (seen.insert(overlayMod).second) {
            overlayModules.push_back(overlayMod);
            worklist.push_back(overlayMod);
            if (auto *clangModule = overlayMod->getUnderlyingModuleIfOverlay())
              worklist.push_back(clangModule);
          }
        }
      }
    }
  }
}

namespace {
  using CrossImportMap =
      llvm::SmallDenseMap<Identifier, SmallVector<OverlayFile *, 1>>;


  Identifier getBystanderIfDeclaring(ModuleDecl *mod, ModuleDecl *overlay,
                                     CrossImportMap modCrossImports) {
    auto ret = std::find_if(modCrossImports.begin(), modCrossImports.end(),
                            [&](CrossImportMap::iterator::value_type &pair) {
      for (OverlayFile *file: std::get<1>(pair)) {
        ArrayRef<Identifier> overlays = file->getOverlayModuleNames(
            mod, SourceLoc(), std::get<0>(pair));
        if (std::find(overlays.begin(), overlays.end(),
                      overlay->getName()) != overlays.end())
          return true;
      }
      return false;
    });
    return ret != modCrossImports.end() ? ret->first : Identifier();
  }
}

std::pair<ModuleDecl *, Identifier>
ModuleDecl::getDeclaringModuleAndBystander() {
  if (declaringModuleAndBystander)
    return *declaringModuleAndBystander;

  if (!hasUnderscoredNaming())
    return *(declaringModuleAndBystander = {nullptr, Identifier()});

  // Search the transitive set of imported @_exported modules to see if any have
  // this module as their overlay.
  SmallPtrSet<ModuleDecl *, 16> seen;
  SmallVector<ImportedModule, 16> imported;
  SmallVector<ImportedModule, 16> furtherImported;
  ModuleDecl *overlayModule = this;

  getImportedModules(imported, ModuleDecl::ImportFilterKind::Exported);
  while (!imported.empty()) {
    ModuleDecl *importedModule = imported.back().importedModule;
    imported.pop_back();
    if (!seen.insert(importedModule).second)
      continue;

    Identifier bystander = getBystanderIfDeclaring(
        importedModule, overlayModule, importedModule->declaredCrossImports);
    if (!bystander.empty())
      return *(declaringModuleAndBystander = {importedModule, bystander});

    // Also check the imported module's underlying module if it's a traditional
    // overlay (i.e. not a cross-import overlay).
    if (auto *clangModule = importedModule->getUnderlyingModuleIfOverlay()) {
      Identifier bystander = getBystanderIfDeclaring(
          clangModule, overlayModule, clangModule->declaredCrossImports);
      if (!bystander.empty())
        return *(declaringModuleAndBystander = {clangModule, bystander});
    }

    if (!importedModule->hasUnderscoredNaming())
      continue;

    furtherImported.clear();
    importedModule->getImportedModules(furtherImported,
                                       ModuleDecl::ImportFilterKind::Exported);
    imported.append(furtherImported.begin(), furtherImported.end());
  }

  return *(declaringModuleAndBystander = {nullptr, Identifier()});
}

bool ModuleDecl::isCrossImportOverlayOf(ModuleDecl *other) {
  ModuleDecl *current = this;
  ModuleDecl *otherClang = other->getUnderlyingModuleIfOverlay();
  while ((current = current->getDeclaringModuleAndBystander().first)) {
    if (current == other || current == otherClang)
      return true;
  }
  return false;
}

ModuleDecl *ModuleDecl::getDeclaringModuleIfCrossImportOverlay() {
  ModuleDecl *current = this, *declaring = nullptr;
  while ((current = current->getDeclaringModuleAndBystander().first))
    declaring = current;
  return declaring;
}

bool ModuleDecl::getRequiredBystandersIfCrossImportOverlay(
    ModuleDecl *declaring, SmallVectorImpl<Identifier> &bystanderNames) {
  auto *clangModule = declaring->getUnderlyingModuleIfOverlay();
  auto current = std::make_pair(this, Identifier());
  while ((current = current.first->getDeclaringModuleAndBystander()).first) {
    bystanderNames.push_back(current.second);
    if (current.first == declaring || current.first == clangModule)
      return true;
  }
  return false;
}

namespace {
struct OverlayFileContents {
  struct Module {
    std::string name;
  };

  unsigned version;
  std::vector<Module> modules;

  static llvm::ErrorOr<OverlayFileContents>
  load(std::unique_ptr<llvm::MemoryBuffer> input,
       SmallVectorImpl<std::string> &errorMessages);
};
} // end anonymous namespace

namespace llvm {
namespace yaml {
template <>
struct MappingTraits<OverlayFileContents::Module> {
  static void mapping(IO &io, OverlayFileContents::Module &module) {
    io.mapRequired("name", module.name);
  }
};

template <>
struct SequenceElementTraits<OverlayFileContents::Module> {
  static const bool flow = false;
};

template <>
struct MappingTraits<OverlayFileContents> {
  static void mapping(IO &io, OverlayFileContents &contents) {
    io.mapRequired("version", contents.version);
    io.mapRequired("modules", contents.modules);
  }
};
}
} // end namespace 'llvm'

static void pushYAMLError(const llvm::SMDiagnostic &diag, void *Context) {
  auto &errorMessages = *static_cast<SmallVectorImpl<std::string> *>(Context);
  errorMessages.emplace_back(diag.getMessage());
}

llvm::ErrorOr<OverlayFileContents>
OverlayFileContents::load(std::unique_ptr<llvm::MemoryBuffer> input,
                          SmallVectorImpl<std::string> &errorMessages) {
  llvm::yaml::Input yamlInput(input->getBuffer(), /*Ctxt=*/nullptr,
                              pushYAMLError, &errorMessages);
  OverlayFileContents contents;
  yamlInput >> contents;

  if (auto error = yamlInput.error())
    return error;

  if (contents.version > 1) {
    auto message = Twine("key 'version' has invalid value: ") + Twine(contents.version);
    errorMessages.push_back(message.str());
    return make_error_code(std::errc::result_out_of_range);
  }

  return contents;
}

bool
OverlayFile::loadOverlayModuleNames(ASTContext &ctx, StringRef module,
                                    StringRef bystanderName,
                                    SourceLoc diagLoc) {
  llvm::vfs::FileSystem &fs = *ctx.SourceMgr.getFileSystem();

  auto bufOrError = fs.getBufferForFile(filePath);
  if (!bufOrError) {
    ctx.Diags.diagnose(diagLoc, diag::cannot_load_swiftoverlay_file,
                       module, bystanderName,
                       bufOrError.getError().message(), filePath);
    return false;
  }

  SmallVector<std::string, 4> errorMessages;
  auto contentsOrErr = OverlayFileContents::load(std::move(bufOrError.get()),
                                                 errorMessages);
  if (!contentsOrErr) {
    if (errorMessages.empty())
      errorMessages.push_back(contentsOrErr.getError().message());

    for (auto message : errorMessages)
      ctx.Diags.diagnose(diagLoc, diag::cannot_load_swiftoverlay_file,
                         module, bystanderName, message, filePath);
    return false;
  }

  auto contents = std::move(*contentsOrErr);

  for (const auto &module : contents.modules) {
    auto moduleIdent = ctx.getIdentifier(module.name);
    overlayModuleNames.push_back(moduleIdent);
  }

  return true;
}

bool
OverlayFile::loadOverlayModuleNames(const ModuleDecl *M, SourceLoc diagLoc,
                                    Identifier bystanderName) {
  return loadOverlayModuleNames(M->getASTContext(),
                                M->getName().str(),
                                bystanderName.str(),
                                diagLoc);
}

//===----------------------------------------------------------------------===//
// SourceFile Implementation
//===----------------------------------------------------------------------===//

void SourceFile::print(raw_ostream &OS, const PrintOptions &PO) {
  StreamPrinter Printer(OS);
  print(Printer, PO);
}

void SourceFile::print(ASTPrinter &Printer, const PrintOptions &PO) {
  std::set<DeclKind> MajorDeclKinds = {DeclKind::Class, DeclKind::Enum,
    DeclKind::Extension, DeclKind::Protocol, DeclKind::Struct};
  for (auto decl : getTopLevelDecls()) {
    if (!decl->shouldPrintInContext(PO))
      continue;
    // For a major decl, we print an empty line before it.
    if (MajorDeclKinds.find(decl->getKind()) != MajorDeclKinds.end())
      Printer << "\n";
    if (decl->print(Printer, PO))
      Printer << "\n";
  }
}

void
SourceFile::setImports(ArrayRef<AttributedImport<ImportedModule>> imports) {
  assert(!Imports && "Already computed imports");
  Imports = getASTContext().AllocateCopy(imports);
}

bool HasImplementationOnlyImportsRequest::evaluate(Evaluator &evaluator,
                                                   SourceFile *SF) const {
  return llvm::any_of(SF->getImports(),
                      [](AttributedImport<ImportedModule> desc) {
    return desc.options.contains(ImportFlags::ImplementationOnly);
  });
}

bool SourceFile::hasImplementationOnlyImports() const {
  auto &ctx = getASTContext();
  auto *mutableThis = const_cast<SourceFile *>(this);
  return evaluateOrDefault(
      ctx.evaluator, HasImplementationOnlyImportsRequest{mutableThis}, false);
}

bool SourceFile::hasTestableOrPrivateImport(
    AccessLevel accessLevel, const swift::ValueDecl *ofDecl,
    SourceFile::ImportQueryKind queryKind) const {
  auto *module = ofDecl->getModuleContext();
  switch (accessLevel) {
  case AccessLevel::Internal:
  case AccessLevel::Public:
    // internal/public access only needs an import marked as @_private. The
    // filename does not need to match (and we don't serialize it for such
    // decls).
    return llvm::any_of(*Imports,
        [module, queryKind](AttributedImport<ImportedModule> desc) -> bool {
          if (queryKind == ImportQueryKind::TestableAndPrivate)
            return desc.module.importedModule == module &&
                   (desc.options.contains(ImportFlags::PrivateImport) ||
                    desc.options.contains(ImportFlags::Testable));
          else if (queryKind == ImportQueryKind::TestableOnly)
            return desc.module.importedModule == module &&
                   desc.options.contains(ImportFlags::Testable);
          else {
            assert(queryKind == ImportQueryKind::PrivateOnly);
            return desc.module.importedModule == module &&
                   desc.options.contains(ImportFlags::PrivateImport);
          }
        });
  case AccessLevel::Open:
    return true;
  case AccessLevel::FilePrivate:
  case AccessLevel::Private:
    // Fallthrough.
    break;
  }

  if (queryKind == ImportQueryKind::TestableOnly)
    return false;

  auto *DC = ofDecl->getDeclContext();
  if (!DC)
    return false;
  auto *scope = DC->getModuleScopeContext();
  if (!scope)
    return false;

  StringRef filename;
  if (auto *file = dyn_cast<LoadedFile>(scope)) {
    filename = file->getFilenameForPrivateDecl(ofDecl);
  } else
    return false;

  if (filename.empty())
    return false;

  return llvm::any_of(*Imports,
      [module, filename](AttributedImport<ImportedModule> desc) {
        return desc.module.importedModule == module &&
              desc.options.contains(ImportFlags::PrivateImport) &&
              desc.sourceFileArg == filename;
      });
}

bool SourceFile::isImportedImplementationOnly(const ModuleDecl *module) const {
  // Implementation-only imports are (currently) always source-file-specific,
  // so if we don't have any, we know the search is complete.
  if (!hasImplementationOnlyImports())
    return false;

  auto &imports = getASTContext().getImportCache();

  // Look at the imports of this source file.
  for (auto &desc : *Imports) {
    // Ignore implementation-only imports.
    if (desc.options.contains(ImportFlags::ImplementationOnly))
      continue;

    // If the module is imported this way, it's not imported
    // implementation-only.
    if (imports.isImportedBy(module, desc.module.importedModule))
      return false;
  }

  // Now check this file's enclosing module in case there are re-exports.
  return !imports.isImportedBy(module, getParentModule());
}

bool ModuleDecl::isImportedImplementationOnly(const ModuleDecl *module) const {
  if (module == this) return false;

  auto &imports = getASTContext().getImportCache();

  // Look through non-implementation-only imports to see if module is imported
  // in some other way. Otherwise we assume it's implementation-only imported.
  ModuleDecl::ImportFilter filter = {
    ModuleDecl::ImportFilterKind::Exported,
    ModuleDecl::ImportFilterKind::Default,
    ModuleDecl::ImportFilterKind::SPIAccessControl,
    ModuleDecl::ImportFilterKind::ShadowedByCrossImportOverlay};
  SmallVector<ImportedModule, 4> results;
  getImportedModules(results, filter);

  for (auto &desc : results) {
    if (imports.isImportedBy(module, desc.importedModule))
      return false;
  }

  return true;
}

bool ModuleDecl::
canBeUsedForCrossModuleOptimization(NominalTypeDecl *nominal) const {
  ModuleDecl *moduleOfNominal = nominal->getParentModule();

  // If the nominal is defined in the same module, it's fine.
  if (moduleOfNominal == this)
    return true;

  // See if nominal is imported in a "regular" way, i.e. not with
  // @_implementationOnly or @_spi.
  ModuleDecl::ImportFilter filter = {
    ModuleDecl::ImportFilterKind::Exported,
    ModuleDecl::ImportFilterKind::Default};
  SmallVector<ImportedModule, 4> results;
  getImportedModules(results, filter);

  auto &imports = getASTContext().getImportCache();
  for (auto &desc : results) {
    if (imports.isImportedBy(moduleOfNominal, desc.importedModule))
      return true;
  }
  return false;
}

void SourceFile::lookupImportedSPIGroups(
                        const ModuleDecl *importedModule,
                        llvm::SmallSetVector<Identifier, 4> &spiGroups) const {
  for (auto &import : *Imports) {
    if (import.options.contains(ImportFlags::SPIAccessControl) &&
        importedModule == import.module.importedModule) {
      auto importedSpis = import.spiGroups;
      spiGroups.insert(importedSpis.begin(), importedSpis.end());
    }
  }
}

bool SourceFile::isImportedAsSPI(const ValueDecl *targetDecl) const {
  auto targetModule = targetDecl->getModuleContext();
  llvm::SmallSetVector<Identifier, 4> importedSPIGroups;
  lookupImportedSPIGroups(targetModule, importedSPIGroups);
  if (importedSPIGroups.empty()) return false;

  auto declSPIGroups = targetDecl->getSPIGroups();

  for (auto declSPI : declSPIGroups)
    if (importedSPIGroups.count(declSPI))
      return true;

  return false;
}

bool ModuleDecl::isImportedAsSPI(const SpecializeAttr *attr,
                                 const ValueDecl *targetDecl) const {
  auto targetModule = targetDecl->getModuleContext();
  llvm::SmallSetVector<Identifier, 4> importedSPIGroups;
  lookupImportedSPIGroups(targetModule, importedSPIGroups);
  if (importedSPIGroups.empty()) return false;

  auto declSPIGroups = attr->getSPIGroups();

  for (auto declSPI : declSPIGroups)
    if (importedSPIGroups.count(declSPI))
      return true;

  return false;
}

bool ModuleDecl::isImportedAsSPI(Identifier spiGroup,
                                 const ModuleDecl *fromModule) const {
  llvm::SmallSetVector<Identifier, 4> importedSPIGroups;
  lookupImportedSPIGroups(fromModule, importedSPIGroups);
  if (importedSPIGroups.empty())
    return false;
  return importedSPIGroups.count(spiGroup);
}

bool Decl::isSPI() const {
  return !getSPIGroups().empty();
}

ArrayRef<Identifier> Decl::getSPIGroups() const {
  if (!isa<ValueDecl>(this) &&
      !isa<ExtensionDecl>(this))
    return ArrayRef<Identifier>();

  return evaluateOrDefault(getASTContext().evaluator,
                           SPIGroupsRequest{ this },
                           ArrayRef<Identifier>());
}

llvm::ArrayRef<Identifier>
SPIGroupsRequest::evaluate(Evaluator &evaluator, const Decl *decl) const {
  // Applies only to public ValueDecls and ExtensionDecls.
  assert (isa<ValueDecl>(decl) ||
          isa<ExtensionDecl>(decl));

  // First, look for local attributes.
  llvm::SetVector<Identifier> spiGroups;
  for (auto attr : decl->getAttrs().getAttributes<SPIAccessControlAttr>())
    for (auto spi : attr->getSPIGroups())
      spiGroups.insert(spi);

  // Backing storage for a wrapped property gets the SPI groups from the
  // original property.
  if (auto varDecl = dyn_cast<VarDecl>(decl))
    if (auto originalDecl = varDecl->getOriginalWrappedProperty()) {
      auto originalSPIs = originalDecl->getSPIGroups();
      spiGroups.insert(originalSPIs.begin(), originalSPIs.end());
    }

  // If there is no local SPI information, look at the context.
  if (spiGroups.empty()) {

    // Then in the extended nominal type.
    if (auto extension = dyn_cast<ExtensionDecl>(decl)) {
      if (auto extended = extension->getExtendedNominal()) {
        auto extSPIs = extended->getSPIGroups();
        if (!extSPIs.empty()) return extSPIs;
      }
    }

    // And finally in the parent context.
    auto parent = decl->getDeclContext();
    if (auto parentD = parent->getAsDecl()) {
      if (!isa<ModuleDecl>(parentD)) {
        return parentD->getSPIGroups();
      }
    }
  }

  auto &ctx = decl->getASTContext();
  return ctx.AllocateCopy(spiGroups.getArrayRef());
}

LibraryLevel ModuleDecl::getLibraryLevel() const {
  return evaluateOrDefault(getASTContext().evaluator,
                           ModuleLibraryLevelRequest{this},
                           LibraryLevel::Other);
}

LibraryLevel
ModuleLibraryLevelRequest::evaluate(Evaluator &evaluator,
                                    const ModuleDecl *module) const {
  auto &ctx = module->getASTContext();

  /// Is \p modulePath from System/Library/PrivateFrameworks/?
  auto fromPrivateFrameworks = [&](StringRef modulePath) -> bool {
    if (!ctx.LangOpts.Target.isOSDarwin()) return false;

    namespace path = llvm::sys::path;
    SmallString<128> scratch;
    scratch = ctx.SearchPathOpts.SDKPath;
    path::append(scratch, "System", "Library", "PrivateFrameworks");
    return hasPrefix(path::begin(modulePath), path::end(modulePath),
                     path::begin(scratch), path::end(scratch));
  };

  if (module->isNonSwiftModule()) {
    if (auto *underlying = module->findUnderlyingClangModule()) {
      // Imported clangmodules are SPI if they are defined by a private
      // modulemap or from the PrivateFrameworks folder in the SDK.
      bool moduleIsSPI = underlying->ModuleMapIsPrivate ||
                         (underlying->isPartOfFramework() &&
                          fromPrivateFrameworks(underlying->PresumedModuleMapFile));
      return moduleIsSPI ? LibraryLevel::SPI : LibraryLevel::API;
    }
    return LibraryLevel::Other;

  } else if (module->isMainModule()) {
    // The current compilation target.
    return ctx.LangOpts.LibraryLevel;

  } else {
    // Other Swift modules are SPI if they are from the PrivateFrameworks
    // folder in the SDK.
    auto modulePath = module->getModuleFilename();
    return fromPrivateFrameworks(modulePath) ?
      LibraryLevel::SPI : LibraryLevel::API;
  }
}

bool SourceFile::shouldCrossImport() const {
  return Kind != SourceFileKind::SIL && Kind != SourceFileKind::Interface &&
         getASTContext().LangOpts.EnableCrossImportOverlays;
}

void ModuleDecl::clearLookupCache() {
  getASTContext().getImportCache().clear();

  if (!Cache)
    return;

  // Abandon any current cache. We'll rebuild it on demand.
  Cache->invalidate();
  Cache.reset();
}

void
SourceFile::cacheVisibleDecls(SmallVectorImpl<ValueDecl*> &&globals) const {
  SmallVectorImpl<ValueDecl*> &cached = getCache().AllVisibleValues;
  cached = std::move(globals);
}

const SmallVectorImpl<ValueDecl *> &
SourceFile::getCachedVisibleDecls() const {
  return getCache().AllVisibleValues;
}

llvm::StringMap<SourceFilePathInfo>
SourceFile::getInfoForUsedFilePaths() const {
  llvm::StringMap<SourceFilePathInfo> result;

  if (BufferID != -1) {
    result[getFilename()].physicalFileLoc =
        getASTContext().SourceMgr.getLocForBufferStart(BufferID);
  }

  for (auto &vpath : VirtualFilePaths) {
    result[vpath.Item].virtualFileLocs.insert(vpath.Loc);
  }

  return result;
}

/// Returns a map of filenames to a map of file paths to SourceFilePathInfo
/// instances, for all SourceFiles in the module.
static llvm::StringMap<llvm::StringMap<SourceFilePathInfo>>
getInfoForUsedFileNames(const ModuleDecl *module) {
  llvm::StringMap<llvm::StringMap<SourceFilePathInfo>> result;

  for (auto *file : module->getFiles()) {
    auto *sourceFile = dyn_cast<SourceFile>(file);
    if (!sourceFile) continue;

    for (auto &pair : sourceFile->getInfoForUsedFilePaths()) {
      StringRef fullPath = pair.first();
      StringRef fileName = llvm::sys::path::filename(fullPath);
      auto &info = pair.second;

      result[fileName][fullPath].merge(info);
    }
  }

  return result;
}

static void computeFileID(const ModuleDecl *module, StringRef name,
                          SmallVectorImpl<char> &result) {
  result.assign(module->getNameStr().begin(), module->getNameStr().end());
  result.push_back('/');
  result.append(name.begin(), name.end());
}

static StringRef
resolveFileIDConflicts(const ModuleDecl *module, StringRef fileString,
                       const llvm::StringMap<SourceFilePathInfo> &paths,
                       bool shouldDiagnose) {
  assert(paths.size() > 1);

  /// The path we consider to be "correct"; we will emit fix-its changing the
  /// other paths to match this one.
  StringRef winner = "";

  // First, select a winner.
  for (const auto &pathPair : paths) {
    // If there is a physical file with this name, we use its path and stop
    // looking.
    if (pathPair.second.physicalFileLoc.isValid()) {
      winner = pathPair.first();
      break;
    }

    // Otherwise, we favor the lexicographically "smaller" path.
    if (winner.empty() || winner > pathPair.first()) {
      winner = pathPair.first();
    }
  }

  // If we're not diagnosing, that's all we need to do.
  if (!shouldDiagnose)
    return winner;

  SmallString<64> winnerLiteral;
  llvm::raw_svector_ostream winnerLiteralStream{winnerLiteral};
  swift::printAsQuotedString(winnerLiteralStream, winner);

  auto &diags = module->getASTContext().Diags;

  // Diagnose the conflict at each #sourceLocation that specifies it.
  for (const auto &pathPair : paths) {
    bool isWinner = (pathPair.first() == winner);

    // Don't diagnose #sourceLocations that match the physical file.
    if (pathPair.second.physicalFileLoc.isValid()) {
      if (!isWinner) {
        // The driver is responsible for diagnosing this, but naughty people who
        // have directly invoked the frontend could make it happen here instead.
        StringRef filename = llvm::sys::path::filename(winner);
        diags.diagnose(SourceLoc(), diag::error_two_files_same_name,
                       filename, winner, pathPair.first());
        diags.diagnose(SourceLoc(), diag::note_explain_two_files_same_name);
      }
      continue;
    }

    for (auto loc : pathPair.second.virtualFileLocs) {
      diags.diagnose(loc,
                     diag::source_location_creates_file_id_conflicts,
                     fileString);

      // Offer a fix-it unless it would be tautological.
      if (!isWinner)
        diags.diagnose(loc, diag::fixit_correct_source_location_file, winner)
          .fixItReplace(loc, winnerLiteral);
    }
  }

  return winner;
}

llvm::StringMap<std::pair<std::string, bool>>
ModuleDecl::computeFileIDMap(bool shouldDiagnose) const {
  llvm::StringMap<std::pair<std::string, bool>> result;
  SmallString<64> scratch;

  for (auto &namePair : getInfoForUsedFileNames(this)) {
    computeFileID(this, namePair.first(), scratch);
    auto &infoForPaths = namePair.second;

    assert(!infoForPaths.empty());

    // TODO: In the future, we'd like to handle these conflicts gracefully by
    // generating a unique `#fileID` string for each conflicting name. For now,
    // we will simply warn about conflicts.
    StringRef winner = infoForPaths.begin()->first();
    if (infoForPaths.size() > 1)
      winner = resolveFileIDConflicts(this, scratch, infoForPaths,
                                      shouldDiagnose);

    for (auto &pathPair : infoForPaths) {
      result[pathPair.first()] =
          std::make_pair(scratch.str().str(), pathPair.first() == winner);
    }
  }

  return result;
}

SourceFile::SourceFile(ModuleDecl &M, SourceFileKind K,
                       Optional<unsigned> bufferID,
                       ParsingOptions parsingOpts, bool isPrimary)
    : FileUnit(FileUnitKind::Source, M), BufferID(bufferID ? *bufferID : -1),
      ParsingOpts(parsingOpts), IsPrimary(isPrimary), Kind(K) {
  M.getASTContext().addDestructorCleanup(*this);

  assert(!IsPrimary || M.isMainModule() &&
         "A primary cannot appear outside the main module");

  if (isScriptMode()) {
    bool problem = M.registerEntryPointFile(this, SourceLoc(), None);
    assert(!problem && "multiple main files?");
    (void)problem;
  }
}

SourceFile::ParsingOptions
SourceFile::getDefaultParsingOptions(const LangOptions &langOpts) {
  ParsingOptions opts;
  if (langOpts.DisablePoundIfEvaluation)
    opts |= ParsingFlags::DisablePoundIfEvaluation;
  if (langOpts.BuildSyntaxTree)
    opts |= ParsingFlags::BuildSyntaxTree;
  if (langOpts.CollectParsedToken)
    opts |= ParsingFlags::CollectParsedTokens;
  return opts;
}

ArrayRef<Token> SourceFile::getAllTokens() const {
  assert(shouldCollectTokens() && "Disabled");
  auto &eval = getASTContext().evaluator;
  auto *mutableThis = const_cast<SourceFile *>(this);
  return *evaluateOrDefault(eval, ParseSourceFileRequest{mutableThis}, {})
              .CollectedTokens;
}

bool SourceFile::shouldCollectTokens() const {
  return Kind != SourceFileKind::SIL &&
         ParsingOpts.contains(ParsingFlags::CollectParsedTokens);
}

bool SourceFile::shouldBuildSyntaxTree() const {
  return Kind != SourceFileKind::SIL &&
         ParsingOpts.contains(ParsingFlags::BuildSyntaxTree);
}

bool SourceFile::hasDelayedBodyParsing() const {
  if (ParsingOpts.contains(ParsingFlags::DisableDelayedBodies))
    return false;

  // Not supported right now.
  if (Kind == SourceFileKind::SIL)
    return false;
  if (shouldCollectTokens())
    return false;
  if (shouldBuildSyntaxTree())
    return false;

  return true;
}

/// Add a hoisted declaration. See Decl::isHoisted().
void SourceFile::addHoistedDecl(Decl *d) {
  assert(d->isHoisted());
  Hoisted.push_back(d);
}

ArrayRef<Decl *> SourceFile::getTopLevelDecls() const {
  auto &ctx = getASTContext();
  auto *mutableThis = const_cast<SourceFile *>(this);
  return evaluateOrDefault(ctx.evaluator, ParseSourceFileRequest{mutableThis},
                           {}).TopLevelDecls;
}

ArrayRef<Decl *> SourceFile::getHoistedDecls() const {
  return Hoisted;
}

bool FileUnit::walk(ASTWalker &walker) {
  SmallVector<Decl *, 64> Decls;
  getTopLevelDecls(Decls);
  llvm::SaveAndRestore<ASTWalker::ParentTy> SAR(walker.Parent,
                                                getParentModule());
  for (Decl *D : Decls) {
#ifndef NDEBUG
    PrettyStackTraceDecl debugStack("walking into decl", D);
#endif

    if (D->walk(walker))
      return true;

    if (walker.shouldWalkAccessorsTheOldWay()) {
      // Pretend that accessors share a parent with the storage.
      //
      // FIXME: Update existing ASTWalkers to deal with accessors appearing as
      // children of the storage instead.
      if (auto *ASD = dyn_cast<AbstractStorageDecl>(D)) {
        for (auto AD : ASD->getAllAccessors()) {
          if (AD->walk(walker))
            return true;
        }
      }
    }
  }

  return false;
}

bool SourceFile::walk(ASTWalker &walker) {
  llvm::SaveAndRestore<ASTWalker::ParentTy> SAR(walker.Parent,
                                                getParentModule());
  for (Decl *D : getTopLevelDecls()) {
#ifndef NDEBUG
    PrettyStackTraceDecl debugStack("walking into decl", D);
#endif

    if (D->walk(walker))
      return true;

    if (walker.shouldWalkAccessorsTheOldWay()) {
      // Pretend that accessors share a parent with the storage.
      //
      // FIXME: Update existing ASTWalkers to deal with accessors appearing as
      // children of the storage instead.
      if (auto *ASD = dyn_cast<AbstractStorageDecl>(D)) {
        for (auto AD : ASD->getAllAccessors()) {
          if (AD->walk(walker))
            return true;
        }
      }
    }
  }
  return false;
}

StringRef SourceFile::getFilename() const {
  if (BufferID == -1)
    return "";
  SourceManager &SM = getASTContext().SourceMgr;
  return SM.getIdentifierForBuffer(BufferID);
}

ASTScope &SourceFile::getScope() {
  if (!Scope)
    Scope = new (getASTContext()) ASTScope(this);
  return *Scope.get();
}

Identifier
SourceFile::getDiscriminatorForPrivateValue(const ValueDecl *D) const {
  assert(D->getDeclContext()->getModuleScopeContext() == this);

  if (!PrivateDiscriminator.empty())
    return PrivateDiscriminator;

  StringRef name = getFilename();
  if (name.empty()) {
    assert(1 == count_if(getParentModule()->getFiles(),
                         [](const FileUnit *FU) -> bool {
                           return isa<SourceFile>(FU) &&
                                  cast<SourceFile>(FU)->getFilename().empty();
                         }) &&
           "can't promise uniqueness if multiple source files are nameless");

    // We still need a discriminator, so keep going.
  }

  // Use a hash of the basename of the source file as our discriminator.
  // This keeps us from leaking information about the original filename
  // while still providing uniqueness. Using the basename makes the
  // discriminator invariant across source checkout locations.
  // FIXME: Use a faster hash here? We don't need security, just uniqueness.
  llvm::MD5 hash;
  hash.update(getParentModule()->getName().str());
  hash.update(llvm::sys::path::filename(name));
  llvm::MD5::MD5Result result;
  hash.final(result);

  // Use the hash as a hex string, prefixed with an underscore to make sure
  // it is a valid identifier.
  // FIXME: There are more compact ways to encode a 16-byte value.
  SmallString<33> buffer{"_"};
  SmallString<32> hashString;
  llvm::MD5::stringifyResult(result, hashString);
  buffer += hashString;
  PrivateDiscriminator = getASTContext().getIdentifier(buffer.str().upper());
  return PrivateDiscriminator;
}

SynthesizedFileUnit &SourceFile::getOrCreateSynthesizedFile() {
  if (SynthesizedFile)
    return *SynthesizedFile;
  SynthesizedFile = new (getASTContext()) SynthesizedFileUnit(*this);
  getParentModule()->addFile(*SynthesizedFile);
  return *SynthesizedFile;
}

TypeRefinementContext *SourceFile::getTypeRefinementContext() {
  return TRC;
}

void SourceFile::setTypeRefinementContext(TypeRefinementContext *Root) {
  TRC = Root;
}

ArrayRef<OpaqueTypeDecl *> SourceFile::getOpaqueReturnTypeDecls() {
  for (auto *vd : UnvalidatedDeclsWithOpaqueReturnTypes.takeVector()) {
    if (auto opaqueDecl = vd->getOpaqueResultTypeDecl()) {
      auto inserted = ValidatedOpaqueReturnTypes.insert(
                {opaqueDecl->getOpaqueReturnTypeIdentifier().str(),
                 opaqueDecl});
      if (inserted.second) {
        OpaqueReturnTypes.push_back(opaqueDecl);
      }
    }
  }

  return OpaqueReturnTypes;
}

OpaqueTypeDecl *
SourceFile::lookupOpaqueResultType(StringRef MangledName) {
  // Check already-validated decls.
  auto found = ValidatedOpaqueReturnTypes.find(MangledName);
  if (found != ValidatedOpaqueReturnTypes.end())
    return found->second;

  // If there are unvalidated decls with opaque types, go through and validate
  // them now.
  (void) getOpaqueReturnTypeDecls();

  found = ValidatedOpaqueReturnTypes.find(MangledName);
  if (found != ValidatedOpaqueReturnTypes.end())
    return found->second;

  // Otherwise, we don't have a matching opaque decl.
  return nullptr;
}

//===----------------------------------------------------------------------===//
// SynthesizedFileUnit Implementation
//===----------------------------------------------------------------------===//

SynthesizedFileUnit::SynthesizedFileUnit(SourceFile &SF)
    : FileUnit(FileUnitKind::Synthesized, *SF.getParentModule()), SF(SF) {
  SF.getASTContext().addDestructorCleanup(*this);
}

Identifier
SynthesizedFileUnit::getDiscriminatorForPrivateValue(const ValueDecl *D) const {
  assert(D->getDeclContext()->getModuleScopeContext() == this);

  // Use cached primitive discriminator if it exists.
  if (!PrivateDiscriminator.empty())
    return PrivateDiscriminator;

  StringRef sourceFileName = getSourceFile().getFilename();
  if (sourceFileName.empty()) {
    assert(1 == count_if(getParentModule()->getFiles(),
                         [](const FileUnit *FU) -> bool {
                           return isa<SourceFile>(FU) &&
                                  cast<SourceFile>(FU)->getFilename().empty();
                         }) &&
           "Cannot promise uniqueness if multiple source files are nameless");
  }

  // Use a discriminator invariant across Swift version: a hash of the module
  // name, the parent source file name, and a special string.
  llvm::MD5 hash;
  hash.update(getParentModule()->getName().str());
  hash.update(llvm::sys::path::filename(sourceFileName));
  // TODO: Use a more robust discriminator for synthesized files. Pick something
  // that cannot conflict with `SourceFile` discriminators.
  hash.update("SYNTHESIZED FILE");
  llvm::MD5::MD5Result result;
  hash.final(result);

  // Use the hash as a hex string, prefixed with an underscore to make sure
  // it is a valid identifier.
  // FIXME: There are more compact ways to encode a 16-byte value.
  SmallString<33> buffer{"_"};
  SmallString<32> hashString;
  llvm::MD5::stringifyResult(result, hashString);
  buffer += hashString;
  PrivateDiscriminator = getASTContext().getIdentifier(buffer.str().upper());
  return PrivateDiscriminator;
}

void SynthesizedFileUnit::lookupValue(
    DeclName name, NLKind lookupKind,
    SmallVectorImpl<ValueDecl *> &result) const {
  for (auto *decl : TopLevelDecls) {
    if (auto *VD = dyn_cast<ValueDecl>(decl))
      if (VD->getName().matchesRef(name))
        result.push_back(VD);
  }
}

void SynthesizedFileUnit::lookupObjCMethods(
    ObjCSelector selector,
    SmallVectorImpl<AbstractFunctionDecl *> &results) const {
  // Synthesized files only contain top-level declarations, no `@objc` methods.
}

void SynthesizedFileUnit::getTopLevelDecls(
    SmallVectorImpl<swift::Decl *> &results) const {
  results.append(TopLevelDecls.begin(), TopLevelDecls.end());
}

//===----------------------------------------------------------------------===//
// Miscellaneous
//===----------------------------------------------------------------------===//

void FileUnit::anchor() {}
void *FileUnit::operator new(size_t Bytes, ASTContext &C, unsigned Alignment) {
  return C.Allocate(Bytes, Alignment);
}

void FileUnit::getTopLevelDeclsWhereAttributesMatch(
            SmallVectorImpl<Decl*> &Results,
            llvm::function_ref<bool(DeclAttributes)> matchAttributes) const {
  auto prevSize = Results.size();
  getTopLevelDecls(Results);

  // Filter out unwanted decls that were just added to Results.
  // Note: We could apply this check in all implementations of
  // getTopLevelDecls instead or in everything that creates a Decl.
  auto newEnd = std::remove_if(Results.begin() + prevSize, Results.end(),
                               [&matchAttributes](const Decl *D) -> bool {
      return !matchAttributes(D->getAttrs());
    });
  Results.erase(newEnd, Results.end());
}

void swift::simple_display(llvm::raw_ostream &out, const FileUnit *file) {
  if (!file) {
    out << "(null)";
    return;
  }

  switch (file->getKind()) {
  case FileUnitKind::Source:
    out << '\"' << cast<SourceFile>(file)->getFilename() << '\"';
    return;
  case FileUnitKind::Builtin:
    out << "(Builtin)";
    return;
  case FileUnitKind::Synthesized:
    out << "(synthesized)";
    return;
  case FileUnitKind::DWARFModule:
  case FileUnitKind::ClangModule:
  case FileUnitKind::SerializedAST:
    out << '\"' << cast<LoadedFile>(file)->getFilename() << '\"';
    return;
  }
  llvm_unreachable("Unhandled case in switch");
}

StringRef LoadedFile::getFilename() const {
  return "";
}

static const clang::Module *
getClangModule(llvm::PointerUnion<const ModuleDecl *, const void *> Union) {
  return static_cast<const clang::Module *>(Union.get<const void *>());
}

StringRef ModuleEntity::getName() const {
  assert(!Mod.isNull());
  if (auto SwiftMod = Mod.dyn_cast<const ModuleDecl*>())
    return SwiftMod->getName().str();
  return getClangModule(Mod)->Name;
}

std::string ModuleEntity::getFullName() const {
  assert(!Mod.isNull());
  if (auto SwiftMod = Mod.dyn_cast<const ModuleDecl*>())
    return std::string(SwiftMod->getName());
  return getClangModule(Mod)->getFullModuleName();
}

bool ModuleEntity::isSystemModule() const {
  assert(!Mod.isNull());
  if (auto SwiftMod = Mod.dyn_cast<const ModuleDecl*>())
    return SwiftMod->isSystemModule();
  return getClangModule(Mod)->IsSystem;
}

bool ModuleEntity::isBuiltinModule() const {
  assert(!Mod.isNull());
  if (auto SwiftMod = Mod.dyn_cast<const ModuleDecl*>())
    return SwiftMod->isBuiltinModule();
  return false;
}

const ModuleDecl* ModuleEntity::getAsSwiftModule() const {
  assert(!Mod.isNull());
  if (auto SwiftMod = Mod.dyn_cast<const ModuleDecl*>())
    return SwiftMod;
  return nullptr;
}

const clang::Module* ModuleEntity::getAsClangModule() const {
  assert(!Mod.isNull());
  if (Mod.is<const ModuleDecl*>())
    return nullptr;
  return getClangModule(Mod);
}

// See swift/Basic/Statistic.h for declaration: this enables tracing SourceFiles, is
// defined here to avoid too much layering violation / circular linkage
// dependency.

struct SourceFileTraceFormatter : public UnifiedStatsReporter::TraceFormatter {
  void traceName(const void *Entity, raw_ostream &OS) const override {
    if (!Entity)
      return;
    const SourceFile *SF = static_cast<const SourceFile *>(Entity);
    OS << llvm::sys::path::filename(SF->getFilename());
  }
  void traceLoc(const void *Entity, SourceManager *SM,
                clang::SourceManager *CSM, raw_ostream &OS) const override {
    // SourceFiles don't have SourceLocs of their own; they contain them.
  }
};

static SourceFileTraceFormatter TF;

template<>
const UnifiedStatsReporter::TraceFormatter*
FrontendStatsTracer::getTraceFormatter<const SourceFile *>() {
  return &TF;
}
