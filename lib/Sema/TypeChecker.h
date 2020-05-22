//===--- TypeChecker.h - Type Checking Class --------------------*- C++ -*-===//
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
//  This file defines the TypeChecking class.
//
//===----------------------------------------------------------------------===//

#ifndef TYPECHECKING_H
#define TYPECHECKING_H

#include "swift/AST/ASTContext.h"
#include "swift/AST/AccessScope.h"
#include "swift/AST/AnyFunctionRef.h"
#include "swift/AST/Availability.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/KnownProtocols.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/TypeRefinementContext.h"
#include "swift/Parse/Lexer.h"
#include "swift/Basic/OptionSet.h"
#include "swift/Config.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/TinyPtrVector.h"
#include <functional>

namespace swift {

class GenericSignatureBuilder;
class NominalTypeDecl;
class NormalProtocolConformance;
class TypeResolution;
class TypeResolutionOptions;
class TypoCorrectionResults;
class ExprPattern;
enum class TypeResolutionStage : uint8_t;

namespace constraints {
  enum class ConstraintKind : char;
  class ConstraintSystem;
  class Solution;
  class SolutionApplicationTarget;
  class SolutionResult;
}

/// Special-case type checking semantics for certain declarations.
enum class DeclTypeCheckingSemantics {
  /// A normal declaration.
  Normal,
  
  /// The type(of:) declaration, which performs a "dynamic type" operation,
  /// with different behavior for existential and non-existential arguments.
  TypeOf,
  
  /// The withoutActuallyEscaping(_:do:) declaration, which makes a nonescaping
  /// closure temporarily escapable.
  WithoutActuallyEscaping,

  /// The _openExistential(_:do:) declaration, which extracts the value inside
  /// an existential and passes it as a value of its own dynamic type.
  OpenExistential,
};

/// An individual result of a name lookup for a type.
struct LookupTypeResultEntry {
  TypeDecl *Member;
  Type MemberType;
  /// The associated type that the Member/MemberType were inferred for, but only
  /// if inference happened when creating this entry.
  AssociatedTypeDecl *InferredAssociatedType;
};

/// The result of name lookup for types.
class LookupTypeResult {
  /// The set of results found.
  SmallVector<LookupTypeResultEntry, 4> Results;

public:
  using iterator = SmallVectorImpl<LookupTypeResultEntry>::iterator;
  iterator begin() { return Results.begin(); }
  iterator end() { return Results.end(); }
  unsigned size() const { return Results.size(); }

  LookupTypeResultEntry operator[](unsigned index) const {
    return Results[index];
  }

  LookupTypeResultEntry front() const { return Results.front(); }
  LookupTypeResultEntry back() const { return Results.back(); }

  /// Add a result to the set of results.
  void addResult(LookupTypeResultEntry result) { Results.push_back(result); }

  /// Determine whether this result set is ambiguous.
  bool isAmbiguous() const {
    return Results.size() > 1;
  }

  /// Determine whether the result set is nonempty.
  explicit operator bool() const {
    return !Results.empty();
  }
};

/// This specifies the purpose of the contextual type, when specified to
/// typeCheckExpression.  This is used for diagnostic generation to produce more
/// specified error messages when the conversion fails.
///
enum ContextualTypePurpose {
  CTP_Unused,           ///< No contextual type is specified.
  CTP_Initialization,   ///< Pattern binding initialization.
  CTP_ReturnStmt,       ///< Value specified to a 'return' statement.
  CTP_ReturnSingleExpr, ///< Value implicitly returned from a function.
  CTP_YieldByValue,     ///< By-value yield operand.
  CTP_YieldByReference, ///< By-reference yield operand.
  CTP_ThrowStmt,        ///< Value specified to a 'throw' statement.
  CTP_EnumCaseRawValue, ///< Raw value specified for "case X = 42" in enum.
  CTP_DefaultParameter, ///< Default value in parameter 'foo(a : Int = 42)'.

  /// Default value in @autoclosure parameter
  /// 'foo(a : @autoclosure () -> Int = 42)'.
  CTP_AutoclosureDefaultParameter,

  CTP_CalleeResult,     ///< Constraint is placed on the result of a callee.
  CTP_CallArgument,     ///< Call to function or operator requires type.
  CTP_ClosureResult,    ///< Closure result expects a specific type.
  CTP_ArrayElement,     ///< ArrayExpr wants elements to have a specific type.
  CTP_DictionaryKey,    ///< DictionaryExpr keys should have a specific type.
  CTP_DictionaryValue,  ///< DictionaryExpr values should have a specific type.
  CTP_CoerceOperand,    ///< CoerceExpr operand coerced to specific type.
  CTP_AssignSource,     ///< AssignExpr source operand coerced to result type.
  CTP_SubscriptAssignSource, ///< AssignExpr source operand coerced to subscript
                             ///< result type.
  CTP_Condition,        ///< Condition expression of various statements e.g.
                        ///< `if`, `for`, `while` etc.
  CTP_ForEachStmt,      ///< "expression/sequence" associated with 'for-in' loop
                        ///< is expected to conform to 'Sequence' protocol.

  CTP_CannotFail,       ///< Conversion can never fail. abort() if it does.
};



/// Flags that can be used to control type checking.
enum class TypeCheckExprFlags {
  /// Whether we know that the result of the expression is discarded.  This
  /// disables constraints forcing an lvalue result to be loadable.
  IsDiscarded = 0x01,

  /// If set, the client wants a best-effort solution to the constraint system,
  /// but can tolerate a solution where all of the constraints are solved, but
  /// not all type variables have been determined.  In this case, the constraint
  /// system is not applied to the expression AST, but the ConstraintSystem is
  /// left in-tact.
  AllowUnresolvedTypeVariables = 0x08,

  /// If set, this expression isn't embedded in a larger expression or
  /// statement. This should only be used for syntactic restrictions, and should
  /// not affect type checking itself.
  IsExprStmt = 0x20,
};

using TypeCheckExprOptions = OptionSet<TypeCheckExprFlags>;

inline TypeCheckExprOptions operator|(TypeCheckExprFlags flag1,
                                      TypeCheckExprFlags flag2) {
  return TypeCheckExprOptions(flag1) | flag2;
}

/// Flags that can be used to control name lookup.
enum class NameLookupFlags {
  /// Whether we know that this lookup is always a private dependency.
  KnownPrivate = 0x01,
  /// Whether name lookup should be able to find protocol members.
  ProtocolMembers = 0x02,
  /// Whether we should map the requirement to the witness if we
  /// find a protocol member and the base type is a concrete type.
  ///
  /// If this is not set but ProtocolMembers is set, we will
  /// find protocol extension members, but not protocol requirements
  /// that do not yet have a witness (such as inferred associated
  /// types, or witnesses for derived conformances).
  PerformConformanceCheck = 0x04,
  /// Whether to perform 'dynamic' name lookup that finds @objc
  /// members of any class or protocol.
  DynamicLookup = 0x08,
  /// Whether to ignore access control for this lookup, allowing inaccessible
  /// results to be returned.
  IgnoreAccessControl = 0x10,
  /// Whether to include results from outside the innermost scope that has a
  /// result.
  IncludeOuterResults = 0x20,
  /// Whether to consider synonyms declared through @_implements().
  IncludeAttributeImplements = 0x40,
};

/// A set of options that control name lookup.
using NameLookupOptions = OptionSet<NameLookupFlags>;

inline NameLookupOptions operator|(NameLookupFlags flag1,
                                   NameLookupFlags flag2) {
  return NameLookupOptions(flag1) | flag2;
}

/// Default options for member name lookup.
const NameLookupOptions defaultMemberLookupOptions
  = NameLookupFlags::ProtocolMembers |
    NameLookupFlags::PerformConformanceCheck;

/// Default options for constructor lookup.
const NameLookupOptions defaultConstructorLookupOptions
  = NameLookupFlags::ProtocolMembers |
    NameLookupFlags::PerformConformanceCheck;

/// Default options for member type lookup.
const NameLookupOptions defaultMemberTypeLookupOptions
  = NameLookupFlags::ProtocolMembers |
    NameLookupFlags::PerformConformanceCheck;

/// Default options for unqualified name lookup.
const NameLookupOptions defaultUnqualifiedLookupOptions
  = NameLookupFlags::ProtocolMembers |
    NameLookupFlags::PerformConformanceCheck;

/// Describes the result of comparing two entities, of which one may be better
/// or worse than the other, or they are unordered.
enum class Comparison {
  /// Neither entity is better than the other.
  Unordered,
  /// The first entity is better than the second.
  Better,
  /// The first entity is worse than the second.
  Worse
};

/// Specify how we handle the binding of underconstrained (free) type variables
/// within a solution to a constraint system.
enum class FreeTypeVariableBinding {
  /// Disallow any binding of such free type variables.
  Disallow,
  /// Allow the free type variables to persist in the solution.
  Allow,
  /// Bind the type variables to UnresolvedType to represent the ambiguity.
  UnresolvedType
};

/// A conditional conformance that implied some other requirements. That is, \c
/// ConformingType conforming to \c Protocol may have required additional
/// requirements to be satisfied.
///
/// This is designed to be used in a stack of such requirements, which can be
/// formatted with \c diagnoseConformanceStack.
struct ParentConditionalConformance {
  Type ConformingType;
  ProtocolType *Protocol;

  /// Format the stack \c conformances as a series of notes that trace a path of
  /// conditional conformances that lead to some other failing requirement (that
  /// is not in \c conformances).
  ///
  /// The end of \c conformances is the active end of the stack, i.e. \c
  /// conformances[0] is a conditional conformance that requires \c
  /// conformances[1], etc.
  static void
  diagnoseConformanceStack(DiagnosticEngine &diags, SourceLoc location,
                           ArrayRef<ParentConditionalConformance> conformances);
};

/// An abstract interface that is used by `checkGenericArguments`.
class GenericRequirementsCheckListener {
public:
  virtual ~GenericRequirementsCheckListener();

  /// Callback invoked before trying to check generic requirement placed
  /// between given types. Note: if either of the types assigned to the
  /// requirement is generic parameter or dependent member, this callback
  /// method is going to get their substitutions.
  ///
  /// \param kind The kind of generic requirement to check.
  ///
  /// \param first The left-hand side type assigned to the requirement,
  /// possibly represented by its generic substitute.
  ///
  /// \param second The right-hand side type assigned to the requirement,
  /// possibly represented by its generic substitute.
  ///
  ///
  /// \returns true if it's ok to validate requirement, false otherwise.
  virtual bool shouldCheck(RequirementKind kind, Type first, Type second);

  /// Callback to report the result of a satisfied conformance requirement.
  ///
  /// \param depTy The dependent type, from the signature.
  /// \param replacementTy The type \c depTy was replaced with.
  /// \param conformance The conformance itself.
  virtual void satisfiedConformance(Type depTy, Type replacementTy,
                                    ProtocolConformanceRef conformance);

  /// Callback to diagnose problem with unsatisfied generic requirement.
  ///
  /// \param req The unsatisfied generic requirement.
  ///
  /// \param first The left-hand side type assigned to the requirement,
  /// possibly represented by its generic substitute.
  ///
  /// \param second The right-hand side type assigned to the requirement,
  /// possibly represented by its generic substitute.
  ///
  /// \returns true if problem has been diagnosed, false otherwise.
  virtual bool diagnoseUnsatisfiedRequirement(
      const Requirement &req, Type first, Type second,
      ArrayRef<ParentConditionalConformance> parents);
};

/// The result of `checkGenericRequirement`.
enum class RequirementCheckResult {
  Success, Failure, SubstitutionFailure
};

/// Describes the kind of checked cast operation being performed.
enum class CheckedCastContextKind {
  /// None: we're just establishing how to perform the checked cast. This
  /// is useful when we don't care to produce any diagnostics.
  None,
  /// A forced cast, with "as!".
  ForcedCast,
  /// A conditional cast, with "as?".
  ConditionalCast,
  /// An "is" expression.
  IsExpr,
  /// An "is" pattern.
  IsPattern,
  /// An enum-element pattern.
  EnumElementPattern,
};

namespace TypeChecker {
Type getArraySliceType(SourceLoc loc, Type elementType);
Type getDictionaryType(SourceLoc loc, Type keyType, Type valueType);
Type getOptionalType(SourceLoc loc, Type elementType);
Type getStringType(ASTContext &ctx);
Type getSubstringType(ASTContext &ctx);
Type getIntType(ASTContext &ctx);
Type getInt8Type(ASTContext &ctx);
Type getUInt8Type(ASTContext &ctx);

/// Bind an UnresolvedDeclRefExpr by performing name lookup and
/// returning the resultant expression.  Context is the DeclContext used
/// for the lookup.
Expr *resolveDeclRefExpr(UnresolvedDeclRefExpr *UDRE, DeclContext *Context);

/// Validate the given type.
///
/// Type validation performs name lookup, checking of generic arguments,
/// and so on to determine whether the given type is well-formed and can
/// be used as a type.
///
/// \param Loc The type (with source location information) to validate.
/// If the type has already been validated, returns immediately.
///
/// \param resolution The type resolution being performed.
///
/// \returns true if type validation failed, or false otherwise.
bool validateType(TypeLoc &Loc, TypeResolution resolution);

/// Check for unsupported protocol types in the given declaration.
void checkUnsupportedProtocolType(Decl *decl);

/// Check for unsupported protocol types in the given statement.
void checkUnsupportedProtocolType(ASTContext &ctx, Stmt *stmt);

/// Check for unsupported protocol types in the given generic requirement
/// list.
void checkUnsupportedProtocolType(ASTContext &ctx,
                                  TrailingWhereClause *whereClause);

/// Check for unsupported protocol types in the given generic requirement
/// list.
void checkUnsupportedProtocolType(ASTContext &ctx,
                                  GenericParamList *genericParams);

/// Resolve a reference to the given type declaration within a particular
/// context.
///
/// This routine aids unqualified name lookup for types by performing the
/// resolution necessary to rectify the declaration found by name lookup with
/// the declaration context from which name lookup started.
///
/// \param typeDecl The type declaration found by name lookup.
/// \param isSpecialized Whether the type will have generic arguments applied.
/// \param resolution The resolution to perform.
///
/// \returns the resolved type.
Type resolveTypeInContext(TypeDecl *typeDecl, DeclContext *foundDC,
                          TypeResolution resolution, bool isSpecialized);

/// Apply generic arguments to the given type.
///
/// This function requires a valid unbound generic type with the correct
/// number of generic arguments given, whereas applyGenericArguments emits
/// diagnostics in those cases.
///
/// \param unboundType The unbound generic type to which to apply arguments.
/// \param decl The declaration of the type.
/// \param loc The source location for diagnostic reporting.
/// \param resolution The type resolution.
/// \param genericArgs The list of generic arguments to apply to the type.
///
/// \returns A BoundGenericType bound to the given arguments, or null on
/// error.
///
/// \see applyGenericArguments
Type applyUnboundGenericArguments(UnboundGenericType *unboundType,
                                  GenericTypeDecl *decl, SourceLoc loc,
                                  TypeResolution resolution,
                                  ArrayRef<Type> genericArgs);

/// Substitute the given base type into the type of the given nested type,
/// producing the effective type that the nested type will have.
///
/// \param module The module in which the substitution will be performed.
/// \param member The member whose type projection is being computed.
/// \param baseTy The base type that will be substituted for the 'Self' of the
/// member.
/// \param useArchetypes Whether to use context archetypes for outer generic
/// parameters if the class is nested inside a generic function.
Type substMemberTypeWithBase(ModuleDecl *module, TypeDecl *member, Type baseTy,
                             bool useArchetypes = true);

/// Determine whether this is a "pass-through" typealias, which has the
/// same type parameters as the nominal type it references and specializes
/// the underlying nominal type with exactly those type parameters.
/// For example, the following typealias \c GX is a pass-through typealias:
///
/// \code
/// struct X<T, U> { }
/// typealias GX<A, B> = X<A, B>
/// \endcode
///
/// whereas \c GX2 and \c GX3 are not pass-through because \c GX2 has
/// different type parameters and \c GX3 doesn't pass its type parameters
/// directly through.
///
/// \code
/// typealias GX2<A> = X<A, A>
/// typealias GX3<A, B> = X<B, A>
/// \endcode
bool isPassThroughTypealias(TypeAliasDecl *typealias, Type underlyingType,
                            NominalTypeDecl *nominal);

/// Determine whether one type is a subtype of another.
///
/// \param t1 The potential subtype.
/// \param t2 The potential supertype.
/// \param dc The context of the check.
///
/// \returns true if \c t1 is a subtype of \c t2.
bool isSubtypeOf(Type t1, Type t2, DeclContext *dc);

/// Determine whether one type is implicitly convertible to another.
///
/// \param t1 The potential source type of the conversion.
///
/// \param t2 The potential destination type of the conversion.
///
/// \param dc The context of the conversion.
///
/// \param unwrappedIUO If non-null, will be set to indicate whether the
/// conversion force-unwrapped an implicitly-unwrapped optional.
///
/// \returns true if \c t1 can be implicitly converted to \c t2.
bool isConvertibleTo(Type t1, Type t2, DeclContext *dc,
                     bool *unwrappedIUO = nullptr);

/// Determine whether one type is explicitly convertible to another,
/// i.e. using an 'as' expression.
///
/// \param t1 The potential source type of the conversion.
///
/// \param t2 The potential destination type of the conversion.
///
/// \param dc The context of the conversion.
///
/// \returns true if \c t1 can be explicitly converted to \c t2.
bool isExplicitlyConvertibleTo(Type t1, Type t2, DeclContext *dc);

/// Determine whether one type is bridged to another type.
///
/// \param t1 The potential source type of the conversion.
///
/// \param t2 The potential destination type of the conversion.
///
/// \param dc The context of the conversion.
///
/// \param unwrappedIUO If non-null, will be set to indicate whether the
/// conversion force-unwrapped an implicitly-unwrapped optional.
///
/// \returns true if \c t1 can be explicitly converted to \c t2.
bool isObjCBridgedTo(Type t1, Type t2, DeclContext *dc,
                     bool *unwrappedIUO = nullptr);

/// Return true if performing a checked cast from one type to another
/// with the "as!" operator could possibly succeed.
///
/// \param t1 The potential source type of the cast.
///
/// \param t2 The potential destination type of the cast.
///
/// \param dc The context of the cast.
///
/// \returns true if a checked cast from \c t1 to \c t2 may succeed, and
/// false if it will certainly fail, e.g. because the types are unrelated.
bool checkedCastMaySucceed(Type t1, Type t2, DeclContext *dc);

/// Determine whether a constraint of the given kind can be satisfied
/// by the two types.
///
/// \param t1 The first type of the constraint.
///
/// \param t2 The second type of the constraint.
///
/// \param openArchetypes If true, archetypes are replaced with type
/// variables, and the result can be interpreted as whether or not the
/// two types can possibly equal at runtime.
///
/// \param dc The context of the conversion.
///
/// \param unwrappedIUO   If non-null, will be set to \c true if the coercion
/// or bridge operation force-unwraps an implicitly-unwrapped optional.
///
/// \returns true if \c t1 and \c t2 satisfy the constraint.
bool typesSatisfyConstraint(Type t1, Type t2, bool openArchetypes,
                            constraints::ConstraintKind kind, DeclContext *dc,
                            bool *unwrappedIUO = nullptr);

/// If the inputs to an apply expression use a consistent "sugar" type
/// (that is, a typealias or shorthand syntax) equivalent to the result type
/// of the function, set the result type of the expression to that sugar type.
Expr *substituteInputSugarTypeForResult(ApplyExpr *E);

bool typeCheckAbstractFunctionBodyUntil(AbstractFunctionDecl *AFD,
                                        SourceLoc EndTypeCheckLoc);
bool typeCheckAbstractFunctionBody(AbstractFunctionDecl *AFD);

/// Try to apply the function builder transform of the given builder type
/// to the body of the function.
///
/// \returns \c None if the builder transformation cannot be applied at all,
/// e.g., because of a \c return statement. Otherwise, returns either the
/// fully type-checked body of the function (on success) or a \c nullptr
/// value if an error occurred while type checking the transformed body.
Optional<BraceStmt *> applyFunctionBuilderBodyTransform(FuncDecl *func,
                                                        Type builderType);

bool typeCheckClosureBody(ClosureExpr *closure);

bool typeCheckTapBody(TapExpr *expr, DeclContext *DC);

Type typeCheckParameterDefault(Expr *&defaultValue, DeclContext *DC,
                               Type paramType, bool isAutoClosure);

void typeCheckTopLevelCodeDecl(TopLevelCodeDecl *TLCD);

void typeCheckDecl(Decl *D);

void addImplicitDynamicAttribute(Decl *D);
void checkDeclAttributes(Decl *D);
void checkParameterAttributes(ParameterList *params);

Type checkReferenceOwnershipAttr(VarDecl *D, Type interfaceType,
                                 ReferenceOwnershipAttr *attr);

/// Infer default value witnesses for all requirements in the given protocol.
void inferDefaultWitnesses(ProtocolDecl *proto);

/// For a generic requirement in a protocol, make sure that the requirement
/// set didn't add any requirements to Self or its associated types.
void checkProtocolSelfRequirements(ValueDecl *decl);

/// All generic parameters of a generic function must be referenced in the
/// declaration's type, otherwise we have no way to infer them.
void checkReferencedGenericParams(GenericContext *dc);

/// Construct a new generic environment for the given declaration context.
///
/// \param paramSource The source of generic info: either a generic parameter
/// list or a generic context with a \c where clause dependent on outer
/// generic parameters.
///
/// \param dc The declaration context in which to perform the validation.
///
/// \param outerSignature The generic signature of the outer
/// context, if not available as part of the \c dc argument (used
/// for SIL parsing).
///
/// \param allowConcreteGenericParams Whether or not to allow
/// same-type constraints between generic parameters and concrete types.
///
/// \param additionalRequirements Additional requirements to add
/// directly to the GSB.
///
/// \param inferenceSources Additional types to infer requirements from.
///
/// \returns the resulting generic signature.
GenericSignature
checkGenericSignature(GenericParamSource paramSource, DeclContext *dc,
                      GenericSignature outerSignature,
                      bool allowConcreteGenericParams,
                      SmallVector<Requirement, 2> additionalRequirements = {},
                      SmallVector<TypeLoc, 2> inferenceSources = {});

/// Create a text string that describes the bindings of generic parameters
/// that are relevant to the given set of types, e.g.,
/// "[with T = Bar, U = Wibble]".
///
/// \param types The types that will be scanned for generic type parameters,
/// which will be used in the resulting type.
///
/// \param genericParams The generic parameters to use to resugar any
/// generic parameters that occur within the types.
///
/// \param substitutions The generic parameter -> generic argument
/// substitutions that will have been applied to these types.
/// These are used to produce the "parameter = argument" bindings in the test.
std::string gatherGenericParamBindingsText(
    ArrayRef<Type> types, TypeArrayView<GenericTypeParamType> genericParams,
    TypeSubstitutionFn substitutions);

/// Check the given set of generic arguments against the requirements in a
/// generic signature.
///
/// \param dc The context in which the generic arguments should be checked.
/// \param loc The location at which any diagnostics should be emitted.
/// \param noteLoc The location at which any notes will be printed.
/// \param owner The type that owns the generic signature.
/// \param genericParams The generic parameters being substituted.
/// \param requirements The requirements against which the generic arguments
/// should be checked.
/// \param substitutions Substitutions from interface types of the signature.
/// \param listener The generic check listener used to pick requirements and
/// notify callers about diagnosed errors.
RequirementCheckResult checkGenericArguments(
    DeclContext *dc, SourceLoc loc, SourceLoc noteLoc, Type owner,
    TypeArrayView<GenericTypeParamType> genericParams,
    ArrayRef<Requirement> requirements, TypeSubstitutionFn substitutions,
    GenericRequirementsCheckListener *listener = nullptr,
    SubstOptions options = None);

/// Add any implicitly-defined constructors required for the given
/// struct or class.
void addImplicitConstructors(NominalTypeDecl *typeDecl);

/// Fold the given sequence expression into an (unchecked) expression
/// tree.
Expr *foldSequence(SequenceExpr *expr, DeclContext *dc);

/// Given an pre-folded expression, find LHS from the expression if a binary
/// operator \c name appended to the expression.
Expr *findLHS(DeclContext *DC, Expr *E, Identifier name);

/// Type check the given expression.
///
/// \param expr The expression to type-check, which will be modified in
/// place.
///
/// \param convertTypePurpose When convertType is specified, this indicates
/// what the conversion is doing.  This allows diagnostics generation to
/// produce more specific and helpful error messages when the conversion fails
/// to be possible.
///
/// \param convertType The type that the expression is being converted to,
/// or null if the expression is standalone.
///
/// \param options Options that control how type checking is performed.
///
/// \returns The type of the top-level expression, or Type() if an
///          error occurred.
Type typeCheckExpression(Expr *&expr, DeclContext *dc,
                         Type convertType = Type(),
                         ContextualTypePurpose convertTypePurpose = CTP_Unused,
                         TypeCheckExprOptions options = TypeCheckExprOptions());

Optional<constraints::SolutionApplicationTarget>
typeCheckExpression(constraints::SolutionApplicationTarget &target,
                    bool &unresolvedTypeExprs,
                    TypeCheckExprOptions options = TypeCheckExprOptions());

/// Type check the given expression and return its type without
/// applying the solution.
///
/// \param expr The expression to type-check.
///
/// \param referencedDecl Will be set to the declaration that is referenced by
/// the expression.
///
/// \param allowFreeTypeVariables Whether free type variables are allowed in
/// the solution, and what to do with them.
///
/// \returns the type of \p expr on success, Type() otherwise.
/// FIXME: expr may still be modified...
Type getTypeOfExpressionWithoutApplying(
    Expr *&expr, DeclContext *dc, ConcreteDeclRef &referencedDecl,
    FreeTypeVariableBinding allowFreeTypeVariables =
        FreeTypeVariableBinding::Disallow);

/// Return the type of operator function for specified LHS, or a null
/// \c Type on error.
FunctionType *getTypeOfCompletionOperator(DeclContext *DC, Expr *LHS,
                                          Identifier opName,
                                          DeclRefKind refKind,
                                          ConcreteDeclRef &refdDecl);

/// Check the key-path expression.
///
/// Returns the type of the last component of the key-path.
Optional<Type> checkObjCKeyPathExpr(DeclContext *dc, KeyPathExpr *expr,
                                    bool requireResultType = false);

/// Type check whether the given type declaration includes members of
/// unsupported recursive value types.
///
/// \param decl The declaration to be type-checked. This process will not
/// modify the declaration.
void checkDeclCircularity(NominalTypeDecl *decl);

/// Type check whether the given switch statement exhaustively covers
/// its domain.
///
/// \param stmt The switch statement to be type-checked.  No modification of
/// the statement occurs.
/// \param DC The decl context containing \p stmt.
/// \param limitChecking The checking process relies on the switch statement
/// being well-formed.  If it is not, pass true to this flag to run a limited
/// form of analysis.
void checkSwitchExhaustiveness(const SwitchStmt *stmt, const DeclContext *DC,
                               bool limitChecking);

/// Type check the given expression as a condition, which converts
/// it to a logic value.
///
/// \param expr The expression to type-check, which will be modified in place
/// to return a logic value (builtin i1).
///
/// \returns true if an error occurred, false otherwise.
bool typeCheckCondition(Expr *&expr, DeclContext *dc);

/// Type check the given 'if' or 'while' statement condition, which
/// either converts an expression to a logic value or bind variables to the
/// contents of an Optional.
///
/// \param cond The condition to type-check, which will be modified in place.
///
/// \returns true if an error occurred, false otherwise.
bool typeCheckStmtCondition(StmtCondition &cond, DeclContext *dc,
                            Diag<> diagnosticForAlwaysTrue);

/// Determine the semantics of a checked cast operation.
///
/// \param fromType       The source type of the cast.
/// \param toType         The destination type of the cast.
/// \param dc             The context of the cast.
/// \param diagLoc        The location at which to report diagnostics.
/// \param fromExpr       The expression describing the input operand.
/// \param diagToRange    The source range of the destination type.
///
/// \returns a CheckedCastKind indicating the semantics of the cast. If the
/// cast is invalid, Unresolved is returned. If the cast represents an implicit
/// conversion, Coercion is returned.
CheckedCastKind typeCheckCheckedCast(Type fromType, Type toType,
                                     CheckedCastContextKind ctxKind,
                                     DeclContext *dc, SourceLoc diagLoc,
                                     Expr *fromExpr, SourceRange diagToRange);

/// Find the Objective-C class that bridges between a value of the given
/// dynamic type and the given value type.
///
/// \param dc The declaration context from which we will look for
/// bridging.
///
/// \param dynamicType A dynamic type from which we are bridging. Class and
/// Objective-C protocol types can be used for bridging.
///
/// \param valueType The value type being queried, e.g., String.
///
/// \returns the Objective-C class type that represents the value
/// type as an Objective-C class, e.g., \c NSString represents \c
/// String, or a null type if there is no such type or if the
/// dynamic type isn't something we can start from.
Type getDynamicBridgedThroughObjCClass(DeclContext *dc, Type dynamicType,
                                       Type valueType);

/// Resolve ambiguous pattern/expr productions inside a pattern using
/// name lookup information. Must be done before type-checking the pattern.
Pattern *resolvePattern(Pattern *P, DeclContext *dc, bool isStmtCondition);

/// Type check the given pattern.
///
/// \returns the type of the pattern, which may be an error type if an
/// unrecoverable error occurred. If the options permit it, the type may
/// involve \c UnresolvedType (for patterns with no type information) and
/// unbound generic types.
Type typeCheckPattern(ContextualPattern pattern);

bool typeCheckCatchPattern(CaseStmt *S, DeclContext *dc);

/// Coerce a pattern to the given type.
///
/// \param pattern The contextual pattern.
/// \param type the type to coerce the pattern to.
/// \param options Options that control the coercion.
///
/// \returns the coerced pattern, or nullptr if the coercion failed.
Pattern *coercePatternToType(ContextualPattern pattern, Type type,
                             TypeResolutionOptions options);
bool typeCheckExprPattern(ExprPattern *EP, DeclContext *DC, Type type);

/// Coerce the specified parameter list of a ClosureExpr to the specified
/// contextual type.
void coerceParameterListToType(ParameterList *P, ClosureExpr *CE,
                               AnyFunctionType *FN);

/// Type-check an initialized variable pattern declaration.
bool typeCheckBinding(Pattern *&P, Expr *&Init, DeclContext *DC,
                      Type patternType,
                      PatternBindingDecl *PBD = nullptr,
                      unsigned patternNumber = 0);
bool typeCheckPatternBinding(PatternBindingDecl *PBD, unsigned patternNumber,
                             Type patternType = Type());

/// Type-check a for-each loop's pattern binding and sequence together.
///
/// \returns true if a failure occurred.
bool typeCheckForEachBinding(DeclContext *dc, ForEachStmt *stmt);

/// Compute the set of captures for the given function or closure.
void computeCaptures(AnyFunctionRef AFR);

/// Check for invalid captures from stored property initializers.
void checkPatternBindingCaptures(IterableDeclContext *DC);

/// Change the context of closures in the given initializer
/// expression to the given context.
void contextualizeInitializer(Initializer *DC, Expr *init);
void contextualizeTopLevelCode(TopLevelCodeDecl *TLCD);

/// Retrieve the default type for the given protocol.
///
/// Some protocols, particularly those that correspond to literals, have
/// default types associated with them. This routine retrieves that default
/// type.
///
/// \returns the default type, or null if there is no default type for
/// this protocol.
Type getDefaultType(ProtocolDecl *protocol, DeclContext *dc);

/// Coerce the given expression to materializable type, if it
/// isn't already.
Expr *coerceToRValue(
    ASTContext &Context, Expr *expr,
    llvm::function_ref<Type(Expr *)> getType =
        [](Expr *expr) { return expr->getType(); },
    llvm::function_ref<void(Expr *, Type)> setType =
        [](Expr *expr, Type type) { expr->setType(type); });

/// Add implicit load expression to given AST, this is sometimes
/// more complicated than simplify wrapping given root in newly created
/// `LoadExpr`, because `ForceValueExpr` and `ParenExpr` supposed to appear
/// only at certain positions in AST.
Expr *addImplicitLoadExpr(
    ASTContext &Context, Expr *expr,
    std::function<Type(Expr *)> getType = [](Expr *E) { return E->getType(); },
    std::function<void(Expr *, Type)> setType =
        [](Expr *E, Type type) { E->setType(type); });

/// Determine whether the given type contains the given protocol.
///
/// \param DC The context in which to check conformance. This affects, for
/// example, extension visibility.
///
/// \returns the conformance, if \c T conforms to the protocol \c Proto, or
/// an empty optional.
ProtocolConformanceRef containsProtocol(Type T, ProtocolDecl *Proto,
                                        DeclContext *DC,
                                        bool skipConditionalRequirements=false);

/// Determine whether the given type conforms to the given protocol.
///
/// Unlike subTypeOfProtocol(), this will return false for existentials of
/// non-self conforming protocols.
///
/// \param DC The context in which to check conformance. This affects, for
/// example, extension visibility.
///
/// \param ComplainLoc If valid, then this function will emit diagnostics if
/// T does not conform to the given protocol. The primary diagnostic will
/// be placed at this location, with notes for each of the protocol
/// requirements not satisfied.
///
/// \returns The protocol conformance, if \c T conforms to the
/// protocol \c Proto, or \c None.
ProtocolConformanceRef conformsToProtocol(Type T, ProtocolDecl *Proto,
                                          DeclContext *DC,
                                          SourceLoc ComplainLoc = SourceLoc());

/// Completely check the given conformance.
void checkConformance(NormalProtocolConformance *conformance);

/// Check all of the conformances in the given context.
void checkConformancesInContext(IterableDeclContext *idc);

/// Check that the type of the given property conforms to NSCopying.
ProtocolConformanceRef checkConformanceToNSCopying(VarDecl *var);

/// Derive an implicit declaration to satisfy a requirement of a derived
/// protocol conformance.
///
/// \param DC           The declaration context where the conformance was
///                     defined, either the type itself or an extension
/// \param TypeDecl     The type for which the requirement is being derived.
/// \param Requirement  The protocol requirement.
///
/// \returns nullptr if the derivation failed, or the derived declaration
///          if it succeeded. If successful, the derived declaration is added
///          to TypeDecl's body.
ValueDecl *deriveProtocolRequirement(DeclContext *DC,
                                     NominalTypeDecl *TypeDecl,
                                     ValueDecl *Requirement);

/// Derive an implicit type witness for the given associated type in
/// the conformance of the given nominal type to some known
/// protocol.
Type deriveTypeWitness(DeclContext *DC, NominalTypeDecl *nominal,
                       AssociatedTypeDecl *assocType);

/// \name Name lookup
///
/// Routines that perform name lookup.
///
/// @{

/// Perform unqualified name lookup at the given source location
/// within a particular declaration context.
///
/// \param dc The declaration context in which to perform name lookup.
/// \param name The name of the entity to look for.
/// \param loc The source location at which name lookup occurs.
/// \param options Options that control name lookup.
LookupResult lookupUnqualified(
    DeclContext *dc, DeclNameRef name, SourceLoc loc,
    NameLookupOptions options = defaultUnqualifiedLookupOptions);

/// Perform unqualified type lookup at the given source location
/// within a particular declaration context.
///
/// \param dc The declaration context in which to perform name lookup.
/// \param name The name of the entity to look for.
/// \param loc The source location at which name lookup occurs.
/// \param options Options that control name lookup.
LookupResult lookupUnqualifiedType(
    DeclContext *dc, DeclNameRef name, SourceLoc loc,
    NameLookupOptions options = defaultUnqualifiedLookupOptions);

/// Lookup a member in the given type.
///
/// \param dc The context that needs the member.
/// \param type The type in which we will look for a member.
/// \param name The name of the member to look for.
/// \param options Options that control name lookup.
///
/// \returns The result of name lookup.
LookupResult
lookupMember(DeclContext *dc, Type type, DeclNameRef name,
             NameLookupOptions options = defaultMemberLookupOptions);

/// Look up a member type within the given type.
///
/// This routine looks for member types with the given name within the
/// given type.
///
/// \param dc The context that needs the member.
/// \param type The type in which we will look for a member type.
/// \param name The name of the member to look for.
/// \param options Options that control name lookup.
///
/// \returns The result of name lookup.
LookupTypeResult
lookupMemberType(DeclContext *dc, Type type, DeclNameRef name,
                 NameLookupOptions options = defaultMemberTypeLookupOptions);

/// Look up the constructors of the given type.
///
/// \param dc The context that needs the constructor.
/// \param type The type for which we will look for constructors.
/// \param options Options that control name lookup.
///
/// \returns the constructors found for this type.
LookupResult lookupConstructors(
    DeclContext *dc, Type type,
    NameLookupOptions options = defaultConstructorLookupOptions);

/// Given an expression that's known to be an infix operator,
/// look up its precedence group.
PrecedenceGroupDecl *lookupPrecedenceGroupForInfixOperator(DeclContext *dc,
                                                           Expr *op);

PrecedenceGroupDecl *lookupPrecedenceGroup(DeclContext *dc, Identifier name,
                                           SourceLoc nameLoc);

/// Check whether the given declaration can be written as a
/// member of the given base type.
bool isUnsupportedMemberTypeAccess(Type type, TypeDecl *typeDecl);

/// @}

/// \name Overload resolution
///
/// Routines that perform overload resolution or provide diagnostics related
/// to overload resolution.
/// @{

/// Compare two declarations to determine whether one is more specialized
/// than the other.
///
/// A declaration is more specialized than another declaration if its type
/// is a subtype of the other declaration's type (ignoring the 'self'
/// parameter of function declarations) and if
Comparison compareDeclarations(DeclContext *dc, ValueDecl *decl1,
                               ValueDecl *decl2);

/// Build a type-checked reference to the given value.
Expr *buildCheckedRefExpr(VarDecl *D, DeclContext *UseDC, DeclNameLoc nameLoc,
                          bool Implicit);

/// Build a reference to a declaration, where name lookup returned
/// the given set of declarations.
Expr *buildRefExpr(ArrayRef<ValueDecl *> Decls, DeclContext *UseDC,
                   DeclNameLoc NameLoc, bool Implicit,
                   FunctionRefKind functionRefKind);
/// @}

/// Retrieve a specific, known protocol.
///
/// \param loc The location at which we need to look for the protocol.
/// \param kind The known protocol we're looking for.
///
/// \returns null if the protocol is not available. This represents a
/// problem with the Standard Library.
ProtocolDecl *getProtocol(ASTContext &ctx, SourceLoc loc,
                          KnownProtocolKind kind);

/// Retrieve the literal protocol for the given expression.
///
/// \returns the literal protocol, if known and available, or null if the
/// expression does not have an associated literal protocol.
ProtocolDecl *getLiteralProtocol(ASTContext &ctx, Expr *expr);

DeclName getObjectLiteralConstructorName(ASTContext &ctx,
                                         ObjectLiteralExpr *expr);

Type getObjectLiteralParameterType(ObjectLiteralExpr *expr,
                                   ConstructorDecl *ctor);

/// Get the module appropriate for looking up standard library types.
///
/// This is "Swift", if that module is imported, or the current module if
/// we're parsing the standard library.
ModuleDecl *getStdlibModule(const DeclContext *dc);

/// \name Resilience diagnostics
bool diagnoseInlinableDeclRef(SourceLoc loc, ConcreteDeclRef declRef,
                              const DeclContext *DC, FragileFunctionKind Kind);

Expr *buildDefaultInitializer(Type type);

bool diagnoseInlinableDeclRefAccess(SourceLoc loc, const ValueDecl *D,
                                    const DeclContext *DC,
                                    FragileFunctionKind Kind);

/// Given that a declaration is used from a particular context which
/// exposes it in the interface of the current module, diagnose if it cannot
/// reasonably be shared.
bool diagnoseDeclRefExportability(SourceLoc loc, ConcreteDeclRef declRef,
                                  const DeclContext *DC,
                                  FragileFunctionKind fragileKind);

/// Given that a type is used from a particular context which
/// exposes it in the interface of the current module, diagnose if its
/// generic arguments require the use of conformances that cannot reasonably
/// be shared.
///
/// This method \e only checks how generic arguments are used; it is assumed
/// that the declarations involved have already been checked elsewhere.
void diagnoseGenericTypeExportability(SourceLoc loc, Type type,
                                      const DeclContext *DC);

/// \name Availability checking
///
/// Routines that perform API availability checking and type checking of
/// potentially unavailable API elements
/// @{

/// Returns true if the availability of the witness
/// is sufficient to safely conform to the requirement in the context
/// the provided conformance. On return, requiredAvailability holds th
/// availability levels required for conformance.
bool
isAvailabilitySafeForConformance(ProtocolDecl *proto, ValueDecl *requirement,
                                 ValueDecl *witness, DeclContext *dc,
                                 AvailabilityContext &requiredAvailability);

/// Returns an over-approximation of the range of operating system versions
/// that could the passed-in location could be executing upon for
/// the target platform. If MostRefined != nullptr, set to the most-refined
/// TRC found while approximating.
AvailabilityContext overApproximateAvailabilityAtLocation(
    SourceLoc loc, const DeclContext *DC,
    const TypeRefinementContext **MostRefined = nullptr);

/// Walk the AST to build the hierarchy of TypeRefinementContexts
void buildTypeRefinementContextHierarchy(SourceFile &SF);

/// Build the hierarchy of TypeRefinementContexts for the entire
/// source file, if it has not already been built. Returns the root
/// TypeRefinementContext for the source file.
TypeRefinementContext *getOrBuildTypeRefinementContext(SourceFile *SF);

/// Returns a diagnostic indicating why the declaration cannot be annotated
/// with an @available() attribute indicating it is potentially unavailable
/// or None if this is allowed.
Optional<Diag<>>
diagnosticIfDeclCannotBePotentiallyUnavailable(const Decl *D);

/// Checks whether a declaration is available when referred to at the given
/// location (this reference location must be in the passed-in
/// reference DeclContext).
/// If the declaration is available, return true.
/// If the declaration is not available, return false and write the
/// declaration's availability info to the out parameter
/// \p OutAvailableRange.
bool isDeclAvailable(const Decl *D, SourceLoc referenceLoc,
                     const DeclContext *referenceDC,
                     AvailabilityContext &OutAvailableRange);

/// Checks whether a declaration should be considered unavailable when
/// referred to at the given location and, if so, returns the reason why the
/// declaration is unavailable. Returns None is the declaration is
/// definitely available.
Optional<UnavailabilityReason>
checkDeclarationAvailability(const Decl *D, SourceLoc referenceLoc,
                             const DeclContext *referenceDC);

/// Checks an "ignored" expression to see if it's okay for it to be ignored.
///
/// An ignored expression is one that is not nested within a larger
/// expression or statement.
void checkIgnoredExpr(Expr *E);

// Emits a diagnostic, if necessary, for a reference to a declaration
// that is potentially unavailable at the given source location.
void diagnosePotentialUnavailability(const ValueDecl *D,
                                     SourceRange ReferenceRange,
                                     const DeclContext *ReferenceDC,
                                     const UnavailabilityReason &Reason);

// Emits a diagnostic, if necessary, for a reference to a declaration
// that is potentially unavailable at the given source location, using
// Name as the diagnostic name.
void diagnosePotentialUnavailability(const Decl *D, DeclName Name,
                                     SourceRange ReferenceRange,
                                     const DeclContext *ReferenceDC,
                                     const UnavailabilityReason &Reason);

void
diagnosePotentialOpaqueTypeUnavailability(SourceRange ReferenceRange,
                                          const DeclContext *ReferenceDC,
                                          const UnavailabilityReason &Reason);

/// Emits a diagnostic for a reference to a storage accessor that is
/// potentially unavailable.
void diagnosePotentialAccessorUnavailability(
    const AccessorDecl *Accessor, SourceRange ReferenceRange,
    const DeclContext *ReferenceDC, const UnavailabilityReason &Reason,
    bool ForInout);

/// Returns the availability attribute indicating deprecation if the
/// declaration is deprecated or null otherwise.
const AvailableAttr *getDeprecated(const Decl *D);

/// Emits a diagnostic for a reference to a declaration that is deprecated.
/// Callers can provide a lambda that adds additional information (such as a
/// fixit hint) to the deprecation diagnostic, if it is emitted.
void diagnoseIfDeprecated(SourceRange SourceRange,
                          const DeclContext *ReferenceDC,
                          const ValueDecl *DeprecatedDecl,
                          const ApplyExpr *Call);
/// @}

/// If LangOptions::DebugForbidTypecheckPrefix is set and the given decl
/// name starts with that prefix, an llvm fatal_error is triggered.
/// This is for testing purposes.
void checkForForbiddenPrefix(ASTContext &C, DeclBaseName Name);

/// Check error handling in the given type-checked top-level code.
void checkTopLevelErrorHandling(TopLevelCodeDecl *D);
void checkFunctionErrorHandling(AbstractFunctionDecl *D);
void checkInitializerErrorHandling(Initializer *I, Expr *E);
void checkEnumElementErrorHandling(EnumElementDecl *D, Expr *expr);
void checkPropertyWrapperErrorHandling(PatternBindingDecl *binding,
                                       Expr *expr);

/// If an expression references 'self.init' or 'super.init' in an
/// initializer context, returns the implicit 'self' decl of the constructor.
/// Otherwise, return nil.
VarDecl *getSelfForInitDelegationInConstructor(DeclContext *DC,
                                               UnresolvedDotExpr *UDE);

/// Diagnose assigning variable to itself.
bool diagnoseSelfAssignment(const Expr *E);

/// Builds a string representing a "default" generic argument list for
/// \p typeDecl. In general, this means taking the bound of each generic
/// parameter. The \p getPreferredType callback can be used to provide a
/// different type from the bound.
///
/// It may not always be possible to find a single appropriate type for a
/// particular parameter (say, if it has two bounds). In this case, an
/// Xcode-style placeholder will be used instead.
///
/// Returns true if the arguments list could be constructed, false if for
/// some reason it could not.
bool getDefaultGenericArgumentsString(
    SmallVectorImpl<char> &buf, const GenericTypeDecl *typeDecl,
    llvm::function_ref<Type(const GenericTypeParamDecl *)> getPreferredType =
        [](const GenericTypeParamDecl *) { return Type(); });

/// Attempt to omit needless words from the name of the given declaration.
Optional<DeclName> omitNeedlessWords(AbstractFunctionDecl *afd);

/// Attempt to omit needless words from the name of the given declaration.
Optional<Identifier> omitNeedlessWords(VarDecl *var);

/// Calculate edit distance between declaration names.
unsigned getCallEditDistance(DeclNameRef writtenName, DeclName correctedName,
                             unsigned maxEditDistance);

enum : unsigned {
  /// Never consider a candidate that's this distance away or worse.
  UnreasonableCallEditDistance = 8,

  /// Don't consider candidates that score worse than the given distance
  /// from the best candidate.
  MaxCallEditDistanceFromBestCandidate = 1
};

/// Check for a typo correction.
void performTypoCorrection(DeclContext *DC, DeclRefKind refKind,
                           Type baseTypeOrNull,
                           NameLookupOptions lookupOptions,
                           TypoCorrectionResults &corrections,
                           GenericSignatureBuilder *gsb = nullptr,
                           unsigned maxResults = 4);

/// Check if the given decl has a @_semantics attribute that gives it
/// special case type-checking behavior.
DeclTypeCheckingSemantics getDeclTypeCheckingSemantics(ValueDecl *decl);

/// Infers the differentiability parameter indices for the given
/// original or derivative `AbstractFunctionDecl`.
///
/// The differentiability parameters are inferred to be:
/// - All parameters of the function that conform to `Differentiable`.
/// - If the function result type is a function type (i.e. the function has
///   a curried method type), then also all parameters of the function result
///   type that conform to `Differentiable`.
///
/// Used by `@differentiable` and `@derivative` attribute type-checking.
IndexSubset *
inferDifferentiabilityParameters(AbstractFunctionDecl *AFD,
                                 GenericEnvironment *derivativeGenEnv);

/// Require that the library intrinsics for working with Optional<T>
/// exist.
bool requireOptionalIntrinsics(ASTContext &ctx, SourceLoc loc);

/// Require that the library intrinsics for working with
/// UnsafeMutablePointer<T> exist.
bool requirePointerArgumentIntrinsics(ASTContext &ctx, SourceLoc loc);

/// Require that the library intrinsics for creating
/// array literals exist.
bool requireArrayLiteralIntrinsics(ASTContext &ctx, SourceLoc loc);
}; // namespace TypeChecker

/// Temporary on-stack storage and unescaping for encoded diagnostic
/// messages.
///
///
class EncodedDiagnosticMessage {
  llvm::SmallString<128> Buf;

public:
  /// \param S A string with an encoded message
  EncodedDiagnosticMessage(StringRef S)
      : Message(Lexer::getEncodedStringSegment(S, Buf, true, true, ~0U)) {}

  /// The unescaped message to display to the user.
  const StringRef Message;
};

/// Returns true if the given method is an valid implementation of a
/// @dynamicCallable attribute requirement. The method is given to be defined
/// as one of the following: `dynamicallyCall(withArguments:)` or
/// `dynamicallyCall(withKeywordArguments:)`.
bool isValidDynamicCallableMethod(FuncDecl *decl, DeclContext *DC,
                                  bool hasKeywordArguments);

/// Returns true if the given subscript method is an valid implementation of
/// the `subscript(dynamicMember:)` requirement for @dynamicMemberLookup.
/// The method is given to be defined as `subscript(dynamicMember:)`.
bool isValidDynamicMemberLookupSubscript(SubscriptDecl *decl, DeclContext *DC,
                                         bool ignoreLabel = false);

/// Returns true if the given subscript method is an valid implementation of
/// the `subscript(dynamicMember:)` requirement for @dynamicMemberLookup.
/// The method is given to be defined as `subscript(dynamicMember:)` which
/// takes a single non-variadic parameter that conforms to
/// `ExpressibleByStringLiteral` protocol.
bool isValidStringDynamicMemberLookup(SubscriptDecl *decl, DeclContext *DC,
                                      bool ignoreLabel = false);

/// Returns true if the given subscript method is an valid implementation of
/// the `subscript(dynamicMember: {Writable}KeyPath<...>)` requirement for
/// @dynamicMemberLookup.
/// The method is given to be defined as `subscript(dynamicMember:)` which
/// takes a single non-variadic parameter of `{Writable}KeyPath<T, U>` type.
bool isValidKeyPathDynamicMemberLookup(SubscriptDecl *decl,
                                       bool ignoreLabel = false);

/// Compute the wrapped value type for the given property that has attached
/// property wrappers, when the backing storage is known to have the given type.
///
/// \param var A property that has attached property wrappers.
/// \param backingStorageType The type of the backing storage property.
/// \param limit How many levels of unwrapping to perform, where 0 means to return the
/// \c backingStorageType directly and the maximum is the number of attached property wrappers
/// (which will produce the original property type). If not specified, defaults to the maximum.
Type computeWrappedValueType(VarDecl *var, Type backingStorageType,
                             Optional<unsigned> limit = None);

/// Build a call to the init(wrappedValue:) initializers of the property
/// wrappers, filling in the given \c value as the original value. Optionally
/// pass a callback that will get invoked with the innermost init(wrappedValue:)
/// call.
Expr *buildPropertyWrapperWrappedValueCall(
    VarDecl *var, Type backingStorageType, Expr *value, bool ignoreAttributeArgs,
    llvm::function_ref<void(ApplyExpr *)> callback = [](ApplyExpr *) {});

/// Whether an overriding declaration requires the 'override' keyword.
enum class OverrideRequiresKeyword {
  /// The keyword is never required.
  Never,
  /// The keyword is always required.
  Always,
  /// The keyword can be implicit; it is not required.
  Implicit,
};

/// Determine whether overriding the given declaration requires a keyword.
OverrideRequiresKeyword overrideRequiresKeyword(ValueDecl *overridden);

/// Compute the type of a member that will be used for comparison when
/// performing override checking.
Type getMemberTypeForComparison(const ValueDecl *member,
                                const ValueDecl *derivedDecl = nullptr);

/// Determine whether the given declaration is an override by comparing type
/// information.
bool isOverrideBasedOnType(const ValueDecl *decl, Type declTy,
                           const ValueDecl *parentDecl, Type parentDeclTy);

/// Determine whether the given declaration is an operator defined in a
/// protocol. If \p type is not null, check specifically whether \p decl
/// could fulfill a protocol requirement for it.
bool isMemberOperator(FuncDecl *decl, Type type);

/// Returns `true` iff `AdditiveArithmetic` derived conformances are enabled.
bool isAdditiveArithmeticConformanceDerivationEnabled(SourceFile &SF);

/// Diagnose any Objective-C method overrides that aren't reflected
/// as overrides in Swift.
bool diagnoseUnintendedObjCMethodOverrides(SourceFile &sf);

/// Diagnose all conflicts between members that have the same
/// Objective-C selector in the same class.
///
/// \param sf The source file for which we are diagnosing conflicts.
///
/// \returns true if there were any conflicts diagnosed.
bool diagnoseObjCMethodConflicts(SourceFile &sf);

/// Diagnose any unsatisfied @objc optional requirements of
/// protocols that conflict with methods.
bool diagnoseObjCUnsatisfiedOptReqConflicts(SourceFile &sf);

/// Retrieve information about the given Objective-C method for
/// diagnostic purposes, to be used with OBJC_DIAG_SELECT in
/// DiagnosticsSema.def.
std::pair<unsigned, DeclName> getObjCMethodDiagInfo(
                                AbstractFunctionDecl *method);

bool areGenericRequirementsSatisfied(const DeclContext *DC,
                                     GenericSignature sig,
                                     SubstitutionMap Substitutions,
                                     bool isExtension);

/// Check for restrictions on the use of the @unknown attribute on a
/// case statement.
void checkUnknownAttrRestrictions(
    ASTContext &ctx, CaseStmt *caseBlock, CaseStmt *fallthroughDest,
    bool &limitExhaustivityChecks);

/// Bind all of the pattern variables that occur within a case statement and
/// all of its case items to their "parent" pattern variables, forming chains
/// of variables with the same name.
///
/// Given a case such as:
/// \code
/// case .a(let x), .b(let x), .c(let x):
/// \endcode
///
/// Each case item contains a (different) pattern variable named.
/// "x". This function will set the "parent" variable of the
/// second and third "x" variables to the "x" variable immediately
/// to its left. A fourth "x" will be the body case variable,
/// whose parent will be set to the "x" within the final case
/// item.
///
/// Each of the "x" variables must eventually have the same type, and agree on
/// let vs. var. This function does not perform any of that validation, leaving
/// it to later stages.
void bindSwitchCasePatternVars(CaseStmt *stmt);

} // end namespace swift

#endif
