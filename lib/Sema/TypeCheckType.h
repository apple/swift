//===--- TypeCheckType.h - Type Resolution Code ----------*- C++ -*-===//
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
//
//  This file defines utilities for resolving types.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_SEMA_TYPE_CHECK_TYPE_H
#define SWIFT_SEMA_TYPE_CHECK_TYPE_H

#include "swift/AST/Type.h"
#include "swift/AST/Types.h"
#include "swift/AST/TypeResolutionStage.h"
#include "swift/Basic/LangOptions.h"
#include "llvm/ADT/None.h"

namespace swift {

class ASTContext;
class TypeRepr;
class IdentTypeRepr;
class PackElementTypeRepr;
class GenericEnvironment;
class GenericSignature;
class SILTypeResolutionContext;

/// Flags that describe the context of type checking a pattern or
/// type.
enum class TypeResolutionFlags : uint16_t {
  /// Whether to allow unspecified types within a pattern.
  AllowUnspecifiedTypes = 1 << 0,

  /// Whether the given type can override the type of a typed pattern.
  OverrideType = 1 << 1,

  /// Whether we are validating the type for SIL.
  // FIXME: Move this flag to TypeResolverContext.
  SILType = 1 << 2,

  /// Whether we are parsing a SIL file.  Not the same as SILType,
  /// because the latter is not set if we're parsing an AST type.
  SILMode = 1 << 3,

  /// Whether this is a resolution based on a non-inferred type pattern.
  FromNonInferredPattern = 1 << 4,

  /// Whether we are at the direct base of a type expression.
  Direct = 1 << 5,

  /// Whether we should not produce diagnostics if the type is invalid.
  SilenceErrors = 1 << 6,

  /// Whether to allow module declaration types.
  AllowModule = 1 << 7,

  /// Make internal @usableFromInline and @inlinable decls visible.
  AllowUsableFromInline = 1 << 8,

  /// Forbid \c some types from resolving as opaque types.
  ///
  /// Needed to enforce that \c any P<some Q> does not resolve to a
  /// parameterized existential with an opaque type constraint.
  DisallowOpaqueTypes = 1 << 9,

  /// We are in a `@preconcurrency` declaration.
  Preconcurrency = 1 << 10,

  /// Whether references to type parameter packs are allowed.
  ///
  /// Pack references are only allowed inside pack expansions
  /// and in generic requirements.
  AllowPackReferences = 1 << 11,

  /// Whether this is a resolution based on a pack reference.
  FromPackReference = 1 << 12,

  /// Whether this resolution happens under an explicit ownership specifier.
  HasOwnership = 1 << 13,
};

/// Type resolution contexts that require special handling.
enum class TypeResolverContext : uint8_t {
  /// No special type handling is required.
  None,

  /// Whether we are checking generic arguments of a bound generic type.
  GenericArgument,

  /// Whether we are checking generic arguments of a parameterized protocol type.
  ProtocolGenericArgument,

  /// Whether we are checking a tuple element type.
  TupleElement,

  /// Whether we are checking a pack element type.
  PackElement,

  /// Whether we are checking the parameter list of a function.
  AbstractFunctionDecl,

  /// Whether we are checking the parameter list of a subscript.
  SubscriptDecl,

  /// Whether we are checking the parameter list of a closure.
  ClosureExpr,

  /// Whether we are in the input type of a function, or under one level of
  /// tuple type.  This is not set for multi-level tuple arguments.
  /// See also: TypeResolutionFlags::Direct
  FunctionInput,

  /// Whether this is a variadic function input.
  VariadicFunctionInput,

  /// Whether this is an 'inout' function input.
  InoutFunctionInput,

  /// Whether we are in the result type of a function.
  FunctionResult,

  /// Whether this is a pattern binding entry.
  PatternBindingDecl,

  /// Whether we are the variable type in a for/in statement.
  ForEachStmt,

  /// Whether we are binding an extension declaration, which limits
  /// the lookup.
  ExtensionBinding,

  /// Whether this type is being used in an expression or local declaration.
  ///
  /// This affects what sort of dependencies are recorded when resolving the
  /// type.
  InExpression,

  /// Whether this type is being used in a cast or coercion expression.
  ExplicitCastExpr,

  /// Whether this type is the value carried in an enum case.
  EnumElementDecl,

  /// Whether this type is a part of a macro declaration.
  MacroDecl,

  /// Whether this is the payload subpattern of an enum pattern.
  EnumPatternPayload,

  /// Whether we are checking the underlying type of a non-generic typealias.
  TypeAliasDecl,

  /// Whether we are checking the underlying type of a generic typealias.
  GenericTypeAliasDecl,

  /// Whether we are in the constraint type of an existential type.
  ExistentialConstraint,

  /// Whether we are in the constraint type of a conformance requirement.
  GenericRequirement,

  /// Whether we are in a same-type requirement of a generic
  /// declaration.
  SameTypeRequirement,

  /// Whether this is the base type of .Protocol
  ProtocolMetatypeBase,

  /// Whether this is the base type of .Type
  MetatypeBase,

  /// Whether we are in a type argument for an optional
  ImmediateOptionalTypeArgument,

  /// Whether this is the type of an editor placeholder.
  EditorPlaceholderExpr,

  /// Whether this is an inheritance clause of a concrete type.
  Inherited,

  /// Whether this is an inheritance clause of a generic parameter.
  GenericParameterInherited,

  /// Whether this is an inheritance clause of an associated type.
  AssociatedTypeInherited,

  /// Whether this is a custom attribute.
  CustomAttr
};

/// Options that determine how type resolution should work.
class TypeResolutionOptions {
  using Context = TypeResolverContext;

  // The "base" type resolution context. This never changes.
  Context base = Context::None;
  // The current type resolution context.
  Context context = Context::None;
  // TypeResolutionFlags
  uint16_t flags = 0;
  static_assert(sizeof(TypeResolutionOptions::flags) ==
                    sizeof(TypeResolutionFlags),
                "Flags size error");

public:
  ~TypeResolutionOptions() = default;
  TypeResolutionOptions(const TypeResolutionOptions &) = default;
  TypeResolutionOptions(TypeResolutionOptions &&) = default;
  TypeResolutionOptions &operator =(const TypeResolutionOptions &) = default;
  TypeResolutionOptions &operator =(TypeResolutionOptions &&) = default;

  // NOTE: Use either setContext() or explicit construction and assignment.
  void operator =(const Context &) = delete;
  void operator =(Context &&) = delete;

  // NOTE: "None" might be more permissive than one wants, therefore no
  // reasonable default context is possible.
  TypeResolutionOptions() = delete;

  TypeResolutionOptions(Context context) : base(context), context(context),
      flags(unsigned(TypeResolutionFlags::Direct)) {}
  // Helper forwarding constructors:
  TypeResolutionOptions(llvm::NoneType) : TypeResolutionOptions(Context::None){}

  /// Test the current type resolution base context.
  bool hasBase(Context context) const { return base == context; }

  /// Get the base type resolution context.
  Context getBaseContext() const { return base; }

  /// Test the current type resolution context.
  bool is(Context context) const { return this->context == context; }

  /// Get the current type resolution context.
  Context getContext() const { return context; }
  /// Set the current type resolution context.
  void setContext(Context newContext) {
    context = newContext;
    flags &= ~unsigned(TypeResolutionFlags::Direct);
  }
  void setContext(llvm::NoneType) { setContext(Context::None); }

  /// Get the current flags.
  TypeResolutionFlags getFlags() const { return TypeResolutionFlags(flags); }

  /// Is this type resolution context an expression.
  bool isAnyExpr() const {
    switch (base) {
    case Context::InExpression:
    case Context::ExplicitCastExpr:
    case Context::ForEachStmt:
    case Context::PatternBindingDecl:
    case Context::EditorPlaceholderExpr:
    case Context::ClosureExpr:
      return true;
    case Context::None:
    case Context::GenericArgument:
    case Context::ProtocolGenericArgument:
    case Context::TupleElement:
    case Context::PackElement:
    case Context::FunctionInput:
    case Context::VariadicFunctionInput:
    case Context::InoutFunctionInput:
    case Context::FunctionResult:
    case Context::ExtensionBinding:
    case Context::SubscriptDecl:
    case Context::EnumElementDecl:
    case Context::MacroDecl:
    case Context::EnumPatternPayload:
    case Context::TypeAliasDecl:
    case Context::GenericTypeAliasDecl:
    case Context::GenericRequirement:
    case Context::ExistentialConstraint:
    case Context::SameTypeRequirement:
    case Context::ProtocolMetatypeBase:
    case Context::MetatypeBase:
    case Context::ImmediateOptionalTypeArgument:
    case Context::AbstractFunctionDecl:
    case Context::Inherited:
    case Context::GenericParameterInherited:
    case Context::AssociatedTypeInherited:
    case Context::CustomAttr:
      return false;
    }
    llvm_unreachable("unhandled kind");
  }

  /// Whether a generic constraint type is implicitly an
  /// existential type in this context.
  bool isConstraintImplicitExistential() const {
    switch (context) {
    case Context::Inherited:
    case Context::GenericParameterInherited:
    case Context::AssociatedTypeInherited:
    case Context::ExtensionBinding:
    case Context::TypeAliasDecl:
    case Context::GenericTypeAliasDecl:
    case Context::GenericRequirement:
    case Context::ExistentialConstraint:
    case Context::MetatypeBase:
      return false;
    case Context::None:
    case Context::GenericArgument:
    case Context::ProtocolGenericArgument:
    case Context::PackElement:
    case Context::TupleElement:
    case Context::InExpression:
    case Context::ExplicitCastExpr:
    case Context::ForEachStmt:
    case Context::PatternBindingDecl:
    case Context::EditorPlaceholderExpr:
    case Context::ClosureExpr:
    case Context::FunctionInput:
    case Context::VariadicFunctionInput:
    case Context::InoutFunctionInput:
    case Context::FunctionResult:
    case Context::SubscriptDecl:
    case Context::EnumElementDecl:
    case Context::MacroDecl:
    case Context::EnumPatternPayload:
    case Context::SameTypeRequirement:
    case Context::ProtocolMetatypeBase:
    case Context::ImmediateOptionalTypeArgument:
    case Context::AbstractFunctionDecl:
    case Context::CustomAttr:
      return true;
    }
  }

  /// Whether pack expansion types are supported in this context.
  bool isPackExpansionSupported() const {
    switch (context) {
    case Context::FunctionInput:
    case Context::VariadicFunctionInput:
    case Context::PackElement:
    case Context::TupleElement:
    case Context::GenericArgument:
      return true;
    case Context::PatternBindingDecl:
    case Context::None:
    case Context::ProtocolGenericArgument:
    case Context::Inherited:
    case Context::GenericParameterInherited:
    case Context::AssociatedTypeInherited:
    case Context::ExtensionBinding:
    case Context::TypeAliasDecl:
    case Context::GenericTypeAliasDecl:
    case Context::GenericRequirement:
    case Context::ExistentialConstraint:
    case Context::MetatypeBase:
    case Context::InExpression:
    case Context::ExplicitCastExpr:
    case Context::ForEachStmt:
    case Context::EditorPlaceholderExpr:
    case Context::ClosureExpr:
    case Context::InoutFunctionInput:
    case Context::FunctionResult:
    case Context::SubscriptDecl:
    case Context::EnumElementDecl:
    case Context::MacroDecl:
    case Context::EnumPatternPayload:
    case Context::SameTypeRequirement:
    case Context::ProtocolMetatypeBase:
    case Context::ImmediateOptionalTypeArgument:
    case Context::AbstractFunctionDecl:
    case Context::CustomAttr:
      return false;
    }
  }

  /// Whether we are resolving a type in a `where` clause, generic parameter
  /// declaration inheritance clause, or associated type inheritance clause.
  bool isGenericRequirement() const {
    switch (base) {
    case Context::GenericRequirement:
    case Context::SameTypeRequirement:
    case Context::GenericParameterInherited:
    case Context::AssociatedTypeInherited:
      return true;

    case Context::None:
    case Context::Inherited:
    case Context::FunctionInput:
    case Context::PackElement:
    case Context::TupleElement:
    case Context::GenericArgument:
    case Context::ProtocolGenericArgument:
    case Context::ExtensionBinding:
    case Context::TypeAliasDecl:
    case Context::GenericTypeAliasDecl:
    case Context::ExistentialConstraint:
    case Context::MetatypeBase:
    case Context::InExpression:
    case Context::ExplicitCastExpr:
    case Context::ForEachStmt:
    case Context::PatternBindingDecl:
    case Context::EditorPlaceholderExpr:
    case Context::ClosureExpr:
    case Context::VariadicFunctionInput:
    case Context::InoutFunctionInput:
    case Context::FunctionResult:
    case Context::SubscriptDecl:
    case Context::EnumElementDecl:
    case Context::MacroDecl:
    case Context::EnumPatternPayload:
    case Context::ProtocolMetatypeBase:
    case Context::ImmediateOptionalTypeArgument:
    case Context::AbstractFunctionDecl:
    case Context::CustomAttr:
      return false;
    }
  }

  /// Determine whether all of the given options are set.
  bool contains(TypeResolutionFlags set) const {
    return !static_cast<bool>(unsigned(set) & ~unsigned(flags));
  }

  friend bool operator==(TypeResolutionOptions lhs, TypeResolutionOptions rhs) {
    return lhs.base == rhs.base && lhs.context == rhs.context &&
           lhs.flags == rhs.flags;
  }

  friend bool operator!=(TypeResolutionOptions lhs, TypeResolutionOptions rhs) {
    return !(lhs == rhs);
  }

  /// Produce type resolution options with additional flags.
  friend TypeResolutionOptions operator|(TypeResolutionOptions lhs,
                                         TypeResolutionFlags rhs) {
    return lhs |= rhs;
  }

  /// Merge additional flags into type resolution options.
  friend TypeResolutionOptions &operator|=(TypeResolutionOptions &lhs,
                                           TypeResolutionFlags rhs) {
    lhs.flags |= unsigned(rhs);
    return lhs;
  }

  /// Test whether any given flag is set in the type resolution options.
  friend bool operator&(TypeResolutionOptions lhs, TypeResolutionFlags rhs) {
    return lhs.flags & unsigned(rhs);
  }

  /// Produce type resolution options with removed flags.
  friend TypeResolutionOptions operator-(TypeResolutionOptions lhs,
                                         TypeResolutionFlags rhs) {
    return lhs -= rhs;
  }

  /// Remove the flags from the type resolution options.
  friend TypeResolutionOptions &operator-=(TypeResolutionOptions &lhs,
                                           TypeResolutionFlags rhs) {
    lhs.flags &= ~unsigned(rhs);
    return lhs;
  }
  /// Strip the contextual options from the given type resolution options.
  inline TypeResolutionOptions withoutContext(bool preserveSIL = false) const {
    auto copy = *this;
    copy.setContext(llvm::None);
    // FIXME: Move SILType to TypeResolverContext.
    if (!preserveSIL) copy -= TypeResolutionFlags::SILType;
    return copy;
  }

  inline
  TypeResolutionOptions withContext(TypeResolverContext context) const {
    auto copy = *this;
    copy.setContext(context);
    return copy;
  }
};

/// A function reference used to "open" the given unbound generic type
/// by introducing generic arguments and constructing a \c BoundGenericType
/// out of them.
///
/// \returns the \c null type on failure.
using OpenUnboundGenericTypeFn = llvm::function_ref<Type(UnboundGenericType *)>;

/// A function reference used to handle a PlaceholderTypeRepr.
using HandlePlaceholderTypeReprFn =
    llvm::function_ref<Type(ASTContext &, PlaceholderTypeRepr *)>;

/// A function reference used to replace pack elements with opened
/// element archetypes when resolving a \c PackElementTypeRepr.
using OpenPackElementFn =
    llvm::function_ref<Type(Type, PackElementTypeRepr *)>;

/// Handles the resolution of types within a given declaration context,
/// which might involve resolving generic parameters to a particular
/// stage.
class TypeResolution {
  DeclContext *dc;
  TypeResolutionStage stage;
  TypeResolutionOptions options;
  OpenUnboundGenericTypeFn unboundTyOpener;
  HandlePlaceholderTypeReprFn placeholderHandler;
  OpenPackElementFn packElementOpener;
  GenericSignature genericSig;

private:
  TypeResolution(DeclContext *dc, GenericSignature genericSig,
                 TypeResolutionStage stage, TypeResolutionOptions options,
                 OpenUnboundGenericTypeFn unboundTyOpener,
                 HandlePlaceholderTypeReprFn placeholderHandler,
                 OpenPackElementFn packElementOpener)
      : dc(dc), stage(stage), options(options),
        unboundTyOpener(unboundTyOpener),
        placeholderHandler(placeholderHandler),
        packElementOpener(packElementOpener), genericSig(genericSig) {}

public:
  /// Form a type resolution for the structure of a type, which does not
  /// attempt to resolve member types of type parameters to a particular
  /// associated type.
  static TypeResolution
  forStructural(DeclContext *dc, TypeResolutionOptions opts,
                OpenUnboundGenericTypeFn unboundTyOpener,
                HandlePlaceholderTypeReprFn placeholderHandler,
                OpenPackElementFn packElementOpener);

  /// Form a type resolution for an interface type, which is a complete
  /// description of the type using generic parameters.
  static TypeResolution
  forInterface(DeclContext *dc, TypeResolutionOptions opts,
               OpenUnboundGenericTypeFn unboundTyOpener,
               HandlePlaceholderTypeReprFn placeholderHandler,
               OpenPackElementFn packElementOpener);

  /// Form a type resolution for an interface type, which is a complete
  /// description of the type using generic parameters.
  static TypeResolution
  forInterface(DeclContext *dc, GenericSignature genericSig,
               TypeResolutionOptions opts,
               OpenUnboundGenericTypeFn unboundTyOpener,
               HandlePlaceholderTypeReprFn placeholderHandler,
               OpenPackElementFn packElementOpener);

  /// Form a type resolution for a contextual type, which is a complete
  /// description of the type using the archetypes of the given generic
  /// environment.
  static Type
  resolveContextualType(TypeRepr *TyR, DeclContext *dc,
                        TypeResolutionOptions opts,
                        OpenUnboundGenericTypeFn unboundTyOpener,
                        HandlePlaceholderTypeReprFn placeholderHandler,
                        OpenPackElementFn packElementOpener,
                        SILTypeResolutionContext *silContext = nullptr);

  static Type resolveContextualType(
      TypeRepr *TyR, DeclContext *dc, GenericSignature genericSig,
      TypeResolutionOptions opts, OpenUnboundGenericTypeFn unboundTyOpener,
      HandlePlaceholderTypeReprFn placeholderHandler,
      OpenPackElementFn packElementOpener,
      SILTypeResolutionContext *silContext = nullptr);

public:
  TypeResolution withOptions(TypeResolutionOptions opts) const;

  TypeResolution withoutPackElementOpener() const;

public:
  /// Retrieve the ASTContext in which this resolution occurs.
  ASTContext &getASTContext() const;

  /// Retrieve the declaration context in which type resolution will be
  /// performed.
  DeclContext *getDeclContext() const { return dc; }

  /// Retrieve the type resolution stage.
  TypeResolutionStage getStage() const { return stage; }

  TypeResolutionOptions getOptions() const { return options; }

  OpenUnboundGenericTypeFn getUnboundTypeOpener() const {
    return unboundTyOpener;
  }

  HandlePlaceholderTypeReprFn getPlaceholderHandler() const {
    return placeholderHandler;
  }

  OpenPackElementFn getPackElementOpener() const {
    return packElementOpener;
  }

  /// Retrieves the generic signature for the context, or NULL if there is
  /// no generic signature to resolve types.
  GenericSignature getGenericSignature() const;

  /// Resolves a TypeRepr to a type.
  ///
  /// Performs name lookup, checking of generic arguments, and so on in order
  /// to create a well-formed type.
  ///
  /// \param TyR The type representation to check.
  /// \param silContext Used to look up generic parameters in SIL mode.
  ///
  /// \returns A well-formed type that is never null, or an \c ErrorType in case of an error.
  Type resolveType(TypeRepr *TyR,
                   SILTypeResolutionContext *silContext = nullptr) const;

  /// Resolve a reference to a member type of the given (dependent) base and
  /// name.
  Type resolveDependentMemberType(Type baseTy, DeclContext *DC,
                                  SourceRange baseRange,
                                  IdentTypeRepr *repr) const;

  /// Determine whether the given two types are equivalent within this
  /// type resolution context.
  bool areSameType(Type type1, Type type2) const;

  /// Resolve a reference to the given type declaration within a particular
  /// context.
  ///
  /// This routine aids unqualified name lookup for types by performing the
  /// resolution necessary to rectify the declaration found by name lookup with
  /// the declaration context from which name lookup started.
  ///
  /// \param typeDecl The type declaration found by name lookup.
  /// \param foundDC The declaration context this type reference was found in.
  /// \param isSpecialized Whether the type will have generic arguments applied.
  ///
  /// \returns the resolved type.
  Type resolveTypeInContext(TypeDecl *typeDecl, DeclContext *foundDC,
                            bool isSpecialized) const;

  /// Apply generic arguments to the unbound generic type represented by the
  /// given declaration and parent type.
  ///
  /// This function requires the correct number of generic arguments,
  /// whereas applyGenericArguments emits diagnostics in those cases.
  ///
  /// \param decl The declaration that the resulting bound generic type
  /// shall reference.
  /// \param parentTy The parent type.
  /// \param loc The source location for diagnostic reporting.
  /// \param genericArgs The list of generic arguments to apply.
  ///
  /// \returns A BoundGenericType bound to the given arguments, or null on
  /// error.
  ///
  /// \see applyGenericArguments
  Type applyUnboundGenericArguments(GenericTypeDecl *decl, Type parentTy,
                                    SourceLoc loc,
                                    ArrayRef<Type> genericArgs) const;
};

} // end namespace swift

#endif /* SWIFT_SEMA_TYPE_CHECK_TYPE_H */
