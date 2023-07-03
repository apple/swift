//===-------- FrontendSourceFileDepGraphFactory.cpp -----------------------===//
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

// This file holds the code to build a SourceFileDepGraph in the frontend.
// This graph captures relationships between definitions and uses, and
// it is written to a file which is read by the driver in order to decide which
// source files require recompilation.

#include "FrontendSourceFileDepGraphFactory.h"

// may not all be needed
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/AbstractSourceFileDepGraphFactory.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/FileSystem.h"
#include "swift/AST/FineGrainedDependencies.h"
#include "swift/AST/FineGrainedDependencyFormat.h"
#include "swift/AST/Module.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Types.h"
#include "swift/Basic/FileSystem.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/ReferenceDependencyKeys.h"
#include "swift/Demangling/Demangle.h"
#include "swift/Frontend/FrontendOptions.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualOutputBackend.h"
#include "llvm/Support/YAMLParser.h"

using namespace swift;
using namespace fine_grained_dependencies;

//==============================================================================
// MARK: Constructing from a SourceFile
//==============================================================================

//==============================================================================
// MARK: Helpers for key construction that must be in frontend
//==============================================================================

static std::string identifierForContext(const DeclContext *DC) {
  if (!DC) return "";

  Mangle::ASTMangler Mangler;
  if (const auto *context = dyn_cast<NominalTypeDecl>(DC)) {
    return Mangler.mangleTypeAsContextUSR(context);
  }

  const auto *ext = cast<ExtensionDecl>(DC);
  auto fp = ext->getBodyFingerprint().value_or(Fingerprint::ZERO());
  auto typeStr = Mangler.mangleTypeAsContextUSR(ext->getExtendedNominal());
  return (typeStr + "@" + fp.getRawValue()).str();
}

//==============================================================================
// MARK: DependencyKey::Builder
//==============================================================================

DependencyKey DependencyKey::Builder::build() && {
  return DependencyKey{
    kind,
    aspect,
    identifierForContext(context),
    name.str()
  };
}

DependencyKey::Builder DependencyKey::Builder::fromReference(
    const evaluator::DependencyCollector::Reference &ref) {
  // Assume that what is depended-upon is the interface
  using Kind = evaluator::DependencyCollector::Reference::Kind;

  switch (ref.kind) {
  case Kind::Empty:
  case Kind::Tombstone:
    llvm_unreachable("Cannot enumerate dead reference!");
  case Kind::PotentialMember:
    return Builder{kind, aspect}.withContext(ref.subject->getAsDecl());
  case Kind::TopLevel:
  case Kind::Dynamic:
    return Builder{kind, aspect, nullptr, ref.name.userFacingName()};
  case Kind::UsedMember:
    return Builder{kind, aspect, nullptr, ref.name.userFacingName()}
        .withContext(ref.subject->getAsDecl());
  }
}

DependencyKey::Builder DependencyKey::Builder::withContext(const Decl *D) && {
  switch (kind) {
  case NodeKind::nominal:
  case NodeKind::potentialMember:
  case NodeKind::member: {
    /// nominal and potential member dependencies are created from a Decl and
    /// use the context field.
    const DeclContext *context = dyn_cast<NominalTypeDecl>(D);
    if (!context) {
      context = cast<ExtensionDecl>(D);
    }
    return Builder{kind, aspect, context, name};
  }
  case NodeKind::topLevel:
  case NodeKind::dynamicLookup:
  case NodeKind::externalDepend:
  case NodeKind::sourceFileProvide:
    // Context field is not used for most kinds
    return Builder{kind, aspect, nullptr, name};
  case NodeKind::kindCount:
    llvm_unreachable("saw count!");
  }
}

DependencyKey::Builder DependencyKey::Builder::withContext(
    std::pair<const NominalTypeDecl *, const ValueDecl *> holderAndMember) && {
  /// \ref member dependencies are created from a pair and use the context
  /// field.
  return std::move(*this).withContext(holderAndMember.first);
}

DependencyKey::Builder DependencyKey::Builder::withName(StringRef name) && {
  assert(!name.empty());
  return Builder{kind, aspect, context, name};
}

DependencyKey::Builder DependencyKey::Builder::withName(const Decl *decl) && {
  switch (kind) {
  case NodeKind::topLevel: {
    auto name = getTopLevelName(decl);
    return Builder{kind, aspect, context, name};
  }
  case NodeKind::dynamicLookup: {
    auto name = cast<ValueDecl>(decl)->getBaseName().userFacingName();
    return Builder{kind, aspect, context, name};
  }
  case NodeKind::nominal:
  case NodeKind::potentialMember:
  case NodeKind::member:
  case NodeKind::externalDepend:
  case NodeKind::sourceFileProvide:
    return Builder{kind, aspect, context, ""};
  case NodeKind::kindCount:
    llvm_unreachable("saw count!");
  }
}

DependencyKey::Builder DependencyKey::Builder::withName(
    std::pair<const NominalTypeDecl *, const ValueDecl *> holderAndMember) && {
  return std::move(*this).withName(
      holderAndMember.second->getBaseName().userFacingName());
}

StringRef DependencyKey::Builder::getTopLevelName(const Decl *decl) {
  switch (decl->getKind()) {
  case DeclKind::PrecedenceGroup:
    // Precedence groups are referenced by name.
    return cast<PrecedenceGroupDecl>(decl)->getName().str();
  case DeclKind::Func:
    // Functions are referenced by base name.
    return cast<FuncDecl>(decl)->getBaseName().userFacingName();

  case DeclKind::InfixOperator:
  case DeclKind::PrefixOperator:
  case DeclKind::PostfixOperator:
    // N.B. It's critical we cast to OperatorDecl here as
    // OperatorDecl::getName is shadowed.
    return cast<OperatorDecl>(decl)->getName().str();

  case DeclKind::Enum:
  case DeclKind::Struct:
  case DeclKind::Class:
  case DeclKind::Protocol:
    // Nominal types are referenced by name.
    return cast<NominalTypeDecl>(decl)->getName().str();

  case DeclKind::BuiltinTuple:
    llvm_unreachable("Should never appear in source file");

  case DeclKind::Extension:
    // FIXME: We ought to provide an identifier for extensions so we can
    // register type body fingerprints for them.
    return "";
  case DeclKind::TypeAlias:
  case DeclKind::Var:
  case DeclKind::Accessor:
  case DeclKind::Constructor:
  case DeclKind::Destructor:
  case DeclKind::Subscript:
  case DeclKind::EnumElement:
  case DeclKind::GenericTypeParam:
  case DeclKind::AssociatedType:
  case DeclKind::Param:
  case DeclKind::OpaqueType:
  case DeclKind::Macro:
    return cast<ValueDecl>(decl)->getBaseName().userFacingName();

  case DeclKind::Import:
  case DeclKind::PatternBinding:
  case DeclKind::EnumCase:
  case DeclKind::TopLevelCode:
  case DeclKind::IfConfig:
  case DeclKind::PoundDiagnostic:
  case DeclKind::Missing:
  case DeclKind::MissingMember:
  case DeclKind::Module:
  case DeclKind::MacroExpansion:
    return "";
  }

  llvm_unreachable("Invalid decl kind!");
}

//==============================================================================
// MARK: Entry point into frontend graph construction
//==============================================================================

bool fine_grained_dependencies::withReferenceDependencies(
    llvm::PointerUnion<const ModuleDecl *, const SourceFile *> MSF,
    const DependencyTracker &depTracker, llvm::vfs::OutputBackend &backend,
    StringRef outputPath, bool alsoEmitDotFile,
    llvm::function_ref<bool(SourceFileDepGraph &&)> cont) {
  if (auto *MD = MSF.dyn_cast<const ModuleDecl *>()) {
    SourceFileDepGraph g =
        ModuleDepGraphFactory(backend, MD, alsoEmitDotFile).construct();
    return cont(std::move(g));
  } else {
    auto *SF = MSF.get<const SourceFile *>();
    SourceFileDepGraph g =
        FrontendSourceFileDepGraphFactory(SF, backend, outputPath, depTracker,
                                          alsoEmitDotFile)
            .construct();
    return cont(std::move(g));
  }
}

//==============================================================================
// MARK: FrontendSourceFileDepGraphFactory
//==============================================================================

FrontendSourceFileDepGraphFactory::FrontendSourceFileDepGraphFactory(
    const SourceFile *SF, llvm::vfs::OutputBackend &backend,
    StringRef outputPath, const DependencyTracker &depTracker,
    const bool alsoEmitDotFile)
    : AbstractSourceFileDepGraphFactory(
          SF->getASTContext().hadError(), outputPath, SF->getInterfaceHash(),
          alsoEmitDotFile, SF->getASTContext().Diags, backend),
      SF(SF), depTracker(depTracker) {}

//==============================================================================
// MARK: FrontendSourceFileDepGraphFactory - adding collections of defined Decls
//==============================================================================
//==============================================================================
// MARK: DeclFinder
//==============================================================================

namespace {
/// Takes all the Decls in a SourceFile, and collects them into buckets by
/// groups of DeclKinds. Also casts them to more specific types
struct DeclFinder {
  // The extracted Decls:
  ConstPtrVec<ExtensionDecl> extensions;
  ConstPtrVec<OperatorDecl> operators;
  ConstPtrVec<PrecedenceGroupDecl> precedenceGroups;
  ConstPtrVec<NominalTypeDecl> topNominals;
  ConstPtrVec<ValueDecl> topValues;
  ConstPtrVec<NominalTypeDecl> allNominals;
  ConstPtrVec<NominalTypeDecl> potentialMemberHolders;
  ConstPtrVec<FuncDecl> memberOperatorDecls;
  ConstPtrPairVec<NominalTypeDecl, ValueDecl> valuesInExtensions;
  ConstPtrVec<ValueDecl> classMembers;

  using LookupClassMember = llvm::function_ref<void(VisibleDeclConsumer &)>;

public:
  /// Construct me and separates the Decls.
  // clang-format off
    DeclFinder(ArrayRef<Decl *> topLevelDecls,
               LookupClassMember lookupClassMember) {
      for (const Decl *const D : topLevelDecls) {
        select<ExtensionDecl, DeclKind::Extension>(D, extensions) ||
        select<OperatorDecl, DeclKind::InfixOperator, DeclKind::PrefixOperator,
        DeclKind::PostfixOperator>(D, operators) ||
        select<PrecedenceGroupDecl, DeclKind::PrecedenceGroup>(
                                                               D, precedenceGroups) ||
        select<NominalTypeDecl, DeclKind::Enum, DeclKind::Struct,
        DeclKind::Class, DeclKind::Protocol>(D, topNominals) ||
        select<ValueDecl, DeclKind::TypeAlias, DeclKind::Var, DeclKind::Func,
        DeclKind::Accessor>(D, topValues);
      }
    // clang-format on
    // The order is important because some of these use instance variables
    // computed by others.
    findNominalsFromExtensions();
    findNominalsInTopNominals();
    findValuesInExtensions();
    findClassMembers(lookupClassMember);
  }

private:
  /// Extensions may contain nominals and operators.
  void findNominalsFromExtensions() {
    for (auto *ED : extensions) {
      const auto *const NTD = ED->getExtendedNominal();
      if (NTD)
        findNominalsAndOperatorsIn(NTD, ED);
    }
  }
  /// Top-level nominals may contain nominals and operators.
  void findNominalsInTopNominals() {
    for (const auto *const NTD : topNominals)
      findNominalsAndOperatorsIn(NTD);
  }
  /// Any nominal may contain nominals and operators.
  /// (indirectly recursive)
  void findNominalsAndOperatorsIn(const NominalTypeDecl *const NTD,
                                  const ExtensionDecl *ED = nullptr) {
    allNominals.push_back(NTD);
    potentialMemberHolders.push_back(NTD);
    findNominalsAndOperatorsInMembers(ED ? ED->getMembers()
                                         : NTD->getMembers());
  }

  /// Search through the members to find nominals and operators.
  /// (indirectly recursive)
  /// TODO: clean this up, maybe recurse separately for each purpose.
  void findNominalsAndOperatorsInMembers(const DeclRange members) {
    for (const Decl *const D : members) {
      auto *VD = dyn_cast<ValueDecl>(D);
      if (!VD)
        continue;
      if (VD->getName().isOperator())
        memberOperatorDecls.push_back(cast<FuncDecl>(D));
      else if (const auto *const NTD = dyn_cast<NominalTypeDecl>(D))
        findNominalsAndOperatorsIn(NTD);
    }
  }

  /// Extensions may contain ValueDecls.
  void findValuesInExtensions() {
    for (const auto *ED : extensions) {
      const auto *const NTD = ED->getExtendedNominal();
      if (!NTD) {
        continue;
      }

      for (const auto *member : ED->getMembers()) {
        const auto *VD = dyn_cast<ValueDecl>(member);
        if (!VD || !VD->hasName()) {
          continue;
        }

        if (const auto *const NTD = ED->getExtendedNominal()) {
          valuesInExtensions.push_back(std::make_pair(NTD, VD));
        }
      }
    }
  }

  /// Class members are needed for dynamic lookup dependency nodes.
  void findClassMembers(LookupClassMember lookup) {
    struct Collector : public VisibleDeclConsumer {
      ConstPtrVec<ValueDecl> &classMembers;
      Collector(ConstPtrVec<ValueDecl> &classMembers)
          : classMembers(classMembers) {}
      void foundDecl(ValueDecl *VD, DeclVisibilityKind,
                     DynamicLookupInfo) override {
        classMembers.push_back(VD);
      }
    } collector{classMembers};
    lookup(collector);
  }

  /// Check \p D to see if it is one of the DeclKinds in the template
  /// arguments. If so, cast it to DesiredDeclType and add it to foundDecls.
  /// \returns true if successful.
  template <typename DesiredDeclType, DeclKind firstKind,
            DeclKind... restOfKinds>
  bool select(const Decl *const D, ConstPtrVec<DesiredDeclType> &foundDecls) {
    if (D->getKind() == firstKind) {
      foundDecls.push_back(cast<DesiredDeclType>(D));
      return true;
    }
    return select<DesiredDeclType, restOfKinds...>(D, foundDecls);
  }

  /// Terminate the template recursion.
  template <typename DesiredDeclType>
  bool select(const Decl *const D, ConstPtrVec<DesiredDeclType> &foundDecls) {
    return false;
  }
};
} // namespace

void FrontendSourceFileDepGraphFactory::addAllDefinedDecls() {
  // TODO: express the multiple provides and depends streams with variadic
  // templates

  // Many kinds of Decls become top-level depends.

  DeclFinder declFinder(SF->getTopLevelDecls(),
                        [this](VisibleDeclConsumer &consumer) {
    SF->lookupClassMembers({}, consumer);
  });

  addAllDefinedDeclsOfAGivenType<NodeKind::topLevel>(
      declFinder.precedenceGroups);
  addAllDefinedDeclsOfAGivenType<NodeKind::topLevel>(
      declFinder.memberOperatorDecls);
  addAllDefinedDeclsOfAGivenType<NodeKind::topLevel>(declFinder.operators);
  addAllDefinedDeclsOfAGivenType<NodeKind::topLevel>(declFinder.topNominals);
  addAllDefinedDeclsOfAGivenType<NodeKind::topLevel>(declFinder.topValues);
  addAllDefinedDeclsOfAGivenType<NodeKind::nominal>(declFinder.allNominals);
  addAllDefinedDeclsOfAGivenType<NodeKind::nominal>(declFinder.extensions);
  addAllDefinedDeclsOfAGivenType<NodeKind::potentialMember>(
      declFinder.potentialMemberHolders);
  addAllDefinedDeclsOfAGivenType<NodeKind::member>(
      declFinder.valuesInExtensions);
  addAllDefinedDeclsOfAGivenType<NodeKind::dynamicLookup>(
      declFinder.classMembers);
}

//==============================================================================
// MARK: FrontendSourceFileDepGraphFactory - adding collections of used Decls
//==============================================================================

namespace {
/// Extracts uses out of a SourceFile
class UsedDeclEnumerator {
  const SourceFile *SF;
  StringRef swiftDeps;

  /// Cache these for efficiency
  const DependencyKey sourceFileImplementation;

public:
  UsedDeclEnumerator(const SourceFile *SF, StringRef swiftDeps)
      : SF(SF), swiftDeps(swiftDeps),
        sourceFileImplementation(DependencyKey::createKeyForWholeSourceFile(
            DeclAspect::implementation, swiftDeps)) {}

public:
  using UseEnumerator =
      llvm::function_ref<void(const DependencyKey &, const DependencyKey &)>;
  void enumerateAllUses(UseEnumerator enumerator) {
    auto &Ctx = SF->getASTContext();
    Ctx.evaluator.enumerateReferencesInFile(SF, [&](const auto &ref) {
      // Assume that what is depended-upon is the interface
      auto key =
          DependencyKey::Builder(translateKind(ref.kind), DeclAspect::interface)
              .fromReference(ref)
              .build();
      return enumerateUse(key, enumerator);
    });
    enumerateNominalUses(enumerator);
  }

private:
  void enumerateUse(DependencyKey key, UseEnumerator createDefUse) {
    // Assume that what is depended-upon is the interface
    createDefUse(key, sourceFileImplementation);
  }

  void enumerateNominalUses(UseEnumerator enumerator) {
    auto &Ctx = SF->getASTContext();
    Ctx.evaluator.enumerateReferencesInFile(SF, [&](const auto &ref) {
      const DeclContext *subject = ref.subject;
      if (!subject) {
        return;
      }

      auto nominalKey =
          DependencyKey::Builder(NodeKind::nominal, DeclAspect::interface)
              .withContext(subject->getSelfNominalTypeDecl())
              .build();
      enumerateUse(nominalKey, enumerator);

      auto declKey =
          DependencyKey::Builder(NodeKind::nominal, DeclAspect::interface)
              .withContext(subject->getAsDecl())
              .build();

      // If the subject of this dependency is not the nominal type itself,
      // record another arc for the extension this member came from.
      if (nominalKey != declKey) {
        assert(isa<ExtensionDecl>(subject));
        enumerateUse(declKey, enumerator);
      }
    });
  }

  using ReferenceKind = evaluator::DependencyCollector::Reference::Kind;
  static NodeKind translateKind(ReferenceKind kind) {
    switch (kind) {
    case ReferenceKind::UsedMember:
      return NodeKind::member;
    case ReferenceKind::PotentialMember:
      return NodeKind::potentialMember;
    case ReferenceKind::TopLevel:
      return NodeKind::topLevel;
    case ReferenceKind::Dynamic:
      return NodeKind::dynamicLookup;
    case ReferenceKind::Empty:
      llvm_unreachable("Found invalid reference kind!");
    case ReferenceKind::Tombstone:
      llvm_unreachable("Found invalid reference kind!");
    }
  }
};
} // end namespace

namespace {
class ExternalDependencyEnumerator {
  const DependencyTracker &depTracker;
  const DependencyKey sourceFileImplementation;

public:
  using UseEnumerator =
      llvm::function_ref<void(const DependencyKey &, const DependencyKey &,
                              llvm::Optional<Fingerprint>)>;

  ExternalDependencyEnumerator(const DependencyTracker &depTracker,
                               StringRef swiftDeps)
      : depTracker(depTracker),
        sourceFileImplementation(DependencyKey::createKeyForWholeSourceFile(
            DeclAspect::implementation, swiftDeps)) {}

  void enumerateExternalUses(UseEnumerator enumerator) {
    for (const auto &id : depTracker.getIncrementalDependencies()) {
      enumerateUse<NodeKind::externalDepend>(enumerator, id.path,
                                             id.fingerprint);
    }
    for (StringRef s : depTracker.getDependencies()) {
      enumerateUse<NodeKind::externalDepend>(enumerator, s, llvm::None);
    }
  }

private:
  template <NodeKind kind>
  void enumerateUse(UseEnumerator createDefUse, StringRef name,
                    llvm::Optional<Fingerprint> maybeFP) {
    static_assert(kind == NodeKind::externalDepend,
                  "Not a kind of external dependency!");
    createDefUse(DependencyKey(kind, DeclAspect::interface, "", name.str()),
                 sourceFileImplementation, maybeFP);
  }
};
} // end namespace

void FrontendSourceFileDepGraphFactory::addAllUsedDecls() {
  UsedDeclEnumerator(SF, swiftDeps)
      .enumerateAllUses(
          [&](const DependencyKey &def, const DependencyKey &use) {
            addAUsedDecl(def, use);
          });
  ExternalDependencyEnumerator(depTracker, swiftDeps)
      .enumerateExternalUses([&](const DependencyKey &def,
                                 const DependencyKey &use,
                                 llvm::Optional<Fingerprint> maybeFP) {
        addAnExternalDependency(def, use, maybeFP);
      });
}

//==============================================================================
// MARK: ModuleDepGraphFactory
//==============================================================================

ModuleDepGraphFactory::ModuleDepGraphFactory(llvm::vfs::OutputBackend &backend,
                                             const ModuleDecl *Mod,
                                             bool emitDot)
    : AbstractSourceFileDepGraphFactory(
          Mod->getASTContext().hadError(), Mod->getNameStr(),
          Mod->getFingerprint(), emitDot, Mod->getASTContext().Diags, backend),
      Mod(Mod) {}

void ModuleDepGraphFactory::addAllDefinedDecls() {
  // TODO: express the multiple provides and depends streams with variadic
  // templates

  // Many kinds of Decls become top-level depends.

  SmallVector<Decl *, 32> TopLevelDecls;
  Mod->getTopLevelDecls(TopLevelDecls);
  DeclFinder declFinder(TopLevelDecls,
                        [this](VisibleDeclConsumer &consumer) {
                          return Mod->lookupClassMembers({}, consumer);
                        });

  addAllDefinedDeclsOfAGivenType<NodeKind::topLevel>(
      declFinder.precedenceGroups);
  addAllDefinedDeclsOfAGivenType<NodeKind::topLevel>(
      declFinder.memberOperatorDecls);
  addAllDefinedDeclsOfAGivenType<NodeKind::topLevel>(declFinder.operators);
  addAllDefinedDeclsOfAGivenType<NodeKind::topLevel>(declFinder.topNominals);
  addAllDefinedDeclsOfAGivenType<NodeKind::topLevel>(declFinder.topValues);
  addAllDefinedDeclsOfAGivenType<NodeKind::nominal>(declFinder.allNominals);
  addAllDefinedDeclsOfAGivenType<NodeKind::nominal>(declFinder.extensions);
  addAllDefinedDeclsOfAGivenType<NodeKind::potentialMember>(
      declFinder.potentialMemberHolders);
  addAllDefinedDeclsOfAGivenType<NodeKind::member>(
      declFinder.valuesInExtensions);
}
