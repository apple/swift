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
#include "swift/AST/Import.h"
#include "swift/AST/ImportCache.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/LinkLibrary.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/PackConformance.h"
#include "swift/AST/ParseRequests.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/PrintOptions.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/SynthesizedFileUnit.h"
#include "swift/AST/Type.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Compiler.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/StringExtras.h"
#include "swift/Demangling/ManglingMacros.h"
#include "swift/Parse/Token.h"
#include "swift/Strings.h"
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
#include "llvm/Support/YAMLTraits.h"

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
  DeclName UniqueMacroNamePlaceholder;

  template<typename T>
  using OperatorMap = llvm::DenseMap<Identifier, TinyPtrVector<T *>>;
  OperatorMap<OperatorDecl> Operators;
  OperatorMap<PrecedenceGroupDecl> PrecedenceGroups;

  template<typename Range>
  void addToUnqualifiedLookupCache(Range decls, bool onlyOperators);
  template<typename Range>
  void addToMemberCache(Range decls);

  using AuxiliaryDeclMap = llvm::DenseMap<DeclName, TinyPtrVector<MissingDecl *>>;
  AuxiliaryDeclMap TopLevelAuxiliaryDecls;

  /// Top-level macros that produce arbitrary names.
  SmallVector<MissingDecl *, 4> TopLevelArbitraryMacros;

  SmallVector<llvm::PointerUnion<Decl *, MacroExpansionExpr *>, 4>
      MayHaveAuxiliaryDecls;
  void populateAuxiliaryDeclCache();
  SourceLookupCache(ASTContext &ctx);

public:
  SourceLookupCache(const SourceFile &SF);
  SourceLookupCache(const ModuleDecl &Mod);

  void lookupValue(DeclName Name, NLKind LookupKind,
                   OptionSet<ModuleLookupFlags> Flags,
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

static Expr *getAsExpr(Decl *decl) { return nullptr; }
static Decl *getAsDecl(Decl *decl) { return decl; }

static Expr *getAsExpr(ASTNode node) { return node.dyn_cast<Expr *>(); }
static Decl *getAsDecl(ASTNode node) { return node.dyn_cast<Decl *>(); }

template<typename Range>
void SourceLookupCache::addToUnqualifiedLookupCache(Range items,
                                                    bool onlyOperators) {
  for (auto item : items) {
    // In script mode, we'll see macro expansion expressions for freestanding
    // macros.
    if (Expr *E = getAsExpr(item)) {
      if (auto MEE = dyn_cast<MacroExpansionExpr>(E)) {
        if (!onlyOperators)
          MayHaveAuxiliaryDecls.push_back(MEE);
      }
      continue;
    }

    Decl *D = getAsDecl(item);
    if (!D)
      continue;

    if (auto *VD = dyn_cast<ValueDecl>(D)) {
      if (onlyOperators ? VD->isOperator() : VD->hasName()) {
        // Cache the value under both its compound name and its full name.
        TopLevelValues.add(VD);

        if (!onlyOperators && VD->getAttrs().hasAttribute<CustomAttr>()) {
          MayHaveAuxiliaryDecls.push_back(VD);
        }
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

      if (ED->getAttrs().hasAttribute<CustomAttr>()) {
        MayHaveAuxiliaryDecls.push_back(ED);
      }

      if (!ED->hasUnparsedMembers() || ED->maybeHasOperatorDeclarations())
        addToUnqualifiedLookupCache(ED->getMembers(), true);
    }

    if (auto *OD = dyn_cast<OperatorDecl>(D))
      Operators[OD->getName()].push_back(OD);

    else if (auto *PG = dyn_cast<PrecedenceGroupDecl>(D))
      PrecedenceGroups[PG->getName()].push_back(PG);

    else if (auto *MED = dyn_cast<MacroExpansionDecl>(D)) {
      if (!onlyOperators)
        MayHaveAuxiliaryDecls.push_back(MED);
    } else if (auto TLCD = dyn_cast<TopLevelCodeDecl>(D)) {
      if (auto body = TLCD->getBody()){
        addToUnqualifiedLookupCache(body->getElements(), onlyOperators);
      }
    }
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
    assert(isa<SourceFile>(file) ||
           isa<SynthesizedFileUnit>(file));
    SmallVector<Decl *, 8> decls;
    file->getTopLevelDecls(decls);
    addToMemberCache(decls);
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

void SourceLookupCache::populateAuxiliaryDeclCache() {
  using MacroRef = llvm::PointerUnion<FreestandingMacroExpansion *, CustomAttr *>;
  for (auto item : MayHaveAuxiliaryDecls) {
    TopLevelCodeDecl *topLevelCodeDecl = nullptr;

    // Gather macro-introduced peer names.
    llvm::SmallDenseMap<MacroRef, llvm::SmallVector<DeclName, 2>>
        introducedNames;

    /// Introduce names for a freestanding macro.
    auto introduceNamesForFreestandingMacro =
      [&](FreestandingMacroExpansion *macroRef, Decl *decl, MacroRole role) {
        bool introducesArbitraryNames = false;
        namelookup::forEachPotentialResolvedMacro(
            decl->getDeclContext()->getModuleScopeContext(),
            macroRef->getMacroName(), role,
            [&](MacroDecl *macro, const MacroRoleAttr *roleAttr) {
              // First check for arbitrary names.
              if (roleAttr->hasNameKind(MacroIntroducedDeclNameKind::Arbitrary)) {
                introducesArbitraryNames = true;
              }

              macro->getIntroducedNames(role,
                                        /*attachedTo*/ nullptr,
                                        introducedNames[macroRef]);
            });

        return introducesArbitraryNames;
      };

    // Handle macro expansion expressions, which show up in when we have
    // freestanding macros in "script" mode.
    if (auto expr = item.dyn_cast<MacroExpansionExpr *>()) {
      topLevelCodeDecl = dyn_cast<TopLevelCodeDecl>(expr->getDeclContext());
      if (topLevelCodeDecl) {
        bool introducesArbitraryNames = false;
        if (introduceNamesForFreestandingMacro(
                expr, topLevelCodeDecl, MacroRole::Declaration))
          introducesArbitraryNames = true;

        if (introduceNamesForFreestandingMacro(
                expr, topLevelCodeDecl, MacroRole::CodeItem))
          introducesArbitraryNames = true;

        // Record this macro if it introduces arbitrary names.
        if (introducesArbitraryNames) {
          TopLevelArbitraryMacros.push_back(
              MissingDecl::forUnexpandedMacro(expr, topLevelCodeDecl));
        }
      }
    }

    auto *decl = item.dyn_cast<Decl *>();
    if (decl) {
      // This code deliberately avoids `forEachAttachedMacro`, because it
      // will perform overload resolution and possibly invoke unqualified
      // lookup for macro arguments, which will recursively populate the
      // auxiliary decl cache and cause request cycles.
      //
      // We do not need a fully resolved macro until expansion. Instead, we
      // conservatively consider peer names for all macro declarations with a
      // custom attribute name. Unqualified lookup for that name will later
      // invoke expansion of the macro, and will yield no results if the resolved
      // macro does not produce the requested name, so the only impact is possibly
      // expanding earlier than needed / unnecessarily looking in the top-level
      // auxiliary decl cache.
      for (auto attrConst : decl->getAttrs().getAttributes<CustomAttr>()) {
        auto *attr = const_cast<CustomAttr *>(attrConst);
        UnresolvedMacroReference macroRef(attr);
        bool introducesArbitraryNames = false;
        namelookup::forEachPotentialResolvedMacro(
            decl->getDeclContext()->getModuleScopeContext(),
            macroRef.getMacroName(), MacroRole::Peer,
            [&](MacroDecl *macro, const MacroRoleAttr *roleAttr) {
              // First check for arbitrary names.
              if (roleAttr->hasNameKind(
                      MacroIntroducedDeclNameKind::Arbitrary)) {
                introducesArbitraryNames = true;
              }

              macro->getIntroducedNames(MacroRole::Peer,
                                        dyn_cast<ValueDecl>(decl),
                                        introducedNames[attr]);
            });

        // Record this macro where appropriate.
        if (introducesArbitraryNames)
          TopLevelArbitraryMacros.push_back(
              MissingDecl::forUnexpandedMacro(attr, decl));
      }
    }

    if (auto *med = dyn_cast_or_null<MacroExpansionDecl>(decl)) {
      bool introducesArbitraryNames =
          introduceNamesForFreestandingMacro(med, decl, MacroRole::Declaration);

      // Note whether this macro produces arbitrary names.
      if (introducesArbitraryNames)
        TopLevelArbitraryMacros.push_back(MissingDecl::forUnexpandedMacro(med, decl));
    }

    // Add macro-introduced names to the top-level auxiliary decl cache as
    // unexpanded decls represented by a MissingDecl.
    auto anchorDecl = decl ? decl : topLevelCodeDecl;
    for (auto macroNames : introducedNames) {
      auto macroRef = macroNames.getFirst();
      for (auto name : macroNames.getSecond()) {
        auto *placeholder = MissingDecl::forUnexpandedMacro(macroRef, anchorDecl);
        name.addToLookupTable(TopLevelAuxiliaryDecls, placeholder);
      }
    }
  }

  MayHaveAuxiliaryDecls.clear();
}

SourceLookupCache::SourceLookupCache(ASTContext &ctx)
  : UniqueMacroNamePlaceholder(MacroDecl::getUniqueNamePlaceholder(ctx)) { }

/// Populate our cache on the first name lookup.
SourceLookupCache::SourceLookupCache(const SourceFile &SF)
    : SourceLookupCache(SF.getASTContext())
{
  FrontendStatsTracer tracer(SF.getASTContext().Stats,
                             "source-file-populate-cache");
  addToUnqualifiedLookupCache(SF.getTopLevelItems(), false);
  addToUnqualifiedLookupCache(SF.getHoistedDecls(), false);
}

SourceLookupCache::SourceLookupCache(const ModuleDecl &M)
  : SourceLookupCache(M.getASTContext())
{
  FrontendStatsTracer tracer(M.getASTContext().Stats,
                             "module-populate-cache");
  for (const FileUnit *file : M.getFiles()) {
    auto *SF = cast<SourceFile>(file);
    addToUnqualifiedLookupCache(SF->getTopLevelItems(), false);
    addToUnqualifiedLookupCache(SF->getHoistedDecls(), false);

    if (auto *SFU = file->getSynthesizedFile()) {
      addToUnqualifiedLookupCache(SFU->getTopLevelDecls(), false);
    }
  }
}

void SourceLookupCache::lookupValue(DeclName Name, NLKind LookupKind,
                                    OptionSet<ModuleLookupFlags> Flags,
                                    SmallVectorImpl<ValueDecl*> &Result) {
  auto I = TopLevelValues.find(Name);
  if (I != TopLevelValues.end()) {
    Result.reserve(I->second.size());
    for (ValueDecl *Elt : I->second)
      Result.push_back(Elt);
  }

  // If we aren't supposed to find names introduced by macros, we're done.
  if (Flags.contains(ModuleLookupFlags::ExcludeMacroExpansions))
    return;

  // Add top-level auxiliary decls to the result.
  //
  // FIXME: We need to not consider auxiliary decls if we're doing lookup
  // from inside a macro argument at module scope.
  populateAuxiliaryDeclCache();
  DeclName keyName = MacroDecl::isUniqueMacroName(Name.getBaseName())
    ? UniqueMacroNamePlaceholder
    : Name;
  auto auxDecls = TopLevelAuxiliaryDecls.find(keyName);

  // Check macro expansions that could produce this name.
  SmallVector<MissingDecl *, 4> unexpandedDecls;
  if (auxDecls != TopLevelAuxiliaryDecls.end()) {
    unexpandedDecls.insert(
      unexpandedDecls.end(), auxDecls->second.begin(), auxDecls->second.end());
  }

  // Check macro expansions that can produce arbitrary names.
  unexpandedDecls.insert(
      unexpandedDecls.end(),
      TopLevelArbitraryMacros.begin(), TopLevelArbitraryMacros.end());

  if (unexpandedDecls.empty())
    return;

  // Add matching expanded peers and freestanding declarations to the results.
  SmallPtrSet<ValueDecl *, 4> macroExpandedDecls;
  for (auto *unexpandedDecl : unexpandedDecls) {
    unexpandedDecl->forEachMacroExpandedDecl(
        [&](ValueDecl *decl) {
          if (decl->getName().matchesRef(Name)) {
            if (macroExpandedDecls.insert(decl).second)
              Result.push_back(decl);
          }
        });
  }
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

  populateAuxiliaryDeclCache();
  SmallVector<MissingDecl *, 4> unexpandedDecls;
  for (auto &entry : TopLevelAuxiliaryDecls) {
    for (auto &decl : entry.second) {
      (void) decl;
      unexpandedDecls.append(entry.second.begin(), entry.second.end());
    }
  }

  // Store macro expanded decls in a 'SmallSetVector' because different
  // MissingDecls might be created by a single macro expansion. (e.g. multiple
  // 'names' in macro role attributes). Since expansions are cached, it doesn't
  // cause duplicated expansions, but different 'unexpandedDecl' may report the
  // same 'ValueDecl'.
  llvm::SmallSetVector<ValueDecl *, 4> macroExpandedDecls;
  for (MissingDecl *unexpandedDecl : unexpandedDecls) {
    unexpandedDecl->forEachMacroExpandedDecl([&](ValueDecl *vd) {
      macroExpandedDecls.insert(vd);
    });
  }
  for (auto *vd : macroExpandedDecls) {
    Consumer.foundDecl(vd, DeclVisibilityKind::VisibleAtTopLevel);
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
  Bits.ModuleDecl.HasHermeticSealAtLink = 0;
  Bits.ModuleDecl.IsEmbeddedSwiftModule = 0;
  Bits.ModuleDecl.IsConcurrencyChecked = 0;
  Bits.ModuleDecl.ObjCNameLookupCachePopulated = 0;
  Bits.ModuleDecl.HasCxxInteroperability = 0;
  Bits.ModuleDecl.AllowNonResilientAccess = 0;
  Bits.ModuleDecl.SerializePackageEnabled = 0;
}

void ModuleDecl::setIsSystemModule(bool flag) {
  Bits.ModuleDecl.IsSystemModule = flag;
}

bool ModuleDecl::isNonUserModule() const {
  // For clang submodules, retrieve their top level module (submodules have no
  // source path, so we'd always return false for them).
  ModuleDecl *mod = const_cast<ModuleDecl *>(this)->getTopLevelModule();

  auto &evaluator = getASTContext().evaluator;
  return evaluateOrDefault(evaluator, IsNonUserModuleRequest{mod}, false);
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

void ModuleDecl::addAuxiliaryFile(SourceFile &sourceFile) {
  AuxiliaryFiles.push_back(&sourceFile);
}

namespace {
  /// Compare the source location ranges for two files, as an ordering to
  /// use for fast searches.
  struct SourceFileRangeComparison {
    SourceManager *sourceMgr;

    bool operator()(SourceFile *lhs, SourceFile *rhs) const {
      auto lhsRange = sourceMgr->getRangeForBuffer(*lhs->getBufferID());
      auto rhsRange = sourceMgr->getRangeForBuffer(*rhs->getBufferID());

      std::less<const char *> pointerCompare;
      return pointerCompare(
          (const char *)lhsRange.getStart().getOpaquePointerValue(),
          (const char *)rhsRange.getStart().getOpaquePointerValue());
    }

    bool operator()(SourceFile *lhs, SourceLoc rhsLoc) const {
      auto lhsRange = sourceMgr->getRangeForBuffer(*lhs->getBufferID());

      std::less<const char *> pointerCompare;
      return pointerCompare(
          (const char *)lhsRange.getEnd().getOpaquePointerValue(),
          (const char *)rhsLoc.getOpaquePointerValue());
    }

    bool operator()(SourceLoc lhsLoc, SourceFile *rhs) const {
      auto rhsRange = sourceMgr->getRangeForBuffer(*rhs->getBufferID());

      std::less<const char *> pointerCompare;
      return pointerCompare(
          (const char *)lhsLoc.getOpaquePointerValue(),
          (const char *)rhsRange.getEnd().getOpaquePointerValue());
    }
  };
}

class swift::ModuleSourceFileLocationMap {
public:
  unsigned numFiles = 0;
  unsigned numAuxiliaryFiles = 0;
  std::vector<SourceFile *> allSourceFiles;
  SourceFile *lastSourceFile = nullptr;
};

void ModuleDecl::updateSourceFileLocationMap() {
  // Allocate a source file location map, if we don't have one already.
  if (!sourceFileLocationMap) {
    ASTContext &ctx = getASTContext();
    sourceFileLocationMap = ctx.Allocate<ModuleSourceFileLocationMap>();
    ctx.addCleanup([sourceFileLocationMap=sourceFileLocationMap]() {
      sourceFileLocationMap->~ModuleSourceFileLocationMap();
    });
  }

  // If we are up-to-date, there's nothing to do.
  ArrayRef<FileUnit *> files = Files;
  if (sourceFileLocationMap->numFiles == files.size() &&
      sourceFileLocationMap->numAuxiliaryFiles ==
          AuxiliaryFiles.size())
    return;

  // Rebuild the range structure.
  sourceFileLocationMap->allSourceFiles.clear();

  // First, add all of the source files with a backing buffer.
  for (auto *fileUnit : files) {
    if (auto sourceFile = dyn_cast<SourceFile>(fileUnit)) {
      if (sourceFile->getBufferID())
        sourceFileLocationMap->allSourceFiles.push_back(sourceFile);
    }
  }

  // Next, add all of the macro expansion files.
  for (auto *sourceFile : AuxiliaryFiles)
    sourceFileLocationMap->allSourceFiles.push_back(sourceFile);

  // Finally, sort them all so we can do a binary search for lookup.
  std::sort(sourceFileLocationMap->allSourceFiles.begin(),
            sourceFileLocationMap->allSourceFiles.end(),
            SourceFileRangeComparison{&getASTContext().SourceMgr});

  sourceFileLocationMap->numFiles = files.size();
  sourceFileLocationMap->numAuxiliaryFiles = AuxiliaryFiles.size();
}

SourceFile *ModuleDecl::getSourceFileContainingLocation(SourceLoc loc) {
  if (loc.isInvalid())
    return nullptr;

  // Check whether this location is in a "replaced" range, in which case
  // we want to use the original source file.
  auto &sourceMgr = getASTContext().SourceMgr;
  SourceLoc adjustedLoc = loc;
  for (const auto &pair : sourceMgr.getReplacedRanges()) {
    if (sourceMgr.rangeContainsTokenLoc(pair.second, loc)) {
      adjustedLoc = pair.first.Start;
      break;
    }
  }

  // Before we do any extra work, check the last source file we found a result
  // in to see if it contains this.
  if (sourceFileLocationMap) {
    if (auto lastSourceFile = sourceFileLocationMap->lastSourceFile) {
      auto range = sourceMgr.getRangeForBuffer(*lastSourceFile->getBufferID());
      if (range.contains(adjustedLoc))
        return lastSourceFile;
    }
  }

  updateSourceFileLocationMap();

  auto found = std::lower_bound(sourceFileLocationMap->allSourceFiles.begin(),
                                sourceFileLocationMap->allSourceFiles.end(),
                                adjustedLoc,
                                SourceFileRangeComparison{&sourceMgr});
  if (found == sourceFileLocationMap->allSourceFiles.end())
    return nullptr;

  auto foundSourceFile = *found;
  auto foundRange = sourceMgr.getRangeForBuffer(*foundSourceFile->getBufferID());
  // Positions inside an empty file or at EOF should still be considered within
  // this file.
  if (!foundRange.contains(adjustedLoc) && adjustedLoc != foundRange.getEnd())
    return nullptr;

  // Update the last source file.
  sourceFileLocationMap->lastSourceFile = foundSourceFile;
  return foundSourceFile;
}

std::pair<unsigned, SourceLoc>
ModuleDecl::getOriginalLocation(SourceLoc loc) const {
  assert(loc.isValid());

  SourceManager &SM = getASTContext().SourceMgr;
  unsigned bufferID = SM.findBufferContainingLoc(loc);

  SourceLoc startLoc = loc;
  unsigned startBufferID = bufferID;
  while (std::optional<GeneratedSourceInfo> info =
             SM.getGeneratedSourceInfo(bufferID)) {
    switch (info->kind) {
#define MACRO_ROLE(Name, Description)  \
    case GeneratedSourceInfo::Name##MacroExpansion:
#include "swift/Basic/MacroRoles.def"
    {
      // Location was within a macro expansion, return the expansion site, not
      // the insertion location.
      if (info->attachedMacroCustomAttr) {
        loc = info->attachedMacroCustomAttr->getLocation();
      } else {
        ASTNode expansionNode = ASTNode::getFromOpaqueValue(info->astNode);
        loc = expansionNode.getStartLoc();
      }
      bufferID = SM.findBufferContainingLoc(loc);
      break;
    }
    case GeneratedSourceInfo::DefaultArgument:
      // No original location as it's not actually in any source file
    case GeneratedSourceInfo::ReplacedFunctionBody:
      // There's not really any "original" location for locations within
      // replaced function bodies. The body is actually different code to the
      // original file.
    case GeneratedSourceInfo::PrettyPrinted:
      // No original location, return the original buffer/location
      return {startBufferID, startLoc};
    }
  }

  return {bufferID, loc};
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

SourceFile *IDEInspectionFileRequest::evaluate(Evaluator &evaluator,
                                               ModuleDecl *mod) const {
  const auto &SM = mod->getASTContext().SourceMgr;
  assert(mod->isMainModule() && "Can only do completion in the main module");
  assert(SM.hasIDEInspectionTargetBuffer() && "Not in IDE inspection mode?");

  for (auto *file : mod->getFiles()) {
    auto *SF = dyn_cast<SourceFile>(file);
    if (SF && SF->getBufferID() == SM.getIDEInspectionTargetBufferID())
      return SF;
  }
  llvm_unreachable("Couldn't find the completion file?");
}

#define FORWARD(name, args)                                                    \
  for (const FileUnit *file : getFiles()) {                                    \
    file->name args;                                                           \
    if (auto *synth = file->getSynthesizedFile()) {                            \
      synth->name args;                                                        \
    }                                                                          \
  }

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
                             OptionSet<ModuleLookupFlags> Flags,
                             SmallVectorImpl<ValueDecl*> &Result) const {
  auto *stats = getASTContext().Stats;
  if (stats)
    ++stats->getFrontendCounters().NumModuleLookupValue;

  if (isParsedModule(this)) {
    getSourceLookupCache().lookupValue(Name, LookupKind, Flags, Result);
    return;
  }

  FORWARD(lookupValue, (Name, LookupKind, Flags, Result));
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
      auto discriminator = enclosingFile->getDiscriminatorForPrivateDecl(VD);
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
                              OptionSet<ModuleLookupFlags> Flags,
                              SmallVectorImpl<ValueDecl*> &result) const {
  getCache().lookupValue(name.getBaseIdentifier(), lookupKind, *this, result);
}

void BuiltinUnit::lookupObjCMethods(
       ObjCSelector selector,
       SmallVectorImpl<AbstractFunctionDecl *> &results) const {
  // No @objc methods in the Builtin module.
}

void SourceFile::lookupValue(DeclName name, NLKind lookupKind,
                             OptionSet<ModuleLookupFlags> flags,
                             SmallVectorImpl<ValueDecl*> &result) const {
  getCache().lookupValue(name, lookupKind, flags, result);
}

void ModuleDecl::lookupVisibleDecls(ImportPath::Access AccessPath,
                                    VisibleDeclConsumer &Consumer,
                                    NLKind LookupKind) const {
  if (isParsedModule(this)) {
    auto &cache = getSourceLookupCache();
    cache.lookupVisibleDecls(AccessPath, Consumer, LookupKind);
    assert(Cache.get() == &cache && "cache invalidated during lookup");
    return;
  }

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

ASTNode SourceFile::getMacroExpansion() const {
  if (Kind != SourceFileKind::MacroExpansion)
    return nullptr;

  return getNodeInEnclosingSourceFile();
}

SourceRange SourceFile::getMacroInsertionRange() const {
  if (Kind != SourceFileKind::MacroExpansion)
    return SourceRange();

  auto generatedInfo =
      *getASTContext().SourceMgr.getGeneratedSourceInfo(*getBufferID());
  auto origRange = generatedInfo.originalSourceRange;
  return {origRange.getStart(), origRange.getEnd()};
}

CustomAttr *SourceFile::getAttachedMacroAttribute() const {
  if (Kind != SourceFileKind::MacroExpansion)
    return nullptr;

  auto genInfo =
      *getASTContext().SourceMgr.getGeneratedSourceInfo(*getBufferID());
  return genInfo.attachedMacroCustomAttr;
}

std::optional<MacroRole> SourceFile::getFulfilledMacroRole() const {
  if (Kind != SourceFileKind::MacroExpansion)
    return std::nullopt;

  auto genInfo =
      *getASTContext().SourceMgr.getGeneratedSourceInfo(*getBufferID());
  switch (genInfo.kind) {
#define MACRO_ROLE(Name, Description)               \
  case GeneratedSourceInfo::Name##MacroExpansion: \
    return MacroRole::Name;
#include "swift/Basic/MacroRoles.def"

  case GeneratedSourceInfo::ReplacedFunctionBody:
  case GeneratedSourceInfo::PrettyPrinted:
  case GeneratedSourceInfo::DefaultArgument:
    return std::nullopt;
  }
}

SourceFile *SourceFile::getEnclosingSourceFile() const {
  if (Kind != SourceFileKind::MacroExpansion &&
      Kind != SourceFileKind::DefaultArgument)
    return nullptr;

  auto genInfo =
      *getASTContext().SourceMgr.getGeneratedSourceInfo(*getBufferID());
  auto sourceLoc = genInfo.originalSourceRange.getStart();
  return getParentModule()->getSourceFileContainingLocation(sourceLoc);
}

ASTNode SourceFile::getNodeInEnclosingSourceFile() const {
  if (Kind != SourceFileKind::MacroExpansion &&
      Kind != SourceFileKind::DefaultArgument)
    return nullptr;

  auto genInfo =
      *getASTContext().SourceMgr.getGeneratedSourceInfo(*getBufferID());
  return ASTNode::getFromOpaqueValue(genInfo.astNode);
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

bool ModuleDecl::shouldCollectDisplayDecls() const {
  for (const FileUnit *file : Files) {
    if (!file->shouldCollectDisplayDecls())
      return false;
  }
  return true;
}

void ModuleDecl::getLocalTypeDecls(SmallVectorImpl<TypeDecl*> &Results) const {
  FORWARD(getLocalTypeDecls, (Results));
}

void ModuleDecl::getTopLevelDecls(SmallVectorImpl<Decl*> &Results) const {
  FORWARD(getTopLevelDecls, (Results));
}

void ModuleDecl::getTopLevelDeclsWithAuxiliaryDecls(
    SmallVectorImpl<Decl *> &Results) const {
  FORWARD(getTopLevelDeclsWithAuxiliaryDecls, (Results));
}

void ModuleDecl::dumpDisplayDecls() const {
  SmallVector<Decl *, 32> Decls;
  getDisplayDecls(Decls);
  for (auto *D : Decls) {
    D->dump(llvm::errs());
    llvm::errs() << "\n";
  }
}

void ModuleDecl::dumpTopLevelDecls() const {
  SmallVector<Decl *, 32> Decls;
  getTopLevelDecls(Decls);
  for (auto *D : Decls) {
    D->dump(llvm::errs());
    llvm::errs() << "\n";
  }
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

void ModuleDecl::lookupTopLevelDeclsByObjCName(SmallVectorImpl<Decl *> &Results,
                                               DeclName name) {
  if (!isObjCNameLookupCachePopulated())
    populateObjCNameLookupCache();

  // A top level decl can't be special anyways
  if (name.isSpecial())
    return;

  auto resultsForFileUnit = ObjCNameLookupCache.find(name.getBaseIdentifier());
  if (resultsForFileUnit == ObjCNameLookupCache.end())
    return;

  Results.append(resultsForFileUnit->second.begin(),
                 resultsForFileUnit->second.end());
}

void ModuleDecl::populateObjCNameLookupCache() {
  SmallVector<Decl *> topLevelObjCExposedDeclsInFileUnit;
  auto hasObjCAttrNamePredicate = [](const DeclAttributes &attrs) -> bool {
    return attrs.hasAttribute<ObjCAttr>();
  };

  for (FileUnit *file : getFiles()) {
    file->getTopLevelDeclsWhereAttributesMatch(
        topLevelObjCExposedDeclsInFileUnit, hasObjCAttrNamePredicate);
    if (auto *synth = file->getSynthesizedFile()) {
      synth->getTopLevelDeclsWhereAttributesMatch(
          topLevelObjCExposedDeclsInFileUnit, hasObjCAttrNamePredicate);
    }
  }

  for (Decl *decl : topLevelObjCExposedDeclsInFileUnit) {
    if (ValueDecl *VD = dyn_cast<ValueDecl>(decl); VD && VD->hasName()) {
      const auto &declObjCAttribute = VD->getAttrs().getAttribute<ObjCAttr>();
      // No top level decl (class, protocol, extension etc.) is allowed to have a
      // compound name, @objc provided or otherwise. Global functions are allowed to
      // have compound names, but not allowed to have @objc attributes. Thus we
      // are sure to not hit asserts getting the simple name.
      //
      // Similarly, init, dealloc and subscript (the special names) can't be top 
      // level decls, so we won't hit asserts getting the base identifier out of the
      // value decl.
      const Identifier &declObjCName =
          declObjCAttribute->hasName()
              ? declObjCAttribute->getName()->getSimpleName()
              : VD->getName().getBaseIdentifier();
      ObjCNameLookupCache[declObjCName].push_back(decl);
    }
  }

  setIsObjCNameLookupCachePopulated(true);
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
  auto decls = getLocalTypeDecls();
  Results.append(decls.begin(), decls.end());
}

void
SourceFile::getOpaqueReturnTypeDecls(SmallVectorImpl<OpaqueTypeDecl*> &Results)
const {
  auto result = const_cast<SourceFile *>(this)->getOpaqueReturnTypeDecls();
  llvm::copy(result, std::back_inserter(Results));
}

TypeDecl *SourceFile::lookupLocalType(llvm::StringRef mangledName) const {
  ASTContext &ctx = getASTContext();
  for (auto typeDecl : getLocalTypeDecls()) {
    auto typeMangledName = evaluateOrDefault(ctx.evaluator,
                                             MangleLocalTypeDeclRequest { typeDecl },
                                             std::string());
    if (mangledName == typeMangledName)
      return typeDecl;
  }

  return nullptr;
}

std::optional<ExternalSourceLocs::RawLocs>
SourceFile::getExternalRawLocsForDecl(const Decl *D) const {
  auto *FileCtx = D->getDeclContext()->getModuleScopeContext();
  assert(FileCtx == this && "D doesn't belong to this source file");
  if (FileCtx != this) {
    // D doesn't belong to this file. This shouldn't happen in practice.
    return std::nullopt;
  }

  SourceLoc MainLoc = D->getLoc(/*SerializedOK=*/false);
  if (MainLoc.isInvalid())
    return std::nullopt;

  // TODO: Rather than grabbing the location of the macro expansion, we should
  // instead add the generated buffer tree - that would need to include source
  // if we want to be able to retrieve documentation within generated buffers.
  SourceManager &SM = getASTContext().SourceMgr;
  bool InGeneratedBuffer =
      !SM.rangeContainsTokenLoc(SM.getRangeForBuffer(BufferID), MainLoc);
  if (InGeneratedBuffer) {
    int UnderlyingBufferID;
    std::tie(UnderlyingBufferID, MainLoc) =
        D->getModuleContext()->getOriginalLocation(MainLoc);
    if (BufferID != UnderlyingBufferID)
      return std::nullopt;
  }

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

  ExternalSourceLocs::RawLocs Result;

  Result.SourceFilePath = SM.getIdentifierForBuffer(BufferID);
  setLoc(Result.Loc, MainLoc);
  if (!InGeneratedBuffer) {
    for (const auto &SRC : D->getRawComment().Comments) {
      Result.DocRanges.emplace_back(ExternalSourceLocs::RawLoc(),
                                    SRC.Range.getByteLength());
      setLoc(Result.DocRanges.back().first, SRC.Range.getStart());
    }
    setLoc(Result.StartLoc, D->getStartLoc());
    setLoc(Result.EndLoc, D->getEndLoc());
  }

  return Result;
}

void ModuleDecl::ImportCollector::collect(
    const ImportedModule &importedModule) {
  auto *module = importedModule.importedModule;

  if (!module->shouldCollectDisplayDecls())
    return;

  if (importFilter && !importFilter(module))
    return;

  if (importedModule.getAccessPath().size() > 0) {
    auto collectDecls = [&](ValueDecl *VD, DeclVisibilityKind reason) {
      if (reason == DeclVisibilityKind::VisibleAtTopLevel)
        this->qualifiedImports[module].insert(VD);
    };
    auto consumer = makeDeclConsumer(std::move(collectDecls));
    module->lookupVisibleDecls(importedModule.getAccessPath(), consumer,
                               NLKind::UnqualifiedLookup);
  } else {
    imports.insert(module);
  }
}

static void
collectExportedImports(const ModuleDecl *module,
                       ModuleDecl::ImportCollector &importCollector) {
  for (const FileUnit *file : module->getFiles()) {
    if (const SourceFile *source = dyn_cast<SourceFile>(file)) {
      if (source->hasImports()) {
        for (const auto &import : source->getImports()) {
          if (import.options.contains(ImportFlags::Exported) &&
              import.docVisibility.value_or(AccessLevel::Public) >=
                  importCollector.minimumDocVisibility) {
            importCollector.collect(import.module);
            collectExportedImports(import.module.importedModule,
                                   importCollector);
          }
        }
      }
    } else {
      SmallVector<ImportedModule, 8> exportedImports;
      file->getImportedModules(exportedImports,
                               ModuleDecl::ImportFilterKind::Exported);
      for (const auto &im : exportedImports) {
        // Skip collecting the underlying clang module as we already have the relevant import.
        if (module->isClangOverlayOf(im.importedModule))
          continue;
        importCollector.collect(im);
        collectExportedImports(im.importedModule, importCollector);
      }
    }
  }
}

void ModuleDecl::getDisplayDecls(SmallVectorImpl<Decl*> &Results, bool Recursive) const {
  if (Recursive) {
    ImportCollector importCollector;
    this->getDisplayDeclsRecursivelyAndImports(Results, importCollector);
  } else {
    // FIXME: Should this do extra access control filtering?
    FORWARD(getDisplayDecls, (Results));
  }
}

void ModuleDecl::getDisplayDeclsRecursivelyAndImports(
    SmallVectorImpl<Decl *> &results, ImportCollector &importCollector) const {
  this->getDisplayDecls(results, /*Recursive=*/false);

  // Look up imports recursively.
  collectExportedImports(this, importCollector);
  for (const auto &QI : importCollector.qualifiedImports) {
    auto Module = QI.getFirst();
    if (importCollector.imports.contains(Module))
      continue;

    auto &Decls = QI.getSecond();
    results.append(Decls.begin(), Decls.end());
  }

  for (const ModuleDecl *import : importCollector.imports)
    import->getDisplayDecls(results);

#ifndef NDEBUG
  llvm::DenseSet<Decl *> visited;
  for (auto *D : results) {
    // decls synthesized from implicit clang decls may appear multiple times;
    // e.g. if multiple modules with underlying clang modules are re-exported.
    // including duplicates of these is harmless, so skip them when counting
    // this assertion
    if (const auto *CD = D->getClangDecl()) {
      if (CD->isImplicit())
        continue;
    }
    auto inserted = visited.insert(D).second;
    assert(inserted && "there should be no duplicate decls");
  }
#endif
}

Fingerprint SourceFile::getInterfaceHash() const {
  assert(hasInterfaceHash() && "Interface hash not enabled");
  auto &eval = getASTContext().evaluator;
  auto *mutableThis = const_cast<SourceFile *>(this);
  std::optional<StableHasher> interfaceHasher =
      evaluateOrDefault(eval, ParseSourceFileRequest{mutableThis}, {})
          .InterfaceHasher;
  return Fingerprint{StableHasher{interfaceHasher.value()}.finalize()};
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

void ModuleDecl::getMissingImportedModules(
    SmallVectorImpl<ImportedModule> &imports) const {
  FORWARD(getMissingImportedModules, (imports));
}

const llvm::DenseMap<const clang::Module *, ModuleDecl *> &
ModuleDecl::getVisibleClangModules(PrintOptions::InterfaceMode contentMode) {
  if (CachedVisibleClangModuleSet.find(contentMode) != CachedVisibleClangModuleSet.end())
    return CachedVisibleClangModuleSet[contentMode];
  else
    CachedVisibleClangModuleSet.emplace(contentMode, VisibleClangModuleSet{});
  VisibleClangModuleSet &result = CachedVisibleClangModuleSet[contentMode];

  // For the current module, consider both private and public imports.
  ModuleDecl::ImportFilter Filter = ModuleDecl::ImportFilterKind::Exported;
  Filter |= ModuleDecl::ImportFilterKind::Default;

  // For private or package swiftinterfaces, also look through @_spiOnly imports.
  if (contentMode != PrintOptions::InterfaceMode::Public)
    Filter |= ModuleDecl::ImportFilterKind::SPIOnly;
  // Consider package import for package interface
  if (contentMode == PrintOptions::InterfaceMode::Package)
    Filter |= ModuleDecl::ImportFilterKind::PackageOnly;

  SmallVector<ImportedModule, 32> Imports;
  getImportedModules(Imports, Filter);

  SmallVector<ModuleDecl *, 32> ModulesToProcess;
  for (const auto &Import : Imports)
    ModulesToProcess.push_back(Import.importedModule);

  SmallPtrSet<ModuleDecl *, 32> Processed;
  while (!ModulesToProcess.empty()) {
    ModuleDecl *Mod = ModulesToProcess.back();
    ModulesToProcess.pop_back();

    if (!Processed.insert(Mod).second)
      continue;

    if (const clang::Module *ClangModule = Mod->findUnderlyingClangModule())
      result[ClangModule] = Mod;

    // For transitive imports, consider only public imports.
    Imports.clear();
    Mod->getImportedModules(Imports, ModuleDecl::ImportFilterKind::Exported);
    for (const auto &Import : Imports) {
      ModulesToProcess.push_back(Import.importedModule);
    }
  }

  return result;
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
    else if (desc.accessLevel <= AccessLevel::Internal)
      requiredFilter |= ModuleDecl::ImportFilterKind::InternalOrBelow;
    else if (desc.accessLevel <= AccessLevel::Package)
      requiredFilter |= ModuleDecl::ImportFilterKind::PackageOnly;
    else if (desc.options.contains(ImportFlags::SPIOnly))
      requiredFilter |= ModuleDecl::ImportFilterKind::SPIOnly;
    else
      requiredFilter |= ModuleDecl::ImportFilterKind::Default;

    if (!separatelyImportedOverlays.lookup(desc.module.importedModule).empty())
      requiredFilter |= ModuleDecl::ImportFilterKind::ShadowedByCrossImportOverlay;

    if (filter.contains(requiredFilter))
      modules.push_back(desc.module);
  }
}

void SourceFile::getMissingImportedModules(
    SmallVectorImpl<ImportedModule> &modules) const {
  for (auto module : MissingImportedModules)
    modules.push_back(module);
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
  // Return the module's real (binary) name, which can be different from
  // the name if module aliasing was used (-module-alias flag). The real
  // name is used for serialization and loading.
  if (auto *swiftModule = current.dyn_cast<const ModuleDecl *>())
    return swiftModule->getRealName().str();

  auto *clangModule =
      static_cast<const clang::Module *>(current.get<const void *>());
  if (!clangModule->isSubModule() && clangModule->Name == "std")
    return "CxxStdlib";
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

Identifier ModuleDecl::getRealName() const {
  // This will return the real name for an alias (if used) or getName()
  return getASTContext().getRealModuleName(getName());
}

bool ModuleDecl::allowImportedBy(ModuleDecl *importer) const {
  if (allowableClientNames.empty())
    return true;
  for (auto id: allowableClientNames) {
    if (importer->getRealName() == id)
      return true;
    if (importer->getABIName() == id)
      return true;
  }
  return false;
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

StringRef ModuleDecl::getModuleSourceFilename() const {
  for (auto F : getFiles()) {
    if (auto LF = dyn_cast<LoadedFile>(F))
      return LF->getSourceFilename();
  }
  return StringRef();
}

StringRef ModuleDecl::getModuleLoadedFilename() const {
  for (auto F : getFiles()) {
    if (auto LF = dyn_cast<LoadedFile>(F)) {
      return LF->getLoadedFilename();
    }
  }
  return StringRef();
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

NominalTypeDecl *ModuleDecl::getMainTypeDecl() const {
  if (!EntryPointInfo.hasEntryPoint())
    return nullptr;
  auto *file = EntryPointInfo.getEntryPointFile();
  if (!file)
    return nullptr;
  auto *mainDecl = file->getMainDecl();
  if (!mainDecl)
    return nullptr;
  auto *func = dyn_cast<FuncDecl>(file->getMainDecl());
  if (!func)
    return nullptr;
  auto *nominalType = dyn_cast<NominalTypeDecl>(func->getDeclContext());
  return nominalType;
}

bool ModuleDecl::registerEntryPointFile(
    FileUnit *file, SourceLoc diagLoc, std::optional<ArtificialMainKind> kind) {
  if (!EntryPointInfo.hasEntryPoint()) {
    EntryPointInfo.setEntryPointFile(file);
    return false;
  }

  if (diagLoc.isInvalid())
    return true;

  assert(kind.has_value() && "multiple entry points without attributes");

  // %select indices for UI/NSApplication-related diagnostics.
  enum : unsigned {
    UIApplicationMainClass = 0,
    NSApplicationMainClass = 1,
    MainType = 2,
  } mainTypeDiagKind;

  switch (kind.value()) {
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
        getASTContext().Diags.diagnose(existingDiagLoc,
                           diag::attr_ApplicationMain_parse_as_library);
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
      ModuleDecl::ImportFilterKind::Default};

  auto *topLevel = getParentModule();

  ModuleDecl::ImportFilter topLevelFilter = filter;
  topLevelFilter |= ModuleDecl::ImportFilterKind::ImplementationOnly;
  topLevelFilter |= ModuleDecl::ImportFilterKind::InternalOrBelow;
  topLevelFilter |= ModuleDecl::ImportFilterKind::PackageOnly,
  topLevelFilter |= ModuleDecl::ImportFilterKind::SPIOnly;
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

void ModuleDecl::collectSerializedSearchPath(
    llvm::function_ref<void(StringRef)> callback) const {
  for (const FileUnit *fileUnit : getFiles()) {
    if (auto *serialized = dyn_cast<LoadedFile>(fileUnit)) {
      serialized->collectSerializedSearchPath(callback);
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

bool ModuleDecl::isExternallyConsumed() const {
  // Modules for executables aren't expected to be consumed by other modules.
  // This picks up all kinds of entrypoints, including script mode,
  // @UIApplicationMain and @NSApplicationMain.
  if (hasEntryPoint()) {
    return false;
  }

  // If an implicit Objective-C header was needed to construct this module, it
  // must be the product of a library target.
  if (!getImplicitImportInfo().BridgingHeaderPath.empty()) {
    return false;
  }

  // App extensions are special beasts because they build without entrypoints
  // like library targets, but they behave like executable targets because
  // their associated modules are not suitable for distribution.
  // However, app extension libraries might be consumed externally.
  if (getASTContext().LangOpts.EnableAppExtensionRestrictions &&
      !getASTContext().LangOpts.EnableAppExtensionLibraryRestrictions) {
    return false;
  }

  // FIXME: This is still a lousy approximation of whether the module file will
  // be externally consumed.
  return true;
}

//===----------------------------------------------------------------------===//
// Cross-Import Overlays
//===----------------------------------------------------------------------===//

namespace swift {
/// Represents a file containing information about cross-module overlays.
class OverlayFile : public ASTAllocated<OverlayFile> {
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
          if (!overlay.hasUnderscoredNaming())
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

bool ModuleDecl::isClangOverlayOf(ModuleDecl *potentialUnderlying) const {
  return getUnderlyingModuleIfOverlay() == potentialUnderlying;
}

bool ModuleDecl::isSameModuleLookingThroughOverlays(
  ModuleDecl *other) {
  if (this == other) {
    return true;
  }

  if (this->isClangOverlayOf(other) || other->isClangOverlayOf(this)) {
    return true;
  }

  return false;
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
    std::string message = (Twine("key 'version' has invalid value: ") + Twine(contents.version)).str();
    errorMessages.emplace_back(std::move(message));
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
  SmallVector<Decl *> topLevelDecls;
  getTopLevelDeclsWithAuxiliaryDecls(topLevelDecls);
  for (auto decl : topLevelDecls) {
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

std::optional<AttributedImport<ImportedModule>>
SourceFile::findImport(const ModuleDecl *module) const {
  return evaluateOrDefault(getASTContext().evaluator,
                           ImportDeclRequest{this, module}, std::nullopt);
}

std::optional<AttributedImport<ImportedModule>>
ImportDeclRequest::evaluate(Evaluator &evaluator, const SourceFile *sf,
                            const ModuleDecl *module) const {
  auto &ctx = sf->getASTContext();
  auto imports = sf->getImports();

  auto mutModule = const_cast<ModuleDecl *>(module);
  // Look to see if the owning module was directly imported.
  for (const auto &import : imports) {
    if (import.module.importedModule
            ->isSameModuleLookingThroughOverlays(mutModule))
      return import;
  }

  // Now look for transitive imports.
  auto &importCache = ctx.getImportCache();
  for (const auto &import : imports) {
    auto &importSet = importCache.getImportSet(import.module.importedModule);
    for (const auto &transitive : importSet.getTransitiveImports()) {
      if (transitive.importedModule
              ->isSameModuleLookingThroughOverlays(mutModule)) {
        return import;
      }
    }
  }

  return std::nullopt;
}

bool SourceFile::hasImportUsedPreconcurrency(
    AttributedImport<ImportedModule> import) const {
  return PreconcurrencyImportsUsed.count(import) != 0;
}

void SourceFile::setImportUsedPreconcurrency(
    AttributedImport<ImportedModule> import) {
  PreconcurrencyImportsUsed.insert(import);
}

AccessLevel
SourceFile::getMaxAccessLevelUsingImport(
    const ModuleDecl *mod) const {
  auto known = ImportsUseAccessLevel.find(mod);
  if (known == ImportsUseAccessLevel.end())
    return AccessLevel::Internal;
  return known->second;
}

void SourceFile::registerAccessLevelUsingImport(
    AttributedImport<ImportedModule> import,
    AccessLevel accessLevel) {
  auto mod = import.module.importedModule;
  auto known = ImportsUseAccessLevel.find(mod);
  if (known == ImportsUseAccessLevel.end())
    ImportsUseAccessLevel[mod] = accessLevel;
  else
    ImportsUseAccessLevel[mod] = std::max(accessLevel, known->second);

  // Also register modules triggering the import of cross-import overlays.
  auto declaringMod = mod->getDeclaringModuleIfCrossImportOverlay();
  if (!declaringMod)
    return;

  SmallVector<Identifier, 1> bystanders;
  auto bystandersValid =
    mod->getRequiredBystandersIfCrossImportOverlay(declaringMod, bystanders);
  if (!bystandersValid)
    return;

  for (auto &otherImport : *Imports) {
    auto otherImportMod = otherImport.module.importedModule;
    auto otherImportModName = otherImportMod->getName();
    if (otherImportMod == declaringMod ||
        llvm::find(bystanders, otherImportModName) != bystanders.end()) {
      registerAccessLevelUsingImport(otherImport, accessLevel);
    }
  }
}

bool HasImportsMatchingFlagRequest::evaluate(Evaluator &evaluator,
                                             SourceFile *SF,
                                             ImportFlags flag) const {
  for (auto desc : SF->getImports()) {
    if (desc.options.contains(flag))
      return true;
  }
  return false;
}

std::optional<bool> HasImportsMatchingFlagRequest::getCachedResult() const {
  SourceFile *sourceFile = std::get<0>(getStorage());
  ImportFlags flag = std::get<1>(getStorage());
  if (sourceFile->validCachedImportOptions.contains(flag))
    return sourceFile->cachedImportOptions.contains(flag);

  return std::nullopt;
}

void HasImportsMatchingFlagRequest::cacheResult(bool value) const {
  SourceFile *sourceFile = std::get<0>(getStorage());
  ImportFlags flag = std::get<1>(getStorage());

  sourceFile->validCachedImportOptions |= flag;
  if (value)
    sourceFile->cachedImportOptions |= flag;
}

void swift::simple_display(llvm::raw_ostream &out, ImportOptions options) {
  using Flag = std::pair<ImportFlags, StringRef>;
  Flag possibleFlags[] = {
#define FLAG(Name) {ImportFlags::Name, #Name},
    FLAG(Exported)
    FLAG(Testable)
    FLAG(PrivateImport)
    FLAG(ImplementationOnly)
    FLAG(SPIAccessControl)
    FLAG(Preconcurrency)
    FLAG(WeakLinked)
    FLAG(Reserved)
#undef FLAG
  };

  auto flagsToPrint = llvm::make_filter_range(
      possibleFlags, [&](Flag flag) { return options & flag.first; });

  out << "{ ";
  interleave(
      flagsToPrint, [&](Flag flag) { out << flag.second; },
      [&] { out << ", "; });
  out << " }";
}

bool SourceFile::hasImportsWithFlag(ImportFlags flag) const {
  auto &ctx = getASTContext();
  auto *mutableThis = const_cast<SourceFile *>(this);
  return evaluateOrDefault(
      ctx.evaluator, HasImportsMatchingFlagRequest{mutableThis, flag}, false);
}

ImportFlags SourceFile::getImportFlags(const ModuleDecl *module) const {
  unsigned flags = 0x0;
  for (auto import : *Imports) {
    if (import.module.importedModule == module)
      flags |= import.options.toRaw();
  }
  return ImportFlags(flags);
}

bool SourceFile::hasTestableOrPrivateImport(
    AccessLevel accessLevel, const swift::ValueDecl *ofDecl,
    SourceFile::ImportQueryKind queryKind) const {
  auto *module = ofDecl->getModuleContext();
  switch (accessLevel) {
  case AccessLevel::Internal:
  case AccessLevel::Package:
  case AccessLevel::Public:
  case AccessLevel::Open:
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

RestrictedImportKind SourceFile::getRestrictedImportKind(const ModuleDecl *module) const {
  auto &imports = getASTContext().getImportCache();
  RestrictedImportKind importKind = RestrictedImportKind::MissingImport;

  // Workaround for the cases where the bridging header isn't properly
  // imported implicitly.
  if (module->getName().str() == CLANG_HEADER_MODULE_NAME)
    return RestrictedImportKind::None;

  // Look at the imports of this source file.
  for (auto &desc : *Imports) {
    if (desc.options.contains(ImportFlags::ImplementationOnly)) {
      if (importKind < RestrictedImportKind::ImplementationOnly &&
          imports.isImportedBy(module, desc.module.importedModule))
        importKind = RestrictedImportKind::ImplementationOnly;
    }
    else if (desc.options.contains(ImportFlags::SPIOnly)) {
      if (importKind < RestrictedImportKind::SPIOnly &&
          imports.isImportedBy(module, desc.module.importedModule))
        importKind = RestrictedImportKind::SPIOnly;
    }
    // If the module is imported publicly, there's no restriction.
    else if (imports.isImportedBy(module, desc.module.importedModule))
      return RestrictedImportKind::None;
  }

  // Now check this file's enclosing module in case there are re-exports.
  if (imports.isImportedBy(module, getParentModule()))
    return RestrictedImportKind::None;

  return importKind;
}

ImportAccessLevel
SourceFile::getImportAccessLevel(const ModuleDecl *targetModule) const {
  assert(Imports.has_value());

  // Leave it to the caller to avoid calling this service for a self import.
  // We want to return AccessLevel::Public, but there's no import site to return.
  assert(targetModule != getParentModule() &&
         "getImportAccessLevel doesn't support checking for a self-import");

  auto &imports = getASTContext().getImportCache();
  ImportAccessLevel restrictiveImport = std::nullopt;

  for (auto &import : *Imports) {
    if ((!restrictiveImport.has_value() ||
         import.accessLevel > restrictiveImport->accessLevel) &&
        imports.isImportedBy(targetModule, import.module.importedModule)) {
      restrictiveImport = import;
    }
  }

  return restrictiveImport;
}

CharSourceRange
IfConfigClauseRangeInfo::getDirectiveRange(const SourceManager &SM) const {
  return CharSourceRange(SM, DirectiveLoc, BodyLoc);
}

CharSourceRange
IfConfigClauseRangeInfo::getBodyRange(const SourceManager &SM) const {
  return CharSourceRange(SM, BodyLoc, EndLoc);
}

CharSourceRange
IfConfigClauseRangeInfo::getWholeRange(const SourceManager &SM) const {
  return CharSourceRange(SM, DirectiveLoc, EndLoc);
}

void SourceFile::recordIfConfigClauseRangeInfo(
    const IfConfigClauseRangeInfo &range) {
  IfConfigClauseRanges.Ranges.push_back(range);
  IfConfigClauseRanges.IsSorted = false;
}

ArrayRef<IfConfigClauseRangeInfo> SourceFile::getIfConfigClauseRanges() const {
  if (!IfConfigClauseRanges.IsSorted) {
    auto &SM = getASTContext().SourceMgr;
    // Sort the ranges if we need to.
    llvm::sort(
        IfConfigClauseRanges.Ranges, [&](const IfConfigClauseRangeInfo &lhs,
                                         const IfConfigClauseRangeInfo &rhs) {
          return SM.isBeforeInBuffer(lhs.getStartLoc(), rhs.getStartLoc());
        });

    // Be defensive and eliminate duplicates in case we've parsed twice.
    auto newEnd = llvm::unique(
        IfConfigClauseRanges.Ranges, [&](const IfConfigClauseRangeInfo &lhs,
                                         const IfConfigClauseRangeInfo &rhs) {
          if (lhs.getStartLoc() != rhs.getStartLoc())
            return false;
          assert(lhs.getBodyRange(SM) == rhs.getBodyRange(SM) &&
                 "range changed on a re-parse?");
          return true;
        });
    IfConfigClauseRanges.Ranges.erase(newEnd,
                                      IfConfigClauseRanges.Ranges.end());
    IfConfigClauseRanges.IsSorted = true;
  }

  return IfConfigClauseRanges.Ranges;
}

ArrayRef<IfConfigClauseRangeInfo>
SourceFile::getIfConfigClausesWithin(SourceRange outer) const {
  auto &SM = getASTContext().SourceMgr;
  assert(SM.getRangeForBuffer(BufferID).contains(outer.Start) &&
         "Range not within this file?");

  // First let's find the first #if that is after the outer start loc.
  auto ranges = getIfConfigClauseRanges();
  auto lower = llvm::lower_bound(
      ranges, outer.Start,
      [&](const IfConfigClauseRangeInfo &range, SourceLoc loc) {
        return SM.isBeforeInBuffer(range.getStartLoc(), loc);
      });
  if (lower == ranges.end() ||
      SM.isBeforeInBuffer(outer.End, lower->getStartLoc())) {
    return {};
  }
  // Next let's find the first #if that's after the outer end loc.
  auto upper = llvm::upper_bound(
      ranges, outer.End,
      [&](SourceLoc loc, const IfConfigClauseRangeInfo &range) {
        return SM.isBeforeInBuffer(loc, range.getStartLoc());
      });
  return llvm::ArrayRef(lower, upper - lower);
}

void ModuleDecl::setPackageName(Identifier name) {
  Package = PackageUnit::create(name, *this, getASTContext());
}

bool ModuleDecl::isImportedImplementationOnly(const ModuleDecl *module) const {
  if (module == this) return false;

  auto &imports = getASTContext().getImportCache();

  // Look through non-implementation-only imports to see if module is imported
  // in some other way. Otherwise we assume it's implementation-only imported.
  ModuleDecl::ImportFilter filter = {
    ModuleDecl::ImportFilterKind::Exported,
    ModuleDecl::ImportFilterKind::Default,
    ModuleDecl::ImportFilterKind::PackageOnly,
    ModuleDecl::ImportFilterKind::SPIOnly,
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
canBeUsedForCrossModuleOptimization(DeclContext *ctxt) const {
  ModuleDecl *moduleOfCtxt = ctxt->getParentModule();

  // If the context defined in the same module - or is the same module, it's
  // fine.
  if (moduleOfCtxt == this)
    return true;

  // See if context is imported in a "regular" way, i.e. not with
  // @_implementationOnly, `package import` or @_spiOnly.
  ModuleDecl::ImportFilter filter = {
    ModuleDecl::ImportFilterKind::ImplementationOnly,
    ModuleDecl::ImportFilterKind::PackageOnly,
    ModuleDecl::ImportFilterKind::SPIOnly
  };
  SmallVector<ImportedModule, 4> results;
  getImportedModules(results, filter);

  auto &imports = getASTContext().getImportCache();
  for (auto &desc : results) {
    if (imports.isImportedBy(moduleOfCtxt, desc.importedModule))
      return false;
  }
  return true;
}

void SourceFile::lookupImportedSPIGroups(
                        const ModuleDecl *importedModule,
                        llvm::SmallSetVector<Identifier, 4> &spiGroups) const {
  auto &imports = getASTContext().getImportCache();
  for (auto &import : *Imports) {
    if (import.options.contains(ImportFlags::SPIAccessControl) &&
        (importedModule == import.module.importedModule ||
         imports.isImportedByViaSwiftOnly(importedModule,
                                       import.module.importedModule))) {
      spiGroups.insert(import.spiGroups.begin(), import.spiGroups.end());
    }
  }
}

bool shouldImplicitImportAsSPI(ArrayRef<Identifier> spiGroups) {
  for (auto group : spiGroups) {
    if (group.empty())
      return true;
  }
  return false;
}

bool SourceFile::isImportedAsSPI(const ValueDecl *targetDecl) const {
  auto targetModule = targetDecl->getModuleContext();
  llvm::SmallSetVector<Identifier, 4> importedSPIGroups;

  // Objective-C SPIs are always imported implicitly.
  if (targetDecl->hasClangNode())
    return !targetDecl->getSPIGroups().empty();
  if (shouldImplicitImportAsSPI(targetDecl->getSPIGroups()))
    return true;

  if (hasTestableOrPrivateImport(AccessLevel::Public, targetDecl, PrivateOnly))
    return true;

  lookupImportedSPIGroups(targetModule, importedSPIGroups);
  if (importedSPIGroups.empty())
    return false;

  auto declSPIGroups = targetDecl->getSPIGroups();
  for (auto declSPI : declSPIGroups)
    if (importedSPIGroups.count(declSPI))
      return true;

  return false;
}

bool SourceFile::importsModuleAsWeakLinked(const ModuleDecl *module) const {
  for (auto &import : *Imports) {
    if (!import.options.contains(ImportFlags::WeakLinked))
      continue;

    const ModuleDecl *importedModule = import.module.importedModule;
    if (module == importedModule)
      return true;

    // Also check whether the target module is actually the underlyingClang
    // module for this @_weakLinked import.
    const ModuleDecl *clangModule =
        importedModule->getUnderlyingModuleIfOverlay();
    if (module == clangModule)
      return true;

    // Traverse the exported modules of this weakly-linked module to ensure
    // that we weak-link declarations from its exported peers.
    SmallVector<ImportedModule, 8> reexportedModules;
    importedModule->getImportedModules(reexportedModules,
                                       ModuleDecl::ImportFilterKind::Exported);
    for (const ImportedModule &reexportedModule : reexportedModules) {
      if (module == reexportedModule.importedModule)
        return true;
    }
  }
  return false;
}

bool ModuleDecl::isImportedAsSPI(const SpecializeAttr *attr,
                                 const ValueDecl *targetDecl) const {
  auto declSPIGroups = attr->getSPIGroups();
  if (shouldImplicitImportAsSPI(declSPIGroups))
    return true;

  auto targetModule = targetDecl->getModuleContext();
  llvm::SmallSetVector<Identifier, 4> importedSPIGroups;
  lookupImportedSPIGroups(targetModule, importedSPIGroups);
  if (importedSPIGroups.empty()) return false;

  for (auto declSPI : declSPIGroups)
    if (importedSPIGroups.count(declSPI))
      return true;

  return false;
}

bool ModuleDecl::isImportedAsSPI(Identifier spiGroup,
                                 const ModuleDecl *fromModule) const {
  if (shouldImplicitImportAsSPI({spiGroup}))
    return true;

  llvm::SmallSetVector<Identifier, 4> importedSPIGroups;
  lookupImportedSPIGroups(fromModule, importedSPIGroups);
  if (importedSPIGroups.empty())
    return false;
  return importedSPIGroups.count(spiGroup);
}

bool ModuleDecl::isImportedAsWeakLinked(const ModuleDecl *module) const {
  for (auto file : getFiles()) {
    if (file->importsModuleAsWeakLinked(module))
      return true;
  }
  return false;
}

bool Decl::isSPI() const {
  return !getSPIGroups().empty();
}

ArrayRef<Identifier> Decl::getSPIGroups() const {
  const Decl *D = abstractSyntaxDeclForAvailableAttribute(this);

  if (!isa<ValueDecl>(D) &&
      !isa<ExtensionDecl>(D))
    return ArrayRef<Identifier>();

  return evaluateOrDefault(getASTContext().evaluator,
                           SPIGroupsRequest{ D },
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
  namespace path = llvm::sys::path;
  SmallString<128> scratch;

  /// Is \p path under the folder SDK/a/b/c/d/e?
  auto hasSDKPrefix =
    [&](StringRef path, const Twine &a, const Twine &b = "",
        const Twine &c = "", const Twine &d = "", const Twine &e = "") {
    scratch = ctx.SearchPathOpts.getSDKPath();
    path::append(scratch, a, b, c, d);
    path::append(scratch, e);
    return path.starts_with(scratch);
  };

  /// Is \p modulePath from System/Library/PrivateFrameworks/?
  auto fromPrivateFrameworks = [&](StringRef modulePath) -> bool {
    if (!ctx.LangOpts.Target.isOSDarwin()) return false;

    return hasSDKPrefix(modulePath, "AppleInternal", "Library", "Frameworks") ||
           hasSDKPrefix(modulePath, "System", "Library", "PrivateFrameworks") ||
           hasSDKPrefix(modulePath, "System", "iOSSupport", "System", "Library", "PrivateFrameworks") ||
           hasSDKPrefix(modulePath, "usr", "local", "include");
  };

  if (module->isNonSwiftModule()) {
    if (auto *underlying = module->findUnderlyingClangModule()) {
      // Imported clangmodules are SPI if they are defined by a private
      // modulemap or from the PrivateFrameworks folder in the SDK.
      bool moduleIsSPI = underlying->ModuleMapIsPrivate ||
                         fromPrivateFrameworks(underlying->PresumedModuleMapFile);
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

  setIsObjCNameLookupCachePopulated(false);
  ObjCNameLookupCache.clear();

  if (!Cache)
    return;

  // Abandon any current cache. We'll rebuild it on demand.
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
                       std::optional<unsigned> bufferID,
                       ParsingOptions parsingOpts, bool isPrimary)
    : FileUnit(FileUnitKind::Source, M), BufferID(bufferID ? *bufferID : -1),
      ParsingOpts(parsingOpts), IsPrimary(isPrimary), Kind(K) {
  M.getASTContext().addDestructorCleanup(*this);

  assert(!IsPrimary || M.isMainModule() &&
         "A primary cannot appear outside the main module");

  if (isScriptMode()) {
    bool problem = M.registerEntryPointFile(this, SourceLoc(), std::nullopt);
    assert(!problem && "multiple main files?");
    (void)problem;
  }

  if (Kind == SourceFileKind::MacroExpansion ||
      Kind == SourceFileKind::DefaultArgument)
    M.addAuxiliaryFile(*this);
}

SourceFile::ParsingOptions
SourceFile::getDefaultParsingOptions(const LangOptions &langOpts) {
  ParsingOptions opts;
  if (langOpts.DisablePoundIfEvaluation)
    opts |= ParsingFlags::DisablePoundIfEvaluation;
  if (langOpts.CollectParsedToken)
    opts |= ParsingFlags::CollectParsedTokens;
  if (langOpts.hasFeature(Feature::ParserRoundTrip))
    opts |= ParsingFlags::RoundTrip;
  if (langOpts.hasFeature(Feature::ParserValidation))
    opts |= ParsingFlags::ValidateNewParserDiagnostics;
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

bool SourceFile::hasDelayedBodyParsing() const {
  if (ParsingOpts.contains(ParsingFlags::DisableDelayedBodies))
    return false;

  // Not supported right now.
  if (Kind == SourceFileKind::SIL)
    return false;
  if (shouldCollectTokens())
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
  return evaluateOrDefault(
      ctx.evaluator, ParseTopLevelDeclsRequest{mutableThis}, {});
}

void SourceFile::addTopLevelDecl(Decl *d) {
  // Force decl parsing if we haven't already.
  (void)getTopLevelItems();
  Items->push_back(d);

  // FIXME: This violates core properties of the evaluator.
  auto &ctx = getASTContext();
  auto *mutableThis = const_cast<SourceFile *>(this);
  ctx.evaluator.clearCachedOutput(ParseTopLevelDeclsRequest{mutableThis});
}

void SourceFile::prependTopLevelDecl(Decl *d) {
  // Force decl parsing if we haven't already.
  (void)getTopLevelItems();
  Items->insert(Items->begin(), d);

  // FIXME: This violates core properties of the evaluator.
  auto &ctx = getASTContext();
  auto *mutableThis = const_cast<SourceFile *>(this);
  ctx.evaluator.clearCachedOutput(ParseTopLevelDeclsRequest{mutableThis});
}

void SourceFile::addDelayedFunction(AbstractFunctionDecl *AFD) {
  // If we defer type checking to runtime, we won't
  // have to type check `AFD` ahead of time
  auto &Ctx = getASTContext();
  if (Ctx.TypeCheckerOpts.DeferToRuntime &&
      Ctx.LangOpts.hasFeature(Feature::LazyImmediate))
    return;
  DelayedFunctions.push_back(AFD);
}

void SourceFile::typeCheckDelayedFunctions() {

  for (unsigned i = 0; i < DelayedFunctions.size(); i++) {
    auto *AFD = DelayedFunctions[i];
    assert(!AFD->getDeclContext()->isLocalContext());
    AFD->getTypecheckedBody();
    (void) AFD->getCaptureInfo();
  }

  DelayedFunctions.clear();
}

ArrayRef<ASTNode> SourceFile::getTopLevelItems() const {
  auto &ctx = getASTContext();
  auto *mutableThis = const_cast<SourceFile *>(this);
  return evaluateOrDefault(ctx.evaluator, ParseSourceFileRequest{mutableThis},
                           {}).TopLevelItems;
}

ArrayRef<Decl *> SourceFile::getHoistedDecls() const {
  return Hoisted;
}

void *SourceFile::getExportedSourceFile() const {
  auto &eval = getASTContext().evaluator;
  return evaluateOrDefault(eval, ExportedSourceFileRequest{this}, nullptr);
}

bool FileUnit::walk(ASTWalker &walker) {
  SmallVector<Decl *, 64> Decls;
  getTopLevelDecls(Decls);
  llvm::SaveAndRestore<ASTWalker::ParentTy> SAR(walker.Parent,
                                                getParentModule());

  bool SkipInternal = getKind() == FileUnitKind::SerializedAST &&
      !walker.shouldWalkSerializedTopLevelInternalDecls();
  for (Decl *D : Decls) {
    if (SkipInternal) {
      // Ignore if the decl isn't visible
      if (auto *VD = dyn_cast<ValueDecl>(D)) {
        if (VD->getFormalAccess() < AccessLevel::Public)
          continue;
      }

      // Also ignore if the extended nominal isn't visible
      if (auto *ED = dyn_cast<ExtensionDecl>(D)) {
        auto *ND = ED->getExtendedNominal();
        if (ND && ND->getFormalAccess() < AccessLevel::Public)
          continue;
      }
    }

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
  for (auto Item : getTopLevelItems()) {
    if (auto D = Item.dyn_cast<Decl *>()) {
      if (D->walk(walker))
        return true;
    } else {
      Item.walk(walker);
    }

    if (walker.shouldWalkAccessorsTheOldWay()) {
      // Pretend that accessors share a parent with the storage.
      //
      // FIXME: Update existing ASTWalkers to deal with accessors appearing as
      // children of the storage instead.
      if (auto *ASD = dyn_cast_or_null<AbstractStorageDecl>(
              Item.dyn_cast<Decl *>())) {
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

Identifier SourceFile::getPrivateDiscriminator(bool createIfMissing) const {
  if (!PrivateDiscriminator.empty() || !createIfMissing)
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

Identifier
SourceFile::getDiscriminatorForPrivateDecl(const Decl *D) const {
  assert(D->getDeclContext()->getModuleScopeContext() == this ||
         D->getDeclContext()->getModuleScopeContext() == getSynthesizedFile());
  return getPrivateDiscriminator(/*createIfMissing=*/true);
}

SynthesizedFileUnit *FileUnit::getSynthesizedFile() const {
  return cast_or_null<SynthesizedFileUnit>(SynthesizedFileAndKind.getPointer());
}

SynthesizedFileUnit &FileUnit::getOrCreateSynthesizedFile() {
  auto SynthesizedFile = getSynthesizedFile();
  if (!SynthesizedFile) {
    if (auto thisSynth = dyn_cast<SynthesizedFileUnit>(this))
      return *thisSynth;
    SynthesizedFile = new (getASTContext()) SynthesizedFileUnit(*this);
    SynthesizedFileAndKind.setPointer(SynthesizedFile);
  }
  return *SynthesizedFile;
}

TypeRefinementContext *SourceFile::getTypeRefinementContext() const {
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

bool SourceFile::isAsyncTopLevelSourceFile() const {
  return isScriptMode() &&
         (bool)evaluateOrDefault(getASTContext().evaluator,
                                 GetSourceFileAsyncNode{this}, ASTNode());
}

ASTNode GetSourceFileAsyncNode::evaluate(Evaluator &eval,
                                         const SourceFile *sf) const {
  for (Decl *d : sf->getTopLevelDecls()) {
    TopLevelCodeDecl *tld = dyn_cast<TopLevelCodeDecl>(d);
    if (tld && tld->getBody()) {
      if (ASTNode asyncNode = tld->getBody()->findAsyncNode())
        return asyncNode;
    }
  }
  return ASTNode();
}

ArrayRef<TypeDecl *> SourceFile::getLocalTypeDecls() const {
  auto *mutableThis = const_cast<SourceFile *>(this);
  return evaluateOrDefault(getASTContext().evaluator,
                           LocalTypeDeclsRequest{mutableThis}, {});
}

namespace {
class LocalTypeDeclCollector : public ASTWalker {
  SmallVectorImpl<TypeDecl *> &results;

public:
  LocalTypeDeclCollector(SmallVectorImpl<TypeDecl *> &results)
      : results(results) {}

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::Expansion;
  }

  PreWalkAction walkToDeclPre(Decl *D) override {
    switch (D->getKind()) {
    case DeclKind::Enum:
    case DeclKind::Struct:
    case DeclKind::Class:
    case DeclKind::Protocol:
    case DeclKind::TypeAlias:
      if (D->getDeclContext()->isLocalContext())
        results.push_back(cast<TypeDecl>(D));
      break;
    default:
      break;
    }
    return Action::Continue();
  }
};
} // namespace

ArrayRef<TypeDecl *> LocalTypeDeclsRequest::evaluate(Evaluator &evaluator,
                                                     SourceFile *sf) const {
  SmallVector<TypeDecl *> results;
  LocalTypeDeclCollector collector(results);
  sf->walk(collector);
  return sf->getASTContext().AllocateCopy(results);
}

//===----------------------------------------------------------------------===//
// SynthesizedFileUnit Implementation
//===----------------------------------------------------------------------===//

SynthesizedFileUnit::SynthesizedFileUnit(FileUnit &FU)
    : FileUnit(FileUnitKind::Synthesized, *FU.getParentModule()), FU(FU) {
  FU.getASTContext().addDestructorCleanup(*this);
}

Identifier
SynthesizedFileUnit::getDiscriminatorForPrivateDecl(const Decl *D) const {
  assert(D->getDeclContext()->getModuleScopeContext() == this);

  // Use cached primitive discriminator if it exists.
  if (!PrivateDiscriminator.empty())
    return PrivateDiscriminator;

  // Start with the discriminator that the file we belong to would use.
  auto ownerDiscriminator = getFileUnit().getDiscriminatorForPrivateDecl(D);

  // Hash that with a special string to produce a different value that preserves
  // the entropy of the original.
  // TODO: Use a more robust discriminator for synthesized files. Pick something
  // that cannot conflict with `SourceFile` discriminators.
  llvm::MD5 hash;
  hash.update(ownerDiscriminator.str());
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
    OptionSet<ModuleLookupFlags> Flags,
    SmallVectorImpl<ValueDecl *> &result) const {
  for (auto *decl : TopLevelDecls) {
    if (auto VD = dyn_cast<ValueDecl>(decl)) {
      if (VD->getName().matchesRef(name)) {
        result.push_back(VD);
      }
    }
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

void FileUnit::getTopLevelDeclsWithAuxiliaryDecls(
    SmallVectorImpl<Decl*> &results) const {

  std::function<void(Decl *)> addResult;
  addResult = [&](Decl *decl) {
    results.push_back(decl);
    decl->visitAuxiliaryDecls(addResult);
  };

  SmallVector<Decl *, 32> nonExpandedDecls;
  nonExpandedDecls.reserve(results.capacity());
  getTopLevelDecls(nonExpandedDecls);
  for (auto *decl : nonExpandedDecls) {
    addResult(decl);
  }
}

void FileUnit::dumpDisplayDecls() const {
  SmallVector<Decl *, 32> Decls;
  getDisplayDecls(Decls);
  for (auto *D : Decls) {
    D->dump(llvm::errs());
  }
}

void FileUnit::dumpTopLevelDecls() const {
  SmallVector<Decl *, 32> Decls;
  getTopLevelDecls(Decls);
  for (auto *D : Decls) {
    D->dump(llvm::errs());
  }
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

StringRef ModuleEntity::getName(bool useRealNameIfAliased) const {
  assert(!Mod.isNull());
  if (auto SwiftMod = Mod.dyn_cast<const ModuleDecl*>())
    return useRealNameIfAliased ? SwiftMod->getRealName().str() : SwiftMod->getName().str();
  return getClangModule(Mod)->Name;
}

std::string ModuleEntity::getFullName(bool useRealNameIfAliased) const {
  assert(!Mod.isNull());
  if (auto SwiftMod = Mod.dyn_cast<const ModuleDecl*>())
    return std::string(useRealNameIfAliased ? SwiftMod->getRealName() : SwiftMod->getName());
  return getClangModule(Mod)->getFullModuleName();
}

bool ModuleEntity::isSystemModule() const {
  assert(!Mod.isNull());
  if (auto SwiftMod = Mod.dyn_cast<const ModuleDecl*>())
    return SwiftMod->isSystemModule();
  return getClangModule(Mod)->IsSystem;
}

bool ModuleEntity::isNonUserModule() const {
  assert(!Mod.isNull());
  if (auto *SwiftMod = Mod.dyn_cast<const ModuleDecl *>())
    return SwiftMod->isNonUserModule();
  // TODO: Should handle clang modules as well
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

bool IsNonUserModuleRequest::evaluate(Evaluator &evaluator, ModuleDecl *mod) const {
  // If there's no SDK path, fallback to checking whether the module was
  // in the system search path or a clang system module
  SearchPathOptions &searchPathOpts = mod->getASTContext().SearchPathOpts;
  StringRef sdkPath = searchPathOpts.getSDKPath();
  if (sdkPath.empty() && mod->isSystemModule())
    return true;

  // Some temporary module's get created with no module name and they have no
  // files. Avoid running `getFiles` on them (which will assert if there
  // aren't any).
  if (!mod->hasName() || mod->getFiles().empty())
    return false;
  
  auto *LF = dyn_cast_or_null<LoadedFile>(mod->getFiles().front());
  if (!LF)
    return false;
  
  StringRef modulePath = LF->getSourceFilename();
  if (modulePath.empty())
    return false;

  StringRef runtimePath = searchPathOpts.RuntimeResourcePath;
  return (!runtimePath.empty() && pathStartsWith(runtimePath, modulePath)) ||
      (!sdkPath.empty() && pathStartsWith(sdkPath, modulePath));
}

version::Version ModuleDecl::getLanguageVersionBuiltWith() const {
  for (auto *F : getFiles()) {
    auto *LD = dyn_cast<LoadedFile>(F);
    if (!LD)
      continue;

    auto version = LD->getLanguageVersionBuiltWith();
    if (!version.empty())
      return version;
  }

  return version::Version();
}

bool swift::diagnoseMissingImportForMember(const ValueDecl *decl,
                                           const DeclContext *dc,
                                           SourceLoc loc) {
  if (decl->findImport(dc))
    return false;

  auto &ctx = dc->getASTContext();
  auto definingModule = decl->getModuleContext();
  ctx.Diags.diagnose(loc, diag::candidate_from_missing_import,
                     decl->getDescriptiveKind(), decl->getName(),
                     definingModule);

  SourceLoc bestLoc =
      ctx.Diags.getBestAddImportFixItLoc(decl, dc->getParentSourceFile());
  if (!bestLoc.isValid())
    return false;

  llvm::SmallString<64> importText;

  // Check other source files for import flags that should be applied to the
  // fix-it for consistency with the rest of the imports in the module.
  auto parentModule = dc->getParentModule();
  OptionSet<ImportFlags> flags;
  for (auto file : parentModule->getFiles()) {
    if (auto sf = dyn_cast<SourceFile>(file))
      flags |= sf->getImportFlags(definingModule);
  }

  if (flags.contains(ImportFlags::Exported) ||
      parentModule->isClangOverlayOf(definingModule))
    importText += "@_exported ";
  if (flags.contains(ImportFlags::ImplementationOnly))
    importText += "@_implementationOnly ";
  if (flags.contains(ImportFlags::WeakLinked))
    importText += "@_weakLinked ";
  if (flags.contains(ImportFlags::SPIOnly))
    importText += "@_spiOnly ";

  // FIXME: Access level should be considered, too.

  // @_spi imports.
  if (decl->isSPI()) {
    auto spiGroups = decl->getSPIGroups();
    if (!spiGroups.empty()) {
      importText += "@_spi(";
      importText += spiGroups[0].str();
      importText += ") ";
    }
  }

  importText += "import ";
  importText += definingModule->getName().str();
  importText += "\n";
  ctx.Diags.diagnose(bestLoc, diag::candidate_add_import, definingModule)
      .fixItInsert(bestLoc, importText);

  return true;
}
