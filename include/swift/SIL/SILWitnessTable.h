//===--- SILWitnessTable.h - Defines the SILWitnessTable class --*- C++ -*-===//
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
// This file defines the SILWitnessTable class, which is used to map a protocol
// conformance for a type to its implementing SILFunctions. This information is
// (FIXME will be) used by IRGen to create witness tables for protocol dispatch.
// It can also be used by generic specialization and existential
// devirtualization passes to promote witness_method and protocol_method
// instructions to static function_refs.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_SILWITNESSTABLE_H
#define SWIFT_SIL_SILWITNESSTABLE_H

#include "swift/SIL/SILAllocated.h"
#include "swift/SIL/SILDeclRef.h"
#include "swift/SIL/SILFunction.h"
#include "swift/AST/ProtocolConformanceRef.h"
#include "llvm/ADT/ilist_node.h"
#include "llvm/ADT/ilist.h"

namespace swift {

class SILFunction;
class SILModule;
class ProtocolConformance;
class RootProtocolConformance;
enum SerializedKind_t : uint8_t;

/// A mapping from each requirement of a protocol to the SIL-level entity
/// satisfying the requirement for a concrete type.
class SILWitnessTable : public llvm::ilist_node<SILWitnessTable>,
                        public SILAllocated<SILWitnessTable>
{
public:
  /// A witness table entry describing the witness for a method.
  struct MethodWitness {
    /// The method required.
    SILDeclRef Requirement;
    /// The witness for the method.
    /// This can be null in case dead function elimination has removed the method
    /// or if the method was not serialized (for de-serialized witness tables).
    SILFunction *Witness;
  };
  
  /// A witness table entry describing the witness for an associated type.
  struct AssociatedTypeWitness {
    /// The associated type required.
    AssociatedTypeDecl *Requirement;
    /// The concrete semantic type of the witness.
    CanType Witness;
  };
  
  /// A witness table entry describing the witness for an associated type's
  /// protocol requirement.
  struct AssociatedTypeProtocolWitness {
    /// The associated type required.  A dependent type in the protocol's
    /// context.
    CanType Requirement;
    /// The protocol requirement on the type.
    ProtocolDecl *Protocol;
    /// The ProtocolConformance satisfying the requirement. Null if the
    /// conformance is dependent.
    ProtocolConformanceRef Witness;
  };
  
  /// A witness table entry referencing the protocol conformance for a refined
  /// base protocol.
  struct BaseProtocolWitness {
    /// The base protocol.
    ProtocolDecl *Requirement;
    /// The ProtocolConformance for the base protocol.
    ProtocolConformance *Witness;
  };
                          
  /// A witness table entry kind.
  enum WitnessKind {
    Invalid,
    Method,
    AssociatedType,
    AssociatedTypeProtocol,
    BaseProtocol
  } ENUM_EXTENSIBILITY_ATTR(open);
  
  /// A witness table entry.
  class Entry {
    WitnessKind Kind;
    union {
      MethodWitness Method;
      AssociatedTypeWitness AssociatedType;
      AssociatedTypeProtocolWitness AssociatedTypeProtocol;
      BaseProtocolWitness BaseProtocol;
    };
    
  public:
    Entry() : Kind(WitnessKind::Invalid) {}
    
    Entry(const MethodWitness &Method)
      : Kind(WitnessKind::Method), Method(Method)
    {}
    
    Entry(const AssociatedTypeWitness &AssociatedType)
      : Kind(WitnessKind::AssociatedType), AssociatedType(AssociatedType)
    {}
    
    Entry(const AssociatedTypeProtocolWitness &AssociatedTypeProtocol)
      : Kind(WitnessKind::AssociatedTypeProtocol),
        AssociatedTypeProtocol(AssociatedTypeProtocol)
    {}
    
    Entry(const BaseProtocolWitness &BaseProtocol)
      : Kind(WitnessKind::BaseProtocol),
        BaseProtocol(BaseProtocol)
    {}
    
    WitnessKind getKind() const { return Kind; }

    bool isValid() const { return Kind != WitnessKind::Invalid; }

    const MethodWitness &getMethodWitness() const {
      assert(Kind == WitnessKind::Method);
      return Method;
    }
    const AssociatedTypeWitness &getAssociatedTypeWitness() const {
      assert(Kind == WitnessKind::AssociatedType);
      return AssociatedType;
    }
    const AssociatedTypeProtocolWitness &
    getAssociatedTypeProtocolWitness() const {
      assert(Kind == WitnessKind::AssociatedTypeProtocol);
      return AssociatedTypeProtocol;
    }
    const BaseProtocolWitness &getBaseProtocolWitness() const {
      assert(Kind == WitnessKind::BaseProtocol);
      return BaseProtocol;
    }
    
    void removeWitnessMethod() {
      assert(Kind == WitnessKind::Method);
      if (Method.Witness) {
        Method.Witness->decrementRefCount();
      }
      Method.Witness = nullptr;
    }

    void print(llvm::raw_ostream &out, bool verbose,
               const PrintOptions &options) const;
  };

  /// An entry for a conformance requirement that makes the requirement
  /// conditional. These aren't public, but any witness thunks need to feed them
  /// into the true witness functions.
  struct ConditionalConformance {
    CanType Requirement;
    ProtocolConformanceRef Conformance;
  };

private:
  /// The module which contains the SILWitnessTable.
  SILModule &Mod;

  /// The symbol name of the witness table that will be propagated to the object
  /// file level.
  StringRef Name;

  /// The linkage of the witness table.
  SILLinkage Linkage;

  /// The conformance mapped to this witness table.
  RootProtocolConformance *Conformance;

  /// The various witnesses containing in this witness table. Is empty if the
  /// table has no witness entries or if it is a declaration.
  MutableArrayRef<Entry> Entries;

  /// Any conditional conformances required for this witness table. These are
  /// private to this conformance.
  ///
  /// (If other private entities are introduced this could/should be switched
  /// into a private version of Entries.)
  MutableArrayRef<ConditionalConformance> ConditionalConformances;

  /// Whether or not this witness table is a declaration. This is separate from
  /// whether or not entries is empty since you can have an empty witness table
  /// that is not a declaration.
  bool IsDeclaration;
 
  /// Whether or not this witness table is serialized, which allows
  /// devirtualization from another module.
  unsigned SerializedKind : 2;

  /// Private constructor for making SILWitnessTable definitions.
  SILWitnessTable(SILModule &M, SILLinkage Linkage, SerializedKind_t Serialized,
                  StringRef name, RootProtocolConformance *conformance,
                  ArrayRef<Entry> entries,
                  ArrayRef<ConditionalConformance> conditionalConformances);

  /// Private constructor for making SILWitnessTable declarations.
  SILWitnessTable(SILModule &M, SILLinkage Linkage, StringRef Name,
                  RootProtocolConformance *conformance);

  void addWitnessTable();

public:
  /// Create a new SILWitnessTable definition with the given entries.
  static SILWitnessTable *
  create(SILModule &M, SILLinkage Linkage, SerializedKind_t SerializedKind,
         RootProtocolConformance *conformance, ArrayRef<Entry> entries,
         ArrayRef<ConditionalConformance> conditionalConformances);

  /// Create a new SILWitnessTable declaration.
  static SILWitnessTable *create(SILModule &M, SILLinkage Linkage,
                                 RootProtocolConformance *conformance);

  ~SILWitnessTable();
  
  SILModule &getModule() const { return Mod; }

  /// Return the AST ProtocolConformance this witness table represents.
  RootProtocolConformance *getConformance() const {
    return Conformance;
  }

  /// Return the context in which the conformance giving rise to this
  /// witness table was defined.
  DeclContext *getDeclContext() const;

  /// Return the protocol for which this witness table is a conformance.
  ProtocolDecl *getProtocol() const;

  /// Return the nominal type declaration which conforms to the protocol.
  NominalTypeDecl *getConformingNominal() const;

  /// Return the symbol name of the witness table that will be propagated to the
  /// object file level.
  StringRef getName() const { return Name; }

  /// Returns true if this witness table is a declaration.
  bool isDeclaration() const { return IsDeclaration; }

  /// Returns true if this witness table is a definition.
  bool isDefinition() const { return !isDeclaration(); }

  /// Returns true if this witness table is going to be (or was) serialized.
  bool isSerialized() const {
    return SerializedKind_t(SerializedKind) == IsSerialized;
  }

  bool isAnySerialized() const {
    return SerializedKind_t(SerializedKind) == IsSerialized ||
           SerializedKind_t(SerializedKind) == IsSerializedForPackage;
  }

  SerializedKind_t getSerializedKind() const {
    return SerializedKind_t(SerializedKind);
  }
  /// Sets the serialized flag.
  void setSerializedKind(SerializedKind_t serializedKind) {
    SerializedKind = serializedKind;
  }

  /// Return all of the witness table entries.
  ArrayRef<Entry> getEntries() const { return Entries; }

  /// Return all of the conditional conformances.
  ArrayRef<ConditionalConformance> getConditionalConformances() const {
    return ConditionalConformances;
  }

  /// Clears methods in MethodWitness entries.
  /// \p predicate Returns true if the passed entry should be set to null.
  template <typename Predicate> void clearMethods_if(Predicate predicate) {
    for (Entry &entry : Entries) {
      if (entry.getKind() == WitnessKind::Method) {
        const MethodWitness &MW = entry.getMethodWitness();
        if (MW.Witness && predicate(MW)) {
          entry.removeWitnessMethod();
        }
      }
    }
  }
  
  /// Verify that the witness table is well-formed.
  void verify(const SILModule &M) const;
  
  /// Get the linkage of the witness table.
  SILLinkage getLinkage() const { return Linkage; }

  /// Set the linkage of the witness table.
  void setLinkage(SILLinkage l) { Linkage = l; }

  /// Change a SILWitnessTable declaration into a SILWitnessTable definition.
  void
  convertToDefinition(ArrayRef<Entry> newEntries,
                      ArrayRef<ConditionalConformance> conditionalConformances,
                      SerializedKind_t serializedKind);

  // Gets conformance serialized kind.
  static SerializedKind_t
  conformanceSerializedKind(const RootProtocolConformance *conformance);

  /// Call \c fn on each (split apart) conditional requirement of \c conformance
  /// that should appear in a witness table, i.e., conformance requirements that
  /// need witness tables themselves.
  ///
  /// The \c unsigned argument to \c fn is a counter for the conditional
  /// conformances, and should be used for indexing arrays of them.
  ///
  /// This acts like \c any_of: \c fn returning \c true will stop the
  /// enumeration and \c enumerateWitnessTableConditionalConformances will
  /// return \c true, while \c fn returning \c false will let it continue.
  static bool enumerateWitnessTableConditionalConformances(
      const ProtocolConformance *conformance,
      llvm::function_ref<bool(unsigned, CanType, ProtocolDecl *)> fn);

  /// Print the witness table.
  void print(llvm::raw_ostream &OS, bool Verbose = false) const;

  /// Dump the witness table to stderr.
  void dump() const;
};

} // end swift namespace

//===----------------------------------------------------------------------===//
// ilist_traits for SILWitnessTable
//===----------------------------------------------------------------------===//

namespace llvm {
  
template <>
struct ilist_traits<::swift::SILWitnessTable> :
public ilist_node_traits<::swift::SILWitnessTable> {
  using SILWitnessTable = ::swift::SILWitnessTable;

public:
  static void deleteNode(SILWitnessTable *WT) { WT->~SILWitnessTable(); }
  
private:
  void createNode(const SILWitnessTable &);
};

} // end llvm namespace

#endif
