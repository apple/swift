//===--- SILVTable.h - Defines the SILVTable class --------------*- C++ -*-===//
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
// This file defines the SILVTable class, which is used to map dynamically
// dispatchable class methods and properties to their concrete implementations
// for a dynamic type. This information is used by IRGen to emit class vtables,
// by the devirtualization pass to promote class_method instructions to static
// function_refs.
//
// Note that vtable layout itself is implemented in SILVTableLayout.h and is
// independent of the SILVTable; in general, for a class from another module we
// might not have a SILVTable to deserialize, and for a class in a different
// translation in the same module the SILVTable is not available either.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_SILVTABLE_H
#define SWIFT_SIL_SILVTABLE_H

#include "swift/SIL/SILAllocated.h"
#include "swift/SIL/SILDeclRef.h"
#include "swift/SIL/SILFunction.h"
#include "llvm/ADT/Optional.h"
#include <algorithm>

namespace swift {

class ClassDecl;
enum IsSerialized_t : unsigned char;
class SILFunction;
class SILModule;

// TODO: Entry should include substitutions needed to invoke an overridden
// generic base class method.
class SILVTableEntry {
  /// The declaration reference to the least-derived method visible through
  /// the class.
  SILDeclRef Method;

  /// The function which implements the method for the class and the entry kind.
  llvm::PointerIntPair<SILFunction *, 2, unsigned> ImplAndKind;

  bool IsNonOverridden;

public:
  enum Kind : uint8_t {
    /// The vtable entry is for a method defined directly in this class.
    Normal,
    /// The vtable entry is inherited from the superclass.
    Inherited,
    /// The vtable entry is inherited from the superclass, and overridden
    /// in this class.
    Override,

    // Please update the PointerIntPair above if you add/remove enums.
  };

  SILVTableEntry() : ImplAndKind(nullptr, Kind::Normal),
                     IsNonOverridden(false) {}

  SILVTableEntry(SILDeclRef Method, SILFunction *Implementation, Kind TheKind,
                 bool NonOverridden)
      : Method(Method), ImplAndKind(Implementation, TheKind),
        IsNonOverridden(NonOverridden) {}

  SILDeclRef getMethod() const { return Method; }

  Kind getKind() const { return Kind(ImplAndKind.getInt()); }
  void setKind(Kind kind) { ImplAndKind.setInt(kind); }

  bool isNonOverridden() const { return IsNonOverridden; }
  void setNonOverridden(bool value) { IsNonOverridden = value; }

  SILFunction *getImplementation() const { return ImplAndKind.getPointer(); }
  
  void print(llvm::raw_ostream &os) const;
  
  bool operator==(const SILVTableEntry &e) const {
    return Method == e.Method
      && getImplementation() == e.getImplementation()
      && getKind() == e.getKind()
      && isNonOverridden() == e.isNonOverridden();
  }
  
  bool operator!=(const SILVTableEntry &e) const {
    return !(*this == e);
  }
};

/// A mapping from each dynamically-dispatchable method of a class to the
/// SILFunction that implements the method for that class.
/// Note that dead methods are completely removed from the vtable.
class SILVTable final : public SILAllocated<SILVTable>,
                        llvm::TrailingObjects<SILVTable, SILVTableEntry> {
  friend TrailingObjects;

public:
  using Entry = SILVTableEntry;

  // Disallow copying into temporary objects.
  SILVTable(const SILVTable &other) = delete;
  SILVTable &operator=(const SILVTable &) = delete;

private:
  /// The ClassDecl mapped to this VTable.
  ClassDecl *Class;

  /// Whether or not this vtable is serialized, which allows
  /// devirtualization from another module.
  bool Serialized : 1;

  /// The number of SILVTables entries.
  unsigned NumEntries : 31;

  /// Private constructor. Create SILVTables by calling SILVTable::create.
  SILVTable(ClassDecl *c, IsSerialized_t serialized, ArrayRef<Entry> entries);

public:
  ~SILVTable();

  /// Create a new SILVTable with the given method-to-implementation mapping.
  /// The SILDeclRef keys should reference the most-overridden members available
  /// through the class.
  static SILVTable *create(SILModule &M, ClassDecl *Class,
                           IsSerialized_t Serialized,
                           ArrayRef<Entry> Entries);

  /// Return the class that the vtable represents.
  ClassDecl *getClass() const { return Class; }

  /// Returns true if this vtable is going to be (or was) serialized.
  IsSerialized_t isSerialized() const {
    return Serialized ? IsSerialized : IsNotSerialized;
  }

  /// Sets the serialized flag.
  void setSerialized(IsSerialized_t serialized) {
    Serialized = (serialized ? 1 : 0);
  }

  /// Return all of the method entries.
  ArrayRef<Entry> getEntries() const {
    return {getTrailingObjects<SILVTableEntry>(), NumEntries};
  }

  /// Return all of the method entries mutably.
  /// If you do modify entries, make sure to invoke `updateVTableCache` to update the
  /// SILModule's cache entry.
  MutableArrayRef<Entry> getMutableEntries() {
    return {getTrailingObjects<SILVTableEntry>(), NumEntries};
  }
                          
  void updateVTableCache(const Entry &entry);

  /// Look up the implementation function for the given method.
  llvm::Optional<Entry> getEntry(SILModule &M, SILDeclRef method) const;

  /// Removes entries from the vtable.
  /// \p predicate Returns true if the passed entry should be removed.
  template <typename Predicate> void removeEntries_if(Predicate predicate) {
    auto Entries = getMutableEntries();
    Entry *end = std::remove_if(
        Entries.begin(), Entries.end(), [&](Entry &entry) -> bool {
          if (predicate(entry)) {
            entry.getImplementation()->decrementRefCount();
            removeFromVTableCache(entry);
            return true;
          }
          return false;
        });
    NumEntries = std::distance(Entries.begin(), end);
  }

  /// Verify that the vtable is well-formed for the given class.
  void verify(const SILModule &M) const;

  /// Print the vtable.
  void print(llvm::raw_ostream &OS, bool Verbose = false) const;
  void dump() const;

private:
  void removeFromVTableCache(Entry &entry);
};

} // end swift namespace

//===----------------------------------------------------------------------===//
// ilist_traits for SILVTable
//===----------------------------------------------------------------------===//

namespace llvm {

template <>
struct ilist_traits<::swift::SILVTable> :
public ilist_node_traits<::swift::SILVTable> {
  using SILVTable = ::swift::SILVTable;

  static void deleteNode(SILVTable *VT) { VT->~SILVTable(); }

private:
  void createNode(const SILVTable &);
};

} // end llvm namespace

#endif
