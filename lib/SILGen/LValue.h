//===--- LValue.h - Logical LValue Representation ---------------*- C++ -*-===//
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
// A storage structure for keeping track of logical lvalues during SILGen.
//
// In general, only the routines in SILGenLValue.cpp should actually be
// accessing LValues and their components.  Everything else should just
// pass them around opaquely.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_LOWERING_LVALUE_H
#define SWIFT_LOWERING_LVALUE_H

#include "FormalEvaluation.h"
#include "SILGenFunction.h"
#include "Scope.h"

namespace swift {
namespace Lowering {

class ArgumentSource;
class LogicalPathComponent;
class ManagedValue;
class PhysicalPathComponent;
class SILGenFunction;
class TranslationPathComponent;

/// Information about the type of an l-value.
struct LValueTypeData {
  /// The abstraction pattern of the l-value.
  ///
  /// The type-of-rvalue should always be the substituted formal type
  /// lowered under this abstraction pattern.
  AbstractionPattern OrigFormalType = AbstractionPattern::getInvalid();

  /// The substituted formal object type of the l-value.
  ///
  /// Tn the most common case, this is the type of an l-value
  /// expression as recorded in the AST, only with the
  /// LValueType/InOutType stripped off.
  CanType SubstFormalType;

  /// The lowered type of value that should be stored in the l-value.
  ///
  /// On physical path components, projection yields an address of
  /// this type.  On logical path components, materialize yields an
  /// address of this type, set expects a value of this type, and
  /// get yields an object of this type.
  CanType TypeOfRValue;

  SGFAccessKind AccessKind;

  LValueTypeData() = default;
  LValueTypeData(SGFAccessKind accessKind, AbstractionPattern origFormalType,
                 CanType substFormalType, CanType typeOfRValue)
    : OrigFormalType(origFormalType), SubstFormalType(substFormalType),
      TypeOfRValue(typeOfRValue), AccessKind(accessKind) {
    assert(substFormalType->isMaterializable());
  }

  SGFAccessKind getAccessKind() const { return AccessKind; }
};

/// An l-value path component represents a chunk of the access path to
/// an object.  Path components may be either "physical" or "logical".
/// A physical path involves elementary address manipulations; these
/// address manipulations may be in some way dynamic, but they are
/// ultimately just pointer arithmetic.  A logical path requires
/// getter/setter logic.
///
/// This divide between physical/logical is closely related to the
/// fragile/resilient split, with two primary differences:
///   - Any sort of implementation can be fragile.  For example, a
///     computed variable can still be fragile, meaning that it is known
///     to be implemented with a getter/setter.  The known
///     implementation must be a direct offset in order to qualify as
///     physical.
///   - A path component's implementation can be resilient and yet
///     still qualify for physical access if we are in a privileged
///     component.
class PathComponent {
  LValueTypeData TypeData;

  friend class LValue;
  unsigned AllocatedSize;
public:
  enum KindTy {
    // Physical lvalue kinds
    RefElementKind,             // ref_element_addr
    TupleElementKind,           // tuple_element_addr
    StructElementKind,          // struct_element_addr
    OptionalObjectKind,         // optional projection
    OpenOpaqueExistentialKind,  // opened opaque existential
    AddressorKind,              // var/subscript addressor
    CoroutineAccessorKind,      // coroutine accessor
    ValueKind,                  // random base pointer as an lvalue
    PhysicalKeyPathApplicationKind, // applying a key path
    BorrowValueKind,            // load_borrow the base rvalue for a projection

    // Logical LValue kinds
    GetterSetterKind,           // property or subscript getter/setter
    MaterializeToTemporaryKind,
    OwnershipKind,              // weak pointer remapping
    AutoreleasingWritebackKind, // autorelease pointer on set
    WritebackPseudoKind,        // a fake component to customize writeback
    OpenNonOpaqueExistentialKind,  // opened class or metatype existential
    LogicalKeyPathApplicationKind, // applying a key path
    InitAccessorKind,           // init accessor
    
    // Translation LValue kinds (a subtype of logical)
    OrigToSubstKind,            // generic type substitution
    SubstToOrigKind,            // generic type substitution
    UncheckedConversionKind,    // unchecked_X_cast

    FirstLogicalKind = GetterSetterKind,
    FirstTranslationKind = OrigToSubstKind,
  };
private:
  const KindTy Kind : 8;

  // This anchor method serves three purposes: it aligns the class to
  // a pointer boundary, it makes the class a primary base so that
  // subclasses will be at offset zero, and it anchors the v-table
  // to a specific file.
  virtual void _anchor();

  PathComponent(const PathComponent &) = delete;
  PathComponent &operator=(const PathComponent &) = delete;

protected:
  PathComponent(LValueTypeData typeData, KindTy Kind)
    : TypeData(typeData), Kind(Kind) {}
public:
  virtual ~PathComponent() {}

  /// Returns sizeof(the final type), plus any extra storage required.
  size_t allocated_size() const { return AllocatedSize; }

  /// Is this component physical or logical?  If physical, this will
  /// be a subclass of PhysicalPathComponent.  If logical, this will
  /// be a subclass of LogicalPathComponent.
  bool isPhysical() const { return Kind < FirstLogicalKind; }
  bool isLogical() const { return Kind >= FirstLogicalKind; }
  bool isTranslation() const { return Kind >= FirstTranslationKind; }

  // These are implemented inline after the respective class declarations.

  PhysicalPathComponent &asPhysical();
  const PhysicalPathComponent &asPhysical() const;

  LogicalPathComponent &asLogical();
  const LogicalPathComponent &asLogical() const;

  TranslationPathComponent &asTranslation();
  const TranslationPathComponent &asTranslation() const;

  /// Apply this component as a projection to the given base component,
  /// producing something usable as the base of the next component.
  virtual ManagedValue project(SILGenFunction &SGF,
                               SILLocation loc,
                               ManagedValue base) && = 0;

  /// Is this some form of open-existential component?
  bool isOpenExistential() const {
    return getKind() == OpenOpaqueExistentialKind ||
           getKind() == OpenNonOpaqueExistentialKind;
  }

  /// Is loading a value from this component guaranteed to have no observable
  /// side effects?
  virtual bool isLoadingPure() const {
    // By default, don't assume any component is pure; components must opt-in.
    return false;
  }

  virtual bool isRValue() const { return false; }

  /// Returns the logical type-as-rvalue of the value addressed by the
  /// component.  This is always an object type, never an address.
  SILType getTypeOfRValue() const {
    return SILType::getPrimitiveObjectType(TypeData.TypeOfRValue);
  }
  AbstractionPattern getOrigFormalType() const {
    return TypeData.OrigFormalType;
  }
  CanType getSubstFormalType() const { return TypeData.SubstFormalType; }

  const LValueTypeData &getTypeData() const { return TypeData; }
  SGFAccessKind getAccessKind() const { return getTypeData().getAccessKind(); }

  KindTy getKind() const { return Kind; }

  void dump() const;
  virtual void dump(raw_ostream &OS, unsigned indent = 0) const = 0;
};

/// An abstract class for "physical" path components, i.e. path
/// components that can be accessed as address manipulations.  See the
/// comment for PathComponent for more information.
///
/// The only operation on this component is `project`.
class PhysicalPathComponent : public PathComponent {
  virtual void _anchor() override;
  llvm::Optional<ActorIsolation> ActorIso;

protected:
  PhysicalPathComponent(LValueTypeData typeData, KindTy Kind,
                        llvm::Optional<ActorIsolation> actorIso = llvm::None)
      : PathComponent(typeData, Kind), ActorIso(actorIso) {
    assert(isPhysical() && "PhysicalPathComponent Kind isn't physical");
  }

public:
  /// Obtains and consumes the actor-isolation required for any loads of
  /// this component.
  llvm::Optional<ActorIsolation> takeActorIsolation() {
    llvm::Optional<ActorIsolation> current = ActorIso;
    ActorIso = llvm::None;
    return current;
  }

  void set(SILGenFunction &SGF, SILLocation loc,
           ArgumentSource &&value, ManagedValue base) &&;

  /// Determines whether this component has any actor-isolation.
  bool hasActorIsolation() const { return ActorIso.has_value(); }
};

inline PhysicalPathComponent &PathComponent::asPhysical() {
  assert(isPhysical());
  return static_cast<PhysicalPathComponent&>(*this);
}
inline const PhysicalPathComponent &PathComponent::asPhysical() const {
  assert(isPhysical());
  return static_cast<const PhysicalPathComponent&>(*this);
}

/// An abstract class for "logical" path components, i.e. path
/// components that require getter/setter methods to access.  See the
/// comment for PathComponent for more information.
class LogicalPathComponent : public PathComponent {
protected:
  LogicalPathComponent(LValueTypeData typeData, KindTy Kind)
    : PathComponent(typeData, Kind) {
    assert(isLogical() && "LogicalPathComponent Kind isn't logical");
  }

  /// Read the value of this component, producing the right kind of result
  /// for the given access kind (which is always some kind of read access).
  ManagedValue projectForRead(SILGenFunction &SGF, SILLocation loc,
                              ManagedValue base, SGFAccessKind kind) &&;

public:
  /// Clone the path component onto the heap.
  virtual std::unique_ptr<LogicalPathComponent>
  clone(SILGenFunction &SGF, SILLocation l) const = 0;
  
  /// Set the property.
  ///
  /// \param base - always an address, but possibly an r-value
  virtual void set(SILGenFunction &SGF, SILLocation loc,
                   ArgumentSource &&value, ManagedValue base) && = 0;

  /// Get the property.
  ///
  /// \param base - always an address, but possibly an r-value
  virtual RValue get(SILGenFunction &SGF, SILLocation loc,
                     ManagedValue base, SGFContext c) && = 0;

  /// The default implementation of project performs a get or materializes
  /// to a temporary as necessary.
  ManagedValue project(SILGenFunction &SGF, SILLocation loc,
                       ManagedValue base) && override;

  struct AccessStorage {
    AbstractStorageDecl *Storage;
    bool IsSuper;
    const PreparedArguments *Indices;
    ArgumentList *ArgListForDiagnostics;
  };

  /// Get the storage accessed by this component.
  virtual llvm::Optional<AccessStorage> getAccessStorage() const = 0;

  /// Perform a writeback on the property.
  ///
  /// \param base - always an address, but possibly an r-value
  virtual void writeback(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue base,
                         MaterializedLValue materialized,
                         bool isFinal);
};

inline LogicalPathComponent &PathComponent::asLogical() {
  assert(isLogical());
  return static_cast<LogicalPathComponent&>(*this);
}
inline const LogicalPathComponent &PathComponent::asLogical() const {
  assert(isLogical());
  return static_cast<const LogicalPathComponent&>(*this);
}

/// An abstract class for components which translate values in some way.
class TranslationPathComponent : public LogicalPathComponent {
protected:
  TranslationPathComponent(LValueTypeData typeData, KindTy kind)
      : LogicalPathComponent(typeData, kind) {
    assert(isTranslation() &&
           "TranslationPathComponent kind isn't value translation");
  }

public:
  llvm::Optional<AccessStorage> getAccessStorage() const override {
    return llvm::None;
  }

  RValue get(SILGenFunction &SGF, SILLocation loc,
             ManagedValue base, SGFContext c) && override;

  void set(SILGenFunction &SGF, SILLocation loc,
           ArgumentSource &&value, ManagedValue base) && override;

  /// Transform from the original pattern.
  virtual RValue translate(SILGenFunction &SGF, SILLocation loc,
                           RValue &&value,
                           SGFContext ctx = SGFContext()) && = 0;

  /// Transform into the original pattern.
  virtual RValue untranslate(SILGenFunction &SGF, SILLocation loc,
                             RValue &&value,
                             SGFContext ctx = SGFContext()) && = 0;
  
};

inline TranslationPathComponent &PathComponent::asTranslation() {
  assert(isTranslation());
  return static_cast<TranslationPathComponent&>(*this);
}
inline const TranslationPathComponent &PathComponent::asTranslation() const {
  assert(isTranslation());
  return static_cast<const TranslationPathComponent&>(*this);
}

/// An lvalue represents a reference to storage holding a value
/// of a type, as opposed to an rvalue, which is an actual value
/// of the type.
class LValue {
  std::vector<std::unique_ptr<PathComponent>> Path;

public:
  LValue() = default;
  LValue(const LValue &other) = delete;
  LValue(LValue &&other) = default;

  LValue &operator=(const LValue &) = delete;
  LValue &operator=(LValue &&) = default;

  static LValue forValue(SGFAccessKind accessKind, ManagedValue value,
                         CanType substFormalType);

  static LValue forAddress(SGFAccessKind accessKind, ManagedValue address,
                           llvm::Optional<SILAccessEnforcement> enforcement,
                           AbstractionPattern origFormalType,
                           CanType substFormalType);

  bool isValid() const { return !Path.empty(); }

  /// Is loading a value from this lvalue guaranteed to have no observable side
  /// effects?
  bool isLoadingPure() {
    assert(isValid());
    for (auto &component : Path)
      if (!component->isLoadingPure())
        return false;
    return true;
  }

  /// Is this lvalue purely physical?
  bool isPhysical() const {
    assert(isValid());
    for (auto &component : Path)
      if (!component->isPhysical())
        return false;
    return true;
  }
  
  /// Is the lvalue's final component physical?
  bool isLastComponentPhysical() const {
    assert(isValid());
    return Path.back()->isPhysical();
  }

  /// Is the lvalue's final component a translation component?
  bool isLastComponentTranslation() const {
    assert(isValid());
    return Path.back()->isTranslation();
  }

  /// Given that the last component is a translation component,
  /// return it.
  TranslationPathComponent &getLastTranslationComponent() & {
    assert(isLastComponentTranslation());
    return Path.back()->asTranslation();
  }

  /// Given that the last component is a translation component,
  /// peel it off.
  void dropLastTranslationComponent() & {
    assert(isLastComponentTranslation());
    Path.pop_back();
  }

  /// Assert that the given component is the last component in the
  /// l-value, drop it.
  void dropLastComponent(PathComponent &component) & {
    assert(&component == Path.back().get());
    Path.pop_back();
  }

  /// Pop the last component off this LValue unsafely. Validates that the
  /// component is of kind \p kind as a sanity check.
  ///
  /// Please be careful when using this!
  void unsafelyDropLastComponent(PathComponent::KindTy kind) & {
    assert(kind == Path.back()->getKind());
    Path.pop_back();
  }

  /// Add a new component at the end of the access path of this lvalue.
  template <class T, class... As>
  void add(As &&... args) {
    Path.emplace_back(new T(std::forward<As>(args)...));
  }

  // NOTE: Optional<ActorIsolation> inside of LValues
  // Some path components carry an ActorIsolation value, which is an indicator
  // that the access to that component must be performed by switching to the
  // given actor's isolation domain. If the indicator is not present, that
  // only means that a switch does not need to be emitted during the access.

  void addNonMemberVarComponent(
      SILGenFunction &SGF, SILLocation loc, VarDecl *var, SubstitutionMap subs,
      LValueOptions options, SGFAccessKind accessKind, AccessStrategy strategy,
      CanType formalRValueType,
      llvm::Optional<ActorIsolation> actorIso = llvm::None);

  /// Add a member component to the access path of this lvalue.
  void addMemberComponent(SILGenFunction &SGF, SILLocation loc,
                          AbstractStorageDecl *storage,
                          SubstitutionMap subs,
                          LValueOptions options,
                          bool isSuper,
                          SGFAccessKind accessKind,
                          AccessStrategy accessStrategy,
                          CanType formalRValueType,
                          PreparedArguments &&indices,
                          ArgumentList *argListForDiagnostics);

  void
  addMemberVarComponent(SILGenFunction &SGF, SILLocation loc, VarDecl *var,
                        SubstitutionMap subs, LValueOptions options,
                        bool isSuper, SGFAccessKind accessKind,
                        AccessStrategy accessStrategy, CanType formalRValueType,
                        bool isOnSelf = false,
                        llvm::Optional<ActorIsolation> actorIso = llvm::None);

  void addMemberSubscriptComponent(
      SILGenFunction &SGF, SILLocation loc, SubscriptDecl *subscript,
      SubstitutionMap subs, LValueOptions options, bool isSuper,
      SGFAccessKind accessKind, AccessStrategy accessStrategy,
      CanType formalRValueType, PreparedArguments &&indices,
      ArgumentList *argListForDiagnostics, bool isOnSelfParameter = false,
      llvm::Optional<ActorIsolation> actorIso = llvm::None);

  /// Add a subst-to-orig reabstraction component.  That is, given
  /// that this l-value trafficks in values following the substituted
  /// abstraction pattern, make an l-value trafficking in values
  /// following the original abstraction pattern.
  void addSubstToOrigComponent(AbstractionPattern origType,
                               SILType loweredResultType);

  /// Add an orig-to-subst reabstraction component.  That is, given
  /// that this l-value trafficks in values following the original
  /// abstraction pattern, make an l-value trafficking in values
  /// following the substituted abstraction pattern.
  void addOrigToSubstComponent(SILType loweredResultType);

  typedef std::vector<std::unique_ptr<PathComponent>>::iterator iterator;
  typedef std::vector<std::unique_ptr<PathComponent>>::const_iterator
    const_iterator;

  iterator begin() { return Path.begin(); }
  iterator end() { return Path.end(); }
  const_iterator begin() const { return Path.begin(); }
  const_iterator end() const { return Path.end(); }

  const LValueTypeData &getTypeData() const {
    return Path.back()->getTypeData();
  }

  /// Return the access kind that this l-value was emitted for.
  SGFAccessKind getAccessKind() const { return getTypeData().getAccessKind(); }

  /// Returns the type-of-rvalue of the logical object referenced by
  /// this l-value.  Note that this may differ significantly from the
  /// type of l-value.
  SILType getTypeOfRValue() const {
    return SILType::getPrimitiveObjectType(getTypeData().TypeOfRValue);
  }
  CanType getSubstFormalType() const { return getTypeData().SubstFormalType; }
  AbstractionPattern getOrigFormalType() const {
    return getTypeData().OrigFormalType;
  }

  /// Returns true when the other access definitely does not begin a formal
  /// access that would conflict with this the accesses begun by this
  /// LValue. This is a best-effort attempt; it may return false in cases
  /// where the two LValues do not conflict.
  bool isObviouslyNonConflicting(const LValue &other,
                                 SGFAccessKind selfAccess,
                                 SGFAccessKind otherAccess);

  SWIFT_DEBUG_DUMP;
  void dump(raw_ostream &os, unsigned indent = 0) const;
};
  
/// RAII object used to enter an inout conversion scope. Writeback scopes formed
/// during the inout conversion scope will be no-ops.
class InOutConversionScope {
  SILGenFunction &SGF;
public:
  InOutConversionScope(SILGenFunction &SGF);
  ~InOutConversionScope();
};

// FIXME: Misnomer. This class is used for both shared (read) and exclusive
// (modify) formal borrows.
struct LLVM_LIBRARY_VISIBILITY ExclusiveBorrowFormalAccess : FormalAccess {
  std::unique_ptr<LogicalPathComponent> component;
  ManagedValue base;
  MaterializedLValue materialized;

  ~ExclusiveBorrowFormalAccess() {}
  ExclusiveBorrowFormalAccess(ExclusiveBorrowFormalAccess &&) = default;
  ExclusiveBorrowFormalAccess &
  operator=(ExclusiveBorrowFormalAccess &&) = default;

  ExclusiveBorrowFormalAccess(SILLocation loc,
                              std::unique_ptr<LogicalPathComponent> &&comp,
                              ManagedValue base,
                              MaterializedLValue materialized,
                              CleanupHandle cleanup)
      : FormalAccess(sizeof(*this), FormalAccess::Exclusive, loc, cleanup),
        component(std::move(comp)), base(base), materialized(materialized) {}

  void diagnoseConflict(const ExclusiveBorrowFormalAccess &rhs,
                        SILGenFunction &SGF) const;

  void performWriteback(SILGenFunction &SGF, bool isFinal) {
    Scope S(SGF.Cleanups, CleanupLocation(loc));
    component->writeback(SGF, loc, base, materialized, isFinal);
  }

  void finishImpl(SILGenFunction &SGF) override {
    performWriteback(SGF, /*isFinal*/ true);
    component.reset();
  }
};

struct LLVM_LIBRARY_VISIBILITY UnenforcedAccess {
  // Make sure someone called `endAccess` before destroying this.
  struct DeleterCheck {
    void operator()(BeginAccessInst *) {
      llvm_unreachable("access scope must be ended");
    }
  };
  typedef std::unique_ptr<BeginAccessInst, DeleterCheck> BeginAccessPtr;
  BeginAccessPtr beginAccessPtr;

  UnenforcedAccess() = default;
  UnenforcedAccess(const UnenforcedAccess &other) = delete;
  UnenforcedAccess(UnenforcedAccess &&other) = default;

  UnenforcedAccess &operator=(const UnenforcedAccess &) = delete;
  UnenforcedAccess &operator=(UnenforcedAccess &&other) = default;

  // Return the a new begin_access if it was required, otherwise return the
  // given `address`.
  SILValue beginAccess(SILGenFunction &SGF, SILLocation loc, SILValue address,
                       SILAccessKind kind);

  // End the access and release beginAccessPtr.
  void endAccess(SILGenFunction &SGF);

  // Emit the end_access (on a branch) without marking this access as ended.
  void emitEndAccess(SILGenFunction &SGF);
};

/// Pseudo-formal access that emits access markers but does not actually
/// require enforcement. It may be used for access to formal memory that is
/// exempt from exclusivity checking, such as initialization, or it may be used
/// for accesses to local memory that are indistinguishable from formal access
/// at the SIL level. Adding the access markers in these cases gives SIL address
/// users a structural property that allows for exhaustive verification.
struct LLVM_LIBRARY_VISIBILITY UnenforcedFormalAccess : FormalAccess {

  static SILValue enter(SILGenFunction &SGF, SILLocation loc, SILValue address,
                        SILAccessKind kind);

  // access.beginAccessPtr is either the begin_access or null if no access was
  // required.
  UnenforcedAccess access;

  UnenforcedFormalAccess(SILLocation loc, UnenforcedAccess &&access,
                         CleanupHandle cleanup)
      : FormalAccess(sizeof(*this), FormalAccess::Unenforced, loc, cleanup),
        access(std::move(access)) {}

  // Emit the end_access (on a branch) without marking this access as ended.
  void emitEndAccess(SILGenFunction &SGF);

  // Only called at the end formal evaluation scope. End this access.
  void finishImpl(SILGenFunction &SGF) override;
};

} // namespace Lowering
} // namespace swift

#endif
