//===--- TypeRepr.h - Swift Language Type Representation --------*- C++ -*-===//
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
// This file defines the TypeRepr and related classes.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_TYPEREPR_H
#define SWIFT_AST_TYPEREPR_H

#include "swift/AST/Attr.h"
#include "swift/AST/DeclContext.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/LifetimeDependence.h"
#include "swift/AST/Type.h"
#include "swift/AST/TypeAlignments.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/InlineBitfield.h"
#include "swift/Basic/Located.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/TrailingObjects.h"

namespace swift {
  class ASTContext;
  class ASTWalker;
  class DeclContext;
  class DeclRefTypeRepr;
  class TupleTypeRepr;
  class TypeDecl;

enum class ParamSpecifier : uint8_t;

enum class TypeReprKind : uint8_t {
#define TYPEREPR(ID, PARENT) ID,
#define LAST_TYPEREPR(ID) Last_TypeRepr = ID,
#include "TypeReprNodes.def"
};
enum : unsigned { NumTypeReprKindBits =
  countBitsUsed(static_cast<unsigned>(TypeReprKind::Last_TypeRepr)) };

class OpaqueReturnTypeRepr;
using CollectedOpaqueReprs = SmallVector<TypeRepr *, 2>;

/// Representation of a type as written in source.
class alignas(1 << TypeReprAlignInBits) TypeRepr
    : public ASTAllocated<TypeRepr> {
  TypeRepr(const TypeRepr&) = delete;
  void operator=(const TypeRepr&) = delete;

protected:
  // clang-format off
  union { uint64_t OpaqueBits;

  SWIFT_INLINE_BITFIELD_BASE(TypeRepr, bitmax(NumTypeReprKindBits,8)+1+1,
    /// The subclass of TypeRepr that this is.
    Kind : bitmax(NumTypeReprKindBits,8),

    /// Whether this type representation is known to contain an invalid
    /// type.
    Invalid : 1,

    /// Whether this type representation had a warning emitted related to it.
    /// This is a hack related to how we resolve type exprs multiple times in
    /// generic contexts.
    Warned : 1
  );

  SWIFT_INLINE_BITFIELD_FULL(TupleTypeRepr, TypeRepr, 32,
    /// The number of elements contained.
    NumElements : 32
  );

  SWIFT_INLINE_BITFIELD_FULL(DeclRefTypeRepr, TypeRepr, 1+32,
    /// Whether this instance has a valid angle bracket source range.
    HasAngleBrackets: 1,
    : NumPadBits,

    NumGenericArgs : 32
  );

  SWIFT_INLINE_BITFIELD_FULL(CompositionTypeRepr, TypeRepr, 32,
    : NumPadBits,
    NumTypes : 32
  );

  SWIFT_INLINE_BITFIELD_FULL(SILBoxTypeRepr, TypeRepr, 32,
    NumGenericArgs : NumPadBits,
    NumFields : 32
  );

  SWIFT_INLINE_BITFIELD_FULL(AttributedTypeRepr, TypeRepr, 32,
    : NumPadBits,
    NumAttributes : 32
  );

  SWIFT_INLINE_BITFIELD_FULL(PackTypeRepr, TypeRepr, 32,
    /// The number of elements contained.
    NumElements : 32
  );

  SWIFT_INLINE_BITFIELD_FULL(LifetimeDependentReturnTypeRepr, TypeRepr, 32,
      : NumPadBits,
      NumDependencies : 32
   );
  } Bits;
  // clang-format on

  TypeRepr(TypeReprKind K) {
    Bits.OpaqueBits = 0;
    Bits.TypeRepr.Kind = static_cast<unsigned>(K);
    Bits.TypeRepr.Invalid = false;
    Bits.TypeRepr.Warned = false;
  }

private:
  SourceLoc getLocImpl() const { return getStartLoc(); }

public:
  TypeReprKind getKind() const {
    return static_cast<TypeReprKind>(Bits.TypeRepr.Kind);
  }

  /// Is this type representation a protocol?
  bool isProtocolOrProtocolComposition(DeclContext *dc);

  /// Is this type representation known to be invalid?
  bool isInvalid() const { return Bits.TypeRepr.Invalid; }

  /// Note that this type representation describes an invalid type.
  void setInvalid() { Bits.TypeRepr.Invalid = true; }

  /// If a warning is produced about this type repr, keep track of that so we
  /// don't emit another one upon further reanalysis.
  bool isWarnedAbout() const { return Bits.TypeRepr.Warned; }
  void setWarned() { Bits.TypeRepr.Warned = true; }
  
  /// Get the representative location for pointing at this type.
  SourceLoc getLoc() const;

  SourceLoc getStartLoc() const;
  SourceLoc getEndLoc() const;
  SourceRange getSourceRange() const;

  /// Find an attribute with the provided kind and return its source location,
  /// or return an invalid source location if there is no such attribute.
  SourceLoc findAttrLoc(TypeAttrKind kind) const;

  /// Is this type grammatically a type-simple?
  inline bool isSimple() const; // bottom of this file

  static bool classof(const TypeRepr *T) { return true; }

  /// Walk this type representation.
  TypeRepr *walk(ASTWalker &walker);
  TypeRepr *walk(ASTWalker &&walker) {
    return walk(walker);
  }

  /// Look through the given type and its children to find a type for
  /// which the given predicate returns true.
  ///
  /// \param pred A predicate function object. It should return true if the
  /// given type node satisfies the criteria.
  ///
  /// \returns true if the predicate returns true for the given type or any of
  /// its children.
  bool findIf(llvm::function_ref<bool(TypeRepr *)> pred);

  /// Check recursively whether this type repr or any of its descendants are
  /// opaque return type reprs.
  bool hasOpaque();

  /// Returns a Boolean value indicating whether this written type is
  /// parenthesized, that is, matches the following grammar: `'(' type ')'`.
  bool isParenType() const;

  /// Retrieve the type repr without any parentheses around it.
  ///
  /// The use of this function must be restricted to contexts where
  /// user-written types are provided, and a syntactic analysis is appropriate.
  /// Most use cases should analyze the resolved \c Type instead and use
  /// \c Type::getCanonicalType() or \c Type::getWithoutParens().
  TypeRepr *getWithoutParens() const;

  /// Whether this is a `UnqualifiedIdentTypeRepr` with no generic arguments,
  /// matching the given identifier.
  bool isSimpleUnqualifiedIdentifier(Identifier identifier) const;

  /// Whether this is a `UnqualifiedIdentTypeRepr` with no generic arguments,
  /// matching the given string.
  bool isSimpleUnqualifiedIdentifier(StringRef str) const;

  //*** Allocation Routines ************************************************/

  void print(raw_ostream &OS, const PrintOptions &Opts = PrintOptions()) const;
  void print(ASTPrinter &Printer, const PrintOptions &Opts) const;
  SWIFT_DEBUG_DUMP;
  void dump(raw_ostream &OS, unsigned indent = 0) const;
};

/// A TypeRepr for a type with a syntax error.  Can be used both as a
/// top-level TypeRepr and as a part of other TypeRepr.
///
/// The client can either emit a detailed diagnostic at the construction time
/// (in the parser) or store a zero-arg diagnostic in this TypeRepr to be
/// emitted after parsing, during type resolution.
///
/// All uses of this type should be ignored and not re-diagnosed.
class ErrorTypeRepr : public TypeRepr {
  SourceRange Range;
  std::optional<ZeroArgDiagnostic> DelayedDiag;

  ErrorTypeRepr(SourceRange Range, std::optional<ZeroArgDiagnostic> Diag)
      : TypeRepr(TypeReprKind::Error), Range(Range), DelayedDiag(Diag) {}

public:
  static ErrorTypeRepr *
  create(ASTContext &Context, SourceRange Range,
         std::optional<ZeroArgDiagnostic> DelayedDiag = std::nullopt) {
    assert((!DelayedDiag || Range) && "diagnostic needs a location");
    return new (Context) ErrorTypeRepr(Range, DelayedDiag);
  }

  static ErrorTypeRepr *
  create(ASTContext &Context, SourceLoc Loc = SourceLoc(),
         std::optional<ZeroArgDiagnostic> DelayedDiag = std::nullopt) {
    return create(Context, SourceRange(Loc), DelayedDiag);
  }

  /// If there is a delayed diagnostic stored in this TypeRepr, consumes and
  /// emits that diagnostic.
  void dischargeDiagnostic(ASTContext &Context);

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::Error;
  }
  static bool classof(const ErrorTypeRepr *T) { return true; }

private:
  SourceLoc getStartLocImpl() const { return Range.Start; }
  SourceLoc getEndLocImpl() const { return Range.End; }
  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend class TypeRepr;
};

/// A type with attributes.
/// \code
///   @convention(thin) Foo
/// \endcode
class AttributedTypeRepr final
    : public TypeRepr,
      private llvm::TrailingObjects<AttributedTypeRepr, TypeOrCustomAttr> {
  TypeRepr *Ty;

  AttributedTypeRepr(ArrayRef<TypeOrCustomAttr> attrs, TypeRepr *Ty)
      : TypeRepr(TypeReprKind::Attributed), Ty(Ty) {
    assert(!attrs.empty());
    Bits.AttributedTypeRepr.NumAttributes = attrs.size();
    std::uninitialized_copy(attrs.begin(), attrs.end(),
                            getTrailingObjects<TypeOrCustomAttr>());

  }

  friend TrailingObjects;

public:
  static AttributedTypeRepr *create(const ASTContext &C,
                                    ArrayRef<TypeOrCustomAttr> attrs,
                                    TypeRepr *ty);

  ArrayRef<TypeOrCustomAttr> getAttrs() const {
    return llvm::ArrayRef(getTrailingObjects<TypeOrCustomAttr>(),
                          Bits.AttributedTypeRepr.NumAttributes);
  }

  TypeAttribute *get(TypeAttrKind kind) const;
  bool has(TypeAttrKind kind) const {
    return get(kind) != nullptr;
  }
  SourceLoc getAttrLoc(TypeAttrKind kind) const {
    auto attr = get(kind);
    return attr ? attr->getAttrLoc() : SourceLoc();
  }

  TypeRepr *getTypeRepr() const { return Ty; }

  /// Return the value of the first SIL ownership attribute (such as
  /// `sil_weak`) in these attributes, or ReferenceOwnership::Strong if
  /// there is no such attribute.
  ReferenceOwnership getSILOwnership() const;

  void printAttrs(llvm::raw_ostream &OS) const;
  void printAttrs(ASTPrinter &Printer, const PrintOptions &Options) const;

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::Attributed;
  }
  static bool classof(const AttributedTypeRepr *T) { return true; }

private:
  SourceLoc getStartLocImpl() const {
    auto attr = getAttrs().front();
    if (auto customAttr = attr.dyn_cast<CustomAttr*>()) {
      return customAttr->getStartLoc();
    } else {
      return attr.get<TypeAttribute*>()->getStartLoc();
    }
  }
  SourceLoc getEndLocImpl() const { return Ty->getEndLoc(); }
  SourceLoc getLocImpl() const { return Ty->getLoc(); }
  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend class TypeRepr;
};

class UnqualifiedIdentTypeRepr;

/// This is the abstract base class for types that directly reference a
/// type declaration. In written syntax, this type representation consists of
/// one or more components, separated by dots.
/// \code
///   Foo.Bar<Gen>
/// \endcode
class DeclRefTypeRepr : public TypeRepr {
  DeclNameLoc NameLoc;

  /// Either the identifier or declaration that describes this
  /// component.
  ///
  /// The initial parsed representation is always an identifier, and
  /// name lookup will resolve this to a specific declaration.
  llvm::PointerUnion<DeclNameRef, TypeDecl *> NameOrDecl;

  /// The declaration context from which the bound declaration was
  /// found. only valid if IdOrDecl is a TypeDecl.
  DeclContext *DC;

protected:
  explicit DeclRefTypeRepr(TypeReprKind K, DeclNameRef Name,
                           DeclNameLoc NameLoc, unsigned NumGenericArgs,
                           bool HasAngleBrackets);

public:
  static DeclRefTypeRepr *create(const ASTContext &C, TypeRepr *Base,
                                 DeclNameLoc NameLoc, DeclNameRef Name);

  static DeclRefTypeRepr *create(const ASTContext &C, TypeRepr *Base,
                                 DeclNameLoc NameLoc, DeclNameRef Name,
                                 ArrayRef<TypeRepr *> GenericArgs,
                                 SourceRange AngleBrackets);

  /// Returns the qualifier or base type representation. For example, `A.B`
  /// for `A.B.C`. The base of a `UnqualifiedIdentTypeRepr` is null.
  TypeRepr *getBase() const;

  /// Returns the root qualifier. For example, `A` for `A.B.C`. The root
  /// qualifier of a `UnqualifiedIdentTypeRepr` is itself.
  TypeRepr *getRoot();

  /// Returns the root qualifier. For example, `A` for `A.B.C`. The root
  /// qualifier of a `UnqualifiedIdentTypeRepr` is itself.
  const TypeRepr *getRoot() const;

  DeclNameLoc getNameLoc() const;
  DeclNameRef getNameRef() const;

  /// Replace the identifier with a new identifier, e.g., due to typo
  /// correction.
  void overwriteNameRef(DeclNameRef newId);

  /// Returns whether this instance has been bound to a type declaration. This
  /// happens during type resolution.
  bool isBound() const;

  /// Returns the type declaration this instance has been bound to.
  TypeDecl *getBoundDecl() const;

  DeclContext *getDeclContext() const;

  void setValue(TypeDecl *TD, DeclContext *DC);

  /// Returns whether this instance has a generic argument list. That is, either
  /// a valid angle bracket source range, or a positive number of generic
  /// arguments.
  bool hasGenericArgList() const;

  unsigned getNumGenericArgs() const;

  ArrayRef<TypeRepr *> getGenericArgs() const;

protected:
  /// Returns whether this instance has a valid angle bracket source range.
  bool hasAngleBrackets() const;

public:
  SourceRange getAngleBrackets() const;

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::UnqualifiedIdent ||
           T->getKind() == TypeReprKind::QualifiedIdent;
  }
  static bool classof(const DeclRefTypeRepr *T) { return true; }

protected:
  SourceLoc getLocImpl() const;
  SourceLoc getEndLocImpl() const;

  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;

  friend class TypeRepr;
};

/// An unqualified identifier type an optional set of generic arguments.
/// \code
///   Foo
///   Bar<Gen>
/// \endcode
class UnqualifiedIdentTypeRepr final
    : public DeclRefTypeRepr,
      private llvm::TrailingObjects<UnqualifiedIdentTypeRepr, TypeRepr *,
                                    SourceRange> {
  friend TrailingObjects;

  size_t numTrailingObjects(OverloadToken<TypeRepr *>) const {
    return getNumGenericArgs();
  }

  UnqualifiedIdentTypeRepr(DeclNameRef Name, DeclNameLoc NameLoc);

  UnqualifiedIdentTypeRepr(DeclNameRef Name, DeclNameLoc NameLoc,
                           ArrayRef<TypeRepr *> GenericArgs,
                           SourceRange AngleBrackets);

public:
  static UnqualifiedIdentTypeRepr *
  create(const ASTContext &C, DeclNameLoc NameLoc, DeclNameRef Name);

  static UnqualifiedIdentTypeRepr *create(const ASTContext &C,
                                          DeclNameLoc NameLoc, DeclNameRef Name,
                                          ArrayRef<TypeRepr *> GenericArgs,
                                          SourceRange AngleBrackets);

  ArrayRef<TypeRepr *> getGenericArgs() const;

  SourceRange getAngleBrackets() const;

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::UnqualifiedIdent;
  }
  static bool classof(const UnqualifiedIdentTypeRepr *T) { return true; }

protected:
  SourceLoc getStartLocImpl() const { return getNameLoc().getStartLoc(); }

  friend class TypeRepr;
};

/// A member type consisting of an arbitrary base component and one or more
/// identifier type member components.
/// \code
///   Foo.Bar<Gen>.Baz
///   [Int].Bar
/// \endcode
class QualifiedIdentTypeRepr final
    : public DeclRefTypeRepr,
      private llvm::TrailingObjects<QualifiedIdentTypeRepr, TypeRepr *,
                                    SourceRange> {
  friend TrailingObjects;

  /// The qualifier or base type representation. For example, `A.B` for `A.B.C`.
  TypeRepr *Base;

  size_t numTrailingObjects(OverloadToken<TypeRepr *>) const {
    return getNumGenericArgs();
  }

  QualifiedIdentTypeRepr(TypeRepr *Base, DeclNameRef Name, DeclNameLoc NameLoc);

  QualifiedIdentTypeRepr(TypeRepr *Base, DeclNameRef Name, DeclNameLoc NameLoc,
                         ArrayRef<TypeRepr *> GenericArgs,
                         SourceRange AngleBrackets);

public:
  static QualifiedIdentTypeRepr *create(const ASTContext &C, TypeRepr *Base,
                                        DeclNameLoc NameLoc, DeclNameRef Name);

  static QualifiedIdentTypeRepr *create(const ASTContext &C, TypeRepr *Base,
                                        DeclNameLoc NameLoc, DeclNameRef Name,
                                        ArrayRef<TypeRepr *> GenericArgs,
                                        SourceRange AngleBrackets);

  /// Returns the qualifier or base type representation. For example, `A.B`
  /// for `A.B.C`.
  TypeRepr *getBase() const;

  /// Returns the root qualifier. For example, `A` for `A.B.C`.
  TypeRepr *getRoot() const;

  ArrayRef<TypeRepr *> getGenericArgs() const;

  SourceRange getAngleBrackets() const;

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::QualifiedIdent;
  }
  static bool classof(const QualifiedIdentTypeRepr *T) { return true; }

private:
  SourceLoc getStartLocImpl() const;

  friend class TypeRepr;
};

/// A function type.
/// \code
///   (Foo) -> Bar
///   (Foo, Bar) -> Baz
///   (x: Foo, y: Bar) -> Baz
/// \endcode
class FunctionTypeRepr : public TypeRepr {
  // The generic params / signature / substitutions fields are only used
  // in SIL mode, which is the only time we can have polymorphic and
  // substituted function values.
  GenericParamList *GenericParams;
  GenericSignature GenericSig;
  ArrayRef<TypeRepr *> InvocationSubs;
  GenericParamList *PatternGenericParams;
  GenericSignature PatternGenericSig;
  ArrayRef<TypeRepr *> PatternSubs;

  TupleTypeRepr *ArgsTy;
  TypeRepr *RetTy;
  TypeRepr *ThrownTy;
  SourceLoc AsyncLoc;
  SourceLoc ThrowsLoc;
  SourceLoc ArrowLoc;

public:
  FunctionTypeRepr(GenericParamList *genericParams, TupleTypeRepr *argsTy,
                   SourceLoc asyncLoc, SourceLoc throwsLoc, 
                   TypeRepr *thrownTy,
                   SourceLoc arrowLoc,
                   TypeRepr *retTy,
                   GenericParamList *patternGenericParams = nullptr,
                   ArrayRef<TypeRepr *> patternSubs = {},
                   ArrayRef<TypeRepr *> invocationSubs = {})
    : TypeRepr(TypeReprKind::Function),
      GenericParams(genericParams),
      InvocationSubs(invocationSubs),
      PatternGenericParams(patternGenericParams),
      PatternSubs(patternSubs),
      ArgsTy(argsTy), RetTy(retTy), ThrownTy(thrownTy),
      AsyncLoc(asyncLoc), ThrowsLoc(throwsLoc), ArrowLoc(arrowLoc) {
  }

  GenericParamList *getGenericParams() const { return GenericParams; }
  GenericSignature getGenericSignature() const { return GenericSig; }

  GenericParamList *getPatternGenericParams() const {
    return PatternGenericParams;
  }
  GenericSignature getPatternGenericSignature() const {
    return PatternGenericSig;
  }

  ArrayRef<TypeRepr*> getPatternSubstitutions() const { return PatternSubs; }
  ArrayRef<TypeRepr*> getInvocationSubstitutions() const {
    return InvocationSubs;
  }

  void setPatternGenericSignature(GenericSignature genericSig) {
    assert(!PatternGenericSig);
    PatternGenericSig = genericSig;
  }

  void setGenericSignature(GenericSignature genericSig) {
    assert(!GenericSig);
    GenericSig = genericSig;
  }

  TupleTypeRepr *getArgsTypeRepr() const { return ArgsTy; }
  TypeRepr *getThrownTypeRepr() const { return ThrownTy; }
  TypeRepr *getResultTypeRepr() const { return RetTy; }
  bool isAsync() const { return AsyncLoc.isValid(); }
  bool isThrowing() const { return ThrowsLoc.isValid(); }

  SourceLoc getAsyncLoc() const { return AsyncLoc; }
  SourceLoc getThrowsLoc() const { return ThrowsLoc; }
  SourceLoc getArrowLoc() const { return ArrowLoc; }

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::Function;
  }
  static bool classof(const FunctionTypeRepr *T) { return true; }

private:
  SourceLoc getStartLocImpl() const;
  SourceLoc getEndLocImpl() const { return RetTy->getEndLoc(); }
  SourceLoc getLocImpl() const { return ArrowLoc; }

  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend class TypeRepr;
};

/// An array type.
/// \code
///   [Foo]
/// \endcode
class ArrayTypeRepr : public TypeRepr {
  TypeRepr *Base;
  SourceRange Brackets;

public:
  ArrayTypeRepr(TypeRepr *Base, SourceRange Brackets)
    : TypeRepr(TypeReprKind::Array), Base(Base), Brackets(Brackets) { }

  TypeRepr *getBase() const { return Base; }
  SourceRange getBrackets() const { return Brackets; }

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::Array;
  }
  static bool classof(const ArrayTypeRepr *T) { return true; }

private:
  SourceLoc getStartLocImpl() const { return Brackets.Start; }
  SourceLoc getEndLocImpl() const { return Brackets.End; }
  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend class TypeRepr;
};

/// A dictionary type.
/// \code
///   [K : V]
/// \endcode
class DictionaryTypeRepr : public TypeRepr {
  TypeRepr *Key;
  TypeRepr *Value;
  SourceLoc ColonLoc;
  SourceRange Brackets;

public:
  DictionaryTypeRepr(TypeRepr *key, TypeRepr *value,
                     SourceLoc colonLoc, SourceRange brackets)
    : TypeRepr(TypeReprKind::Dictionary), Key(key), Value(value),
      ColonLoc(colonLoc), Brackets(brackets) { }

  TypeRepr *getKey() const { return Key; }
  TypeRepr *getValue() const { return Value; }
  SourceRange getBrackets() const { return Brackets; }
  SourceLoc getColonLoc() const { return ColonLoc; }

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::Dictionary;
  }
  static bool classof(const DictionaryTypeRepr *T) { return true; }

private:
  SourceLoc getStartLocImpl() const { return Brackets.Start; }
  SourceLoc getEndLocImpl() const { return Brackets.End; }
  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend class TypeRepr;
};

/// An optional type.
/// \code
///   Foo?
/// \endcode
class OptionalTypeRepr : public TypeRepr {
  TypeRepr *Base;
  SourceLoc QuestionLoc;

public:
  OptionalTypeRepr(TypeRepr *Base, SourceLoc Question)
    : TypeRepr(TypeReprKind::Optional), Base(Base), QuestionLoc(Question) {
  }

  TypeRepr *getBase() const { return Base; }
  SourceLoc getQuestionLoc() const { return QuestionLoc; }

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::Optional;
  }

private:
  SourceLoc getStartLocImpl() const { return Base->getStartLoc(); }
  SourceLoc getEndLocImpl() const {
    return QuestionLoc.isValid() ? QuestionLoc : Base->getEndLoc();
  }
  SourceLoc getLocImpl() const {
    return QuestionLoc.isValid() ? QuestionLoc : Base->getLoc();
  }
  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend class TypeRepr;
};

/// An implicitly unwrapped optional type.
/// \code
///   Foo!
/// \endcode
class ImplicitlyUnwrappedOptionalTypeRepr : public TypeRepr {
  TypeRepr *Base;
  SourceLoc ExclamationLoc;

public:
  ImplicitlyUnwrappedOptionalTypeRepr(TypeRepr *Base, SourceLoc Exclamation)
    : TypeRepr(TypeReprKind::ImplicitlyUnwrappedOptional),
      Base(Base),
      ExclamationLoc(Exclamation) {}

  TypeRepr *getBase() const { return Base; }
  SourceLoc getExclamationLoc() const { return ExclamationLoc; }

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::ImplicitlyUnwrappedOptional;
  }

private:
  SourceLoc getStartLocImpl() const { return Base->getStartLoc(); }
  SourceLoc getEndLocImpl() const { return ExclamationLoc; }
  SourceLoc getLocImpl() const { return ExclamationLoc; }
  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend class TypeRepr;
};

/// A parsed element within a tuple type.
struct TupleTypeReprElement {
  Identifier Name;
  SourceLoc NameLoc;
  Identifier SecondName;
  SourceLoc SecondNameLoc;
  SourceLoc UnderscoreLoc;
  SourceLoc ColonLoc;
  TypeRepr *Type;
  SourceLoc TrailingCommaLoc;

  TupleTypeReprElement(): Type(nullptr) {}
  TupleTypeReprElement(TypeRepr *Type): Type(Type) {}
};

/// A vararg type 'T...' with element type 'T'.
class VarargTypeRepr final : public TypeRepr {
  TypeRepr *Element;
  SourceLoc EllipsisLoc;

public:
  VarargTypeRepr(TypeRepr *Element, SourceLoc EllipsisLoc)
  : TypeRepr(TypeReprKind::Vararg), Element(Element),
    EllipsisLoc(EllipsisLoc) {}

  TypeRepr *getElementType() const { return Element; }
  SourceLoc getEllipsisLoc() const { return EllipsisLoc; }

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::Vararg;
  }
  static bool classof(const VarargTypeRepr *T) { return true; }

private:
  SourceLoc getStartLocImpl() const { return Element->getEndLoc(); }
  SourceLoc getEndLocImpl() const { return EllipsisLoc; }
  SourceLoc getLocImpl() const { return EllipsisLoc; }
  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend class TypeRepr;
};

/// A pack expansion 'repeat T' with a pattern 'T'.
///
/// Can appear in the following positions:
/// - The type of a parameter declaration in a function declaration
/// - The type of a parameter in a function type
/// - The element of a tuple
class PackExpansionTypeRepr final : public TypeRepr {
  SourceLoc RepeatLoc;
  TypeRepr *Pattern;

public:
  PackExpansionTypeRepr(SourceLoc RepeatLoc, TypeRepr *Pattern)
    : TypeRepr(TypeReprKind::PackExpansion), RepeatLoc(RepeatLoc),
      Pattern(Pattern) {}

  SourceLoc getRepeatLoc() const { return RepeatLoc; }
  TypeRepr *getPatternType() const { return Pattern; }

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::PackExpansion;
  }
  static bool classof(const PackExpansionTypeRepr *T) { return true; }

private:
  SourceLoc getStartLocImpl() const { return RepeatLoc; }
  SourceLoc getEndLocImpl() const { return Pattern->getEndLoc(); }
  SourceLoc getLocImpl() const { return RepeatLoc; }
  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend class TypeRepr;
};

/// An explicit pack grouping, `Pack{...}`.
///
/// This allows packs to be explicitly grouped.  It is currently only
/// allowed in SIL files.
class PackTypeRepr final
    : public TypeRepr,
      private llvm::TrailingObjects<PackTypeRepr, TypeRepr *> {
  friend TrailingObjects;
  SourceLoc KeywordLoc;
  SourceRange BraceLocs;

  size_t numTrailingObjects(OverloadToken<TypeRepr*>) const {
    return Bits.PackTypeRepr.NumElements;
  }

  PackTypeRepr(SourceLoc keywordLoc, SourceRange braceLocs,
               ArrayRef<TypeRepr*> elements);
public:
  static PackTypeRepr *create(const ASTContext &ctx,
                              SourceLoc keywordLoc,
                              SourceRange braceLocs,
                              ArrayRef<TypeRepr*> elements);

  SourceLoc getKeywordLoc() const { return KeywordLoc; }
  SourceRange getBracesRange() const { return BraceLocs; }

  MutableArrayRef<TypeRepr*> getMutableElements() {
    return llvm::MutableArrayRef(getTrailingObjects<TypeRepr *>(),
                                 Bits.PackTypeRepr.NumElements);
  }
  ArrayRef<TypeRepr*> getElements() const {
    return llvm::ArrayRef(getTrailingObjects<TypeRepr *>(),
                          Bits.PackTypeRepr.NumElements);
  }

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::Pack;
  }
  static bool classof(const PackTypeRepr *T) { return true; }

private:
  SourceLoc getStartLocImpl() const { return KeywordLoc; }
  SourceLoc getEndLocImpl() const { return BraceLocs.End; }
  SourceLoc getLocImpl() const { return KeywordLoc; }
  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend class TypeRepr;
};

/// A pack reference spelled with the \c each keyword.
///
/// Pack references can only appear inside pack expansions and in
/// generic requirements.
///
/// \code
/// struct Generic<T...> {
///   func f(value: (each T)...) where each T: P {}
/// }
/// \endcode
class PackElementTypeRepr: public TypeRepr {
  TypeRepr *PackType;
  SourceLoc EachLoc;

public:
  PackElementTypeRepr(SourceLoc eachLoc, TypeRepr *packType)
    : TypeRepr(TypeReprKind::PackElement), PackType(packType),
      EachLoc(eachLoc) {}

  TypeRepr *getPackType() const { return PackType; }
  SourceLoc getEachLoc() const { return EachLoc; }

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::PackElement;
  }
  static bool classof(const PackElementTypeRepr *T) { return true; }

private:
  SourceLoc getStartLocImpl() const { return EachLoc; }
  SourceLoc getEndLocImpl() const { return PackType->getEndLoc(); }
  SourceLoc getLocImpl() const { return EachLoc; }
  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend class TypeRepr;
};

/// A tuple type.
/// \code
///   (Foo, Bar)
///   (x: Foo)
///   (_ x: Foo)
/// \endcode
class TupleTypeRepr final : public TypeRepr,
    private llvm::TrailingObjects<TupleTypeRepr, TupleTypeReprElement> {
  friend TrailingObjects;

  SourceRange Parens;
  
  size_t numTrailingObjects(OverloadToken<TupleTypeReprElement>) const {
    return Bits.TupleTypeRepr.NumElements;
  }

  TupleTypeRepr(ArrayRef<TupleTypeReprElement> Elements, SourceRange Parens);

public:
  unsigned getNumElements() const { return Bits.TupleTypeRepr.NumElements; }
  bool hasElementNames() const {
    for (auto &Element : getElements()) {
      if (Element.NameLoc.isValid()) {
        return true;
      }
    }
    return false;
  }

  ArrayRef<TupleTypeReprElement> getElements() const {
    return { getTrailingObjects<TupleTypeReprElement>(),
             Bits.TupleTypeRepr.NumElements };
  }

  void getElementTypes(SmallVectorImpl<TypeRepr *> &Types) const {
    for (auto &Element : getElements()) {
      Types.push_back(Element.Type);
    }
  }

  TypeRepr *getElementType(unsigned i) const {
    return getElement(i).Type;
  }

  TupleTypeReprElement getElement(unsigned i) const {
    return getElements()[i];
  }

  void getElementNames(SmallVectorImpl<Identifier> &Names) {
    for (auto &Element : getElements()) {
      Names.push_back(Element.Name);
    }
  }

  Identifier getElementName(unsigned i) const {
    return getElement(i).Name;
  }

  SourceLoc getElementNameLoc(unsigned i) const {
    return getElement(i).NameLoc;
  }

  SourceLoc getUnderscoreLoc(unsigned i) const {
    return getElement(i).UnderscoreLoc;
  }

  bool isNamedParameter(unsigned i) const {
    return getUnderscoreLoc(i).isValid();
  }

  SourceRange getParens() const { return Parens; }

  bool isParenType() const {
    return Bits.TupleTypeRepr.NumElements == 1 &&
           getElementNameLoc(0).isInvalid() &&
           !isa<PackExpansionTypeRepr>(getElementType(0));
  }

  static TupleTypeRepr *create(const ASTContext &C,
                               ArrayRef<TupleTypeReprElement> Elements,
                               SourceRange Parens);
  static TupleTypeRepr *createEmpty(const ASTContext &C, SourceRange Parens);

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::Tuple;
  }
  static bool classof(const TupleTypeRepr *T) { return true; }

private:
  SourceLoc getStartLocImpl() const { return Parens.Start; }
  SourceLoc getEndLocImpl() const { return Parens.End; }
  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend class TypeRepr;
};

/// A type composite type.
/// \code
///   Foo & Bar
/// \endcode
class CompositionTypeRepr final : public TypeRepr,
    private llvm::TrailingObjects<CompositionTypeRepr, TypeRepr*> {
  friend TrailingObjects;
  SourceLoc FirstTypeLoc;
  SourceRange CompositionRange;

  CompositionTypeRepr(ArrayRef<TypeRepr *> Types,
                      SourceLoc FirstTypeLoc,
                      SourceRange CompositionRange)
      : TypeRepr(TypeReprKind::Composition), FirstTypeLoc(FirstTypeLoc),
        CompositionRange(CompositionRange) {
    Bits.CompositionTypeRepr.NumTypes = Types.size();
    std::uninitialized_copy(Types.begin(), Types.end(),
                            getTrailingObjects<TypeRepr*>());
  }

public:
  ArrayRef<TypeRepr *> getTypes() const {
    return {getTrailingObjects<TypeRepr*>(), Bits.CompositionTypeRepr.NumTypes};
  }
  SourceLoc getSourceLoc() const { return FirstTypeLoc; }
  SourceRange getCompositionRange() const { return CompositionRange; }

  /// 'Any' is understood as CompositionTypeRepr by the compiler but its type array will be empty
  ///  becasue it is a  nonspecific type
  bool isTypeReprAny() {
        return getTypes().size() == 0 ?  true : false;
  }
  
  static CompositionTypeRepr *create(const ASTContext &C,
                                     ArrayRef<TypeRepr*> Protocols,
                                     SourceLoc FirstTypeLoc,
                                     SourceRange CompositionRange);
  
  static CompositionTypeRepr *createEmptyComposition(ASTContext &C,
                                                     SourceLoc AnyLoc) {
    return CompositionTypeRepr::create(C, {}, AnyLoc, {AnyLoc, AnyLoc});
  }
  
  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::Composition;
  }
  static bool classof(const CompositionTypeRepr *T) { return true; }

private:
  SourceLoc getStartLocImpl() const { return FirstTypeLoc; }
  SourceLoc getLocImpl() const { return CompositionRange.Start; }
  SourceLoc getEndLocImpl() const { return CompositionRange.End; }
  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend class TypeRepr;
};

/// A 'metatype' type.
/// \code
///   Foo.Type
/// \endcode
class MetatypeTypeRepr : public TypeRepr {
  TypeRepr *Base;
  SourceLoc MetaLoc;

public:
  MetatypeTypeRepr(TypeRepr *Base, SourceLoc MetaLoc)
    : TypeRepr(TypeReprKind::Metatype), Base(Base), MetaLoc(MetaLoc) {
  }

  TypeRepr *getBase() const { return Base; }
  SourceLoc getMetaLoc() const { return MetaLoc; }

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::Metatype;
  }
  static bool classof(const MetatypeTypeRepr *T) { return true; }

private:
  SourceLoc getStartLocImpl() const { return Base->getStartLoc(); }
  SourceLoc getEndLocImpl() const { return MetaLoc; }
  SourceLoc getLocImpl() const { return MetaLoc; }
  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend class TypeRepr;
};

/// A 'protocol' type.
/// \code
///   Foo.Protocol
/// \endcode
class ProtocolTypeRepr : public TypeRepr {
  TypeRepr *Base;
  SourceLoc ProtocolLoc;

public:
  ProtocolTypeRepr(TypeRepr *Base, SourceLoc ProtocolLoc)
    : TypeRepr(TypeReprKind::Protocol), Base(Base), ProtocolLoc(ProtocolLoc) {
  }

  TypeRepr *getBase() const { return Base; }
  SourceLoc getProtocolLoc() const { return ProtocolLoc; }

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::Protocol;
  }
  static bool classof(const ProtocolTypeRepr *T) { return true; }

private:
  SourceLoc getStartLocImpl() const { return Base->getStartLoc(); }
  SourceLoc getEndLocImpl() const { return ProtocolLoc; }
  SourceLoc getLocImpl() const { return ProtocolLoc; }
  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend class TypeRepr;
};

class SpecifierTypeRepr : public TypeRepr {
  TypeRepr *Base;
  SourceLoc SpecifierLoc;
  
public:
  SpecifierTypeRepr(TypeReprKind Kind, TypeRepr *Base, SourceLoc Loc)
    : TypeRepr(Kind), Base(Base), SpecifierLoc(Loc) {
    // ensure the kind passed-in matches up with our TypeRepr classof.
    assert(SpecifierTypeRepr::classof(cast<TypeRepr>(this)));
  }
  
  TypeRepr *getBase() const { return Base; }
  SourceLoc getSpecifierLoc() const { return SpecifierLoc; }
  
  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::Ownership ||
           T->getKind() == TypeReprKind::Isolated ||
           T->getKind() == TypeReprKind::CompileTimeConst ||
           T->getKind() == TypeReprKind::LifetimeDependentReturn ||
           T->getKind() == TypeReprKind::Transferring ||
           T->getKind() == TypeReprKind::Sending;
  }
  static bool classof(const SpecifierTypeRepr *T) { return true; }
  
private:
  SourceLoc getStartLocImpl() const { return SpecifierLoc; }
  SourceLoc getEndLocImpl() const { return Base->getEndLoc(); }
  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend class TypeRepr;
};

/// A parameter type with an ownership specifier, such as `inout`, `borrowing`,
/// or `consuming`.
/// \code
///   x : inout Int
///   y : consuming Int
///   z : borrowing Int
/// \endcode

class OwnershipTypeRepr : public SpecifierTypeRepr {
  ParamSpecifier Specifier;
public:
  OwnershipTypeRepr(TypeRepr *Base, ParamSpecifier Specifier,
                    SourceLoc ModifierLoc)
    : SpecifierTypeRepr(TypeReprKind::Ownership, Base, ModifierLoc),
      Specifier(Specifier) {}
  
  ParamSpecifier getSpecifier() const { return Specifier; }
  
  /// Return the \c ValueOwnership kind that corresponds to the specifier.
  ValueOwnership getValueOwnership() const;
  
  /// Return the spelling of the ownership specifier as a string.
  StringRef getSpecifierSpelling() const;

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::Ownership;
  }
  static bool classof(const OwnershipTypeRepr *T) { return true; }
};
  
/// An 'isolated' type.
/// \code
///   x : isolated Actor
/// \endcode
class IsolatedTypeRepr : public SpecifierTypeRepr {
public:
  IsolatedTypeRepr(TypeRepr *Base, SourceLoc InOutLoc)
    : SpecifierTypeRepr(TypeReprKind::Isolated, Base, InOutLoc) {}

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::Isolated;
  }
  static bool classof(const IsolatedTypeRepr *T) { return true; }
};

/// An '_const' type.
/// \code
///   x : _const Int
/// \endcode
class CompileTimeConstTypeRepr : public SpecifierTypeRepr {
public:
  CompileTimeConstTypeRepr(TypeRepr *Base, SourceLoc InOutLoc)
    : SpecifierTypeRepr(TypeReprKind::CompileTimeConst, Base, InOutLoc) {}

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::CompileTimeConst;
  }
  static bool classof(const CompileTimeConstTypeRepr *T) { return true; }
};

/// A transferring type.
/// \code
///   x : transferring Int
/// \endcode
class TransferringTypeRepr : public SpecifierTypeRepr {
public:
  TransferringTypeRepr(TypeRepr *Base, SourceLoc InOutLoc)
      : SpecifierTypeRepr(TypeReprKind::Transferring, Base, InOutLoc) {}

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::Transferring;
  }
  static bool classof(const TransferringTypeRepr *T) { return true; }
};

/// A sending type.
/// \code
///   x : sending Int
/// \endcode
class SendingTypeRepr : public SpecifierTypeRepr {
public:
  SendingTypeRepr(TypeRepr *Base, SourceLoc Loc)
      : SpecifierTypeRepr(TypeReprKind::Sending, Base, Loc) {}

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::Sending;
  }
  static bool classof(const SendingTypeRepr *T) { return true; }
};

/// A TypeRepr for a known, fixed type.
///
/// Fixed type representations should be used sparingly, in places
/// where we need to specify some type (usually some built-in type)
/// that cannot be spelled in the language proper.
class FixedTypeRepr : public TypeRepr {
  Type Ty;
  SourceLoc Loc;

public:
  FixedTypeRepr(Type Ty, SourceLoc Loc)
    : TypeRepr(TypeReprKind::Fixed), Ty(Ty), Loc(Loc) {}

  // SmallVector::emplace_back will never need to call this because
  // we reserve the right size, but it does try statically.
  FixedTypeRepr(const FixedTypeRepr &repr) : FixedTypeRepr(repr.Ty, repr.Loc) {
    llvm_unreachable("should not be called dynamically");
  }

  /// Retrieve the location.
  SourceLoc getLoc() const { return Loc; }

  /// Retrieve the fixed type.
  Type getType() const { return Ty; }

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::Fixed;
  }
  static bool classof(const FixedTypeRepr *T) { return true; }

private:
  SourceLoc getStartLocImpl() const { return Loc; }
  SourceLoc getEndLocImpl() const { return Loc; }
  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend class TypeRepr;
};

/// A TypeRepr for uses of 'Self' in the type of a declaration.
class SelfTypeRepr : public TypeRepr {
  Type Ty;
  SourceLoc Loc;

public:
  SelfTypeRepr(Type Ty, SourceLoc Loc)
    : TypeRepr(TypeReprKind::Self), Ty(Ty), Loc(Loc) {}

  /// Retrieve the location.
  SourceLoc getLoc() const { return Loc; }

  /// Retrieve the fixed type.
  Type getType() const { return Ty; }

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::Self;
  }
  static bool classof(const SelfTypeRepr *T) { return true; }

private:
  SourceLoc getStartLocImpl() const { return Loc; }
  SourceLoc getEndLocImpl() const { return Loc; }
  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend class TypeRepr;
};

class SILBoxTypeReprField {
  SourceLoc VarOrLetLoc;
  llvm::PointerIntPair<TypeRepr*, 1, bool> FieldTypeAndMutable;

public:
  SILBoxTypeReprField(SourceLoc loc, bool isMutable, TypeRepr *fieldType)
     : VarOrLetLoc(loc), FieldTypeAndMutable(fieldType, isMutable) {
  }

  SourceLoc getLoc() const { return VarOrLetLoc; }
  TypeRepr *getFieldType() const { return FieldTypeAndMutable.getPointer(); }
  bool isMutable() const { return FieldTypeAndMutable.getInt(); }
};

/// A TypeRepr for anonymous opaque return types.
///
/// This can occur in the return type of a function declaration, or the type of
/// a property, to specify that the concrete return type should be abstracted
/// from callers, given a set of generic constraints that the concrete return
/// type satisfies:
///
/// func foo() -> some Collection { return [1,2,3] }
/// var bar: some SignedInteger = 1
///
/// It is currently illegal for this to appear in any other position.
class OpaqueReturnTypeRepr : public TypeRepr {
  /// The type repr for the immediate constraints on the opaque type.
  /// In valid code this must resolve to a class, protocol, or composition type.
  TypeRepr *Constraint;
  SourceLoc OpaqueLoc;
  
public:
  OpaqueReturnTypeRepr(SourceLoc opaqueLoc, TypeRepr *constraint)
    : TypeRepr(TypeReprKind::OpaqueReturn), Constraint(constraint),
      OpaqueLoc(opaqueLoc)
  {}
  
  TypeRepr *getConstraint() const { return Constraint; }
  SourceLoc getOpaqueLoc() const { return OpaqueLoc; }
  
  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::OpaqueReturn;
  }
  static bool classof(const OpaqueReturnTypeRepr *T) { return true; }

private:
  SourceLoc getStartLocImpl() const { return OpaqueLoc; }
  SourceLoc getEndLocImpl() const { return Constraint->getEndLoc(); }
  SourceLoc getLocImpl() const { return OpaqueLoc; }
  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend class TypeRepr;
};

/// A TypeRepr for a type with a generic parameter list of named opaque return
/// types.
///
/// This can occur only as the return type of a function declaration, or the
/// type of a property, to specify types which should be abstracted from
/// callers, given a set of generic constraints that the concrete types satisfy:
///
/// func foo() -> <T: Collection> T { return [1] }
class NamedOpaqueReturnTypeRepr : public TypeRepr {
  TypeRepr *Base;
  GenericParamList *GenericParams;

public:
  NamedOpaqueReturnTypeRepr(TypeRepr *Base, GenericParamList *GenericParams)
      : TypeRepr(TypeReprKind::NamedOpaqueReturn), Base(Base),
        GenericParams(GenericParams) {
    assert(Base && GenericParams);
  }

  TypeRepr *getBase() const { return Base; }
  GenericParamList *getGenericParams() const { return GenericParams; }

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::NamedOpaqueReturn;
  }
  static bool classof(const NamedOpaqueReturnTypeRepr *T) { return true; }

private:
  SourceLoc getStartLocImpl() const;
  SourceLoc getEndLocImpl() const;
  SourceLoc getLocImpl() const;
  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend class TypeRepr;
};

/// A TypeRepr for an existential type spelled with \c any
///
/// Can appear anywhere a normal existential type would. This is
/// purely a more explicit spelling for existential types.
class ExistentialTypeRepr: public TypeRepr {
  TypeRepr *Constraint;
  SourceLoc AnyLoc;

public:
  ExistentialTypeRepr(SourceLoc anyLoc, TypeRepr *constraint)
    : TypeRepr(TypeReprKind::Existential), Constraint(constraint),
      AnyLoc(anyLoc) {}

  TypeRepr *getConstraint() const { return Constraint; }
  SourceLoc getAnyLoc() const { return AnyLoc; }

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::Existential;
  }
  static bool classof(const ExistentialTypeRepr *T) { return true; }

private:
  SourceLoc getStartLocImpl() const { return AnyLoc; }
  SourceLoc getEndLocImpl() const { return Constraint->getEndLoc(); }
  SourceLoc getLocImpl() const { return AnyLoc; }
  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend class TypeRepr;
};

/// A type repr representing the inverse of some constraint. For example,
///    ~Copyable
/// where `Copyable` is the constraint type.
class InverseTypeRepr : public TypeRepr {
  TypeRepr *Constraint;
  SourceLoc TildeLoc;

public:
  InverseTypeRepr(SourceLoc tildeLoc, TypeRepr *constraint)
      : TypeRepr(TypeReprKind::Inverse), Constraint(constraint),
        TildeLoc(tildeLoc) {}

  TypeRepr *getConstraint() const { return Constraint; }
  SourceLoc getTildeLoc() const { return TildeLoc; }

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::Inverse;
  }
  static bool classof(const InverseTypeRepr *T) { return true; }

private:
  SourceLoc getStartLocImpl() const { return TildeLoc; }
  SourceLoc getEndLocImpl() const { return Constraint->getEndLoc(); }
  SourceLoc getLocImpl() const { return TildeLoc; }
  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend class TypeRepr;
};

/// TypeRepr for a user-specified placeholder (essentially, a user-facing
/// representation of an anonymous type variable.
///
/// Can occur anywhere a normal type would occur, though usually expected to be
/// used in structural positions like \c Generic<Int,_>.
class PlaceholderTypeRepr: public TypeRepr {
  SourceLoc UnderscoreLoc;

public:
  PlaceholderTypeRepr(SourceLoc loc)
    : TypeRepr(TypeReprKind::Placeholder), UnderscoreLoc(loc)
  {}

    SourceLoc getUnderscoreLoc() const { return UnderscoreLoc; }

    static bool classof(const TypeRepr *T) {
      return T->getKind() == TypeReprKind::Placeholder;
    }
    static bool classof(const PlaceholderTypeRepr *T) { return true; }

  private:
    SourceLoc getStartLocImpl() const { return UnderscoreLoc; }
    SourceLoc getEndLocImpl() const { return UnderscoreLoc; }
    SourceLoc getLocImpl() const { return UnderscoreLoc; }
    void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
    friend class TypeRepr;
};

/// SIL-only TypeRepr for box types.
///
/// Boxes are either concrete: { var Int, let String }
/// or generic:                <T: Runcible> { var T, let String } <Int>
class SILBoxTypeRepr final : public TypeRepr,
    private llvm::TrailingObjects<SILBoxTypeRepr,
                                  SILBoxTypeReprField, TypeRepr *> {
  friend TrailingObjects;
  GenericParamList *GenericParams;
  GenericSignature GenericSig;

  SourceLoc LBraceLoc, RBraceLoc;
  SourceLoc ArgLAngleLoc, ArgRAngleLoc;

  size_t numTrailingObjects(OverloadToken<SILBoxTypeReprField>) const {
    return Bits.SILBoxTypeRepr.NumFields;
  }
  size_t numTrailingObjects(OverloadToken<TypeRepr*>) const {
    return Bits.SILBoxTypeRepr.NumGenericArgs;
  }
  
public:
  using Field = SILBoxTypeReprField;

  SILBoxTypeRepr(GenericParamList *GenericParams,
                 SourceLoc LBraceLoc, ArrayRef<Field> Fields,
                 SourceLoc RBraceLoc,
                 SourceLoc ArgLAngleLoc, ArrayRef<TypeRepr *> GenericArgs,
                 SourceLoc ArgRAngleLoc)
    : TypeRepr(TypeReprKind::SILBox),
      GenericParams(GenericParams), LBraceLoc(LBraceLoc), RBraceLoc(RBraceLoc),
      ArgLAngleLoc(ArgLAngleLoc), ArgRAngleLoc(ArgRAngleLoc)
  {
    Bits.SILBoxTypeRepr.NumFields = Fields.size();
    Bits.SILBoxTypeRepr.NumGenericArgs = GenericArgs.size();

    std::uninitialized_copy(Fields.begin(), Fields.end(),
                            getTrailingObjects<SILBoxTypeReprField>());

    std::uninitialized_copy(GenericArgs.begin(), GenericArgs.end(),
                            getTrailingObjects<TypeRepr*>());
  }
  
  static SILBoxTypeRepr *create(ASTContext &C,
                      GenericParamList *GenericParams,
                      SourceLoc LBraceLoc, ArrayRef<Field> Fields,
                      SourceLoc RBraceLoc,
                      SourceLoc ArgLAngleLoc, ArrayRef<TypeRepr *> GenericArgs,
                      SourceLoc ArgRAngleLoc);
  
  void setGenericSignature(GenericSignature Sig) {
    assert(!GenericSig);
    GenericSig = Sig;
  }
  
  ArrayRef<Field> getFields() const {
    return {getTrailingObjects<Field>(),
            Bits.SILBoxTypeRepr.NumFields};
  }
  ArrayRef<TypeRepr *> getGenericArguments() const {
    return {getTrailingObjects<TypeRepr*>(),
            static_cast<size_t>(Bits.SILBoxTypeRepr.NumGenericArgs)};
  }
  
  GenericParamList *getGenericParams() const {
    return GenericParams;
  }
  GenericSignature getGenericSignature() const {
    return GenericSig;
  }

  SourceLoc getLBraceLoc() const { return LBraceLoc; }
  SourceLoc getRBraceLoc() const { return RBraceLoc; }
  SourceLoc getArgumentLAngleLoc() const { return ArgLAngleLoc; }
  SourceLoc getArgumentRAngleLoc() const { return ArgRAngleLoc; }

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::SILBox;
  }
  static bool classof(const SILBoxTypeRepr *T) { return true; }
  
private:
  SourceLoc getStartLocImpl() const;
  SourceLoc getEndLocImpl() const;
  SourceLoc getLocImpl() const;
  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend TypeRepr;
};

class LifetimeDependentReturnTypeRepr final
    : public SpecifierTypeRepr,
      private llvm::TrailingObjects<LifetimeDependentReturnTypeRepr,
                                    LifetimeDependenceSpecifier> {
  friend TrailingObjects;

  size_t
  numTrailingObjects(OverloadToken<LifetimeDependentReturnTypeRepr>) const {
    return Bits.LifetimeDependentReturnTypeRepr.NumDependencies;
  }

public:
  LifetimeDependentReturnTypeRepr(
      TypeRepr *base, ArrayRef<LifetimeDependenceSpecifier> specifiers)
      : SpecifierTypeRepr(TypeReprKind::LifetimeDependentReturn, base,
                          specifiers.front().getLoc()) {
    assert(base);
    Bits.LifetimeDependentReturnTypeRepr.NumDependencies = specifiers.size();
    std::uninitialized_copy(specifiers.begin(), specifiers.end(),
                            getTrailingObjects<LifetimeDependenceSpecifier>());
  }

  static LifetimeDependentReturnTypeRepr *
  create(ASTContext &C, TypeRepr *base,
         ArrayRef<LifetimeDependenceSpecifier> specifiers);

  ArrayRef<LifetimeDependenceSpecifier> getLifetimeDependencies() const {
    return {getTrailingObjects<LifetimeDependenceSpecifier>(),
            Bits.LifetimeDependentReturnTypeRepr.NumDependencies};
  }

  static bool classof(const TypeRepr *T) {
    return T->getKind() == TypeReprKind::LifetimeDependentReturn;
  }
  static bool classof(const LifetimeDependentReturnTypeRepr *T) { return true; }

private:
  SourceLoc getStartLocImpl() const;
  SourceLoc getEndLocImpl() const;
  SourceLoc getLocImpl() const;
  void printImpl(ASTPrinter &Printer, const PrintOptions &Opts) const;
  friend class TypeRepr;
};

inline bool TypeRepr::isSimple() const {
  // NOTE: Please keep this logic in sync with TypeBase::hasSimpleTypeRepr().
  switch (getKind()) {
  case TypeReprKind::Attributed:
  case TypeReprKind::Error:
  case TypeReprKind::Function:
  case TypeReprKind::Ownership:
  case TypeReprKind::Composition:
  case TypeReprKind::OpaqueReturn:
  case TypeReprKind::NamedOpaqueReturn:
  case TypeReprKind::Existential:
  case TypeReprKind::PackElement:
    return false;
  case TypeReprKind::UnqualifiedIdent:
  case TypeReprKind::QualifiedIdent:
  case TypeReprKind::Metatype:
  case TypeReprKind::Protocol:
  case TypeReprKind::Dictionary:
  case TypeReprKind::Optional:
  case TypeReprKind::ImplicitlyUnwrappedOptional:
  case TypeReprKind::Inverse:
  case TypeReprKind::Vararg:
  case TypeReprKind::PackExpansion:
  case TypeReprKind::Pack:
  case TypeReprKind::Tuple:
  case TypeReprKind::Fixed:
  case TypeReprKind::Self:
  case TypeReprKind::Array:
  case TypeReprKind::SILBox:
  case TypeReprKind::Isolated:
  case TypeReprKind::Transferring:
  case TypeReprKind::Sending:
  case TypeReprKind::Placeholder:
  case TypeReprKind::CompileTimeConst:
  case TypeReprKind::LifetimeDependentReturn:
    return true;
  }
  llvm_unreachable("bad TypeRepr kind");
}

} // end namespace swift

namespace llvm {
  static inline raw_ostream &
  operator<<(raw_ostream &OS, swift::TypeRepr *TyR) {
    TyR->print(OS);
    return OS;
  }
} // end namespace llvm

#endif
