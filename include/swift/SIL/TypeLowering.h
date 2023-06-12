//===--- TypeLowering.h - Convert Swift Types to SILTypes -------*- C++ -*-===//
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

#ifndef SWIFT_SIL_TYPELOWERING_H
#define SWIFT_SIL_TYPELOWERING_H

#include "swift/ABI/ProtocolDispatchStrategy.h"
#include "swift/AST/CaptureInfo.h"
#include "swift/AST/Module.h"
#include "swift/SIL/AbstractionPattern.h"
#include "swift/SIL/SILDeclRef.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILLocation.h"
#include "swift/SIL/SILValue.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Allocator.h"

namespace clang {
  class Type;
}

namespace swift {
  class AnyFunctionRef;
  enum class Bridgeability : unsigned;
  class ForeignErrorConvention;
  enum IsInitialization_t : bool;
  enum IsTake_t : bool;
  class ModuleDecl;
  class SILBuilder;
  class SILLocation;
  class SILModule;
  class ValueDecl;

namespace Lowering {

/// The default convention for handling the callee object on thick
/// callees.
const ParameterConvention DefaultThickCalleeConvention =
    ParameterConvention::Direct_Guaranteed;

/// Given an AST function type, return a type that is identical except
/// for using the given ExtInfo.
CanAnyFunctionType adjustFunctionType(CanAnyFunctionType type,
                                      AnyFunctionType::ExtInfo extInfo);

/// Change the given function type's representation.
inline CanAnyFunctionType
adjustFunctionType(CanAnyFunctionType t, AnyFunctionType::Representation rep,
                   ClangTypeInfo clangTypeInfo) {
  auto extInfo = t->getExtInfo()
                     .intoBuilder()
                     .withRepresentation(rep)
                     .withClangFunctionType(clangTypeInfo.getType())
                     .build();
  return adjustFunctionType(t, extInfo);
}

/// Given a SIL function type, return a type that is identical except
/// for using the given ExtInfo.
CanSILFunctionType
adjustFunctionType(CanSILFunctionType type, SILFunctionType::ExtInfo extInfo,
                   ParameterConvention calleeConv,
                   ProtocolConformanceRef witnessMethodConformance);
inline CanSILFunctionType
adjustFunctionType(CanSILFunctionType type, SILFunctionType::ExtInfo extInfo,
                   ProtocolConformanceRef witnessMethodConformance) {
  return adjustFunctionType(type, extInfo, type->getCalleeConvention(),
                            witnessMethodConformance);
}
inline CanSILFunctionType
adjustFunctionType(CanSILFunctionType t, SILFunctionType::Representation rep,
                   ProtocolConformanceRef witnessMethodConformance) {
  if (t->getRepresentation() == rep) return t;
  auto extInfo = t->getExtInfo().withRepresentation(rep);
  auto contextConvention = DefaultThickCalleeConvention;
  return adjustFunctionType(t, extInfo,
                            extInfo.hasContext()
                                ? contextConvention
                                : ParameterConvention::Direct_Unowned,
                            witnessMethodConformance);
}

/// Is a lowered SIL type trivial?  That is, are copies ultimately just
/// bit-copies, and it takes no work to destroy a value?
enum IsTrivial_t : bool {
  IsNotTrivial = false,
  IsTrivial = true
};

/// Is a lowered SIL type the Builtin.RawPointer or a struct/tuple/enum which
/// contains a Builtin.RawPointer?
/// HasRawPointer is true only for types that are known to contain
/// Builtin.RawPointer. It is not assumed true for generic or resilient types.
enum HasRawPointer_t : bool {
  DoesNotHaveRawPointer = false,
  HasRawPointer = true
};

/// Is a lowered SIL type fixed-ABI?  That is, can the current context
/// assign it a fixed size and alignment and perform value operations on it
/// (such as copies, destroys, constructions, and projections) without
/// metadata?
///
/// Note that a fully concrete type can be non-fixed-ABI without being
/// itself resilient if it contains a subobject which is not fixed-ABI.
///
/// Also note that we're only concerned with the external value ABI here:
/// resilient class types are still fixed-ABI, indirect enum cases do not
/// affect the fixed-ABI-ness of the enum, and so on.
enum IsFixedABI_t : bool {
  IsNotFixedABI = false,
  IsFixedABI = true
};

/// Is a lowered SIL type address-only?  That is, is the current context
/// required to keep the value in memory for some reason?
///
/// A type might be address-only because:
///
///   - it is not fixed-size (e.g. because it is a resilient type) and
///     therefore cannot be loaded into a statically-boundable set of
///     registers; or
///
///   - it is semantically bound to memory, either because its storage
///     address is used by the language runtime to implement its semantics
///     (as with a weak reference) or because its representation is somehow
///     address-dependent (as with something like a relative pointer).
///
/// An address-only type can be fixed-layout and/or trivial.
/// A non-fixed-layout type is always address-only.
enum IsAddressOnly_t : bool {
  IsNotAddressOnly = false,
  IsAddressOnly = true
};

/// Is this type somewhat like a reference-counted type?
enum IsReferenceCounted_t : bool {
  IsNotReferenceCounted = false,
  IsReferenceCounted = true
};

/// Is this type address only because it's resilient?
enum IsResilient_t : bool {
  IsNotResilient = false,
  IsResilient = true
};

/// Does this type contain an opaque result type that affects type lowering?
enum IsTypeExpansionSensitive_t : bool {
  IsNotTypeExpansionSensitive = false,
  IsTypeExpansionSensitive = true
};

/// Is the type infinitely defined in terms of itself? (Such types can never
/// be concretely instantiated, but may still arise from generic specialization.)
enum IsInfiniteType_t : bool {
  IsNotInfiniteType = false,
  IsInfiniteType = true,
};

/// Does this type contain at least one non-trivial, non-eager-move type?
enum IsLexical_t : bool {
  IsNotLexical = false,
  IsLexical = true,
};

/// Extended type information used by SIL.
class TypeLowering {
public:
  class RecursiveProperties {
    // These are chosen so that bitwise-or merges the flags properly.
    //
    // clang-format off
    enum : unsigned {
      NonTrivialFlag             = 1 << 0,
      NonFixedABIFlag            = 1 << 1,
      AddressOnlyFlag            = 1 << 2,
      ResilientFlag              = 1 << 3,
      TypeExpansionSensitiveFlag = 1 << 4,
      InfiniteFlag               = 1 << 5,
      HasRawPointerFlag          = 1 << 6,
      LexicalFlag                = 1 << 7,
    };
    // clang-format on

    uint8_t Flags;
  public:
    /// Construct a default RecursiveProperties, which corresponds to
    /// a trivial, loadable, fixed-layout type.
    constexpr RecursiveProperties() : Flags(0) {}

    constexpr RecursiveProperties(
        IsTrivial_t isTrivial, IsFixedABI_t isFixedABI,
        IsAddressOnly_t isAddressOnly, IsResilient_t isResilient,
        IsTypeExpansionSensitive_t isTypeExpansionSensitive =
            IsNotTypeExpansionSensitive,
        HasRawPointer_t hasRawPointer = DoesNotHaveRawPointer,
        IsLexical_t isLexical = IsNotLexical)
        : Flags((isTrivial ? 0U : NonTrivialFlag) |
                (isFixedABI ? 0U : NonFixedABIFlag) |
                (isAddressOnly ? AddressOnlyFlag : 0U) |
                (isResilient ? ResilientFlag : 0U) |
                (isTypeExpansionSensitive ? TypeExpansionSensitiveFlag : 0U) |
                (hasRawPointer ? HasRawPointerFlag : 0U) |
                (isLexical ? LexicalFlag : 0U)) {}

    constexpr bool operator==(RecursiveProperties p) const {
      return Flags == p.Flags;
    }

    static constexpr RecursiveProperties forTrivial() {
      return {IsTrivial, IsFixedABI, IsNotAddressOnly, IsNotResilient};
    }

    static constexpr RecursiveProperties forRawPointer() {
      return {IsTrivial, IsFixedABI, IsNotAddressOnly, IsNotResilient,
              IsNotTypeExpansionSensitive, HasRawPointer};
    }

    static constexpr RecursiveProperties forReference() {
      return {IsNotTrivial, IsFixedABI, IsNotAddressOnly, IsNotResilient,
              IsNotTypeExpansionSensitive, DoesNotHaveRawPointer, IsLexical};
    }

    static constexpr RecursiveProperties forOpaque() {
      return {IsNotTrivial, IsNotFixedABI, IsAddressOnly, IsNotResilient,
              IsNotTypeExpansionSensitive, DoesNotHaveRawPointer, IsLexical};
    }

    static constexpr RecursiveProperties forResilient() {
      return {IsTrivial, IsFixedABI, IsNotAddressOnly, IsResilient};
    }

    void addSubobject(RecursiveProperties other) {
      Flags |= other.Flags;
    }

    IsTrivial_t isTrivial() const {
      return IsTrivial_t((Flags & NonTrivialFlag) == 0);
    }
    HasRawPointer_t isOrContainsRawPointer() const {
      return HasRawPointer_t((Flags & HasRawPointerFlag) != 0);
    }
    IsFixedABI_t isFixedABI() const {
      return IsFixedABI_t((Flags & NonFixedABIFlag) == 0);
    }
    IsAddressOnly_t isAddressOnly() const {
      return IsAddressOnly_t((Flags & AddressOnlyFlag) != 0);
    }
    IsResilient_t isResilient() const {
      return IsResilient_t((Flags & ResilientFlag) != 0);
    }
    IsTypeExpansionSensitive_t isTypeExpansionSensitive() const {
      return IsTypeExpansionSensitive_t(
          (Flags & TypeExpansionSensitiveFlag) != 0);
    }
    IsInfiniteType_t isInfinite() const {
      return IsInfiniteType_t((Flags & InfiniteFlag) != 0);
    }
    IsLexical_t isLexical() const {
      return IsLexical_t((Flags & LexicalFlag) != 0);
    }

    void setNonTrivial() { Flags |= NonTrivialFlag; }
    void setNonFixedABI() { Flags |= NonFixedABIFlag; }
    void setAddressOnly() { Flags |= AddressOnlyFlag; }
    void setTypeExpansionSensitive(
        IsTypeExpansionSensitive_t isTypeExpansionSensitive) {
      Flags = (Flags & ~TypeExpansionSensitiveFlag) |
              (isTypeExpansionSensitive ? TypeExpansionSensitiveFlag : 0);
    }
    void setInfinite() { Flags |= InfiniteFlag; }
    void setLexical(IsLexical_t isLexical) {
      Flags = (Flags & ~LexicalFlag) | (isLexical ? LexicalFlag : 0);
    }
  };

private:
  friend class TypeConverter;

  virtual void setLoweredAddresses() const {}

protected:
  /// The SIL type of values with this Swift type.
  mutable SILType LoweredType;

private:
  RecursiveProperties Properties;

  /// The resilience expansion for this type lowering.
  /// If the type is not resilient at all, this is always Minimal.
  TypeExpansionContext expansionContext;

  unsigned ReferenceCounted : 1;

  /// A single linked list of lowerings for different resilience expansions.
  /// The first lowering is always for ResilientExpansion::Minimal.
  mutable const TypeLowering *NextExpansion = nullptr;

protected:
  TypeLowering(SILType type, RecursiveProperties properties,
               IsReferenceCounted_t isRefCounted,
               TypeExpansionContext expansionContext)
      : LoweredType(type), Properties(properties),
        expansionContext(expansionContext), ReferenceCounted(isRefCounted) {}

public:
  TypeLowering(const TypeLowering &) = delete;
  TypeLowering &operator=(const TypeLowering &) = delete;

  virtual ~TypeLowering() {}

  /// Print out the internal state of this type lowering into \p os.
  void print(llvm::raw_ostream &os) const;

  /// Dump out the internal state of this type lowering to llvm::dbgs().
  SWIFT_DEBUG_DUMP;

  RecursiveProperties getRecursiveProperties() const {
    return Properties;
  }

  /// isAddressOnly - Returns true if the type is an address-only type. A type
  /// is address-only if it is a resilient value type, or if it is a fragile
  /// value type with a resilient member. In either case, the full layout of
  /// values of the type is unavailable to the compiler.
  bool isAddressOnly() const {
    return Properties.isAddressOnly();
  }
  /// isLoadable - Returns true if the type is loadable, in other words, its
  /// full layout is available to the compiler. This is the inverse of
  /// isAddressOnly.
  bool isLoadable() const {
    return !isAddressOnly();
  }

  /// isFixedABI - Returns true if the type has a known fixed layout.
  /// If this is true, value operations on the type can be performed even if
  /// the type is inaccessible.
  bool isFixedABI() const {
    return Properties.isFixedABI();
  }
  
  /// Returns true if the type is trivial, meaning it is a loadable
  /// value type with no reference type members that require releasing.
  bool isTrivial() const {
    return Properties.isTrivial();
  }
  
  bool isOrContainsRawPointer() const {
    return Properties.isOrContainsRawPointer();
  }
  
  /// Returns true if the type is a scalar reference-counted reference, which
  /// can be retained and released.
  bool isReferenceCounted() const {
    return ReferenceCounted;
  }

  /// getLoweredType - Get the type used to represent values of the Swift type
  /// in SIL.
  SILType getLoweredType() const {
    return LoweredType;
  }

  /// Returns true if the SIL type is an address.
  bool isAddress() const {
    return LoweredType.isAddress();
  }

  bool isResilient() const {
    return Properties.isResilient();
  }

  /// Does this type contain an opaque result type that could influence how the
  /// type is lowered if we could look through to the underlying type.
  bool isTypeExpansionSensitive() const {
    return Properties.isTypeExpansionSensitive();
  }

  /// Should a value of this type have its lifetime tied to its lexical scope?
  bool isLexical() const {
    return Properties.isLexical();
  }

  ResilienceExpansion getResilienceExpansion() const {
    return expansionContext.getResilienceExpansion();
  }

  TypeExpansionContext getExpansionContext() const {
    return expansionContext;
  }

  /// Produce an exact copy of the value in the given address as a
  /// scalar.  The caller is responsible for destroying this value,
  /// e.g. by releasing it.
  ///
  /// This type must be loadable.
  virtual SILValue emitLoadOfCopy(SILBuilder &B,
                                  SILLocation loc,
                                  SILValue addr,
                                  IsTake_t isTake) const = 0;

  /// Store an exact copy of the given value in the destination
  /// address, taking ownership of it.
  ///
  /// This type must be loadable.
  ///
  /// Note that, for an assignment, this produces lowered code: that
  /// is, for non-POD types, an explicit load and release.  Therefore,
  /// it is generally not correct to call this during SIL-gen.
  virtual void emitStoreOfCopy(SILBuilder &B,
                               SILLocation loc,
                               SILValue value,
                               SILValue addr,
                               IsInitialization_t isInit) const = 0;

  /// Emit a store of \p value into \p addr given the StoreOwnershipQualifier
  /// qual.
  ///
  /// This abstracts over the differences in between trivial and non-trivial
  /// types.
  virtual void emitStore(SILBuilder &B, SILLocation loc, SILValue value,
                         SILValue addr, StoreOwnershipQualifier qual) const = 0;

  /// Emit a load from \p addr given the LoadOwnershipQualifier \p qual.
  ///
  /// This abstracts over the differences in between trivial and non-trivial
  /// types.
  virtual SILValue emitLoad(SILBuilder &B, SILLocation loc, SILValue addr,
                            LoadOwnershipQualifier qual) const = 0;

  /// Put an exact copy of the value in the source address in the
  /// destination address.
  virtual void emitCopyInto(SILBuilder &B,
                            SILLocation loc,
                            SILValue src,
                            SILValue dest,
                            IsTake_t isTake,
                            IsInitialization_t isInit) const = 0;

  /// Given an address, emit operations to destroy it.
  ///
  /// This produces canonicalized SIL.
  virtual void emitDestroyAddress(SILBuilder &B, SILLocation loc,
                                  SILValue value) const = 0;

  /// Given a +1 r-value which we are claiming ownership of, destroy it.
  ///
  /// Note that an r-value might be an address.
  virtual void emitDestroyRValue(SILBuilder &B, SILLocation loc,
                                 SILValue value) const = 0;

  /// When using "Lowered" APIs on a type lowering, how far should type lowering
  /// expand a type into its subtypes when emitting an operation.
  ///
  /// If we emit operations on a subtype of the type, we expand the type into
  /// the subtypes and perform the requested operation at that level of the
  /// type tree.
  enum class TypeExpansionKind {
    None,           ///> Emit operations on the actual value.
    DirectChildren, ///> Expand the value into its direct children and place
                    ///> operations on the children.
    MostDerivedDescendents, ///> Expand the value into its most derived
                            ///> subtypes and perform operations on these
                            ///> types.
  };

  /// Emit a load from \p addr given the LoadOwnershipQualifier \p qual.
  ///
  /// This abstracts over the differences in between trivial and non-trivial
  /// types and lets one specify an expansion kind that gets passed to any
  /// copy_value that we create.
  virtual SILValue emitLoweredLoad(
      SILBuilder &B, SILLocation loc, SILValue addr,
      LoadOwnershipQualifier qual,
      Lowering::TypeLowering::TypeExpansionKind expansionKind) const = 0;

  /// Emit a store of \p value into \p addr given the StoreOwnershipQualifier
  /// qual.
  ///
  /// This abstracts over the differences in between trivial and non-trivial
  /// types and allows for one to specify an expansion kind that is passed to
  /// any destroy operations we create if we are asked to assign in non-ossa
  /// code.
  virtual void emitLoweredStore(
      SILBuilder &B, SILLocation loc, SILValue value, SILValue addr,
      StoreOwnershipQualifier qual,
      Lowering::TypeLowering::TypeExpansionKind expansionKind) const = 0;

  //===--------------------------------------------------------------------===//
  // DestroyValue
  //===--------------------------------------------------------------------===//

  /// Emit a lowered destroy value operation.
  ///
  /// This type must be loadable.
  virtual void
  emitLoweredDestroyValue(SILBuilder &B, SILLocation loc, SILValue value,
                          TypeExpansionKind loweringStyle) const = 0;

  void emitLoweredDestroyChildValue(SILBuilder &B, SILLocation loc,
                                    SILValue value,
                                    TypeExpansionKind loweringStyle) const {
    switch (loweringStyle) {
    case TypeExpansionKind::None:
      llvm_unreachable("This does not apply to children of aggregate types");
    case TypeExpansionKind::DirectChildren:
      return emitDestroyValue(B, loc, value);
    case TypeExpansionKind::MostDerivedDescendents:
      return emitLoweredDestroyValueMostDerivedDescendents(B, loc, value);
    }
  }

  /// Emit a lowered destroy value operation.
  ///
  /// This type must be loadable.
  void emitLoweredDestroyValueDirectChildren(SILBuilder &B, SILLocation loc,
                                             SILValue value) const {
    emitLoweredDestroyValue(B, loc, value, TypeExpansionKind::DirectChildren);
  }

  /// Emit a lowered destroy_value operation.
  ///
  /// This type must be loadable.
  void emitLoweredDestroyValueMostDerivedDescendents(SILBuilder &B,
                                                     SILLocation loc,
                                                     SILValue value) const {
    emitLoweredDestroyValue(B, loc, value,
                            TypeExpansionKind::MostDerivedDescendents);
  }

  /// Given a primitively loaded value of this type (which must be
  /// loadable), -1 it.
  ///
  /// This should be used when dropping a value which has been copied
  /// from place to place with exactly the same semantics.  For
  /// example, it performs an unowned_release on a value of [unknown]
  /// type.  It is therefore not necessarily the right thing to do on
  /// a semantic load.
  virtual void emitDestroyValue(SILBuilder &B, SILLocation loc,
                                SILValue value) const = 0;

  //===--------------------------------------------------------------------===//
  // CopyValue
  //===--------------------------------------------------------------------===//

  /// Emit a lowered copy value operation.
  ///
  /// This type must be loadable.
  virtual SILValue emitLoweredCopyValue(SILBuilder &B, SILLocation loc,
                                        SILValue value,
                                        TypeExpansionKind style) const = 0;

  /// Emit a lowered copy value operation.
  ///
  /// This type must be loadable.
  SILValue emitLoweredCopyValueDirectChildren(SILBuilder &B, SILLocation loc,
                                              SILValue value) const {
    return emitLoweredCopyValue(B, loc, value,
                                TypeExpansionKind::DirectChildren);
  }

  /// Emit a lowered copy value operation.
  ///
  /// This type must be loadable.
  SILValue emitLoweredCopyValueMostDerivedDescendents(SILBuilder &B,
                                                      SILLocation loc,
                                                      SILValue value) const {
    return emitLoweredCopyValue(B, loc, value,
                                TypeExpansionKind::MostDerivedDescendents);
  }

  /// Given a primitively loaded value of this type (which must be
  /// loadable), Perform a copy of this value. This is equivalent to performing
  /// +1 on class values.
  ///
  /// This should be used for duplicating a value from place to place
  /// with exactly the same semantics.  For example, it performs an
  /// unowned_retain on a value of [unknown] type.  It is therefore
  /// not necessarily the right thing to do on a semantic load.
  virtual SILValue emitCopyValue(SILBuilder &B, SILLocation loc,
                                 SILValue value) const = 0;

  SILValue emitLoweredCopyChildValue(SILBuilder &B, SILLocation loc,
                                     SILValue value,
                                     TypeExpansionKind style) const {
    switch (style) {
    case TypeExpansionKind::None:
      llvm_unreachable("This does not apply to children of aggregate");
    case TypeExpansionKind::DirectChildren:
      return emitCopyValue(B, loc, value);
    case TypeExpansionKind::MostDerivedDescendents:
      return emitLoweredCopyValueMostDerivedDescendents(B, loc, value);
    }
    llvm_unreachable("unhandled style");
  }

  /// Allocate a new TypeLowering using the TypeConverter's allocator.
  void *operator new(size_t size, TypeConverter &tc);
  void *operator new[](size_t size, TypeConverter &tc);

  // Forbid 'new FooTypeLowering' and try to forbid 'delete tl'.
  // The latter is made challenging because the existence of the
  // virtual destructor requires an accessible 'operator delete'.
  void *operator new(size_t) = delete;

protected:
  void operator delete(void*) {}
};

/// Type and lowering information about a constant function.
struct SILConstantInfo {
  /// The formal type of the constant, still curried.  For a normal
  /// function, this is just its declared type; for a getter or
  /// setter, computing this can be more involved.
  CanAnyFunctionType FormalType;

  /// The abstraction pattern of the constant.  Its type structure
  /// matches the formal type, but with types replaced with their
  /// bridged equivalents.
  AbstractionPattern FormalPattern;

  /// The uncurried and bridged type of the constant.
  CanAnyFunctionType LoweredType;

  /// The SIL function type of the constant.
  CanSILFunctionType SILFnType;

  SILConstantInfo(CanAnyFunctionType formalType,
                  AbstractionPattern formalPattern,
                  CanAnyFunctionType loweredType,
                  CanSILFunctionType silFnTy)
    : FormalType(formalType),
      FormalPattern(formalPattern),
      LoweredType(loweredType),
      SILFnType(silFnTy) {}
  
  SILType getSILType() const {
    return SILType::getPrimitiveObjectType(SILFnType);
  }

  friend bool operator==(SILConstantInfo lhs, SILConstantInfo rhs) {
    return lhs.FormalType == rhs.FormalType &&
           lhs.LoweredType == rhs.LoweredType &&
           lhs.SILFnType == rhs.SILFnType;
  }
  friend bool operator!=(SILConstantInfo lhs, SILConstantInfo rhs) {
    return !(lhs == rhs);
  }
};

/// Different ways in which a function can capture context.
enum class CaptureKind {
  /// A local value captured as a mutable box.
  Box,
  /// A local value captured as an immutable box.
  ImmutableBox,
  /// A local value captured as a single pointer to storage (formed with
  /// @noescape closures).
  StorageAddress,
  /// A local value captured as a constant.
  Constant,
  /// A let constant captured as a pointer to storage
  Immutable
};

/// TypeConverter - helper class for creating and managing TypeLowerings.
class TypeConverter {
  friend class TypeLowering;

  llvm::BumpPtrAllocator TypeLoweringBPA;

  struct CachingTypeKey {
    CanGenericSignature Sig;
    AbstractionPattern::CachingKey OrigType;
    CanType SubstType;
    TypeExpansionContext expansionContext;

    friend bool operator==(const CachingTypeKey &lhs,
                           const CachingTypeKey &rhs) {
      return lhs.Sig == rhs.Sig && lhs.OrigType == rhs.OrigType &&
             lhs.SubstType == rhs.SubstType &&
             lhs.expansionContext == rhs.expansionContext;
    }
    friend bool operator!=(const CachingTypeKey &lhs,
                           const CachingTypeKey &rhs) {
      return !(lhs == rhs);
    }
  };

  struct TypeKey {
    /// An unsubstituted version of a type, dictating its abstraction patterns.
    AbstractionPattern OrigType;

    /// The substituted version of the type, dictating the types that
    /// should be used in the lowered type.
    CanType SubstType;

    TypeExpansionContext expansionContext;

    bool IsCacheable;

    CachingTypeKey getCachingKey() const {
      assert(isCacheable());
      return { (OrigType.hasGenericSignature()
                   ? OrigType.getGenericSignature()
                   : nullptr),
               OrigType.getCachingKey(),
               SubstType,
               expansionContext };
    }

    bool isCacheable() const {
      return IsCacheable;
    }
    
    TypeKey getKeyForMinimalExpansion() const {
      return {OrigType, SubstType, TypeExpansionContext::minimal(),
              IsCacheable};
    }

    void computeCacheable() {
      IsCacheable = (OrigType.hasCachingKey() &&
                     !isTreacherousInterfaceType(SubstType));
    }

    static bool isTreacherousInterfaceType(CanType type) {
      // Don't cache lowerings for interface function types that involve
      // type parameters; we might need a contextual generic signature to
      // handle them correctly.
      if (!type->hasTypeParameter()) return false;
      return type.findIf([](CanType type) {
        return isa<FunctionType>(type) && type->hasTypeParameter();
      });
    }
  };

  friend struct llvm::DenseMapInfo<CachingTypeKey>;

  TypeKey getTypeKey(AbstractionPattern origTy, CanType substTy,
                     TypeExpansionContext context) {
    TypeKey result = {origTy, substTy, context, false};
    result.computeCacheable();
    return result;
  }

  struct OverrideKey {
    SILDeclRef derived;
    SILDeclRef base;
    
    friend bool operator==(const OverrideKey &lhs,
                           const OverrideKey &rhs) {
      return lhs.derived == rhs.derived
          && lhs.base == rhs.base;
    }
    friend bool operator!=(const OverrideKey &lhs,
                           const OverrideKey &rhs) {
      return !(lhs == rhs);
    }
  };
  
  friend struct llvm::DenseMapInfo<OverrideKey>;

  /// Find a cached TypeLowering by TypeKey, or return null if one doesn't
  /// exist.
  const TypeLowering *find(const TypeKey &k);
  /// Insert a mapping into the cache.
  void insert(const TypeKey &k, const TypeLowering *tl);
#ifndef NDEBUG
  /// Remove the nullptr entry from the type map.
  void removeNullEntry(const TypeKey &k);
#endif

  /// True if SIL conventions force address-only to be passed by address.
  bool LoweredAddresses;

  CanGenericSignature CurGenericSignature;

  /// Stack of types currently being lowered as part of an aggregate.
  llvm::SetVector<CanType> AggregateFieldsBeingLowered;
  
  /// Mapping for types independent on contextual generic parameters.
  llvm::DenseMap<CachingTypeKey, const TypeLowering *> LoweredTypes;

  llvm::DenseMap<std::pair<TypeExpansionContext, SILDeclRef>, SILConstantInfo *>
      ConstantTypes;

  llvm::DenseMap<OverrideKey, SILConstantInfo *> ConstantOverrideTypes;

  llvm::DenseMap<SILDeclRef, CaptureInfo> LoweredCaptures;

  /// Cache of loadable SILType to number of (estimated) fields
  ///
  /// Second element is a ResilienceExpansion.
  llvm::DenseMap<std::pair<SILType, unsigned>, unsigned> TypeFields;
  
  llvm::DenseMap<AbstractClosureExpr *, Optional<AbstractionPattern>>
    ClosureAbstractionPatterns;
  llvm::DenseMap<SILDeclRef, TypeExpansionContext>
    CaptureTypeExpansionContexts;

  CanAnyFunctionType makeConstantInterfaceType(SILDeclRef constant);
  
  // Types converted during foreign bridging.
#define BRIDGING_KNOWN_TYPE(BridgedModule,BridgedType) \
  Optional<CanType> BridgedType##Ty;
#include "swift/SIL/BridgedTypes.def"

  const TypeLowering &getTypeLoweringForLoweredType(
      AbstractionPattern origType, CanType loweredType,
      TypeExpansionContext forExpansion,
      IsTypeExpansionSensitive_t isTypeExpansionSensitive);

  const TypeLowering *getTypeLoweringForExpansion(
      TypeKey key, TypeExpansionContext forExpansion,
      const TypeLowering *minimalExpansionLowering,
      IsTypeExpansionSensitive_t isOrigTypeExpansionSensitive);

public:
  ModuleDecl &M;
  ASTContext &Context;

  TypeConverter(ModuleDecl &m, bool loweredAddresses = true);
  ~TypeConverter();
  TypeConverter(TypeConverter const &) = delete;
  TypeConverter &operator=(TypeConverter const &) = delete;

  CanGenericSignature getCurGenericSignature() const {
    return CurGenericSignature;
  }
  
  // RAII type used when lowering nominal aggregate types (structs and enums)
  // to catch self-recursion. Although Sema tries to catch direct circularity
  // in value type definitions, it can't catch circularity that arises from
  // generic substitutions. SIL may however be exposed to these circularities
  // at any point by substituting bound generic types either during SILGen or
  // after passes that perform generic specialization.
  class LowerAggregateTypeRAII {
    TypeConverter &TC;
    
  public:
    // True if the aggregate about to be lowered is already being lowered,
    // indicating a circularity.
    const bool IsInfinite;
    
    LowerAggregateTypeRAII(TypeConverter &TC, CanType aggregate)
      : TC(TC), IsInfinite(!TC.AggregateFieldsBeingLowered.insert(aggregate))
    {}
    
    ~LowerAggregateTypeRAII() {
      if (!IsInfinite) {
        TC.AggregateFieldsBeingLowered.pop_back();
      }
    }
  };

  class GenericContextRAII {
    TypeConverter &TC;
    CanGenericSignature SavedSig;
  public:
    GenericContextRAII(TypeConverter &TC, CanGenericSignature sig)
        : TC(TC), SavedSig(TC.CurGenericSignature) {
      TC.CurGenericSignature = sig;
    }

    GenericContextRAII(const GenericContextRAII &) = delete;
    GenericContextRAII &operator=(const GenericContextRAII &) = delete;

    ~GenericContextRAII() {
      TC.CurGenericSignature = SavedSig;
    }
  };

  /// Return the CaptureKind to use when capturing a decl.
  CaptureKind getDeclCaptureKind(CapturedValue capture,
                                 TypeExpansionContext expansion);

  /// Return a most-general-possible abstraction pattern.
  AbstractionPattern getMostGeneralAbstraction() {
    return AbstractionPattern::getOpaque();
  }

  /// Get the calling convention used by witnesses of a protocol.
  static SILFunctionTypeRepresentation
  getProtocolWitnessRepresentation(ProtocolDecl *P) {
    // ObjC protocols use the objc method convention.
    if (P->isObjC())
      return SILFunctionTypeRepresentation::ObjCMethod;

    // Native protocols use the witness calling convention.
    return SILFunctionTypeRepresentation::WitnessMethod;
  }
  
  /// Get the calling convention used to call a declaration.
  SILFunctionTypeRepresentation getDeclRefRepresentation(SILDeclRef c);
  
  /// Get the method dispatch strategy for a protocol.
  static ProtocolDispatchStrategy getProtocolDispatchStrategy(ProtocolDecl *P);

  /// Count the total number of fields inside the given SIL Type
  unsigned countNumberOfFields(SILType Ty, TypeExpansionContext expansion);

  /// True if a protocol uses witness tables for dynamic dispatch.
  static bool protocolRequiresWitnessTable(ProtocolDecl *P) {
    if (P->isMarkerProtocol())
      return false;

    return swift::protocolRequiresWitnessTable(getProtocolDispatchStrategy(P));
  }
  
  /// True if a type is passed indirectly at +0 when used as the "self"
  /// parameter of its own methods.
  ///
  /// TODO: We want this always to hold.
  static bool isIndirectPlusZeroSelfParameter(Type T) {
    // Calls through opaque protocols can be done with +0 rvalues.  This allows
    // us to avoid materializing copies of existentials.
    return !T->hasReferenceSemantics()
        && (T->isExistentialType() || T->is<ArchetypeType>());
  }
  
  static bool isIndirectPlusZeroSelfParameter(SILType T) {
    return isIndirectPlusZeroSelfParameter(T.getASTType());
  }
  
  /// Lowers a context-independent Swift type to a SILType, and returns the SIL TypeLowering
  /// for that type.
  ///
  /// If `t` contains generic parameters, then the overload that also takes an
  /// `AbstractionPattern` must be used.
  const TypeLowering &
  getTypeLowering(Type t, TypeExpansionContext forExpansion) {
    AbstractionPattern pattern(t->getCanonicalType());
    return getTypeLowering(pattern, t, forExpansion);
  }

  /// Lowers a Swift type to a SILType according to the abstraction
  /// patterns of the given original type.
  const TypeLowering &getTypeLowering(AbstractionPattern origType,
                                      Type substType,
                                      TypeExpansionContext forExpansion);

  /// Returns the SIL TypeLowering for an already lowered SILType. If the
  /// SILType is an address, returns the TypeLowering for the pointed-to
  /// type.
  ///
  /// If `t` contains type parameters, then the generic signature for its context
  /// must be provided.
  const TypeLowering &
  getTypeLowering(SILType t, TypeExpansionContext forExpansion,
                  CanGenericSignature signature = nullptr);

  /// Returns the SIL TypeLowering for an already lowered SILType. If the
  /// SILType is an address, returns the TypeLowering for the pointed-to
  /// type in the context of the given SILFunction.
  const TypeLowering &
  getTypeLowering(SILType t, SILFunction &F);

  // Returns the lowered SIL type for a Swift type.
  SILType getLoweredType(Type t, TypeExpansionContext forExpansion) {
    return getTypeLowering(t, forExpansion).getLoweredType();
  }

  // Returns the lowered SIL type for a Swift type.
  SILType getLoweredType(AbstractionPattern origType, Type substType,
                         TypeExpansionContext forExpansion) {
    return getTypeLowering(origType, substType, forExpansion)
      .getLoweredType();
  }

  SILType getLoweredLoadableType(Type t,
                                 TypeExpansionContext forExpansion,
                                 SILModule &M) {
    const TypeLowering &ti = getTypeLowering(t, forExpansion);
    assert(
        (ti.isLoadable() || !SILModuleConventions(M).useLoweredAddresses()) &&
        "unexpected address-only type");
    return ti.getLoweredType();
  }

  CanType getLoweredRValueType(TypeExpansionContext context, Type t) {
    return getLoweredType(t, context).getRawASTType();
  }

  CanType getLoweredRValueType(TypeExpansionContext context,
                               AbstractionPattern origType, Type substType) {
    return getLoweredType(origType, substType, context).getRawASTType();
  }

  AbstractionPattern getAbstractionPattern(AbstractStorageDecl *storage,
                                           bool isNonObjC = false);
  AbstractionPattern getAbstractionPattern(VarDecl *var,
                                           bool isNonObjC = false);
  AbstractionPattern getAbstractionPattern(SubscriptDecl *subscript,
                                           bool isNonObjC = false);
  AbstractionPattern getAbstractionPattern(EnumElementDecl *element);

  CanType getLoweredTypeOfGlobal(VarDecl *var);

  /// Return the SILFunctionType for a native function value of the
  /// given type.
  CanSILFunctionType getSILFunctionType(TypeExpansionContext context,
                                        AbstractionPattern origType,
                                        CanFunctionType substFnType);

  /// Returns the formal type, lowered AST type, and SILFunctionType
  /// for a constant reference.
  const SILConstantInfo &getConstantInfo(TypeExpansionContext context,
                                         SILDeclRef constant);

  /// Get the generic environment for a constant.
  GenericSignature getConstantGenericSignature(SILDeclRef constant);

  /// Get the generic environment for a constant.
  GenericEnvironment *getConstantGenericEnvironment(SILDeclRef constant);

  /// Returns the SIL type of a constant reference.
  SILType getConstantType(TypeExpansionContext context, SILDeclRef constant) {
    return getConstantInfo(context, constant).getSILType();
  }

  /// Returns the SILFunctionType for the given declaration.
  CanSILFunctionType getConstantFunctionType(TypeExpansionContext context,
                                             SILDeclRef constant) {
    return getConstantInfo(context, constant).SILFnType;
  }
  
  /// Returns the SILParameterInfo for the given declaration's `self` parameter.
  /// `constant` must refer to a method.
  SILParameterInfo getConstantSelfParameter(TypeExpansionContext context,
                                            SILDeclRef constant);

  /// Returns the SILFunctionType that must be used to perform a vtable dispatch
  /// to the given declaration.
  ///
  /// Will be the same as getConstantFunctionType() if the declaration does not
  /// override anything.
  CanSILFunctionType getConstantOverrideType(TypeExpansionContext context,
                                             SILDeclRef constant) {
    return getConstantOverrideInfo(context, constant).SILFnType;
  }

  /// Returns the SILConstantInfo that must be used to perform a vtable dispatch
  /// to the given declaration.
  ///
  /// Will be the same as getConstantInfo() if the declaration does not
  /// override anything.
  const SILConstantInfo &getConstantOverrideInfo(TypeExpansionContext context,
                                                 SILDeclRef constant) {
    // Fast path if the constant does not override anything.
    auto next = constant.getNextOverriddenVTableEntry();
    if (next.isNull())
      return getConstantInfo(context, constant);

    auto base = constant.getOverriddenVTableEntry();
    return getConstantOverrideInfo(context, constant, base);
  }

  const SILConstantInfo &getConstantOverrideInfo(TypeExpansionContext context,
                                                 SILDeclRef constant,
                                                 SILDeclRef base);

  /// Get the empty tuple type as a SILType.
  SILType getEmptyTupleType() {
    return SILType::getPrimitiveObjectType(TupleType::getEmpty(Context));
  }

  /// Describes what we're trying to compute a bridged type for.
  ///
  /// \see getLoweredBridgedType
  enum BridgedTypePurpose {
    ForArgument,
    ForNonOptionalResult, // A result that should not be made more optional
    ForResult,
    ForMemory,
  };

  /// Map an AST-level type to the corresponding foreign representation type we
  /// implicitly convert to for a given calling convention.
  Type getLoweredBridgedType(AbstractionPattern pattern, Type t,
                             Bridgeability bridging,
                             SILFunctionTypeRepresentation rep,
                             BridgedTypePurpose purpose);

  struct LoweredFormalTypes {
    /// The abstraction pattern of the type.  This always has a type; the
    /// type is a function type parallel in structure to the original formal
    /// type but with types replaced with appropriate bridged types.
    AbstractionPattern Pattern;

    /// The bridged and uncurried type of the constant.
    CanAnyFunctionType Uncurried;
  };

  /// Derive the lowered formal type of the given constant.
  LoweredFormalTypes getLoweredFormalTypes(SILDeclRef constant,
                                           CanAnyFunctionType formalType);

  /// Given a function type, yield its bridged formal type.
  CanAnyFunctionType getBridgedFunctionType(AbstractionPattern fnPattern,
                                            CanAnyFunctionType fnType,
                                            Bridgeability bridging,
                                            SILFunctionTypeRepresentation rep);

  /// Given a referenced value and the substituted formal type of a
  /// resulting l-value expression, produce the substituted formal
  /// type of the storage of the value.
  ///
  /// \return - always an address type
  SILType getSubstitutedStorageType(TypeExpansionContext context,
                                    AbstractStorageDecl *value,
                                    Type lvalueType);

  /// Known types for bridging.
#define BRIDGING_KNOWN_TYPE(BridgedModule,BridgedType) \
  CanType get##BridgedType##Type();
#include "swift/SIL/BridgedTypes.def"

  /// Get the capture list for a function or default argument, with transitive
  /// function captures flattened.
  CaptureInfo getLoweredLocalCaptures(SILDeclRef fn);
  bool hasLoweredLocalCaptures(SILDeclRef fn);

  enum class ABIDifference : uint8_t {
    // Types have compatible calling conventions and representations, so can
    // be trivially bitcast.
    //
    // Furthermore, if two function types have
    // arguments of function type that differ only in
    // `CompatibleRepresentation`, those outer function types are transitively
    // `CompatibleRepresentation`. (In all other cases, the outer function types
    // would fall into the `NeedsThunk` case, because a thunk would be needed
    // to change the representation of the function argument.)
    CompatibleRepresentation,
    
    // No convention differences, but there may still be a representation
    // difference between values of the compared function types, such as a
    // different ptrauth discriminator. The conversion can be performed by a
    // `convert_function` instruction.
    CompatibleCallingConvention,
    
    // Representation difference requires thin-to-thick conversion with a
    // `thin_to_thick_function` conversion.
    CompatibleRepresentation_ThinToThick,
    // Function types have `CompatibleCallingConvention` but additionally need
    // a thin-to-thick conversion, so a `convert_function` followed by a
    // `thin_to_thick_function` sequence is necessary to convert.
    CompatibleCallingConvention_ThinToThick,
    
    // Non-trivial difference requires thunk.
    NeedsThunk
  };
  
  /// Test if type1 is ABI compatible with type2, and can be converted
  /// with a trivial bitcast.
  ///
  /// Note that type1 and type2 must be lowered types, and type1 must be a
  /// subtype of type2.
  ///
  /// The ABI compatible relation is not symmetric on function types -- while
  /// T and T! are both subtypes of each other, a calling convention conversion
  /// of T! to T always requires a thunk.
  ABIDifference checkForABIDifferences(SILModule &M,
                                       SILType type1, SILType type2,
                                       bool thunkOptionals = true);

  /// Same as above but for SIL function types.
  ABIDifference checkFunctionForABIDifferences(SILModule &M,
                                               SILFunctionType *fnTy1,
                                               SILFunctionType *fnTy2);


  /// Lower the function type as a possible substitution for the type of
  /// \p constant. The result is not cached as part of the constant's normal
  /// ConstantInfo.
  CanSILFunctionType
  getUncachedSILFunctionTypeForConstant(TypeExpansionContext expansion,
                                        SILDeclRef constant,
                                        CanAnyFunctionType origInterfaceType);

  /// Get the boxed interface type to use for a capture of the given decl.
  CanSILBoxType
  getInterfaceBoxTypeForCapture(ValueDecl *captured,
                                CanType loweredInterfaceType,
                                bool isMutable);
  /// Get the boxed contextual type to use for a capture of the given decl
  /// in the given generic environment.
  CanSILBoxType
  getContextBoxTypeForCapture(ValueDecl *captured,
                              CanType loweredContextType,
                              GenericEnvironment *env,
                              bool isMutable);

  CanSILBoxType getBoxTypeForEnumElement(TypeExpansionContext context,
                                         SILType enumType,
                                         EnumElementDecl *elt);

  /// Get the preferred abstraction pattern, if any, by which to lower a
  /// declaration.
  ///
  /// This can be set using \c setAbstractionPattern , but only before
  /// the abstraction pattern is queried using this function. Once the
  /// abstraction pattern has been asked for, it may not be changed.
  Optional<AbstractionPattern> getConstantAbstractionPattern(SILDeclRef constant);
  TypeExpansionContext getCaptureTypeExpansionContext(SILDeclRef constant);
  
  /// Set the preferred abstraction pattern for a closure.
  ///
  /// The abstraction pattern can only be set before any calls to
  /// \c getConstantAbstractionPattern on the same closure. It may not be
  /// changed once it has been read.
  void setAbstractionPattern(AbstractClosureExpr *closure,
                             AbstractionPattern pattern);
  
  void setCaptureTypeExpansionContext(SILDeclRef constant,
                                      SILModule &M);

  void setLoweredAddresses();

private:
  CanType computeLoweredRValueType(TypeExpansionContext context,
                                   AbstractionPattern origType,
                                   CanType substType);

  Type getLoweredCBridgedType(AbstractionPattern pattern, Type t,
                              Bridgeability bridging,
                              SILFunctionTypeRepresentation rep,
                              BridgedTypePurpose purpose);

  AnyFunctionType::Param
  getBridgedParam(SILFunctionTypeRepresentation rep,
                  AbstractionPattern pattern,
                  AnyFunctionType::Param param,
                  Bridgeability bridging);

  void getBridgedParams(SILFunctionTypeRepresentation rep,
                        AbstractionPattern pattern,
                        ArrayRef<AnyFunctionType::Param> params,
                        SmallVectorImpl<AnyFunctionType::Param> &bridged,
                        Bridgeability bridging);

  CanType getBridgedResultType(SILFunctionTypeRepresentation rep,
                               AbstractionPattern pattern,
                               CanType result,
                               Bridgeability bridging,
                               bool suppressOptional);
#ifndef NDEBUG
  /// Check the result of
  /// getTypeLowering(AbstractionPattern,Type,TypeExpansionContext).
  void verifyLowering(const TypeLowering &, AbstractionPattern origType,
                      CanType origSubstType,
                      TypeExpansionContext forExpansion);
  bool
  visitAggregateLeaves(Lowering::AbstractionPattern origType,
                       CanType substType,
                       TypeExpansionContext context,
                       std::function<bool(CanType, Lowering::AbstractionPattern,
                                          ValueDecl *, Optional<unsigned>)>
                           isLeafAggregate,
                       std::function<bool(CanType, Lowering::AbstractionPattern,
                                          ValueDecl *, Optional<unsigned>)>
                           visit);
#endif
};

} // namespace Lowering

CanSILFunctionType getNativeSILFunctionType(
    Lowering::TypeConverter &TC, TypeExpansionContext context,
    Lowering::AbstractionPattern origType, CanAnyFunctionType substType,
    SILExtInfo silExtInfo, Optional<SILDeclRef> origConstant = None,
    Optional<SILDeclRef> constant = None,
    Optional<SubstitutionMap> reqtSubs = None,
    ProtocolConformanceRef witnessMethodConformance = ProtocolConformanceRef());

/// The thunk kinds used in the differentiation transform.
enum class DifferentiationThunkKind {
  /// A reabstraction thunk.
  ///
  /// Reabstraction thunks transform a function-typed value to another one with
  /// different parameter/result abstraction patterns. This is identical to the
  /// thunks generated by SILGen.
  Reabstraction,

  /// An index subset thunk.
  ///
  /// An index subset thunk is used transform JVP/VJPs into a version that is
  /// "wrt" fewer differentiation parameters.
  /// - Differentials of thunked JVPs use zero for non-requested differentiation
  ///    parameters.
  /// - Pullbacks of thunked VJPs discard results for non-requested
  ///   differentiation parameters.
  IndexSubset
};

/// Build the type of a function transformation thunk.
CanSILFunctionType buildSILFunctionThunkType(
    SILFunction *fn,
    CanSILFunctionType &sourceType,
    CanSILFunctionType &expectedType,
    CanType &inputSubstType,
    CanType &outputSubstType,
    GenericEnvironment *&genericEnv,
    SubstitutionMap &interfaceSubs,
    CanType &dynamicSelfType,
    bool withoutActuallyEscaping,
    Optional<DifferentiationThunkKind> differentiationThunkKind = None);

} // namespace swift

namespace llvm {
  template<> struct DenseMapInfo<swift::Lowering::TypeConverter::CachingTypeKey> {
    using CachingTypeKey = swift::Lowering::TypeConverter::CachingTypeKey;

    using APCachingKey = swift::Lowering::AbstractionPattern::CachingKey;
    using CachingKeyInfo = DenseMapInfo<APCachingKey>;

    using CanTypeInfo = DenseMapInfo<swift::CanType>;

    // Use the second field because the first field can validly be null.
    static CachingTypeKey getEmptyKey() {
      return {nullptr, APCachingKey(), CanTypeInfo::getEmptyKey(),
              swift::TypeExpansionContext::minimal()};
    }
    static CachingTypeKey getTombstoneKey() {
      return {nullptr, APCachingKey(), CanTypeInfo::getTombstoneKey(),
              swift::TypeExpansionContext::minimal()};
    }
    static unsigned getHashValue(CachingTypeKey val) {
      auto hashSig =
        DenseMapInfo<swift::GenericSignature>::getHashValue(val.Sig);
      auto hashOrig =
        CachingKeyInfo::getHashValue(val.OrigType);
      auto hashSubst =
        DenseMapInfo<swift::CanType>::getHashValue(val.SubstType);
      auto hashContext =
          DenseMapInfo<swift::TypeExpansionContext>::getHashValue(
              val.expansionContext);
      return hash_combine(hashSig, hashOrig, hashSubst, hashContext);
    }
    static bool isEqual(CachingTypeKey LHS, CachingTypeKey RHS) {
      return LHS == RHS;
    }
  };

  template<> struct DenseMapInfo<swift::Lowering::TypeConverter::OverrideKey> {
    using OverrideKey = swift::Lowering::TypeConverter::OverrideKey;

    using SILDeclRefInfo = DenseMapInfo<swift::SILDeclRef>;

    static OverrideKey getEmptyKey() {
      return {SILDeclRefInfo::getEmptyKey(), SILDeclRefInfo::getEmptyKey()};
    }
    static OverrideKey getTombstoneKey() {
      return {SILDeclRefInfo::getTombstoneKey(), SILDeclRefInfo::getTombstoneKey()};
    }
    static unsigned getHashValue(OverrideKey val) {
      return hash_combine(SILDeclRefInfo::getHashValue(val.base),
                          SILDeclRefInfo::getHashValue(val.derived));
    }
    static bool isEqual(OverrideKey LHS, OverrideKey RHS) {
      return LHS == RHS;
    }
  };
}

#endif
