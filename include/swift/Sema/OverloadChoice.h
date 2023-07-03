//===--- OverloadChoice.h - A Choice from an Overload Set  ------*- C++ -*-===//
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
// This file provides the \c OverloadChoice class and its related types,
// which is used by the constraint-based type checker to describe the
// selection of a particular overload from a set.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_SEMA_OVERLOADCHOICE_H
#define SWIFT_SEMA_OVERLOADCHOICE_H

#include "llvm/ADT/PointerIntPair.h"
#include "llvm/Support/ErrorHandling.h"
#include "swift/AST/Availability.h"
#include "swift/AST/FunctionRefKind.h"
#include "swift/AST/Types.h"

namespace swift {

class ValueDecl;

namespace constraints {
  class ConstraintSystem;

/// The kind of overload choice.
enum class OverloadChoiceKind : int {
  /// The overload choice selects a particular declaration from a
  /// set of declarations.
  Decl,
  /// The overload choice selects a particular declaration that was
  /// found via dynamic lookup and, therefore, might not actually be
  /// available at runtime.
  DeclViaDynamic,
  /// The overload choice selects a key path subscripting operation.
  KeyPathApplication,
  /// The member is looked up using @dynamicMemberLookup.
  DynamicMemberLookup,
  /// The member with KeyPath parameter is looked up using
  /// @dynamicMemberLookup.
  KeyPathDynamicMemberLookup,
  /// The overload choice selects a particular declaration that
  /// was found by bridging the base value type to its Objective-C
  /// class type.
  DeclViaBridge,
  /// The overload choice selects a particular declaration that
  /// was found by unwrapping an optional context type.
  DeclViaUnwrappedOptional,
  /// The overload choice materializes a pack from a tuple.
  MaterializePack,
  /// The overload choice indexes into a tuple. Index zero will
  /// have the value of this enumerator, index one will have the value of this
  /// enumerator + 1, and so on. Thus, this enumerator must always be last.
  TupleIndex,
};

/// The kind of implicitly unwrapped optional for an overload reference.
enum class IUOReferenceKind : uint8_t {
  /// This overload references an IUO value which may be directly unwrapped.
  Value,

  /// This overload references a function, the return value of which may be
  /// unwrapped.
  ReturnValue,
};

/// Describes a particular choice within an overload set.
///
class OverloadChoice {
  enum : unsigned {
    /// Indicates that this is a normal "Decl" kind, or isn't a decl.
    IsDecl = 0x00,
    /// Indicates that this declaration was bridged, turning a
    /// "Decl" kind into "DeclViaBridge" kind.
    IsDeclViaBridge = 0x01,
    /// Indicates that this declaration was resolved by unwrapping an
    /// optional context type, turning a "Decl" kind into
    /// "DeclViaUnwrappedOptional".
    IsDeclViaUnwrappedOptional = 0x02,
    /// Indicates that there are viable members found on `Optional`
    /// type and its underlying type. And current overload choice
    /// is a backup one, which should be picked only if members
    /// found directly on `Optional` do not match.
    IsFallbackDeclViaUnwrappedOptional = 0x03,
    /// Indicates that this declaration was dynamic, turning a
    /// "Decl" kind into "DeclViaDynamic" kind.
    IsDeclViaDynamic = 0x07,
  };

  /// The base type to be used when referencing the declaration
  /// along with the three bits above.
  llvm::PointerIntPair<Type, 3, unsigned> BaseAndDeclKind;

  /// We mash together OverloadChoiceKind with tuple indices into a single
  /// integer representation.
  using OverloadChoiceKindWithTupleIndex =
      llvm::PointerEmbeddedInt<uint32_t, 29>;

  /// Depending on the OverloadChoiceKind, this could be one of two cases:
  /// 1) A ValueDecl for the cases that match to a Decl.  The exactly kind of
  ///    decl reference is disambiguated with the DeclKind bits in
  ///    BaseAndDeclKind.
  /// 2) An OverloadChoiceKindWithTupleIndex if this is an overload kind without
  ///    a decl (e.g., a BaseType, keypath, tuple, etc).
  ///
  llvm::PointerUnion<ValueDecl*, OverloadChoiceKindWithTupleIndex> DeclOrKind;

  /// If this OverloadChoice represents a DynamicMemberLookup result,
  /// then this holds the identifier for the original member being
  /// looked up, as well as 1 bit tag which identifies whether this
  /// choice represents a key-path based dynamic lookup.
  llvm::PointerIntPair<Identifier, 1, unsigned> DynamicMember;

  /// This holds the kind of function reference (Unapplied, SingleApply,
  /// DoubleApply, Compound).
  /// FIXME: This needs two bits. Can we pack them somewhere?
  FunctionRefKind TheFunctionRefKind;

public:
  OverloadChoice()
    : BaseAndDeclKind(nullptr, 0), DeclOrKind(),
      TheFunctionRefKind(FunctionRefKind::Unapplied) {}

  OverloadChoice(Type base, ValueDecl *value,
                 FunctionRefKind functionRefKind)
    : BaseAndDeclKind(base, 0),
      TheFunctionRefKind(functionRefKind) {
    assert(!base || !base->hasTypeParameter());
    assert((reinterpret_cast<uintptr_t>(value) & (uintptr_t)0x03) == 0 &&
           "Badly aligned decl");
    
    DeclOrKind = value;
  }

  OverloadChoice(Type base, OverloadChoiceKind kind)
      : BaseAndDeclKind(base, 0), DeclOrKind(uint32_t(kind)),
        TheFunctionRefKind(FunctionRefKind::Unapplied) {
    assert(base && "Must have a base type for overload choice");
    assert(!base->hasTypeParameter());
    assert(kind != OverloadChoiceKind::Decl &&
           kind != OverloadChoiceKind::DeclViaDynamic &&
           kind != OverloadChoiceKind::DeclViaBridge &&
           kind != OverloadChoiceKind::DeclViaUnwrappedOptional &&
           "wrong constructor for decl");
  }

  OverloadChoice(Type base, unsigned index)
      : BaseAndDeclKind(base, 0),
        DeclOrKind(uint32_t(OverloadChoiceKind::TupleIndex)+index),
        TheFunctionRefKind(FunctionRefKind::Unapplied) {
    assert(base->getRValueType()->is<TupleType>() && "Must have tuple type");
  }

  bool isInvalid() const {
    return BaseAndDeclKind.getPointer().isNull() &&
           BaseAndDeclKind.getInt() == 0 &&
           DeclOrKind.isNull() &&
           TheFunctionRefKind == FunctionRefKind::Unapplied;
  }

  /// Retrieve an overload choice for a declaration that was found via
  /// dynamic lookup.
  static OverloadChoice getDeclViaDynamic(Type base, ValueDecl *value,
                                          FunctionRefKind functionRefKind) {
    OverloadChoice result;
    result.BaseAndDeclKind.setPointer(base);
    result.BaseAndDeclKind.setInt(IsDeclViaDynamic);
    result.DeclOrKind = value;
    result.TheFunctionRefKind = functionRefKind;
    return result;
  }

  /// Retrieve an overload choice for a declaration that was found via
  /// bridging to an Objective-C class.
  static OverloadChoice getDeclViaBridge(Type base, ValueDecl *value,
                                         FunctionRefKind functionRefKind) {
    OverloadChoice result;
    result.BaseAndDeclKind.setPointer(base);
    result.BaseAndDeclKind.setInt(IsDeclViaBridge);
    result.DeclOrKind = value;
    result.TheFunctionRefKind = functionRefKind;
    return result;
  }

  /// Retrieve an overload choice for a declaration that was found
  /// by unwrapping an optional context type.
  ///
  /// \param isFallback Indicates that this result should be used
  /// as a backup, if member found directly on `Optional` doesn't
  /// match.
  static OverloadChoice
  getDeclViaUnwrappedOptional(Type base, ValueDecl *value, bool isFallback,
                              FunctionRefKind functionRefKind) {
    OverloadChoice result;
    result.BaseAndDeclKind.setPointer(base);
    result.BaseAndDeclKind.setInt(isFallback
                                      ? IsFallbackDeclViaUnwrappedOptional
                                      : IsDeclViaUnwrappedOptional);
    result.DeclOrKind = value;
    result.TheFunctionRefKind = functionRefKind;
    return result;
  }

  /// Retrieve an overload choice for a declaration that was found via
  /// dynamic member lookup. The `ValueDecl` is a `subscript(dynamicMember:)`
  /// method.
  static OverloadChoice getDynamicMemberLookup(Type base, ValueDecl *value,
                                               Identifier name,
                                               bool isKeyPathBased) {
    OverloadChoice result;
    result.BaseAndDeclKind.setPointer(base);
    result.DeclOrKind = value;
    result.DynamicMember.setPointer(name);
    result.DynamicMember.setInt(isKeyPathBased);
    result.TheFunctionRefKind = FunctionRefKind::SingleApply;
    return result;
  }

  /// Retrieve the base type used to refer to the declaration.
  Type getBaseType() const {
    return BaseAndDeclKind.getPointer();
  }
  
  /// Determines the kind of overload choice this is.
  OverloadChoiceKind getKind() const {
    if (!DynamicMember.getPointer().empty()) {
      return DynamicMember.getInt()
                 ? OverloadChoiceKind::KeyPathDynamicMemberLookup
                 : OverloadChoiceKind::DynamicMemberLookup;
    }
    
    if (DeclOrKind.is<ValueDecl*>()) {
      switch (BaseAndDeclKind.getInt()) {
      case IsDeclViaBridge: return OverloadChoiceKind::DeclViaBridge;
      case IsDeclViaDynamic: return OverloadChoiceKind::DeclViaDynamic;
      case IsDeclViaUnwrappedOptional:
      case IsFallbackDeclViaUnwrappedOptional:
        return OverloadChoiceKind::DeclViaUnwrappedOptional;
      default: return OverloadChoiceKind::Decl;
      }
    }
    uint32_t kind = DeclOrKind.get<OverloadChoiceKindWithTupleIndex>();
    if (kind >= (uint32_t)OverloadChoiceKind::TupleIndex)
      return OverloadChoiceKind::TupleIndex;

    return (OverloadChoiceKind)kind;
  }

  /// Determine whether this choice is for a declaration.
  bool isDecl() const {
    return DeclOrKind.is<ValueDecl*>();
  }

  /// Retrieve the declaration that corresponds to this overload choice.
  ValueDecl *getDecl() const {
    return DeclOrKind.get<ValueDecl*>();
  }

  /// Retrieves the declaration that corresponds to this overload choice, or
  /// \c nullptr if this choice is not for a declaration.
  ValueDecl *getDeclOrNull() const {
    return isDecl() ? getDecl() : nullptr;
  }

  /// Retrieve the type of implicitly unwrapped optional for a reference to this
  /// overload choice, or \c None if the choice is not for an IUO decl.
  llvm::Optional<IUOReferenceKind>
  getIUOReferenceKind(ConstraintSystem &cs,
                      bool forSecondApplication = false) const;

  bool isKeyPathDynamicMemberLookup() const {
    return getKind() == OverloadChoiceKind::KeyPathDynamicMemberLookup;
  }

  /// Determine whether this member is a backup in case
  /// members found directly on `Optional` didn't match.
  bool isFallbackMemberOnUnwrappedBase() const {
    return BaseAndDeclKind.getInt() == IsFallbackDeclViaUnwrappedOptional;
  }

  /// Whether this choice is for any kind of dynamic member lookup.
  bool isAnyDynamicMemberLookup() const {
    return getKind() == OverloadChoiceKind::DynamicMemberLookup ||
           isKeyPathDynamicMemberLookup();
  }

  /// Get the name of the overload choice.
  DeclName getName() const;

  /// Retrieve the tuple index that corresponds to this overload
  /// choice.
  unsigned getTupleIndex() const {
    assert(getKind() == OverloadChoiceKind::TupleIndex);
    uint32_t kind = DeclOrKind.get<OverloadChoiceKindWithTupleIndex>();
    return kind-(uint32_t)OverloadChoiceKind::TupleIndex;
  }

  /// Retrieves an opaque choice that ignores the base type.
  void *getOpaqueChoiceSimple() const {
    return DeclOrKind.getOpaqueValue();
  }

  FunctionRefKind getFunctionRefKind() const {
    assert(isDecl() && "only makes sense for declaration choices");
    return TheFunctionRefKind;
  }

  /// Print selected overload choice kind found for Solution in debug output.
  void dump(Type adjustedOpenedType, SourceManager *sm, raw_ostream &out) const;
};

} // end namespace constraints
} // end namespace swift

#endif // LLVM_SWIFT_SEMA_OVERLOADCHOICE_H
