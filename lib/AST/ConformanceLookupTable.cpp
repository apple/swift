//===--- ConformanceLookupTable - Conformance Lookup Table ----------------===//
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
//  This file implements the ConformanceLookupTable class.
//
//===----------------------------------------------------------------------===//

#include "ConformanceLookupTable.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/Module.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/ProtocolConformanceRef.h"
#include "swift/AST/TypeCheckRequests.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace swift;

DeclContext *ConformanceLookupTable::ConformanceSource::getDeclContext() const {
  switch (getKind()) {
  case ConformanceEntryKind::Inherited:
    return getInheritingClass();

  case ConformanceEntryKind::Explicit:
    return getExplicitDeclContext();

  case ConformanceEntryKind::Implied:
    return getImpliedSource()->Source.getDeclContext();

  case ConformanceEntryKind::Synthesized:
    return getSynthesizedDeclContext();

  case ConformanceEntryKind::PreMacroExpansion:
    return getMacroGeneratedDeclContext();
  }

  llvm_unreachable("Unhandled ConformanceEntryKind in switch.");
}

ProtocolDecl *ConformanceLookupTable::ConformanceEntry::getProtocol() const {
  if (auto protocol = Conformance.dyn_cast<ProtocolDecl *>())
    return protocol;

  return Conformance.get<ProtocolConformance *>()->getProtocol();
}

void ConformanceLookupTable::ConformanceEntry::markSupersededBy(
       ConformanceLookupTable &table,
       ConformanceEntry *entry,
       bool diagnose) {
  assert(!isSuperseded() && "Already superseded");

  // Note that we've been superseded.
  SupersededBy = entry;

  if (diagnose) {
    // Record the problem in the conformance table. We'll
    // diagnose these in semantic analysis.
    table.AllSupersededDiagnostics[getDeclContext()].push_back(this);
  }
}

void ConformanceLookupTable::ConformanceEntry::dump() const {
  dump(llvm::errs());
}

void ConformanceLookupTable::ConformanceEntry::dump(raw_ostream &os,
                                                    unsigned indent) const {
  os.indent(indent) << "(conformance @" << static_cast<const void *>(this);

  os << " protocol=";
  getProtocol()->dumpRef(os);

  if (Loc.isValid()) {
    os << " loc=";
    Loc.print(os, getProtocol()->getASTContext().SourceMgr);
  }

  switch (getKind()) {
  case ConformanceEntryKind::Implied:
    os << " implied_by=@" 
       << static_cast<const void *>(Source.getImpliedSource());
    break;

  case ConformanceEntryKind::Explicit:
    os << " explicit";
    break;

  case ConformanceEntryKind::Inherited:
    os << " inherited";
    break;

  case ConformanceEntryKind::Synthesized:
    os << " synthesized";
    break;

  case ConformanceEntryKind::PreMacroExpansion:
    os << " unexpanded macro";
    break;
  }

  if (auto conf = getConformance()) {
    os << " fixed_conformance=@" << static_cast<const void *>(conf);
  }

  if (SupersededBy)
    os << " superseded_by=@" << static_cast<const void *>(SupersededBy);

  os << ")\n";
}

ConformanceLookupTable::ConformanceLookupTable(ASTContext &ctx) {
  // Register a cleanup with the ASTContext to call the conformance
  // table destructor.
  ctx.addCleanup([this]() {
    this->destroy();
  });
}

void ConformanceLookupTable::destroy() {
  this->~ConformanceLookupTable();
}

namespace {
  struct ConformanceConstructionInfo : public Located<ProtocolDecl *> {
    /// The location of the "unchecked" attribute, if this
    const SourceLoc uncheckedLoc;

    ConformanceConstructionInfo() { }

    ConformanceConstructionInfo(
      ProtocolDecl *item, SourceLoc loc,
      SourceLoc uncheckedLoc
    ) : Located(item, loc), uncheckedLoc(uncheckedLoc) { }
  };
}

template<typename NominalFunc, typename ExtensionFunc>
void ConformanceLookupTable::forEachInStage(ConformanceStage stage,
                                            NominalTypeDecl *nominal,
                                            NominalFunc nominalFunc,
                                            ExtensionFunc extensionFunc) {
  assert(static_cast<unsigned>(stage) < NumConformanceStages &&
         "NumConformanceStages has not been updated");

  LastProcessedEntry &lastProcessed
    = LastProcessed[nominal][static_cast<unsigned>(stage)];
  
  // Handle the nominal type.
  if (!lastProcessed.getInt()) {
    lastProcessed.setInt(true);

    // If we have conformances we can load, do so.
    // FIXME: This could be lazier.
    auto loader = nominal->takeConformanceLoader();
    if (loader.first) {
      SmallVector<ProtocolConformance *, 2> conformances;
      loader.first->loadAllConformances(nominal, loader.second, conformances);
      loadAllConformances(nominal, conformances);
    }

    nominalFunc(nominal);
  }

  // Protocol extensions do not contribute protocol conformances. This
  // is enforced by semantic analysis, so the early exit here is a
  // performance optimization and also prevents us from erroneously
  // including those protocols before they get diagnosed.
  if (isa<ProtocolDecl>(nominal))
    return;

  // Handle the extensions that we have not yet visited.
  nominal->prepareExtensions();
  while (auto next = lastProcessed.getPointer()
                       ? lastProcessed.getPointer()->NextExtension.getPointer()
                       : nominal->FirstExtension) {
    lastProcessed.setPointer(next);

    SmallVector<ConformanceConstructionInfo, 2> protocols;

    // If we have conformances we can load, do so.
    // FIXME: This could be lazier.
    auto loader = next->takeConformanceLoader();
    if (loader.first) {
      SmallVector<ProtocolConformance *, 2> conformances;
      loader.first->loadAllConformances(next, loader.second, conformances);
      loadAllConformances(next, conformances);
      for (auto conf : conformances) {
        protocols.push_back({conf->getProtocol(), SourceLoc(), SourceLoc()});
      }
    } else if (next->getParentSourceFile()) {
      bool anyObject = false;
      for (const auto &found :
               getDirectlyInheritedNominalTypeDecls(next, anyObject)) {
        if (auto proto = dyn_cast<ProtocolDecl>(found.Item))
          protocols.push_back({proto, found.Loc, found.uncheckedLoc});
      }
    }

    extensionFunc(next, protocols);
  }
}

void ConformanceLookupTable::inheritConformances(ClassDecl *classDecl, 
                                                 ClassDecl *superclassDecl,
                                                 ExtensionDecl *superclassExt) {
  // Local function to return the location of the superclass. This
  // takes a little digging, so compute on first use and cache it.
  SourceLoc superclassLoc;
  auto getSuperclassLoc = [&] {
    if (superclassLoc.isValid())
      return superclassLoc;

    for (const auto &inherited : classDecl->getInherited()) {
      if (auto inheritedType = inherited.getType()) {
        if (inheritedType->getClassOrBoundGenericClass()) {
          superclassLoc = inherited.getSourceRange().Start;
          return superclassLoc;
        }
        if (inheritedType->isExistentialType()) {
          auto layout = inheritedType->getExistentialLayout();
          if (layout.explicitSuperclass) {
            superclassLoc = inherited.getSourceRange().Start;
            return superclassLoc;
          }
        }
      }
    }

    superclassLoc = superclassDecl->getLoc();
    return superclassLoc;
  };

  llvm::SmallPtrSet<ProtocolDecl *, 4> protocols;
  auto addInheritedConformance = [&](ConformanceEntry *entry) {
    auto protocol = entry->getProtocol();

    // Don't add unavailable conformances.
    if (auto dc = entry->Source.getDeclContext()) {
      if (auto ext = dyn_cast<ExtensionDecl>(dc)) {
        if (AvailableAttr::isUnavailable(ext))
          return;
      }
    }

    // Don't add redundant conformances here. This is merely an
    // optimization; resolveConformances() would zap the duplicates
    // anyway.
    if (!protocols.insert(protocol).second)
      return;
    
    // Add the inherited entry.
    (void)addProtocol(protocol, getSuperclassLoc(), 
                      ConformanceSource::forInherited(classDecl));
  };

  // Add inherited conformances.
  DeclContext *superDC = superclassExt;
  if (!superclassExt)
    superDC = superclassDecl;

  for (auto entry : superclassDecl->ConformanceTable->AllConformances[superDC]){
    addInheritedConformance(entry);
  }
}

void ConformanceLookupTable::updateLookupTable(NominalTypeDecl *nominal,
                                               ConformanceStage stage) {
  switch (stage) {
  case ConformanceStage::RecordedExplicit:
    // Record all of the explicit conformances.
    forEachInStage(
        stage, nominal,
        [&](NominalTypeDecl *nominal) {
          addInheritedProtocols(nominal,
                                ConformanceSource::forExplicit(nominal));

          addMacroGeneratedProtocols(
              nominal, ConformanceSource::forUnexpandedMacro(nominal));
        },
        [&](ExtensionDecl *ext,
            ArrayRef<ConformanceConstructionInfo> protos) {
          // The extension decl may not be validated, so we can't use
          // its inherited protocols directly.
          auto source = ConformanceSource::forExplicit(ext);
          for (auto locAndProto : protos)
            addProtocol(
              locAndProto.Item, locAndProto.Loc,
              source.withUncheckedLoc(locAndProto.uncheckedLoc));
        });
    break;

  case ConformanceStage::Inherited:
    updateLookupTable(nominal, ConformanceStage::RecordedExplicit);

    // For classes, expand implied conformances of the superclass,
    // because an implied conformance in the superclass is considered
    // "fixed" in the subclass.
    if (auto classDecl = dyn_cast<ClassDecl>(nominal)) {
      if (auto superclassDecl = classDecl->getSuperclassDecl()) {
        // Break infinite recursion when visiting ill-formed classes
        // with circular inheritance.
        if (VisitingSuperclass)
          return;
        llvm::SaveAndRestore<bool> visiting(VisitingSuperclass, true);

        // Don't update our own lookup table if we inherit from ourselves.
        if (classDecl == superclassDecl)
          break;

        // Resolve the conformances of the superclass.
        superclassDecl->prepareConformanceTable();
        superclassDecl->ConformanceTable->updateLookupTable(
          superclassDecl,
          ConformanceStage::Resolved);
        
        // Expand inherited conformances from all superclasses.
        // We may have circular inheritance in ill-formed classes, so keep an
        // eye out for that.
        auto circularSuperclass = superclassDecl->getSuperclassDecl();
        
        do {
          forEachInStage(
              stage, superclassDecl,
              [&](NominalTypeDecl *superclass) {
                inheritConformances(classDecl, superclassDecl, nullptr);
              },
              [&](ExtensionDecl *ext,
                  ArrayRef<ConformanceConstructionInfo> protos) {
                (void)protos;
                inheritConformances(classDecl, superclassDecl, ext);
              });
          superclassDecl = superclassDecl->getSuperclassDecl();
          if (circularSuperclass)
            circularSuperclass = circularSuperclass->getSuperclassDecl();
          if (circularSuperclass)
            circularSuperclass = circularSuperclass->getSuperclassDecl();
        } while (superclassDecl != circularSuperclass);
      }
    }
    break;    

  case ConformanceStage::ExpandedImplied:
    // Record explicit conformances and import inherited conformances
    // before expanding.
    updateLookupTable(nominal, ConformanceStage::Inherited);

    // Expand inherited conformances.
    forEachInStage(
        stage, nominal,
        [&](NominalTypeDecl *nominal) {
          expandImpliedConformances(nominal, nominal);
        },
        [&](ExtensionDecl *ext,
            ArrayRef<ConformanceConstructionInfo> protos) {
          (void)protos;
          expandImpliedConformances(nominal, ext);
        });
    break;

  case ConformanceStage::Resolved:
    // Expand inherited conformances so we have the complete set of
    // conformances.
    updateLookupTable(nominal, ConformanceStage::ExpandedImplied);
    
    /// Determine whether any extensions were added that might require
    /// us to compute conformances again.
    bool anyChanged = false;
    forEachInStage(stage, nominal,
                   [&](NominalTypeDecl *nominal) { anyChanged = true; },
                   [&](ExtensionDecl *ext,
                       ArrayRef<ConformanceConstructionInfo>) {
                     anyChanged = true;
                   });

    if (anyChanged) {
      // Compute the conformances for each protocol.
      bool anySuperseded = false;
      for (const auto &entry : Conformances) {
        if (resolveConformances(entry.first))
          anySuperseded = true;
      }

      if (anySuperseded) {
        // Update the lists of all conformances to remove superseded
        // conformances.
        for (auto &conformances : AllConformances) {
          conformances.second.erase(
            std::remove_if(conformances.second.begin(),
                           conformances.second.end(),
                           [&](ConformanceEntry *entry) {
                             return entry->isSuperseded();
                           }),
            conformances.second.end());
        }
      }
    }
    break;
  }
}

void ConformanceLookupTable::loadAllConformances(
       DeclContext *dc,
       ArrayRef<ProtocolConformance*> conformances) {
  // If this declaration context came from source, there's nothing to
  // do here.
  if (dc->getParentSourceFile())
    return;

  // Add entries for each loaded conformance.
  for (auto conformance : conformances) {
    registerProtocolConformance(conformance);
  }
}

bool ConformanceLookupTable::addProtocol(ProtocolDecl *protocol, SourceLoc loc,
                                         ConformanceSource source) {
  DeclContext *dc = source.getDeclContext();
  ASTContext &ctx = dc->getASTContext();

  // Determine the kind of conformance.
  ConformanceEntryKind kind = source.getKind();

  // If this entry is synthesized or implied, scan to determine
  // whether there are any explicit better conformances that make this
  // conformance trivially superseded (and, therefore, not worth
  // recording).
  auto &conformanceEntries = Conformances[protocol];
  if (kind == ConformanceEntryKind::Implied ||
      kind == ConformanceEntryKind::Synthesized) {
    for (const auto *existingEntry : conformanceEntries) {
      switch (existingEntry->getKind()) {
      case ConformanceEntryKind::Explicit:
      case ConformanceEntryKind::Inherited:
      case ConformanceEntryKind::PreMacroExpansion:
        return false;

      case ConformanceEntryKind::Implied:
        // Ignore implied circular protocol inheritance
        if (existingEntry->getDeclContext() == dc)
          return false;

        // An implied conformance is better than a synthesized one, unless
        // the implied conformance was deserialized.
        if (kind == ConformanceEntryKind::Synthesized &&
            existingEntry->getDeclContext()->getParentSourceFile() == nullptr)
          return false;

        break;

      case ConformanceEntryKind::Synthesized:
        // An implied conformance is better unless it was deserialized.
        if (dc->getParentSourceFile() == nullptr)
          return false;

        break;
      }
    }
  }

  /// Build the conformance entry (if it hasn't been built before).
  ConformanceEntry *entry = new (ctx) ConformanceEntry(loc, protocol, source);
  conformanceEntries.push_back(entry);

  // Record this as a conformance within the given declaration
  // context.
  AllConformances[dc].push_back(entry);

  return true;
}

void ConformanceLookupTable::addInheritedProtocols(
    llvm::PointerUnion<const TypeDecl *, const ExtensionDecl *> decl,
    ConformanceSource source) {
  // Find all of the protocols in the inheritance list.
  bool anyObject = false;
  for (const auto &found :
          getDirectlyInheritedNominalTypeDecls(decl, anyObject)) {
    if (auto proto = dyn_cast<ProtocolDecl>(found.Item)) {
      addProtocol(
        proto, found.Loc, source.withUncheckedLoc(found.uncheckedLoc));
    }
  }
}

void ConformanceLookupTable::addMacroGeneratedProtocols(
    NominalTypeDecl *nominal, ConformanceSource source) {
  nominal->forEachAttachedMacro(
      MacroRole::Extension,
      [&](CustomAttr *attr, MacroDecl *macro) {
        SmallVector<ProtocolDecl *, 2> conformances;
        macro->getIntroducedConformances(nominal, conformances);

        for (auto *protocol : conformances) {
          addProtocol(protocol, attr->getLocation(), source);
        }
      });
}

void ConformanceLookupTable::expandImpliedConformances(NominalTypeDecl *nominal,
                                                       DeclContext *dc) {
  // Note: recursive type-checking implies that AllConformances
  // may be reallocated during this traversal, so pay the lookup cost
  // during each iteration.
  for (unsigned i = 0; i != AllConformances[dc].size(); ++i) {
    /// FIXME: Avoid the possibility of an infinite loop by fixing the root
    ///        cause instead (incomplete circularity detection).
    assert(i <= 16384 &&
           "Infinite loop due to circular protocol inheritance?");
    ConformanceEntry *conformanceEntry = AllConformances[dc][i];
    ProtocolDecl *conformingProtocol = conformanceEntry->getProtocol();

    // An @objc enum that explicitly conforms to the Error protocol
    // also implicitly conforms to _ObjectiveCBridgeableError, via the
    // known protocol _BridgedNSError.
    if (conformingProtocol->isSpecificProtocol(
          KnownProtocolKind::Error) &&
        isa<EnumDecl>(nominal) && nominal->isObjC() &&
        cast<EnumDecl>(nominal)->hasCases() &&
        cast<EnumDecl>(nominal)->hasOnlyCasesWithoutAssociatedValues()) {
      ASTContext &ctx = nominal->getASTContext();
      if (auto bridgedNSError
            = ctx.getProtocol(KnownProtocolKind::BridgedNSError)) {
        addProtocol(bridgedNSError, SourceLoc(),
                    ConformanceSource::forImplied(conformanceEntry));
      }
    }

    addInheritedProtocols(conformingProtocol,
                          ConformanceSource::forImplied(conformanceEntry));
  }
}

/// Determine whether the given conformance entry kind can be replaced.
static bool isReplaceable(ConformanceEntryKind kind) {
  switch (kind) {
  case ConformanceEntryKind::Implied:
  case ConformanceEntryKind::Synthesized:
  case ConformanceEntryKind::PreMacroExpansion:
    return true;

  case ConformanceEntryKind::Explicit:
  case ConformanceEntryKind::Inherited:
    return false;
  }

  llvm_unreachable("Unhandled ConformanceEntryKind in switch.");
}

ConformanceLookupTable::Ordering ConformanceLookupTable::compareConformances(
                                   ConformanceEntry *lhs,
                                   ConformanceEntry *rhs,
                                   bool &diagnoseSuperseded) {
  // If only one of the conformances is unconditionally available on the
  // current deployment target, pick that one.
  //
  // FIXME: Conformance lookup should really depend on source location for
  // this to be 100% correct.
  // FIXME: When a class and an extension with the same availability declare the
  // same conformance, this silently takes the class and drops the extension.
  if (lhs->getDeclContext()->isAlwaysAvailableConformanceContext() !=
      rhs->getDeclContext()->isAlwaysAvailableConformanceContext()) {
    return (lhs->getDeclContext()->isAlwaysAvailableConformanceContext()
            ? Ordering::Before
            : Ordering::After);
  }

  ConformanceEntryKind lhsKind = lhs->getRankingKind();
  ConformanceEntryKind rhsKind = rhs->getRankingKind();

  // Pre-expanded macro conformances are always superseded by
  // conformances written in source. If the conformance is not
  // written in the original source, the pre-expanded conformance
  // will be superseded by the conformance in the macro expansion
  // buffer.
  if (lhsKind == ConformanceEntryKind::PreMacroExpansion ||
      rhsKind == ConformanceEntryKind::PreMacroExpansion) {
    if (lhsKind != rhsKind) {
      return (lhs->getKind() < rhs->getKind()
              ? Ordering::Before
              : Ordering::After);
    }
  }

  // If one entry is fixed and the other is not, we have our answer.
  if (lhs->isFixed() != rhs->isFixed()) {
    auto isReplaceableOrMarker = [](ConformanceEntry *entry) -> bool {
      ConformanceEntryKind kind = entry->getRankingKind();
      if (isReplaceable(kind))
        return true;

      // Allow replacement of an explicit conformance to a marker protocol.
      // (This permits redundant explicit declarations of `Sendable`.)
      return (kind == ConformanceEntryKind::Explicit
              && entry->getProtocol()->isMarkerProtocol());
    };

    // If the non-fixed conformance is not replaceable, we have a failure to
    // diagnose.
    // FIXME: We should probably diagnose if they have different constraints.
    diagnoseSuperseded = (lhs->isFixed() && !isReplaceableOrMarker(rhs)) ||
                         (rhs->isFixed() && !isReplaceableOrMarker(lhs));
      
    return lhs->isFixed() ? Ordering::Before : Ordering::After;
  }

  if (lhsKind != ConformanceEntryKind::Implied ||
      rhsKind != ConformanceEntryKind::Implied) {
    // If both conformances are non-replaceable, diagnose the
    // superseded one.
    diagnoseSuperseded = !isReplaceable(lhsKind) && !isReplaceable(rhsKind) &&
      !(lhsKind == ConformanceEntryKind::Inherited &&
        rhsKind == ConformanceEntryKind::Inherited);

    // If we can order by kind, do so.
    if (lhsKind != rhsKind) {
      return (static_cast<unsigned>(lhsKind) < static_cast<unsigned>(rhsKind))
               ? Ordering::Before
               : Ordering::After;
    }

    // We shouldn't get two synthesized conformances. It's not harmful
    // per se, but it's indicative of redundant logic in the frontend.
    assert((lhs->getKind() != ConformanceEntryKind::Synthesized ||
            rhs->getKind() != ConformanceEntryKind::Synthesized) &&
          "Shouldn't ever get two truly synthesized conformances");

    // FIXME: Deterministic ordering.
    return Ordering::Before;
  }

  // Both the left- and right-hand sides are implied, so determine where the
  // conformance should go.
  assert(lhsKind == ConformanceEntryKind::Implied &&
         "Expected implied conformance");
  assert(rhsKind == ConformanceEntryKind::Implied &&
         "Expected implied conformance");
  diagnoseSuperseded = false;

  // First, try to use the stated explicit conformances to determine where the
  // conformance should go.
  auto lhsExplicit = lhs->getDeclaredConformance();
  auto lhsExplicitProtocol = lhsExplicit->getProtocol();
  auto rhsExplicit = rhs->getDeclaredConformance();
  auto rhsExplicitProtocol = rhsExplicit->getProtocol();
  if (lhsExplicitProtocol != rhsExplicitProtocol) {
    // If the explicit protocol for the left-hand side is implied by
    // the explicit protocol for the right-hand side, the left-hand
    // side supersedes the right-hand side.
    if (rhsExplicitProtocol->inheritsFrom(lhsExplicitProtocol))
      return Ordering::Before;

    // If the explicit protocol for the right-hand side is implied by
    // the explicit protocol for the left-hand side, the right-hand
    // side supersedes the left-hand side.
    if (lhsExplicitProtocol->inheritsFrom(rhsExplicitProtocol))
      return Ordering::After;
  }

  // Prefer the least conditional implier, which we approximate by seeing if one
  // of the contexts syntactically has no generic requirements. This misses
  // redundant cases like `struct Foo<T: P> {} extension Foo: P where T: P {}`
  // (Foo : P is unconditional), but isConstrainedExtension doesn't fly as it
  // requires the generic signature of the extension to exist, which requires
  // conformances to exist, which is what we're doing here.
  auto hasAdditionalRequirements = [&](ConformanceEntry *entry) {
    if (auto ED = dyn_cast<ExtensionDecl>(entry->getDeclContext()))
      if (auto TWC = ED->getTrailingWhereClause())
        return !TWC->getRequirements().empty();

    return false;
  };
  bool lhsHasReqs = hasAdditionalRequirements(lhs);
  bool rhsHasReqs = hasAdditionalRequirements(rhs);
  if (lhsHasReqs != rhsHasReqs)
    return lhsHasReqs ? Ordering::After : Ordering::Before;

  // If the two conformances come from the same file, pick the first context
  // in the file.
  auto lhsSF = lhs->getDeclContext()->getParentSourceFile();
  auto rhsSF = rhs->getDeclContext()->getParentSourceFile();
  if (lhsSF && lhsSF == rhsSF) {
    ASTContext &ctx = lhsSF->getASTContext();
    return ctx.SourceMgr.isBeforeInBuffer(lhs->getDeclaredLoc(),
                                          rhs->getDeclaredLoc())
             ? Ordering::Before
             : Ordering::After;
  }

  // If one of the conformances comes from the same file as the type
  // declaration, pick that one; this is so that conformance synthesis works if
  // there's any implied conformance in the same file as the type.
  auto NTD = lhs->getDeclContext()->getSelfNominalTypeDecl();
  auto typeSF = NTD->getParentSourceFile();
  if (typeSF) {
    if (typeSF == lhsSF)
      return Ordering::Before;
    if (typeSF == rhsSF)
      return Ordering::After;
  }

  // Otherwise, pick the earlier file unit.
  auto lhsFileUnit
    = dyn_cast<FileUnit>(lhs->getDeclContext()->getModuleScopeContext());
  auto rhsFileUnit
    = dyn_cast<FileUnit>(rhs->getDeclContext()->getModuleScopeContext());
  assert(lhsFileUnit && rhsFileUnit && "Not from a file unit?");
  if (lhsFileUnit == rhsFileUnit) {
    // If the file units are the same, just pick arbitrarily; we're not
    // actually emitting anything.
    // FIXME: Only because we're synthesizing conformances for deserialized
    // protocols. Once that's no longer true (because we're serializing
    // everything appropriately in the module), we should assert that this
    // does not happen.
    assert(!lhsSF && !rhsSF && "Source files shouldn't conflict");
    return Ordering::Before;
  }
  auto module = lhs->getDeclContext()->getParentModule();
  assert(lhs->getDeclContext()->getParentModule()
           == rhs->getDeclContext()->getParentModule() &&
         "conformances should be in the same module");
  for (auto file : module->getFiles()) {
    if (file == lhsFileUnit)
      return Ordering::Before;
    if (file == rhsFileUnit)
      return Ordering::After;
  }

  llvm_unreachable("files weren't in the parent module?");
}

bool ConformanceLookupTable::resolveConformances(ProtocolDecl *protocol) {
  // Find any entries that are superseded by other entries.
  ConformanceEntries &entries = Conformances[protocol];
  llvm::SmallPtrSet<DeclContext *, 4> knownConformances;
  bool anySuperseded = false;
  for (auto entry : entries) {
    // If this entry has a conformance associated with it, note that.
    if (entry->getConformance())
      knownConformances.insert(entry->getDeclContext());

    // If this entry was superseded, move on.
    if (entry->isSuperseded()) {
      anySuperseded = true;
      continue;
    }

    // Determine whether this entry is superseded by (or supersedes)
    // some other entry.
    for (auto otherEntry : entries) {
      if (entry == otherEntry)
        continue;

      if (otherEntry->isSuperseded()) {
        anySuperseded = true;
        continue;
      }

      bool diagnoseSuperseded = false;
      bool doneWithEntry = false;
      switch (compareConformances(entry, otherEntry, diagnoseSuperseded)) {
      case Ordering::Equivalent:
        break;

      case Ordering::Before:
        otherEntry->markSupersededBy(*this, entry, diagnoseSuperseded);
        anySuperseded = true; 
        break;

      case Ordering::After:
        entry->markSupersededBy(*this, otherEntry, diagnoseSuperseded);
        anySuperseded = true;
        doneWithEntry = true;
        break;
      }
    
      if (doneWithEntry)
        break;
    }
  }

  // If any entries were superseded, remove them now.
  if (anySuperseded) {
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](ConformanceEntry *entry) {
                                   return entry->isSuperseded();
                                 }),
                  entries.end());
  }

  return anySuperseded;
}

DeclContext *ConformanceLookupTable::getConformingContext(
               NominalTypeDecl *nominal,
               ConformanceEntry *entry) {
  ProtocolDecl *protocol = entry->getProtocol();

  // Dig through the inherited entries to find a non-inherited one.
  // Handle recursive inheritance.
  SmallPtrSet<ClassDecl *, 4> visited;
  while (entry->getKind() == ConformanceEntryKind::Inherited) {
    // Make sure we have an up-to-date conformance table for the
    // superclass.
    auto classDecl = cast<ClassDecl>(nominal);
    if (!visited.insert(classDecl).second)
      return nullptr;

    // If we had a circular dependency, the superclass may not exist.
    auto superclassDecl
      = classDecl->getSuperclassDecl();
    
    if (!superclassDecl)
      return nullptr;

    if (!classDecl->ConformanceTable->VisitingSuperclass) {
      llvm::SaveAndRestore<bool> visiting(
                                   classDecl->ConformanceTable
                                     ->VisitingSuperclass,
                                   true);

      superclassDecl->prepareConformanceTable();
      superclassDecl->ConformanceTable->resolveConformances(protocol);
    }

    // Grab the superclass entry and continue searching for a
    // non-inherited conformance.
    // FIXME: Ambiguity detection and resolution.
    const auto &superclassConformances =
        superclassDecl->ConformanceTable->Conformances[protocol];
    if (superclassConformances.empty()) {
      assert(protocol->isSpecificProtocol(KnownProtocolKind::Sendable));

      // Go dig for a superclass that does conform to Sendable.
      // FIXME: This is a hack because the inherited conformances aren't
      // getting updated properly.
      Type classTy = nominal->getDeclaredInterfaceType();
      ModuleDecl *module = nominal->getParentModule();
      do {
        Type superclassTy = classTy->getSuperclassForDecl(superclassDecl);
        if (superclassTy->is<ErrorType>())
          return nullptr;
        auto inheritedConformance = module->lookupConformance(
            superclassTy, protocol, /*allowMissing=*/false);
        if (inheritedConformance.hasUnavailableConformance())
          inheritedConformance = ProtocolConformanceRef::forInvalid();
        if (inheritedConformance)
          return superclassDecl;
      } while ((superclassDecl = superclassDecl->getSuperclassDecl()));

      return nullptr;
    }

    entry = superclassConformances.front();
    nominal = superclassDecl;
  }

  return entry->getDeclContext();
}

ProtocolConformance *
ConformanceLookupTable::getConformance(NominalTypeDecl *nominal,
                                       ConformanceEntry *entry) {
  // If we already have a conformance, we're done.
  if (auto conformance = entry->getConformance())
    return conformance;

  ProtocolDecl *protocol = entry->getProtocol();

  // Determine where the explicit conformance actually lives.
  // FIXME: This is a hack to ensure that inherited conformances are
  // always "single step", which is bad for resilience but is assumed
  // elsewhere in the compiler.
  DeclContext *conformingDC = getConformingContext(nominal, entry);
  if (!conformingDC)
    return nullptr;

  auto *conformingNominal = conformingDC->getSelfNominalTypeDecl();

  // Form the conformance.
  Type type = entry->getDeclContext()->getDeclaredInterfaceType();
  ASTContext &ctx = nominal->getASTContext();
  if (entry->getKind() == ConformanceEntryKind::Inherited) {
    // For an inherited conformance, the conforming nominal type will
    // be different from the nominal type.
    assert(conformingNominal != nominal && "Broken inherited conformance");

    // Find the superclass type that matches where the conformance was
    // declared.
    auto *conformingClass = cast<ClassDecl>(conformingNominal);
    Type superclassTy = type->getSuperclassForDecl(conformingClass);
    if (superclassTy->is<ErrorType>())
      return nullptr;

    // Look up the inherited conformance.
    ModuleDecl *module = entry->getDeclContext()->getParentModule();
    auto inheritedConformance = module->lookupConformance(
        superclassTy, protocol, /*allowMissing=*/true);

    // Form the inherited conformance.
    entry->Conformance =
        ctx.getInheritedConformance(type, inheritedConformance.getConcrete());
  } else {
    // Protocols don't have conformance lookup tables. Self-conformance is
    // handled directly in lookupConformance().
    assert(!isa<ProtocolDecl>(conformingNominal));
    assert(!isa<ProtocolDecl>(conformingDC->getSelfNominalTypeDecl()));
    Type conformingType = conformingDC->getSelfInterfaceType();

    SourceLoc conformanceLoc
      = conformingNominal == conformingDC
          ? conformingNominal->getLoc()
          : cast<ExtensionDecl>(conformingDC)->getLoc();

    // Create or find the normal conformance.
    auto normalConf =
        ctx.getConformance(conformingType, protocol, conformanceLoc,
                           conformingDC, ProtocolConformanceState::Incomplete,
                           entry->Source.getUncheckedLoc().isValid());
    // Invalid code may cause the getConformance call below to loop, so break
    // the infinite recursion by setting this eagerly to shortcircuit with the
    // early return at the start of this function.
    entry->Conformance = normalConf;

    NormalProtocolConformance *implyingConf = nullptr;
    if (entry->Source.getKind() == ConformanceEntryKind::Implied) {
      auto implyingEntry = entry->Source.getImpliedSource();
      implyingConf = getConformance(conformingNominal, implyingEntry)
                         ->getRootNormalConformance();
    }
    normalConf->setSourceKindAndImplyingConformance(entry->Source.getKind(),
                                                    implyingConf);

    // If the conformance was synthesized by the ClangImporter, give it a
    // lazy loader that will be used to populate the conformance.

    // First, if this is a conformance to a base protocol of a derived
    // protocol, find the most derived protocol.
    auto *impliedEntry = entry;
    while (impliedEntry->getKind() == ConformanceEntryKind::Implied)
      impliedEntry = impliedEntry->Source.getImpliedSource();

    // Check if this was a synthesized conformance.
    if (impliedEntry->getKind() == ConformanceEntryKind::Synthesized) {
      auto *impliedProto = impliedEntry->getProtocol();

      // Find a SynthesizedProtocolAttr corresponding to the protocol.
      for (auto attr : conformingNominal->getAttrs()
             .getAttributes<SynthesizedProtocolAttr>()) {
        auto otherProto = attr->getProtocol();
        if (otherProto == impliedProto) {
          // Set the conformance loader to the loader stashed inside
          // the attribute.
          normalConf->setLazyLoader(attr->getLazyLoader(), /*context=*/0);
          if (attr->isUnchecked())
            normalConf->setUnchecked();
          break;
        }
      }
    }
  }

  return entry->Conformance.get<ProtocolConformance *>();
}

void ConformanceLookupTable::addSynthesizedConformance(
    NominalTypeDecl *nominal, ProtocolDecl *protocol,
    DeclContext *conformanceDC) {
  addProtocol(protocol, nominal->getLoc(),
              ConformanceSource::forSynthesized(conformanceDC));
}

void ConformanceLookupTable::registerProtocolConformance(
       ProtocolConformance *conformance,
       bool synthesized) {
  auto protocol = conformance->getProtocol();
  auto dc = conformance->getDeclContext();
  auto nominal = dc->getSelfNominalTypeDecl();

  // If there is an entry to update, do so.
  auto &dcConformances = AllConformances[dc];
  for (auto entry : dcConformances) {
    if (entry->getProtocol() == protocol) {
      assert(!entry->getConformance() ||
             entry->getConformance() == conformance &&
             "Mismatched conformances");
      entry->Conformance = conformance;
      return;
    }
  }

  // Otherwise, add a new entry.
  auto inherited = dyn_cast<InheritedProtocolConformance>(conformance);
  ConformanceSource source
    = inherited   ? ConformanceSource::forInherited(cast<ClassDecl>(nominal)) :
      synthesized ? ConformanceSource::forSynthesized(dc) :
                    ConformanceSource::forExplicit(dc);

  ASTContext &ctx = nominal->getASTContext();
  ConformanceEntry *entry = new (ctx) ConformanceEntry(SourceLoc(),
                                                       protocol,
                                                       source);
  entry->Conformance = conformance;

  // Record that this type conforms to the given protocol.
  Conformances[protocol].push_back(entry);

  // Record this as a conformance within the given declaration
  // context.
  dcConformances.push_back(entry);
}

bool ConformanceLookupTable::lookupConformance(
       NominalTypeDecl *nominal,
       ProtocolDecl *protocol, 
       SmallVectorImpl<ProtocolConformance *> &conformances) {
  // Update to record all explicit and inherited conformances.
  updateLookupTable(nominal, ConformanceStage::Inherited);

  // Look for conformances to this protocol.
  auto known = Conformances.find(protocol);
  if (known == Conformances.end()) {
    // If we didn't find anything, expand implied conformances.
    updateLookupTable(nominal, ConformanceStage::ExpandedImplied);
    known = Conformances.find(protocol);

    // We didn't find anything.
    if (known == Conformances.end())
      return false;
  }

  // Resolve the conformances for this protocol.
  resolveConformances(protocol);
  for (auto entry : Conformances[protocol]) {
    if (auto conformance = getConformance(nominal, entry)) {
      conformances.push_back(conformance);
    }
  }
  return !conformances.empty();
}

void ConformanceLookupTable::lookupConformances(
       NominalTypeDecl *nominal,
       DeclContext *dc,
       std::vector<ProtocolConformance *> *conformances,
       SmallVectorImpl<ConformanceDiagnostic> *diagnostics) {
  // We need to expand all implied conformances before we can find
  // those conformances that pertain to this declaration context.
  updateLookupTable(nominal, ConformanceStage::ExpandedImplied);

  /// Resolve conformances for each of the protocols to which this
  /// declaration may provide a conformance. Only some of these will
  /// result in conformances that are attributed to this declaration
  /// context.
  auto &potentialConformances = AllConformances[dc]; 
  for (const auto &potential : potentialConformances) {
    resolveConformances(potential->getProtocol());
  }

  // Remove any superseded conformances from AllConformances.
  potentialConformances.erase(
    std::remove_if(potentialConformances.begin(),
                   potentialConformances.end(),
                   [&](ConformanceEntry *entry) {
                     if (entry->isSuperseded())
                       return true;

                     // Record the conformance.
                     if (conformances) {
                       if (auto conformance = getConformance(nominal, entry))
                         conformances->push_back(conformance);
                     }
                     return false;
                   }),
    potentialConformances.end());

  // Gather any diagnostics we've produced.
  if (diagnostics) {
    auto knownDiags = AllSupersededDiagnostics.find(dc);
    if (knownDiags != AllSupersededDiagnostics.end()) {
      for (const auto *entry : knownDiags->second) {
        ConformanceEntry *supersededBy = entry->getSupersededBy();

        diagnostics->push_back({entry->getProtocol(), 
                                entry->getDeclaredLoc(),
                                entry->getKind(),
                                entry->getDeclaredConformance()->getProtocol(),
                                supersededBy->getDeclContext(),
                                supersededBy->getKind(),
                                supersededBy->getDeclaredConformance()
                                  ->getProtocol()});
      }

      // We have transferred these diagnostics; erase them.
      AllSupersededDiagnostics.erase(knownDiags);
    }
  }
}

void ConformanceLookupTable::getAllProtocols(
    NominalTypeDecl *nominal, SmallVectorImpl<ProtocolDecl *> &scratch,
    bool sorted) {
  // We need to expand all implied conformances to find the complete
  // set of protocols to which this nominal type conforms.
  updateLookupTable(nominal, ConformanceStage::ExpandedImplied);

  // Gather all of the protocols.
  for (const auto &conformance : Conformances) {
    if (conformance.second.empty())
      continue;

    scratch.push_back(conformance.first);
  }

  if (sorted) {
    llvm::array_pod_sort(scratch.begin(), scratch.end(), TypeDecl::compare);
  }
}

int ConformanceLookupTable::compareProtocolConformances(
      ProtocolConformance * const *lhsPtr,
      ProtocolConformance * const *rhsPtr) {
  ProtocolConformance *lhs = *lhsPtr;
  ProtocolConformance *rhs = *rhsPtr;

  // If the two conformances are normal conformances with locations,
  // sort by location.
  if (auto lhsNormal = dyn_cast<NormalProtocolConformance>(lhs)) {
    if (auto rhsNormal = dyn_cast<NormalProtocolConformance>(rhs)) {
      if (lhsNormal->getLoc().isValid() && rhsNormal->getLoc().isValid()) {
        ASTContext &ctx = lhs->getDeclContext()->getASTContext();
        unsigned lhsBuffer
          = ctx.SourceMgr.findBufferContainingLoc(lhsNormal->getLoc());
        unsigned rhsBuffer
          = ctx.SourceMgr.findBufferContainingLoc(rhsNormal->getLoc());

        // If the buffers are the same, use source location ordering.
        if (lhsBuffer == rhsBuffer) {
          return ctx.SourceMgr.isBeforeInBuffer(lhsNormal->getLoc(),
                                                rhsNormal->getLoc());
        }

        // Otherwise, order by buffer identifier.
        return ctx.SourceMgr.getIdentifierForBuffer(lhsBuffer)
                 .compare(ctx.SourceMgr.getIdentifierForBuffer(rhsBuffer));
      }
    }
  }

  // Otherwise, sort by protocol.
  ProtocolDecl *lhsProto = lhs->getProtocol();
  ProtocolDecl *rhsProto = rhs->getProtocol();
  return TypeDecl::compare(lhsProto, rhsProto);
}

void ConformanceLookupTable::getAllConformances(
       NominalTypeDecl *nominal,
       bool sorted,
       SmallVectorImpl<ProtocolConformance *> &scratch) {
  // We need to expand and resolve all conformances to enumerate them.
  updateLookupTable(nominal, ConformanceStage::Resolved);

  // Gather all of the protocols.
  for (const auto &conformance : AllConformances) {
    for (auto entry : conformance.second) {
      if (auto conformance = getConformance(nominal, entry))
        scratch.push_back(conformance);
    }
  }

  // If requested, sort the results.
  if (sorted) {
    llvm::array_pod_sort(scratch.begin(), scratch.end(),
                         &compareProtocolConformances);
  }
}

void ConformanceLookupTable::getImplicitProtocols(
       NominalTypeDecl *nominal,
       SmallVectorImpl<ProtocolDecl *> &protocols) {
  for (auto conformance : AllConformances[nominal]) {
    if (conformance->getKind() == ConformanceEntryKind::Synthesized) {
      protocols.push_back(conformance->getProtocol());
    }
  }
}

ArrayRef<ValueDecl *>
ConformanceLookupTable::getSatisfiedProtocolRequirementsForMember(
                                                const ValueDecl *member,
                                                NominalTypeDecl *nominal,
                                                bool sorted) {
  auto It = ConformingDeclMap.find(member);
  if (It != ConformingDeclMap.end())
    return It->second;

  SmallVector<ProtocolConformance *, 4> result;
  getAllConformances(nominal, sorted, result);

  auto &reqs = ConformingDeclMap[member];
  if (isa<TypeDecl>(member)) {
    for (auto *conf : result) {
      if (conf->isInvalid())
        continue;

      conf->forEachTypeWitness([&](const AssociatedTypeDecl *assoc,
                                   Type type,
                                   TypeDecl *typeDecl) -> bool {
        if (typeDecl == member)
          reqs.push_back(const_cast<AssociatedTypeDecl*>(assoc));
        return false;
      });
    }
  } else {
    for (auto *conf : result) {
      if (conf->isInvalid())
        continue;

      auto root = conf->getRootConformance();
      root->forEachValueWitness([&](ValueDecl *req, Witness witness) {
        if (witness.getDecl() == member)
          reqs.push_back(req);
      });
    }
  }

  return reqs;
}

void ConformanceLookupTable::dump() const {
  dump(llvm::errs());
}
  
void ConformanceLookupTable::dump(raw_ostream &os) const {
  for (const auto &dcEntries : AllConformances) {
    os << "Conformances in context:\n";
    dcEntries.first->printContext(os);
    for (auto entry : dcEntries.second) {
      entry->dump(os);
    }
  }
}

