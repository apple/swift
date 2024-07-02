//===--- CodeCompletionResultType.h -----------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IDE_CODECOMPLETIONRESULTTYPE_H
#define SWIFT_IDE_CODECOMPLETIONRESULTTYPE_H

#include "swift/AST/Types.h"
#include "swift/Basic/LLVM.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/StringMap.h"

namespace swift {
namespace ide {

enum class CustomAttributeKind : uint8_t {
  /// A type that can be used as a property wrapper.
  PropertyWrapper = 1 << 0,
  /// A type that can be used as a result builder.
  ResultBuilder = 1 << 1,
  /// A type that can be used as a global actor.
  GlobalActor = 1 << 2,
  /// A macro that can be used on variables or subscripts.
  VarMacro = 1 << 3,
  /// A macro that can be used on any type context.
  ContextMacro = 1 << 4,
  /// A macro that can be used on any declaration.
  DeclMacro = 1 << 5,
  /// A macro that can by used on any function.
  FunctionMacro = 1 << 6,
};

/// The expected contextual type(s) for code-completion.
class ExpectedTypeContext {
  /// Possible types of the code completion expression.
  llvm::SmallVector<Type, 4> PossibleTypes;

  /// Pre typechecked type of the expression at the completion position.
  Type IdealType;

  /// Whether the `ExpectedTypes` comes from an implied result, e.g.
  /// `foo({ here })`.
  ///
  /// Since the input may be incomplete, we take into account that the types are
  /// only a hint.
  bool IsImpliedResult = false;
  bool PreferNonVoid = false;

  /// If not empty, \c PossibleTypes are ignored and types that have an
  /// attribute kind that is contained in this list will be reported as
  /// 'Convertible'. All other types are related as 'Invalid'.
  OptionSet<CustomAttributeKind> ExpectedCustomAttributeKinds;

public:
  ExpectedTypeContext() = default;

  bool empty() const { return PossibleTypes.empty(); }

  ArrayRef<Type> getPossibleTypes() const { return PossibleTypes; }

  void setPossibleTypes(ArrayRef<Type> Types) {
    PossibleTypes.clear();
    PossibleTypes.reserve(Types.size());
    for (auto T : Types) {
      if (T) {
        PossibleTypes.push_back(T);
      }
    }
  }

  /// Form a union of this expected type context with \p Other.
  ///
  /// Any possible type from either type context will be considered a possible
  /// type in the merged type context.
  void merge(const ExpectedTypeContext &Other) {
    PossibleTypes.append(Other.PossibleTypes);

    // We can't merge ideal types. If they are different, setting to a null type
    // is the best thing we can do.
    if (!IdealType || !Other.IdealType || !IdealType->isEqual(Other.IdealType)) {
      IdealType = Type();
    }
    IsImpliedResult |= Other.IsImpliedResult;
    PreferNonVoid &= Other.PreferNonVoid;
    ExpectedCustomAttributeKinds |= Other.ExpectedCustomAttributeKinds;
  }

  Type getIdealType() const { return IdealType; }

  void setIdealType(Type IdealType) { this->IdealType = IdealType; }

  bool requiresNonVoid() const {
    if (IsImpliedResult)
      return false;
    if (PreferNonVoid)
      return true;
    if (PossibleTypes.empty())
      return false;
    return llvm::all_of(PossibleTypes, [](Type Ty) { return !Ty->isVoid(); });
  }

  bool isImpliedResult() const {
    return IsImpliedResult;
  }

  void setIsImpliedResult(bool IsImpliedResult) {
    this->IsImpliedResult = IsImpliedResult;
  }

  bool getPreferNonVoid() const { return PreferNonVoid; }

  void setPreferNonVoid(bool PreferNonVoid) {
    this->PreferNonVoid = PreferNonVoid;
  }

  OptionSet<CustomAttributeKind> getExpectedCustomAttributeKinds() const {
    return ExpectedCustomAttributeKinds;
  }

  void setExpectedCustomAttributeKinds(
      OptionSet<CustomAttributeKind> ExpectedAttributeKinds) {
    this->ExpectedCustomAttributeKinds = ExpectedAttributeKinds;
  }
};

/// Describes the relationship between the type of the completion results and
/// the expected type at the code completion position.
enum class CodeCompletionResultTypeRelation : uint8_t {
  /// The result does not have a type (e.g. keyword).
  NotApplicable,

  /// The type relation have not been calculated.
  Unknown,

  /// The relationship of the result's type to the expected type is not
  /// invalid, not convertible, and not identical.
  Unrelated,

  /// The result's type is invalid at the expected position.
  Invalid,

  /// The result's type is convertible or identical to the type of the expected.
  Convertible,

  MAX_VALUE = Convertible
};

class USRBasedType;

/// Holds \c USRBasedTypes. Two \c USRBasedTypes with the same USR are
/// represented by the same object if they live in the same arena.
class USRBasedTypeArena {
  friend class USRBasedType;

  /// The allocator allocating the \c USRBasedTypes
  llvm::BumpPtrAllocator Allocator;

  /// Maps USRs to their \c USRBasedType instances.
  llvm::StringMap<const USRBasedType *> CanonicalTypes;

  /// Cache of the \c Void type because its frequently needed to compute type
  /// relations.
  const USRBasedType *VoidType;

public:
  /// Return the USR-based \c Void type.
  const USRBasedType *getVoidType() const;

  USRBasedTypeArena();
};

/// The equivalent of a \c ExpectedTypeContext to compute the type relation of
/// \c USRBasedTypes.
class USRBasedTypeContext {
public:
  class ContextualType {
    /// The types that the result type must be identical/convertible to to
    /// compute the type relation. Needs to be a vector because for the
    /// conextual type `some MyProto & MyOtherProto`, the return type must be
    /// convertible to both \c MyProto and \c MyOtherProto to be considered
    /// convertible.
    llvm::SmallVector<const USRBasedType *, 1> Types;

  public:
    /// Compute the type relation of \p ResultType to this conextual type.
    CodeCompletionResultTypeRelation
    typeRelation(const USRBasedType *ResultType,
                 const USRBasedType *VoidType) const;

    ContextualType(ArrayRef<const USRBasedType *> Types)
        : Types(Types.begin(), Types.end()) {
      assert(!Types.empty() && "USRBasedTypeContext::ContextualType should "
                               "contain at least one type");
    }
  };

private:
  /// The arena in which the contextual types of this type context are
  /// allocated.
  const USRBasedTypeArena &Arena;

  SmallVector<ContextualType, 4> ContextualTypes;

  /// See \c ExpectedTypeContext::ExpectedAttributeKinds.
  OptionSet<CustomAttributeKind> ExpectedCustomAttributeKinds;

public:
  /// Create a USR-based equivalent of the \p TypeContext.
  USRBasedTypeContext(const ExpectedTypeContext *TypeContext,
                      USRBasedTypeArena &Arena);

  /// Type relation of \p ResultType to this context.
  CodeCompletionResultTypeRelation
  typeRelation(const USRBasedType *ResultType) const;
};

/// A type that is purely USR-based and thus independent of an \c ASTContext.
/// Its identity is defined by its USR. The object also stores the USRs of all
/// its known supertypes (its superclasses and protocol conformances declared
/// within the same module) so it is able to determine whether one type is
/// convertible to another.
/// Because the type is purely based on USRs, it can be serialized into the code
/// completion cache and read from it later.
/// \c USRBasedTypes are always allocated within a \c USRBasedTypeArena. Types
/// in the same arena that have the same USR are represented by the same object.
/// \c USRBasedTypes always represent canonical types so that equivalent types
/// are represented by the same USR.
/// A type with an empty USR represents the null type.
class USRBasedType {
  StringRef USR;

  /// The superclasses of the type and all protocol conformances that are
  /// declared in the same module that the type was defined.
  ArrayRef<const USRBasedType *> Supertypes;

  /// The kinds of attributes this type can be used as.
  OptionSet<CustomAttributeKind> CustomAttributeKinds;

  /// Memberwise initializer. \p USR and \p Supertypes need to be allocated in
  /// the same arena as this \c USRBasedType.
  USRBasedType(StringRef USR, ArrayRef<const USRBasedType *> Supertypes,
               OptionSet<CustomAttributeKind> CustomAttributeKinds)
      : USR(USR), Supertypes(Supertypes),
        CustomAttributeKinds(CustomAttributeKinds) {}

  /// Implementation of \c typeRelation. \p VisistedTypes keeps track which
  /// types have already been visited.
  CodeCompletionResultTypeRelation
  typeRelationImpl(const USRBasedType *ResultType, const USRBasedType *VoidType,
                   SmallPtrSetImpl<const USRBasedType *> &VisitedTypes) const;

public:
  /// A null \c USRBasedType that's represented by an empty USR and has no
  /// supertypes.
  static const USRBasedType *null(USRBasedTypeArena &Arena);

  /// Construct a \c USRBasedType from an AST-bound type. Computes the type's
  /// supertypes.
  static const USRBasedType *fromType(Type Ty, USRBasedTypeArena &Arena);

  /// Construct as \c USRBasedType from a USR and its supertypes. This method
  /// takes care of copying \p USR and \p Supertypes to \p Arena.
  static const USRBasedType *
  fromUSR(StringRef USR, ArrayRef<const USRBasedType *> Supertypes,
          OptionSet<CustomAttributeKind> CustomAttributeKinds,
          USRBasedTypeArena &Arena);

  StringRef getUSR() const { return USR; }
  ArrayRef<const USRBasedType *> getSupertypes() const { return Supertypes; }

  OptionSet<CustomAttributeKind> getCustomAttributeKinds() const {
    return CustomAttributeKinds;
  }

  /// Compute the relation of \c ResultType when to this contextual type.
  CodeCompletionResultTypeRelation
  typeRelation(const USRBasedType *ResultType,
               const USRBasedType *VoidType) const;
};

/// The type returned by a \c ContextFreeCodeCompletionResult. Can be either of
/// the following:
///  - NotApplicable: The completion result doesn't produce something that's
///    valid inside an expression like a keyword
///  - An empty list of types if the completion result produces something that's
///    valid inside an expression but the result type isn't known
///  - A list of proper type if the type produced by this completion result is
///    known
/// A \c CodeCompletionResultType does not have a single unique type because for
/// code completion we consider e.g. the expression \c Int as producing both an
/// \c Int metatype and an \c Int instance type. Thus we consider both the
/// instance and the metatype as result types of the expression \c Int.
class CodeCompletionResultType {
  /// \c ResultType1AndIsApplicable and \c ResultType2 store the following
  /// information:
  ///  - Whether the type is not applicable (see above). In this case
  ///    \c ResultType1 and \c ResultType2 should be \c nullptr.
  ///  - \c ResultType1 and \c ResultType2 form a collection of at most two
  ///    result types. A \c CodeCompletionResultType cannot have a single unique
  ///    type because for code completion we consider e.g. \c Int as producing
  ///    both an \c Int metatype and an \c Int instance.
  ///    It would be nice if we could use a SmallVector to store those types
  ///    instead but \c CodeCompletionResultType is allocated in a bump
  ///    allocator and freeing the bump allocator does not call the
  ///    SmallVecotor's destructor, leaking potential heap memory. Since we only
  ///    need two result types at the moment, I decided to implement it in a
  ///    hacky way with two pointers.
  /// The \c getResultTypes and \c isApplicable methods mask away this
  /// implementation detail.
  llvm::PointerIntPair<PointerUnion<Type, const USRBasedType *>, 1, bool>
      ResultType1AndIsApplicable;
  PointerUnion<Type, const USRBasedType *> ResultType2;

  llvm::SmallVector<PointerUnion<Type, const USRBasedType *>, 1>
  getResultTypes() const {
    if (ResultType1AndIsApplicable.getPointer() && ResultType2) {
      return {ResultType1AndIsApplicable.getPointer(), ResultType2};
    } else if (ResultType1AndIsApplicable.getPointer()) {
      return {ResultType1AndIsApplicable.getPointer()};
    } else {
      assert(
          !ResultType2 &&
          "We shouldn't have a second result type if there was no first one");
      return {};
    }
  }

  /// Memberwise initializer
  CodeCompletionResultType(bool IsApplicable,
                           PointerUnion<Type, const USRBasedType *> ResultType1,
                           PointerUnion<Type, const USRBasedType *> ResultType2)
      : ResultType1AndIsApplicable(ResultType1, IsApplicable),
        ResultType2(ResultType2) {}

public:
  static CodeCompletionResultType notApplicable() {
    return CodeCompletionResultType(/*IsApplicable=*/true,
                                    /*ResultType1=*/nullptr,
                                    /*ResultType2=*/nullptr);
  }

  static CodeCompletionResultType unknown() {
    return CodeCompletionResultType(ArrayRef<Type>());
  }

  explicit CodeCompletionResultType(ArrayRef<Type> Types)
      : CodeCompletionResultType(
            /*IsApplicable=*/false,
            /*ResultType1=*/Types.size() > 0 ? Types[0] : nullptr,
            /*ResultType2=*/Types.size() > 1 ? Types[1] : nullptr) {
    assert(Types.size() <= 2 && "Can only store two different result types in "
                                "CodeCompletionResultType");
  }

  explicit CodeCompletionResultType(Type Ty)
      : CodeCompletionResultType(ArrayRef<Type>(Ty)) {}

  explicit CodeCompletionResultType(ArrayRef<const USRBasedType *> Types)
      : CodeCompletionResultType(
            /*IsApplicable=*/false,
            /*ResultType1=*/Types.size() > 0 ? Types[0] : nullptr,
            /*ResultType2=*/Types.size() > 1 ? Types[1] : nullptr) {
    assert(Types.size() <= 2 && "Can only store two different result types in "
                                "CodeCompletionResultType");
  }

  /// Returns whether the \c CodeCompletionResult type is backed by USRs and
  /// thus not associated with any AST.
  /// Intended to be used in assertions.
  bool isBackedByUSRs() const;

  /// Returns whether the \c CodeCompletionREsultType is considered not
  /// applicable (see comment on \c CodeCompletionResultType).
  bool isNotApplicable() const { return ResultType1AndIsApplicable.getInt(); }

  /// Return the result types as a \c USRBasedTypes, converting an AST-bound
  /// type to a \c USRBasedTypes if necessary.
  llvm::SmallVector<const USRBasedType *, 1>
  getUSRBasedResultTypes(USRBasedTypeArena &Arena) const;

  /// Return the same \c CodeCompletionResultType with the guarantee that it is
  /// backed by USRs instead of AST-bound types
  CodeCompletionResultType usrBasedType(USRBasedTypeArena &Arena) const;

  /// Calculates the type relation of this type to the given type context. The
  /// caller is responsible for creating a \p USRTypeContext that matches the
  /// \p TypeContext and \p DC, because it might be able to pre-compute it for
  /// result types of multiple code completion items.
  ///
  /// \p USRTypeContext may be \c nullptr in one of the following situations:
  ///  - If \p TypeContext or \p DC are \c nullptr
  ///  - If the caller can guarantee that this \c CodeCompletionResultType is
  ///    AST-based
  /// In all other cases the \p USRTypeContext must contain types allocated in
  /// the same arena as the \c USRBasedType of this \c CodeCompletionResultType.
  CodeCompletionResultTypeRelation
  calculateTypeRelation(const ExpectedTypeContext *TypeContext,
                        const DeclContext *DC,
                        const USRBasedTypeContext *USRTypeContext) const;
};
} // namespace ide
} // namespace swift

#endif // SWIFT_IDE_CODECOMPLETIONRESULTTYPE_H
