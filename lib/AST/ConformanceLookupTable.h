//===--- ConformanceLookupTable - Conformance Lookup Table ------*- C++ -*-===//
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
//  This file defines the ConformanceLookupTable class, which manages protocol
//  conformances for a given nominal type. Most clients should not access this
//  table directly; rather, they should go through the NominalTypeDecl or
//  DeclContext entry points.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_CONFORMANCE_LOOKUP_TABLE_H
#define SWIFT_AST_CONFORMANCE_LOOKUP_TABLE_H

#include "swift/AST/DeclContext.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/SourceLoc.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SetVector.h"
#include <unordered_map>

namespace swift {

class ExtensionDecl;
class ModuleDecl;

/// Keeps track of the protocols to which a particular nominal type conforms.
///
/// This table is a lower-level detail that clients should generally not
/// access directly. Rather, one should use the protocol- and
/// conformance-centric entry points in \c NominalTypeDecl and \c DeclContext.
class ConformanceLookupTable : public ASTAllocated<ConformanceLookupTable> {
  /// Describes the stage at which a particular nominal type or
  /// extension's conformances has been processed.
  enum class ConformanceStage : uint8_t {
    /// The explicit conformances have been recorded in the lookup table.
    RecordedExplicit,

    /// Conformances from the superclass have been inherited.
    Inherited,

    /// The explicit conformances have been expanded out to include
    /// the conformances they imply.
    ExpandedImplied,

    /// The complete set of conformances have been fully resolved to
    /// assign conformances, diagnose conflicts, etc.
    Resolved,
  };

  /// The number of conformance stages.
  static const unsigned NumConformanceStages = 4;

  /// An entry in the last-processed list, which contains a pointer to
  /// the last extension that was processed at a particular stage (or
  /// nullptr if no extensions have been processed) and indicates
  /// whether the nominal type declaration itself has been processed
  /// at that stage.
  typedef llvm::PointerIntPair<ExtensionDecl *, 1, bool> LastProcessedEntry;

  /// Array indicating how far we have gotten in processing each
  /// nominal type and list of extensions for each stage of
  /// conformance checking.
  ///
  /// Uses std::unordered_map instead of DenseMap so that stable interior
  /// references can be taken.
  std::unordered_map<NominalTypeDecl *,
                     std::array<LastProcessedEntry, NumConformanceStages>>
  LastProcessed;
  
  struct ConformanceEntry;

  /// Describes the "source" of a conformance, indicating where the
  /// conformance came from.
  class ConformanceSource {
    void *Storage;

    ConformanceEntryKind Kind;

    /// The location of the "unchecked" attribute, if there is one.
    SourceLoc uncheckedLoc;

    ConformanceSource(void *ptr, ConformanceEntryKind kind) 
      : Storage(ptr), Kind(kind) { }

  public:
    /// Create an inherited conformance.
    ///
    /// The given class will have an inherited conformance for the
    /// requested protocol.
    static ConformanceSource forInherited(ClassDecl *classDecl) {
      return ConformanceSource(classDecl, ConformanceEntryKind::Inherited);
    }

    /// Create an explicit conformance.
    ///
    /// The given declaration context (nominal type declaration or
    /// extension thereof) explicitly specifies conformance to the
    /// protocol.
    static ConformanceSource forExplicit(DeclContext *dc) {
      return ConformanceSource(dc, ConformanceEntryKind::Explicit);
    }

    /// Create an implied conformance.
    ///
    /// Conformance to the protocol is implied by the given
    /// conformance entry. The chain of conformance entries will
    /// eventually terminate in a non-implied conformance.
    static ConformanceSource forImplied(ConformanceEntry *entry) {
      return ConformanceSource(entry, ConformanceEntryKind::Implied);
    }

    /// Create a synthesized conformance.
    ///
    /// The given declaration context (for a type) will get a synthesized
    /// conformance to the requested protocol.
    static ConformanceSource forSynthesized(DeclContext *dc) {
      return ConformanceSource(dc, ConformanceEntryKind::Synthesized);
    }

    static ConformanceSource forUnexpandedMacro(DeclContext *dc) {
      return ConformanceSource(dc, ConformanceEntryKind::PreMacroExpansion);
    }

    /// Return a new conformance source with the given location of "@unchecked".
    ConformanceSource withUncheckedLoc(SourceLoc uncheckedLoc) {
      ConformanceSource result(*this);
      if (uncheckedLoc.isValid())
        result.uncheckedLoc = uncheckedLoc;
      return result;
    }

    /// Retrieve the kind of conformance formed from this source.
    ConformanceEntryKind getKind() const { return Kind; }

    /// Retrieve kind of the conformance for ranking purposes.
    ///
    /// The only difference between the ranking kind and the kind is
    /// that implied conformances originating from a synthesized
    /// conformance are considered to be synthesized (which has a
    /// lower ranking).
    ConformanceEntryKind getRankingKind() const {
      switch (auto kind = getKind()) {
      case ConformanceEntryKind::Explicit:
      case ConformanceEntryKind::Inherited:
      case ConformanceEntryKind::Synthesized:
      case ConformanceEntryKind::PreMacroExpansion:
        return kind;

      case ConformanceEntryKind::Implied:
        return (getImpliedSource()->getDeclaredConformance()->getKind()
                  == ConformanceEntryKind::Synthesized)
                 ? ConformanceEntryKind::Synthesized
                 : ConformanceEntryKind::Implied;
      }

      llvm_unreachable("Unhandled ConformanceEntryKind in switch.");
    }

    /// The location of the @unchecked attribute, if any.
    SourceLoc getUncheckedLoc() const {
      return uncheckedLoc;
    }

    /// For an inherited conformance, retrieve the class declaration
    /// for the inheriting class.
    ClassDecl *getInheritingClass() const {
      assert(getKind() == ConformanceEntryKind::Inherited);
      return static_cast<ClassDecl *>(Storage);
    }

    /// For an explicit conformance, retrieve the declaration context
    /// that specifies the conformance.
    DeclContext *getExplicitDeclContext() const {
      assert(getKind() == ConformanceEntryKind::Explicit);
      return static_cast<DeclContext *>(Storage);
    }

    DeclContext *getMacroGeneratedDeclContext() const {
      assert(getKind() == ConformanceEntryKind::PreMacroExpansion);
      return static_cast<DeclContext *>(Storage);
    }

    /// For a synthesized conformance, retrieve the nominal type decl
    /// that will receive the conformance.
    ConformanceEntry *getImpliedSource() const {
      assert(getKind() == ConformanceEntryKind::Implied);
      return static_cast<ConformanceEntry *>(Storage);
    }

    /// For a synthesized conformance, retrieve the nominal type decl
    /// that will receive the conformance.
    DeclContext *getSynthesizedDeclContext() const {
      assert(getKind() == ConformanceEntryKind::Synthesized);
      return static_cast<DeclContext *>(Storage);
    }

    /// Get the declaration context that this conformance will be
    /// associated with.
    DeclContext *getDeclContext() const;
  };

  /// An entry in the conformance table.
  struct ConformanceEntry : public ASTAllocated<ConformanceEntry> {
    /// The source location within the current context where the
    /// protocol conformance was specified.
    SourceLoc Loc;

    /// If this conformance entry has been superseded, the conformance
    /// that superseded it.
    ConformanceEntry *SupersededBy = nullptr;

    /// The source of this conformance entry , which is either a
    /// DeclContext (for an explicitly-specified conformance) or a
    /// link to the conformance that implied this conformance.
    ConformanceSource Source;

    /// Either the protocol to be resolved or the resolved protocol conformance.
    llvm::PointerUnion<ProtocolDecl *, ProtocolConformance *> Conformance;

    ConformanceEntry(SourceLoc loc, ProtocolDecl *protocol,
                     ConformanceSource source)
      : Loc(loc), Source(source), Conformance(protocol) { }

    /// Retrieve the location at which this conformance was declared
    /// or synthesized.
    SourceLoc getLoc() const { return Loc; }

    /// Whether this conformance is already "fixed" and cannot be superseded.
    bool isFixed() const {
      // A conformance from an unexpanded macro can always be superseded.
      if (getKind() == ConformanceEntryKind::PreMacroExpansion)
        return true;

      // If a conformance has been assigned, it cannot be superseded.
      if (getConformance())
        return true;

      // Otherwise, only inherited conformances are fixed.
      switch (getKind()) {
      case ConformanceEntryKind::Explicit:
      case ConformanceEntryKind::Implied:
      case ConformanceEntryKind::Synthesized:
      case ConformanceEntryKind::PreMacroExpansion:
        return false;

      case ConformanceEntryKind::Inherited:
        return true;
      }

      llvm_unreachable("Unhandled ConformanceEntryKind in switch.");
    }

    /// Whether this protocol conformance was superseded by another
    /// conformance.
    bool isSuperseded() const { return SupersededBy != nullptr; }

    /// Retrieve the conformance entry that superseded this one.
    ConformanceEntry *getSupersededBy() const { return SupersededBy; }

    /// Note that this conformance entry was superseded by the given
    /// entry.
    void markSupersededBy(ConformanceLookupTable &table,
                          ConformanceEntry *entry,
                          bool diagnose);

    /// Determine the kind of conformance.
    ConformanceEntryKind getKind() const {
      return Source.getKind();
    }

    /// Determine the kind of conformance for ranking purposes.
    ConformanceEntryKind getRankingKind() const {
      return Source.getRankingKind();
    }

    /// Retrieve the declaration context associated with this conformance.
    DeclContext *getDeclContext() const {
      return Source.getDeclContext();
    }

    /// Retrieve the protocol to which this conformance entry refers.
    ProtocolDecl *getProtocol() const;

    /// Retrieve the conformance for this entry, if it has one.
    ProtocolConformance *getConformance() const {
      return Conformance.dyn_cast<ProtocolConformance *>();
    }

    /// Retrieve the conformance entry where the conformance was
    /// declared.
    const ConformanceEntry *getDeclaredConformance() const {
      if (Source.getKind() == ConformanceEntryKind::Implied)
        return Source.getImpliedSource()->getDeclaredConformance();

      return this;
    }

    /// Retrieve the source location of the place where the
    /// conformance was introduced, e.g., an explicit conformance or
    /// the point at which a subclass inherits a conformance from its
    /// superclass.
    SourceLoc getDeclaredLoc() const {
      if (Source.getKind() == ConformanceEntryKind::Implied)
        return Source.getImpliedSource()->getDeclaredLoc();

      return Loc;
    }

    SWIFT_DEBUG_DUMP;
    void dump(raw_ostream &os, unsigned indent = 0) const;
  };

  /// The set of conformance entries for a given protocol.
  typedef llvm::TinyPtrVector<ConformanceEntry *> ConformanceEntries;

  /// The type of the internal conformance table.
  typedef llvm::MapVector<ProtocolDecl *, ConformanceEntries> ConformanceTable;

  /// The conformance table.
  ConformanceTable Conformances;

  typedef llvm::SmallVector<ProtocolDecl *, 2> ProtocolList;

  /// List of all of the protocols to which a given context declares
  /// conformance, both explicitly and implicitly.
  llvm::MapVector<DeclContext *, SmallVector<ConformanceEntry *, 4>>
    AllConformances;

  /// The complete set of diagnostics about erroneously superseded
  /// protocol conformances.
  llvm::SmallDenseMap<DeclContext *, std::vector<ConformanceEntry *> >
    AllSupersededDiagnostics;

  /// Associates a conforming decl to its protocol conformance decls.
  llvm::DenseMap<const ValueDecl *, llvm::TinyPtrVector<ValueDecl *>>
    ConformingDeclMap;

  /// Indicates whether we are visiting the superclass.
  bool VisitingSuperclass = false;

  /// Add a protocol.
  bool addProtocol(ProtocolDecl *protocol, SourceLoc loc,
                   ConformanceSource source);

  /// Add the protocols from the given list.
  void addInheritedProtocols(
      llvm::PointerUnion<const TypeDecl *, const ExtensionDecl *> decl,
      ConformanceSource source);

  /// Add the protocols added by attached extension macros that are not
  /// yet expanded.
  void addMacroGeneratedProtocols(
      NominalTypeDecl *nominal, ConformanceSource source);

  /// Expand the implied conformances for the given DeclContext.
  void expandImpliedConformances(NominalTypeDecl *nominal, DeclContext *dc);

  /// A three-way ordering
  enum class Ordering {
    Before,
    Equivalent,
    After,
  };

  /// Determine whether the first conformance entry supersedes the
  /// second when determining where to place the conformance.
  ///
  /// \param diagnoseSuperseded When one entry is better than another,
  /// whether to diagnose the problem as an error.
  Ordering compareConformances(ConformanceEntry *lhs, ConformanceEntry *rhs,
                               bool &diagnoseSuperseded);

  /// Resolve the set of conformances that will be generated for the
  /// given protocol.
  ///
  /// \returns true if any conformance entries were superseded by this
  /// operation.
  bool resolveConformances(ProtocolDecl *protocol);

  /// Retrieve the declaration context that provides the
  /// (non-inherited) conformance described by the given conformance
  /// entry.
  DeclContext *getConformingContext(NominalTypeDecl *nominal,
                                    ConformanceEntry *entry);

  /// Resolve the given conformance entry to an actual protocol conformance.
  ProtocolConformance *getConformance(NominalTypeDecl *nominal,
                                      ConformanceEntry *entry);

  /// Enumerate each of the unhandled contexts (nominal type
  /// declaration or extension) within the given stage.
  ///
  /// \param stage The stage to process. Note that it is up to the
  /// caller to ensure that prior stages have already been handled.
  ///
  /// \param nominalFunc Function object to be invoked when the
  /// nominal type declaration itself needs to be processed. It takes
  /// the nominal type declaration and its result is ignored.
  ///
  /// \param extensionFunc Function object to be invoked with a given
  /// extension needs to be processed. It takes the extension as an
  /// argument and its result is ignored.
  template<typename NominalFunc, typename ExtensionFunc>
  void forEachInStage(ConformanceStage stage,
                      NominalTypeDecl *nominal,
                      NominalFunc nominalFunc,
                      ExtensionFunc extensionFunc);

  /// Inherit the conformances from the given superclass into the
  /// given nominal type.
  ///
  /// \param classDecl The class into which the conformances will be
  /// inherited.
  ///
  /// \param superclassDecl The superclass from which the conformances
  /// will be inherited.
  ///
  /// \param superclassExt If non-null, the superclass extension from
  /// which conformances will be inherited. If null, the conformances
  /// on the superclass declaration itself will be inherited.
  void inheritConformances(ClassDecl *classDecl, 
                           ClassDecl *superclassDecl,
                           ExtensionDecl *superclassExt);

  /// Update a lookup table with conformances from newly-added extensions.
  void updateLookupTable(NominalTypeDecl *nominal, ConformanceStage stage);

  /// Load all of the protocol conformances for the given (serialized)
  /// declaration context.
  void loadAllConformances(DeclContext *dc,
                           ArrayRef<ProtocolConformance *> conformances);

public:
  /// Create a new conformance lookup table.
  ConformanceLookupTable(ASTContext &ctx);

  /// Destroy the conformance table.
  void destroy();

  /// Add a synthesized conformance to the lookup table.
  void addSynthesizedConformance(NominalTypeDecl *nominal,
                                 ProtocolDecl *protocol,
                                 DeclContext *conformanceDC);

  /// Register an externally-supplied protocol conformance.
  void registerProtocolConformance(ProtocolConformance *conformance,
                                   bool synthesized = false);

  /// Look for conformances to the given protocol.
  ///
  /// \param conformances Will be populated with the set of protocol
  /// conformances found for this protocol and nominal type.
  ///
  /// \returns true if any conformances were found. 
  bool lookupConformance(NominalTypeDecl *nominal,
                         ProtocolDecl *protocol, 
                         SmallVectorImpl<ProtocolConformance *> &conformances);

  /// Look for all of the conformances within the given declaration context.
  void lookupConformances(NominalTypeDecl *nominal,
                          DeclContext *dc,
                          std::vector<ProtocolConformance *> *conformances,
                          SmallVectorImpl<ConformanceDiagnostic> *diagnostics);

  /// Retrieve the complete set of protocols to which this nominal
  /// type conforms (if the set contains a protocol, the same is true for any
  /// inherited protocols).
  ///
  /// \param sorted Whether to sort the protocols in canonical order.
  void getAllProtocols(NominalTypeDecl *nominal,
                       SmallVectorImpl<ProtocolDecl *> &scratch, bool sorted);

  /// Retrieve the complete set of protocol conformances for this
  /// nominal type.
  void getAllConformances(NominalTypeDecl *nominal,
                          bool sorted,
                          SmallVectorImpl<ProtocolConformance *> &scratch);

  /// Retrieve the protocols that would be implicitly synthesized.
  /// FIXME: This is a hack, because it's the wrong question to ask. It
  /// skips over the possibility that there is an explicit conformance
  /// somewhere.
  void getImplicitProtocols(NominalTypeDecl *nominal,
                            SmallVectorImpl<ProtocolDecl *> &protocols);

  /// Returns the protocol requirements that \c Member conforms to.
  ArrayRef<ValueDecl *>
  getSatisfiedProtocolRequirementsForMember(const ValueDecl *member,
                                            NominalTypeDecl *nominal,
                                            bool sorted);

  SWIFT_DEBUG_DUMP;
  void dump(raw_ostream &os) const;

  /// Compare two protocol conformances to place them in some canonical order.
  static int compareProtocolConformances(ProtocolConformance * const *lhsPtr,
                                         ProtocolConformance * const *rhsPtr);
};

}

#endif /* SWIFT_AST_CONFORMANCE_LOOKUP_TABLE_H */
