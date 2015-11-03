//===------------------------ MemLocation.h ----------------------------- -===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines the class Location. A MemLocation is an abstraction of an
// object field in program. It consists of a base that is the tracked SILValue
// and a projection path to the represented field.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_MEM_LOCATION_H
#define SWIFT_MEM_LOCATION_H

#include "swift/SILAnalysis/AliasAnalysis.h"
#include "swift/SIL/Projection.h"
#include "swift/SILPasses/Utils/Local.h"
#include "swift/SILAnalysis/ValueTracking.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Debug.h"

namespace swift {

//===----------------------------------------------------------------------===//
//                            Load Store Value
//===----------------------------------------------------------------------===//

class MemLocation;
class LoadStoreValue;
using LoadStoreValueList = llvm::SmallVector<LoadStoreValue, 8>;
using MemLocationValueMap = llvm::DenseMap<MemLocation, LoadStoreValue>; 

/// This class represents either a single SILValue or a covering of values that
/// we can forward from via the introdution of a SILArgument. This enables us
/// to treat the case of having one value or multiple values and load and store
/// cases all at once abstractly and cleanly.
///
/// A LoadStoreValue is an abstraction of an object field value in program. It
/// consists of a base that is the tracked SILValue, and a projection path to
/// the represented field.
///
/// In this example below, 2 LoadStoreValues will be created for the 2 stores,
/// they will have %6 and %7 as their Bases and empty projection paths.
///
///  struct A {
///    var a: Int
///    var b: Int
///  }
///
/// sil hidden @test_1 : $@convention(thin) () -> () {
///   %0 = alloc_stack $A  // var x                   // users: %4, %7
///   %5 = integer_literal $Builtin.Int64, 19         // user: %6
///   %6 = struct $Int (%5 : $Builtin.Int64)          // user: %8
///   %7 = struct_element_addr %0#1 : $*A, #A.a       // user: %8
///   store %6 to %7 : $*Int                          // id: %8
///   %9 = integer_literal $Builtin.Int64, 20         // user: %10
///   %10 = struct $Int (%9 : $Builtin.Int64)         // user: %12
///   %11 = struct_element_addr %0#1 : $*A, #A.b      // user: %12
///   store %10 to %11 : $*Int                        // id: %12
/// }
///
/// In this example below, 2 LoadStoreValues will be created with %3 as their
/// bases and #a and #b as their projection paths respectively.
///
/// sil hidden @test_1 : $@convention(thin) () -> () {
///   %0 = alloc_stack $A  // var x                   // users: %4, %6
///   // function_ref a.A.init (a.A.Type)() -> a.A
///   %1 = function_ref @a.A.init : $@convention(thin) (@thin A.Type) -> A
///   %2 = metatype $@thin A.Type                     // user: %3
///   %3 = apply %1(%2) : $@convention(thin) (@thin A.Type) -> A // user: %4
///   store %3 to %0#1 : $*A                          // id: %4
/// }
///
///
/// NOTE: LoadStoreValue can take 2 forms.
///
/// 1. It can take a concrete value, i.e. with a valid Base and ProjectionPath.
/// using the extract function, it can be materialized in IR.
///
/// 2. It can represent a covering set of LoadStoreValues from all predecessor
/// blocks. and to get the forwardable SILValue, we need to go to its
/// predecessors to materialize each one of them and create the forwarding
/// SILValue through a SILArgument.
///
/// Given a set of MemLocations and their available LoadStoreValues,
/// reduceWithValues will create the forwarding SILValue by merging them while
/// creating as few value extraction and aggregation as possible.
///
class LoadStoreValue {
  /// The base of the memory value.
  SILValue Base;
  /// The path to reach the accessed field of the object.
  Optional<ProjectionPath> Path;
  /// If this is a covering value, we need to go to each predecessor to
  /// materialize the value.
  bool IsCoveringValue;

  /// Create a path of ValueProjection with the given VA and Path.
  SILValue createExtract(SILValue VA, Optional<ProjectionPath> &Path,
                         SILInstruction *Inst);
public:
  /// Constructors.
  LoadStoreValue() : Base(), IsCoveringValue(false) {}
  LoadStoreValue(SILValue B) : Base(B), IsCoveringValue(false) {}
  LoadStoreValue(SILValue B, ProjectionPath &P)
      : Base(B), Path(std::move(P)), IsCoveringValue(false) {}

  SILValue getBase() const { return Base; }
  Optional<ProjectionPath> &getPath() { return Path; }

  /// Copy constructor.
  LoadStoreValue(const LoadStoreValue &RHS) {
    Base = RHS.Base;
    IsCoveringValue = RHS.IsCoveringValue;
    Path.reset();
    if (!RHS.Path.hasValue())
      return;
    ProjectionPath X;
    X.append(RHS.Path.getValue());
    Path = std::move(X);
  }

  LoadStoreValue &operator=(const LoadStoreValue &RHS) {
    Base = RHS.Base;
    IsCoveringValue = RHS.IsCoveringValue;
    Path.reset();
    if (!RHS.Path.hasValue())
      return *this;
    ProjectionPath X;
    X.append(RHS.Path.getValue());
    Path = std::move(X);
    return *this;
  }

  /// Returns whether the LoadStoreValue has been initialized properly.
  bool isValid() const {
    if (IsCoveringValue)
      return true;
    return Base && Path.hasValue();
  }

  /// Returns true if the LoadStoreValue has a non-empty projection path.
  bool hasEmptyProjectionPath() const { return !Path.getValue().size(); }

  /// Take the last level projection off. Return the resulting LoadStoreValue.
  LoadStoreValue &stripLastLevelProjection();
 
  bool isCoveringValue() const { return IsCoveringValue; }
  /// Mark this LoadStoreValue as a covering value.
  void setCoveringValue(); 
 
  /// Print the base and the path of the LoadStoreValue.
  void print();

  /// Materialize the SILValue that this LoadStoreValue represents in IR.
  ///
  /// In the case where we have a single value this can be materialized by
  /// applying Path to the Base.
  ///
  /// In the case where we are handling a covering set, this is initially null
  /// and when we insert the PHI node, this is set to the SILArgument which
  /// represents the PHI node.
  SILValue materialize(SILInstruction *Inst) {
    //
    // TODO: handle covering value.
    //
    if (IsCoveringValue)
      return SILValue();
    return createExtract(Base, Path, Inst);
  }

  ///============================/// 
  ///       static functions.    ///
  ///============================/// 

  static LoadStoreValue createLoadStoreValue(SILValue Base) {
    ProjectionPath P;
    return LoadStoreValue(Base, P);
  }

  static LoadStoreValue createLoadStoreValue(SILValue Base, ProjectionPath &P) {
    return LoadStoreValue(Base, P);
  }
};


//===----------------------------------------------------------------------===//
//                              Memory Location
//===----------------------------------------------------------------------===//
/// Forward declaration.
class MemLocation;

/// Type declarations.
using MemLocationSet = llvm::DenseSet<MemLocation>;
using MemLocationList = llvm::SmallVector<MemLocation, 8>;
using MemLocationIndexMap = llvm::DenseMap<MemLocation, unsigned>;
using TypeExpansionMap = llvm::DenseMap<SILType, ProjectionPathList>;

class MemLocation {
public:
  enum KeyKind : uint8_t { EmptyKey = 0, TombstoneKey, NormalKey };

private:
  /// The base of the object.
  SILValue Base;
  /// Empty key, tombstone key or normal key.
  KeyKind Kind;
  /// The path to reach the accessed field of the object.
  Optional<ProjectionPath> Path;

public:
  /// Constructors.
  MemLocation() : Base(), Kind(NormalKey) {}
  MemLocation(SILValue B) : Base(B), Kind(NormalKey) { initialize(B); }
  MemLocation(SILValue B, ProjectionPath &P, KeyKind Kind = NormalKey)
      : Base(B), Kind(Kind), Path(std::move(P)) {}

  /// Copy constructor.
  MemLocation(const MemLocation &RHS) {
    Base = RHS.Base;
    Path.reset();
    Kind = RHS.Kind;
    if (!RHS.Path.hasValue())
      return;
    ProjectionPath X;
    X.append(RHS.Path.getValue());
    Path = std::move(X);
  }

  MemLocation &operator=(const MemLocation &RHS) {
    Base = RHS.Base;
    Path.reset();
    Kind = RHS.Kind;
    if (!RHS.Path.hasValue())
      return *this;
    ProjectionPath X;
    X.append(RHS.Path.getValue());
    Path = std::move(X);
    return *this;
  }

  /// Getters and setters for MemLocation.
  KeyKind getKind() const { return Kind; }
  void setKind(KeyKind K) { Kind = K; }
  SILValue getBase() const { return Base; }
  Optional<ProjectionPath> &getPath() { return Path; }

  /// Returns the hashcode for the location.
  llvm::hash_code getHashCode() const {
    llvm::hash_code HC = llvm::hash_combine(Base.getDef(),
                                            Base.getResultNumber(),
                                            Base.getType());
    if (!Path.hasValue())
      return HC;
    HC = llvm::hash_combine(HC, hash_value(Path.getValue()));
    return HC;
  }

  /// Returns the type of the object the MemLocation represents.
  SILType getType() const {
    // Base might be a address type, e.g. from alloc_stack of struct,
    // enum or tuples.
    if (Path.getValue().empty())
      return Base.getType().getObjectType();
    return Path.getValue().front().getType().getObjectType();
  }

  /// Returns whether the memory location has been initialized properly.
  bool isValid() const {
    return Base && Path.hasValue();
  }

  void subtractPaths(Optional<ProjectionPath> &P) {
    if (!P.hasValue())
      return;
    ProjectionPath::subtractPaths(Path.getValue(), P.getValue());
  }

  /// Return false if one projection path is a prefix of another. false
  /// otherwise.
  bool hasNonEmptySymmetricPathDifference(const MemLocation &RHS) const {
    const ProjectionPath &P = RHS.Path.getValue();
    return Path.getValue().hasNonEmptySymmetricDifference(P);
  }

  /// Return true if the 2 locations have identical projection paths.
  /// If both locations have empty paths, they are treated as having
  /// identical projection path.
  bool hasIdenticalProjectionPath(const MemLocation &RHS) const;

  /// Comparisons.
  bool operator!=(const MemLocation &RHS) const { return !(*this == RHS); }
  bool operator==(const MemLocation &RHS) const {
    // If type is not the same, then locations different.
    if (Kind != RHS.Kind)
      return false;
    // If Base is different, then locations different.
    if (Base != RHS.Base)
      return false;

    // If the projection paths are different, then locations are different.
    if (!hasIdenticalProjectionPath(RHS))
      return false;

    // These locations represent the same memory location.
    return true;
  }

  /// Trace the given SILValue till the base of the accessed object. Also
  /// construct the projection path to the field accessed.
  void initialize(SILValue val);

  /// Reset the memory location, i.e. clear base and path. 
  void reset() {
    Base = SILValue();
    Path.reset();
    Kind = NormalKey;
  }

  /// Get the first level locations based on this location's first level
  /// projection.
  void getFirstLevelMemLocations(MemLocationList &Locs, SILModule *Mod);

  /// Check whether the 2 MemLocations may alias each other or not.
  bool isMayAliasMemLocation(const MemLocation &RHS, AliasAnalysis *AA);

  /// Check whether the 2 MemLocations must alias each other or not.
  bool isMustAliasMemLocation(const MemLocation &RHS, AliasAnalysis *AA);

  /// Print MemLocation.
  void print() const;

  ///============================/// 
  ///       static functions.    ///
  ///============================/// 

  /// Given Base and 2 ProjectionPaths, create a MemLocation out of them.
  static MemLocation createMemLocation(SILValue Base, ProjectionPath &P1,
                                       ProjectionPath &P2);

  /// Expand this location to all individual fields it contains.
  ///
  /// In SIL, we can have a store to an aggregate and loads from its individual
  /// fields. Therefore, we expand all the operations on aggregates onto
  /// individual fields and process them separately.
  static void expand(MemLocation &Base, SILModule *Mod, MemLocationList &Locs,
                     TypeExpansionMap &TypeExpansionVault);

  /// Given a set of locations derived from the same base, try to merge/reduce
  /// them into smallest number of MemLocations possible.
  static void reduce(MemLocation &Base, SILModule *Mod, MemLocationSet &Locs);

  /// Given a memory location and a SILValue, expand the location into its
  /// individual fields and the values that is in each individual field.
  static void expandWithValues(MemLocation &Base, SILValue &Val, SILModule *Mod,
                               MemLocationList &Locs, LoadStoreValueList &Vals);

  /// Given a memory location and a map between the expansions of the location
  /// and their corresponding values, try to come up with a single SILValue this
  /// location holds. This may involve extracting and aggregating available values.
  ///
  /// NOTE: reduceValues assumes that every component of the location has an
  /// concrete (i.e. not coverings set) available value in LocAndVal.
  static SILValue reduceWithValues(MemLocation &Base, SILModule *Mod,
                                   MemLocationValueMap &LocAndVal,
                                   SILInstruction *InsertPt);

  /// Enumerate the given Mem MemLocation.
  static void enumerateMemLocation(SILModule *M, SILValue Mem,
                                   std::vector<MemLocation> &MemLocationVault,
                                   MemLocationIndexMap &LocToBit,
                                   TypeExpansionMap &TypeExpansionVault);

  /// Enumerate all the locations in the function.
  static void enumerateMemLocations(SILFunction &F,
                                    std::vector<MemLocation> &MemLocationVault,
                                    MemLocationIndexMap &LocToBit,
                                    TypeExpansionMap &TypeExpansionVault);
};

static inline llvm::hash_code hash_value(const MemLocation &L) {
  return llvm::hash_combine(L.getBase().getDef(), L.getBase().getResultNumber(),
                            L.getBase().getType());
}

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, MemLocation V) {
  V.getBase().print(OS);
  OS << V.getPath().getValue();
  return OS;
}

} // end swift namespace


/// MemLocation is used in DenseMap, define functions required by DenseMap.
namespace llvm {

using swift::MemLocation;

template <> struct DenseMapInfo<MemLocation> {
  static inline MemLocation getEmptyKey() {
    MemLocation L;
    L.setKind(MemLocation::EmptyKey);
    return L;
  }
  static inline MemLocation getTombstoneKey() {
    MemLocation L;
    L.setKind(MemLocation::TombstoneKey);
    return L;
  }
  static unsigned getHashValue(const MemLocation &Loc) {
    return hash_value(Loc);
  }
  static bool isEqual(const MemLocation &LHS, const MemLocation &RHS) {
    if (LHS.getKind() == MemLocation::EmptyKey &&
        RHS.getKind() == MemLocation::EmptyKey)
      return true;
    if (LHS.getKind() == MemLocation::TombstoneKey &&
        RHS.getKind() == MemLocation::TombstoneKey)
      return true;
    return LHS == RHS;
  }
};

} // namespace llvm

#endif  // SWIFT_SIL_MEMLOCATION_H
