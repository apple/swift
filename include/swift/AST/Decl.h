//===--- Decl.h - Swift Language Declaration ASTs ---------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines the Decl class and subclasses.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_DECL_H
#define SWIFT_DECL_H

#include "swift/AST/AccessScope.h"
#include "swift/AST/Attr.h"
#include "swift/AST/Availability.h"
#include "swift/AST/CaptureInfo.h"
#include "swift/AST/ClangNode.h"
#include "swift/AST/ConcreteDeclRef.h"
#include "swift/AST/DefaultArgumentKind.h"
#include "swift/AST/DiagnosticConsumer.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/FreestandingMacroExpansion.h"
#include "swift/AST/GenericParamKey.h"
#include "swift/AST/IfConfigClause.h"
#include "swift/AST/LayoutConstraint.h"
#include "swift/AST/LifetimeAnnotation.h"
#include "swift/AST/ReferenceCounting.h"
#include "swift/AST/RequirementSignature.h"
#include "swift/AST/StorageImpl.h"
#include "swift/AST/TypeAlignments.h"
#include "swift/AST/TypeWalker.h"
#include "swift/AST/Types.h"
#include "swift/Basic/ArrayRefView.h"
#include "swift/Basic/Compiler.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/InlineBitfield.h"
#include "swift/Basic/Located.h"
#include "swift/Basic/NullablePtr.h"
#include "swift/Basic/OptionalEnum.h"
#include "swift/Basic/Range.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/TrailingObjects.h"
#include <map>
#include <type_traits>

namespace swift {
  enum class AccessSemantics : unsigned char;
  class AccessorDecl;
  class ApplyExpr;
  class AvailabilityContext;
  class GenericEnvironment;
  class ArchetypeType;
  class ASTContext;
  struct ASTNode;
  class ASTPrinter;
  class ASTWalker;
  enum class BuiltinMacroKind: uint8_t;
  class ConstructorDecl;
  class DestructorDecl;
  class DiagnosticEngine;
  class DynamicSelfType;
  class Type;
  class Expr;
  struct ExternalSourceLocs;
  class CaptureListExpr;
  class DeclRefExpr;
  class ForeignAsyncConvention;
  class ForeignErrorConvention;
  class LiteralExpr;
  class BraceStmt;
  class DeclAttributes;
  class GenericContext;
  class GenericParamList;
  class GenericSignature;
  class GenericTypeParamDecl;
  class GenericTypeParamType;
  class MacroDecl;
  class MacroDefinition;
  class ModuleDecl;
  class NamedPattern;
  class EnumCaseDecl;
  class EnumElementDecl;
  class ParameterList;
  class ParameterTypeFlags;
  class Pattern;
  struct PrintOptions;
  struct PropertyWrapperAuxiliaryVariables;
  class PropertyWrapperInitializerInfo;
  struct PropertyWrapperTypeInfo;
  struct PropertyWrapperMutability;
  class ProtocolDecl;
  class PolymorphicEffectRequirementList;
  class ProtocolType;
  struct RawComment;
  enum class ResilienceExpansion : unsigned;
  enum class EffectKind : uint8_t;
  enum class PolymorphicEffectKind : uint8_t;
  class TrailingWhereClause;
  class TypeAliasDecl;
  class Stmt;
  class SubscriptDecl;
  class UnboundGenericType;
  class ValueDecl;
  class VarDecl;
  class OpaqueReturnTypeRepr;
  class Witness;

  namespace ast_scope {
  class AbstractPatternEntryScope;
  class GenericParamScope;
  class PatternEntryDeclScope;
  class PatternEntryInitializerScope;
  } // namespace ast_scope

enum class DeclKind : uint8_t {
#define DECL(Id, Parent) Id,
#define LAST_DECL(Id) Last_Decl = Id,
#define DECL_RANGE(Id, FirstId, LastId) \
  First_##Id##Decl = FirstId, Last_##Id##Decl = LastId,
#include "swift/AST/DeclNodes.def"
};
enum : unsigned { NumDeclKindBits =
  countBitsUsed(static_cast<unsigned>(DeclKind::Last_Decl)) };


/// Fine-grained declaration kind that provides a description of the
/// kind of entity a declaration represents, as it would be used in
/// diagnostics.
///
/// For example, \c FuncDecl is a single declaration class, but it has
/// several descriptive entries depending on whether it is an
/// operator, global function, local function, method, (observing)
/// accessor, etc.
enum class DescriptiveDeclKind : uint8_t {
  Import,
  Extension,
  EnumCase,
  TopLevelCode,
  IfConfig,
  PoundDiagnostic,
  PatternBinding,
  Var,
  Param,
  Let,
  Property,
  StaticProperty,
  ClassProperty,
  DistributedProperty,
  InfixOperator,
  PrefixOperator,
  PostfixOperator,
  PrecedenceGroup,
  TypeAlias,
  GenericTypeParam,
  AssociatedType,
  Type,
  Enum,
  Struct,
  Class,
  Actor,
  Protocol,
  GenericEnum,
  GenericStruct,
  GenericClass,
  GenericActor,
  GenericType,
  Subscript,
  StaticSubscript,
  ClassSubscript,
  Constructor,
  Destructor,
  LocalFunction,
  GlobalFunction,
  OperatorFunction,
  Method,
  StaticMethod,
  ClassMethod,
  DistributedMethod,
  Getter,
  Setter,
  InitAccessor,
  Addressor,
  MutableAddressor,
  ReadAccessor,
  ModifyAccessor,
  WillSet,
  DidSet,
  EnumElement,
  Module,
  Missing,
  MissingMember,
  Requirement,
  OpaqueResultType,
  OpaqueVarType,
  Macro,
  MacroExpansion
};

/// Describes which spelling was used in the source for the 'static' or 'class'
/// keyword.
enum class StaticSpellingKind : uint8_t {
  None,
  KeywordStatic,
  KeywordClass,
};

/// Keeps track of whether an enum has cases that have associated values.
enum class AssociatedValueCheck {
  /// We have not yet checked.
  Unchecked,
  /// The enum contains no cases or all cases contain no associated values.
  NoAssociatedValues,
  /// The enum contains at least one case with associated values.
  HasAssociatedValues,
};

/// Diagnostic printing of \c StaticSpellingKind.
llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, StaticSpellingKind SSK);

/// Diagnostic printing of \c ReferenceOwnership.
llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, ReferenceOwnership RO);

enum class SelfAccessKind : uint8_t {
  NonMutating,
  Mutating,
  LegacyConsuming,
  Consuming,
  Borrowing,
  LastSelfAccessKind = Borrowing,
};
enum : unsigned {
  NumSelfAccessKindBits =
      countBitsUsed(static_cast<unsigned>(SelfAccessKind::LastSelfAccessKind))
};
static_assert(uint8_t(SelfAccessKind::LastSelfAccessKind) <
                  (NumSelfAccessKindBits << 1),
              "Self Access Kind is too small to fit in SelfAccess kind bits. "
              "Please expand ");

/// Diagnostic printing of \c SelfAccessKind.
llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, SelfAccessKind SAK);

/// Encapsulation of the overload signature of a given declaration,
/// which is used to determine uniqueness of a declaration within a
/// given context.
///
/// Two definitions in the same context may not have the same overload
/// signature.
struct OverloadSignature {
  /// The full name of the declaration.
  DeclName Name;

  /// The kind of unary operator.
  UnaryOperatorKind UnaryOperator;

  /// Whether this is an instance member.
  unsigned IsInstanceMember : 1;

  /// Whether this is a variable.
  unsigned IsVariable : 1;

  /// Whether this is a function.
  unsigned IsFunction : 1;

  /// Whether this is an async function.
  unsigned IsAsyncFunction : 1;

  /// Whether this is a enum element.
  unsigned IsEnumElement : 1;

  /// Whether this is a nominal type.
  unsigned IsNominal : 1;

  /// Whether this is a type alias.
  unsigned IsTypeAlias : 1;

  /// Whether this is a macro.
  unsigned IsMacro : 1;

  /// Whether this signature is part of a protocol extension.
  unsigned InProtocolExtension : 1;

  /// Whether this signature is of a member defined in an extension of a generic
  /// type.
  unsigned InExtensionOfGenericType : 1;
  
  /// Whether this declaration has an opaque return type.
  unsigned HasOpaqueReturnType : 1;

  OverloadSignature()
      : UnaryOperator(UnaryOperatorKind::None), IsInstanceMember(false),
        IsVariable(false), IsFunction(false), IsAsyncFunction(false),
        InProtocolExtension(false), InExtensionOfGenericType(false),
        HasOpaqueReturnType(false) { }
};

/// Determine whether two overload signatures conflict.
///
/// \param sig1 The overload signature of the first declaration.
/// \param sig2 The overload signature of the second declaration.
/// \param skipProtocolExtensionCheck If \c true, members of protocol extensions
///        will be allowed to conflict with members of protocol declarations.
bool conflicting(const OverloadSignature& sig1, const OverloadSignature& sig2,
                 bool skipProtocolExtensionCheck = false);

/// Determine whether two overload signatures and overload types conflict.
///
/// \param ctx The AST context.
/// \param sig1 The overload signature of the first declaration.
/// \param sig1Type The overload type of the first declaration.
/// \param sig2 The overload signature of the second declaration.
/// \param sig2Type The overload type of the second declaration.
/// \param wouldConflictInSwift5 If non-null, the referenced boolean will be set
///        to \c true iff the function returns \c false for this version of
///        Swift, but the given overloads will conflict in Swift 5 mode.
/// \param skipProtocolExtensionCheck If \c true, members of protocol extensions
///        will be allowed to conflict with members of protocol declarations.
bool conflicting(ASTContext &ctx,
                 const OverloadSignature& sig1, CanType sig1Type,
                 const OverloadSignature& sig2, CanType sig2Type,
                 bool *wouldConflictInSwift5 = nullptr,
                 bool skipProtocolExtensionCheck = false);

/// The kind of artificial main to generate.
enum class ArtificialMainKind : uint8_t {
  NSApplicationMain,
  UIApplicationMain,
  TypeMain,
};

/// Decl - Base class for all declarations in Swift.
class alignas(1 << DeclAlignInBits) Decl : public ASTAllocated<Decl> {
protected:
  // clang-format off
  //
  // We format these different than clang-format wishes us to... so turn if off
  // for the inline bitfields.
  union { uint64_t OpaqueBits;

  SWIFT_INLINE_BITFIELD_BASE(Decl, bitmax(NumDeclKindBits,8)+1+1+1+1+1,
    Kind : bitmax(NumDeclKindBits,8),

    /// Whether this declaration is invalid.
    Invalid : 1,

    /// Whether this declaration was implicitly created, e.g.,
    /// an implicit constructor in a struct.
    Implicit : 1,

    /// Whether this declaration was mapped directly from a Clang AST.
    ///
    /// Use getClangNode() to retrieve the corresponding Clang AST.
    FromClang : 1,

    /// Whether this declaration was added to the surrounding
    /// DeclContext of an active #if config clause.
    EscapedFromIfConfig : 1,

    /// Whether this declaration is syntactically scoped inside of
    /// a local context, but should behave like a top-level
    /// declaration for name lookup purposes. This is used by
    /// lldb.
    Hoisted : 1
  );

  SWIFT_INLINE_BITFIELD_FULL(PatternBindingDecl, Decl, 1+1+2+16,
    /// Whether this pattern binding declares static variables.
    IsStatic : 1,

    /// Whether this pattern binding is synthesized by the debugger.
    IsDebugger : 1,

    /// Whether 'static' or 'class' was used.
    StaticSpelling : 2,

    : NumPadBits,

    /// The number of pattern binding declarations.
    NumPatternEntries : 16
  );
  
  SWIFT_INLINE_BITFIELD_FULL(EnumCaseDecl, Decl, 32,
    : NumPadBits,

    /// The number of tail-allocated element pointers.
    NumElements : 32
  );

  SWIFT_INLINE_BITFIELD(ValueDecl, Decl, 1+1+1+1,
    AlreadyInLookupTable : 1,

    /// Whether we have already checked whether this declaration is a 
    /// redeclaration.
    CheckedRedeclaration : 1,

    /// Whether the decl can be accessed by swift users; for instance,
    /// a.storage for lazy var a is a decl that cannot be accessed.
    IsUserAccessible : 1,

    /// Whether this member was synthesized as part of a derived
    /// protocol conformance.
    Synthesized : 1
  );

  SWIFT_INLINE_BITFIELD(AbstractStorageDecl, ValueDecl, 1,
    /// Whether this property is a type property (currently unfortunately
    /// called 'static').
    IsStatic : 1
  );

  SWIFT_INLINE_BITFIELD(VarDecl, AbstractStorageDecl, 2+1+1+1+1+1,
    /// Encodes whether this is a 'let' binding.
    Introducer : 2,

    /// Whether this declaration captures the 'self' param under the same name.
    IsSelfParamCapture : 1,

    /// Whether this is a property used in expressions in the debugger.
    /// It is up to the debugger to instruct SIL how to access this variable.
    IsDebuggerVar : 1,

    /// Whether this is the backing storage for a lazy property.
    IsLazyStorageProperty : 1,

    /// Whether this is the backing storage for a property wrapper.
    IsPropertyWrapperBackingProperty : 1,

    /// Whether this is a lazily top-level global variable from the main file.
    IsTopLevelGlobal : 1
  );

  SWIFT_INLINE_BITFIELD(ParamDecl, VarDecl, 1+2+NumDefaultArgumentKindBits,
    /// The specifier associated with this parameter + 1, or zero if the
    /// specifier has not been computed yet.  This determines
    /// the ownership semantics of the parameter.
    OwnershipSpecifier : 3,

    /// Information about a symbolic default argument, like #file.
    defaultArgumentKind : NumDefaultArgumentKindBits
  );

  SWIFT_INLINE_BITFIELD(SubscriptDecl, VarDecl, 2,
    StaticSpelling : 2
  );
  SWIFT_INLINE_BITFIELD(AbstractFunctionDecl, ValueDecl, 3+2+8+1+1+1+1+1+1+1,
    /// \see AbstractFunctionDecl::BodyKind
    BodyKind : 3,

    /// \see AbstractFunctionDecl::SILSynthesizeKind
    SILSynthesizeKind : 2,

    /// Import as member status.
    IAMStatus : 8,

    /// Whether the function has an implicit 'self' parameter.
    HasImplicitSelfDecl : 1,

    /// Whether we are overridden later.
    Overridden : 1,

    /// Whether the function is async.
    Async : 1,

    /// Whether the function body throws.
    Throws : 1,

    /// Whether this member's body consists of a single expression.
    HasSingleExpressionBody : 1,

    /// Whether peeking into this function detected nested type declarations.
    /// This is set when skipping over the decl at parsing.
    HasNestedTypeDeclarations : 1,

    /// Whether this function is a distributed thunk for a distributed
    /// function or computed property.
    DistributedThunk: 1
  );

  SWIFT_INLINE_BITFIELD(FuncDecl, AbstractFunctionDecl,
                        1+1+2+1+1+NumSelfAccessKindBits+1,
    /// Whether we've computed the 'static' flag yet.
    IsStaticComputed : 1,

    /// Whether this function is a 'static' method.
    IsStatic : 1,

    /// Whether 'static' or 'class' was used.
    StaticSpelling : 2,

    /// Whether we are statically dispatched even if overridable
    ForcedStaticDispatch : 1,

    /// Whether we've computed the 'self' access kind yet.
    SelfAccessComputed : 1,

    /// Backing bits for 'self' access kind.
    SelfAccess : NumSelfAccessKindBits,

    /// Whether this is a top-level function which should be treated
    /// as if it were in local context for the purposes of capture
    /// analysis.
    HasTopLevelLocalContextCaptures : 1
  );

  SWIFT_INLINE_BITFIELD(AccessorDecl, FuncDecl, 4 + 1 + 1,
    /// The kind of accessor this is.
    AccessorKind : 4,

    /// Whether the accessor is transparent.
    IsTransparent : 1,

    /// Whether we have computed the above.
    IsTransparentComputed : 1
  );

  SWIFT_INLINE_BITFIELD(ConstructorDecl, AbstractFunctionDecl, 1+1,
    /// Whether this constructor can fail, by building an Optional type.
    Failable : 1,

    /// Whether this initializer is a stub placed into a subclass to
    /// catch invalid delegations to a designated initializer not
    /// overridden by the subclass. A stub will always trap at runtime.
    ///
    /// Initializer stubs can be invoked from Objective-C or through
    /// the Objective-C runtime; there is no way to directly express
    /// an object construction that will invoke a stub.
    HasStubImplementation : 1
  );

  SWIFT_INLINE_BITFIELD_EMPTY(TypeDecl, ValueDecl);

  SWIFT_INLINE_BITFIELD_FULL(GenericTypeParamDecl, TypeDecl, 16+16+1+1,
    : NumPadBits,

    Depth : 16,
    Index : 16,
    ParameterPack : 1,

    /// Whether this generic parameter represents an opaque type.
    IsOpaqueType : 1
  );

  SWIFT_INLINE_BITFIELD_EMPTY(GenericTypeDecl, TypeDecl);

  SWIFT_INLINE_BITFIELD(TypeAliasDecl, GenericTypeDecl, 1+1,
    /// Whether the typealias forwards perfectly to its underlying type.
    IsCompatibilityAlias : 1,
    /// Whether this was a global typealias synthesized by the debugger.
    IsDebuggerAlias : 1
  );

  SWIFT_INLINE_BITFIELD(NominalTypeDecl, GenericTypeDecl, 1+1+1,
    /// Whether we have already added implicitly-defined initializers
    /// to this declaration.
    AddedImplicitInitializers : 1,

    /// Whether there is are lazily-loaded conformances for this nominal type.
    HasLazyConformances : 1,

    /// Whether this nominal type is having its semantic members resolved.
    IsComputingSemanticMembers : 1
  );

  SWIFT_INLINE_BITFIELD_FULL(ProtocolDecl, NominalTypeDecl, 1+1+1+1+1+1+1+1+1+1+1+1+1+8,
    /// Whether the \c RequiresClass bit is valid.
    RequiresClassValid : 1,

    /// Whether this is a class-bounded protocol.
    RequiresClass : 1,

    /// Whether the \c ExistentialConformsToSelf bit is valid.
    ExistentialConformsToSelfValid : 1,

    /// Whether the existential of this protocol conforms to itself.
    ExistentialConformsToSelf : 1,

    /// Whether the \c HasSelfOrAssociatedTypeRequirements bit is valid.
    HasSelfOrAssociatedTypeRequirementsValid : 1,

    /// Whether this protocol has \c Self or associated type requirements.
    /// See \c hasSelfOrAssociatedTypeRequirements() for clarification.
    HasSelfOrAssociatedTypeRequirements : 1,

    /// True if the protocol has requirements that cannot be satisfied (e.g.
    /// because they could not be imported from Objective-C).
    HasMissingRequirements : 1,

    /// Whether we've computed the inherited protocols list yet.
    InheritedProtocolsValid : 1,

    /// Whether we have a lazy-loaded requirement signature.
    HasLazyRequirementSignature : 1,

    /// Whether we have computed the list of associated types.
    HasAssociatedTypes : 1,

    /// Whether we have a lazy-loaded list of associated types.
    HasLazyAssociatedTypes : 1,

    /// Whether we have a lazy-loaded list of primary associated types.
    HasLazyPrimaryAssociatedTypes : 1,

    /// Whether we've computed the protocol requirements list yet.
    ProtocolRequirementsValid : 1,

    : NumPadBits,

    /// If this is a compiler-known protocol, this will be a KnownProtocolKind
    /// value, plus one. Otherwise, it will be 0.
    KnownProtocol : 8 // '8' for speed. This only needs 6.
  );

  SWIFT_INLINE_BITFIELD(ClassDecl, NominalTypeDecl, 1+1+2+1+1+1+1+1+1,
    /// Whether this class inherits its superclass's convenience initializers.
    InheritsSuperclassInits : 1,
    ComputedInheritsSuperclassInits : 1,

    /// \see ClassDecl::ForeignKind
    RawForeignKind : 2,

    HasMissingDesignatedInitializers : 1,
    ComputedHasMissingDesignatedInitializers : 1,

    HasMissingVTableEntries : 1,
    ComputedHasMissingVTableEntries : 1,

    /// Whether instances of this class are incompatible
    /// with weak and unowned references.
    IsIncompatibleWithWeakReferences : 1,

    /// Set when the class represents an actor
    IsActor : 1
  );

  SWIFT_INLINE_BITFIELD(StructDecl, NominalTypeDecl, 1 + 1 + 1,
                        /// True if this struct has storage for fields that
                        /// aren't accessible in Swift.
                        HasUnreferenceableStorage : 1,
                        /// True if this struct is imported from C++ and does
                        /// not have trivial value witness functions.
                        IsCxxNonTrivial : 1,
                        /// True if this struct is imported from C and has
                        /// address diversified ptrauth qualified field.
                        IsNonTrivialPtrAuth : 1);

  SWIFT_INLINE_BITFIELD(EnumDecl, NominalTypeDecl, 2+1,
    /// True if the enum has cases and at least one case has associated values.
    HasAssociatedValues : 2,
    /// True if the enum has at least one case that has some availability
    /// attribute.  A single bit because it's lazily computed along with the
    /// HasAssociatedValues bit.
    HasAnyUnavailableValues : 1
  );

  SWIFT_INLINE_BITFIELD(ModuleDecl, TypeDecl, 1+1+1+1+1+1+1+1+1+1+1+1+1+1+1+1,
    /// If the module is compiled as static library.
    StaticLibrary : 1,

    /// If the module was or is being compiled with `-enable-testing`.
    TestingEnabled : 1,

    /// If the module failed to load
    FailedToLoad : 1,

    /// Whether the module is resilient.
    ///
    /// \sa ResilienceStrategy
    RawResilienceStrategy : 1,

    /// Whether the module was rebuilt from a module interface instead of being
    /// build from the full source.
    IsBuiltFromInterface : 1,

    /// Whether all imports have been resolved. Used to detect circular imports.
    HasResolvedImports : 1,

    /// If the module was or is being compiled with `-enable-private-imports`.
    PrivateImportsEnabled : 1,

    /// If the module is compiled with `-enable-implicit-dynamic`.
    ImplicitDynamicEnabled : 1,

    /// Whether the module is a system module.
    IsSystemModule : 1,

    /// Whether the module was imported from Clang (or, someday, maybe another
    /// language).
    IsNonSwiftModule : 1,

    /// Whether this module is the main module.
    IsMainModule : 1,

    /// Whether this module has incremental dependency information available.
    HasIncrementalInfo : 1,

    /// Whether this module was built with -experimental-hermetic-seal-at-link.
    HasHermeticSealAtLink : 1,

    /// Whether this module has been compiled with comprehensive checking for
    /// concurrency, e.g., Sendable checking.
    IsConcurrencyChecked : 1,

    /// If the map from @objc provided name to top level swift::Decl in this
    /// module is populated
    ObjCNameLookupCachePopulated : 1,

    /// Whether this module has been built with C++ interoperability enabled.
    HasCxxInteroperability : 1
  );

  SWIFT_INLINE_BITFIELD(PrecedenceGroupDecl, Decl, 1+2,
    /// Is this an assignment operator?
    IsAssignment : 1,

    /// The group's associativity.  A value of the Associativity enum.
    Associativity : 2
  );

  SWIFT_INLINE_BITFIELD(ImportDecl, Decl, 3+8,
    ImportKind : 3,

    /// The number of elements in this path.
    NumPathElements : 8
  );

  SWIFT_INLINE_BITFIELD(ExtensionDecl, Decl, 4+1,
    /// An encoding of the default and maximum access level for this extension.
    /// The value 4 corresponds to AccessLevel::Public
    ///
    /// This is encoded as (1 << (maxAccess-1)) | (1 << (defaultAccess-1)),
    /// which works because the maximum is always greater than or equal to the
    /// default, and 'private' is never used. 0 represents an uncomputed value.
    DefaultAndMaxAccessLevel : 4,

    /// Whether there is are lazily-loaded conformances for this extension.
    HasLazyConformances : 1
  );

  SWIFT_INLINE_BITFIELD(IfConfigDecl, Decl, 1,
    /// Whether this decl is missing its closing '#endif'.
    HadMissingEnd : 1
  );

  SWIFT_INLINE_BITFIELD(PoundDiagnosticDecl, Decl, 1+1,
    /// `true` if the diagnostic is an error, `false` if it's a warning.
    IsError : 1,

    /// Whether this diagnostic has already been emitted.
    HasBeenEmitted : 1
  );

  SWIFT_INLINE_BITFIELD(MissingMemberDecl, Decl, 1+2,
    NumberOfFieldOffsetVectorEntries : 1,
    NumberOfVTableEntries : 2
  );

  SWIFT_INLINE_BITFIELD(MacroExpansionDecl, Decl, 16,
    Discriminator : 16
  );

  } Bits;
  // Turn back on clang-format now that we have defined our inline bitfields.
  // clang-format on

  // Storage for the declaration attributes.
  DeclAttributes Attrs;
  
  /// The next declaration in the list of declarations within this
  /// member context.
  Decl *NextDecl = nullptr;
  
  friend class DeclIterator;
  friend class IterableDeclContext;
  friend class MemberLookupTable;
  friend class DeclDeserializer;
  friend class RawCommentRequest;

private:
  llvm::PointerUnion<DeclContext *, ASTContext *> Context;

  /// The imported Clang declaration representing the \c @_objcInterface for
  /// this declaration (or vice versa), or \c nullptr if there is none.
  ///
  /// If \c this (an otherwise nonsensical value), the value has not yet been
  /// computed.
  Decl *CachedObjCImplementationDecl;

  Decl(const Decl&) = delete;
  void operator=(const Decl&) = delete;
  SourceLoc getLocFromSource() const;

  /// Returns the serialized locations of this declaration from the
  /// corresponding .swiftsourceinfo file. "Empty" (ie. \c BufferID of 0, an
  /// invalid \c Loc, and empty \c DocRanges) if either there is no
  /// .swiftsourceinfo or the buffer could not be created, eg. if the file
  /// no longer exists.
  const ExternalSourceLocs *getSerializedLocs() const;

  /// Directly set the invalid bit
  void setInvalidBit();

protected:

  Decl(DeclKind kind, llvm::PointerUnion<DeclContext *, ASTContext *> context)
    : Context(context), CachedObjCImplementationDecl(this) {
    Bits.OpaqueBits = 0;
    Bits.Decl.Kind = unsigned(kind);
    Bits.Decl.Invalid = false;
    Bits.Decl.Implicit = false;
    Bits.Decl.FromClang = false;
    Bits.Decl.EscapedFromIfConfig = false;
    Bits.Decl.Hoisted = false;
  }

  /// Get the Clang node associated with this declaration.
  ClangNode getClangNodeImpl() const;

  /// Set the Clang node associated with this declaration.
  void setClangNode(ClangNode Node);

  void updateClangNode(ClangNode node) {
    assert(hasClangNode());
    setClangNode(node);
  }
  friend class ClangImporter;

  DeclContext *getDeclContextForModule() const;

public:
  DeclKind getKind() const { return DeclKind(Bits.Decl.Kind); }

  /// Retrieve the name of the given declaration kind.
  ///
  /// This name should only be used for debugging dumps and other
  /// developer aids, and should never be part of a diagnostic or exposed
  /// to the user of the compiler in any way.
  static StringRef getKindName(DeclKind K);

  /// Retrieve the descriptive kind for this declaration.
  DescriptiveDeclKind getDescriptiveKind() const;

  /// Produce a name for the given descriptive declaration kind, which
  /// is suitable for use in diagnostics.
  static StringRef getDescriptiveKindName(DescriptiveDeclKind K);

  /// Whether swift users should be able to access this decl. For instance,
  /// var a.storage for lazy var a is an inaccessible decl. An inaccessible decl
  /// has to be implicit; but an implicit decl does not have to be inaccessible,
  /// for instance, self.
  bool isUserAccessible() const;

  /// Determine if the decl can have a comment.  If false, a comment will
  /// not be serialized.
  bool canHaveComment() const;

  LLVM_READONLY
  DeclContext *getDeclContext() const {
    if (auto dc = Context.dyn_cast<DeclContext *>())
      return dc;

    return getDeclContextForModule();
  }
  void setDeclContext(DeclContext *DC);

  /// Retrieve the innermost declaration context corresponding to this
  /// declaration, which will either be the declaration itself (if it's
  /// also a declaration context) or its declaration context.
  DeclContext *getInnermostDeclContext() const;

  /// Retrieve the module in which this declaration resides.
  LLVM_READONLY
  ModuleDecl *getModuleContext() const;

  /// getASTContext - Return the ASTContext that this decl lives in.
  LLVM_READONLY
  ASTContext &getASTContext() const {
    if (auto dc = Context.dyn_cast<DeclContext *>())
      return dc->getASTContext();

    return *Context.get<ASTContext *>();
  }

  const DeclAttributes &getAttrs() const {
    return Attrs;
  }

  DeclAttributes &getAttrs() {
    return Attrs;
  }

  /// Retrieve runtime discoverable attributes (if any) associated
  /// with this declaration.
  ArrayRef<CustomAttr *> getRuntimeDiscoverableAttrs() const;

  /// Returns the attributes that were directly attached to this declaration
  /// as written in source, ie. does not include attributes generated by macro
  /// expansions.
  OrigDeclAttributes getOriginalAttrs() const;

  /// Returns the semantic attributes attached to this declaration,
  /// including attributes that are generated as the result of member
  /// attribute macro expansion.
  DeclAttributes getSemanticAttrs() const;

  using AuxiliaryDeclCallback = llvm::function_ref<void(Decl *)>;

  /// Iterate over the auxiliary declarations for this declaration,
  /// invoking the given callback with each auxiliary decl.
  ///
  /// Auxiliary declarations can be property wrapper backing variables,
  /// backing variables for 'lazy' vars, or peer macro expansions.
  ///
  /// When \p visitFreestandingExpanded is true (the default), this will also
  /// visit the declarations produced by a freestanding macro expansion.
  void visitAuxiliaryDecls(
      AuxiliaryDeclCallback callback,
      bool visitFreestandingExpanded = true
  ) const;

  using MacroCallback = llvm::function_ref<void(CustomAttr *, MacroDecl *)>;

  /// Iterate over each attached macro with the given role, invoking the
  /// given callback with each macro custom attribute and corresponding macro
  /// declaration.
  void forEachAttachedMacro(MacroRole role, MacroCallback) const;

  /// Returns the resolved macro for the given custom attribute
  /// attached to this declaration.
  MacroDecl *getResolvedMacro(CustomAttr *attr) const;

  /// Retrieve the discriminator for the given custom attribute that names
  /// an attached macro.
  unsigned getAttachedMacroDiscriminator(DeclBaseName macroName, MacroRole role,
                                         const CustomAttr *attr) const;

  /// Returns the innermost enclosing decl with an availability annotation.
  const Decl *getInnermostDeclWithAvailability() const;

  /// Returns the introduced OS version in the given platform kind specified
  /// by @available attribute.
  /// This function won't consider the parent context to get the information.
  Optional<llvm::VersionTuple> getIntroducedOSVersion(PlatformKind Kind) const;

  /// Returns the OS version in which the decl became ABI as specified by the
  /// `@backDeployed` attribute.
  Optional<llvm::VersionTuple>
  getBackDeployedBeforeOSVersion(ASTContext &Ctx) const;

  /// Returns true if the decl has an active `@backDeployed` attribute for the
  /// given context.
  bool isBackDeployed(ASTContext &Ctx) const;

  /// Returns the starting location of the entire declaration.
  SourceLoc getStartLoc() const { return getSourceRange().Start; }

  /// Returns the end location of the entire declaration.
  SourceLoc getEndLoc() const { return getSourceRange().End; }

  /// Returns the preferred location when referring to declarations
  /// in diagnostics.
  SourceLoc getLoc(bool SerializedOK = true) const;

  /// Returns the source range of the entire declaration.
  SourceRange getSourceRange() const;

  /// Returns the source range of the declaration including its attributes.
  SourceRange getSourceRangeIncludingAttrs() const;

  SourceLoc TrailingSemiLoc;

  /// Whether this declaration is within a macro expansion relative to
  /// its decl context. If the decl context is itself in a macro expansion,
  /// the method returns \c true if this decl is in a different macro
  /// expansion buffer than the context.
  ///
  /// \Note this method returns \c false if this declaration was
  /// constructed from a serialized module.
  bool isInMacroExpansionInContext() const;

  /// Returns the appropriate kind of entry point to generate for this class,
  /// based on its attributes.
  ///
  /// It is an error to call this on a type that does not have either an
  /// *ApplicationMain or an main attribute.
  ArtificialMainKind getArtificialMainKind() const;

  SWIFT_DEBUG_DUMP;
  SWIFT_DEBUG_DUMPER(dump(const char *filename));
  void dump(raw_ostream &OS, unsigned Indent = 0) const;

  /// Pretty-print the given declaration.
  ///
  /// \param OS Output stream to which the declaration will be printed.
  void print(raw_ostream &OS) const;
  void print(raw_ostream &OS, const PrintOptions &Opts) const;

  void printInherited(ASTPrinter &Printer, const PrintOptions &Options) const;

  /// Pretty-print the given declaration.
  ///
  /// \param Printer ASTPrinter object.
  ///
  /// \param Opts Options to control how pretty-printing is performed.
  ///
  /// \returns true if the declaration was printed or false if the print options
  /// required the declaration to be skipped from printing.
  bool print(ASTPrinter &Printer, const PrintOptions &Opts) const;

  /// Determine whether this declaration should be printed when
  /// encountered in its declaration context's list of members.
  bool shouldPrintInContext(const PrintOptions &PO) const;

  bool walk(ASTWalker &walker);

  /// Return whether this declaration has been determined invalid.
  bool isInvalid() const;
  
  /// Mark this declaration invalid.
  void setInvalid();

  /// Determine whether this declaration was implicitly generated by the
  /// compiler (rather than explicitly written in source code).
  bool isImplicit() const { return Bits.Decl.Implicit; }

  /// Mark this declaration as implicit.
  void setImplicit(bool implicit = true) { Bits.Decl.Implicit = implicit; }

  /// Determine whether this declaration is syntactically scoped inside of
  /// a local context, but should behave like a top-level declaration
  /// for name lookup purposes. This is used by lldb.
  bool isHoisted() const { return Bits.Decl.Hoisted; }

  /// Set whether this declaration should be syntactically scoped inside
  /// of a local context, but should behave like a top-level declaration,
  /// but should behave like a top-level declaration. This is used by lldb.
  void setHoisted(bool hoisted = true) { Bits.Decl.Hoisted = hoisted; }

  /// Whether this declaration predates the introduction of concurrency.
  bool preconcurrency() const;

public:
  bool escapedFromIfConfig() const {
    return Bits.Decl.EscapedFromIfConfig;
  }

  void setEscapedFromIfConfig(bool Escaped) {
    Bits.Decl.EscapedFromIfConfig = Escaped;
  }

  /// \returns the unparsed comment attached to this declaration.
  RawComment getRawComment() const;

  Optional<StringRef> getGroupName() const;

  Optional<StringRef> getSourceFileName() const;

  Optional<unsigned> getSourceOrder() const;

  /// \returns The brief comment attached to this declaration, or the brief
  /// comment attached to the comment providing decl.
  StringRef getSemanticBriefComment() const;

  /// Returns true if there is a Clang AST node associated
  /// with self.
  bool hasClangNode() const {
    return Bits.Decl.FromClang;
  }

  /// Retrieve the Clang AST node from which this declaration was
  /// synthesized, if any.
  LLVM_READONLY
  ClangNode getClangNode() const {
    if (!Bits.Decl.FromClang)
      return ClangNode();

    return getClangNodeImpl();
  }

  /// Retrieve the Clang declaration from which this declaration was
  /// synthesized, if any.
  LLVM_READONLY
  const clang::Decl *getClangDecl() const {
    if (!Bits.Decl.FromClang)
      return nullptr;

    return getClangNodeImpl().getAsDecl();
  }

  /// Retrieve the Clang macro from which this declaration was
  /// synthesized, if any.
  LLVM_READONLY
  const clang::MacroInfo *getClangMacro() {
    if (!Bits.Decl.FromClang)
      return nullptr;

    return getClangNodeImpl().getAsMacro();
  }

  /// If this is the Swift implementation of a declaration imported from ObjC,
  /// returns the imported declaration. Otherwise return \c nullptr.
  ///
  /// \seeAlso ExtensionDecl::isObjCInterface()
  Decl *getImplementedObjCDecl() const;

  /// If this is the ObjC interface of a declaration implemented in Swift,
  /// returns the implementating declaration. Otherwise return \c nullptr.
  ///
  /// \seeAlso ExtensionDecl::isObjCInterface()
  Decl *getObjCImplementationDecl() const;

  Optional<Decl *> getCachedObjCImplementationDecl() const {
    if (CachedObjCImplementationDecl == this)
      return None;
    return CachedObjCImplementationDecl;
  }

  void setCachedObjCImplementationDecl(Decl *decl) {
    assert((CachedObjCImplementationDecl == this
              || CachedObjCImplementationDecl == decl)
               && "can't change CachedObjCInterfaceDecl once it's computed");
    assert(decl != this && "can't form circular reference");
    CachedObjCImplementationDecl = decl;
  }

  /// Return the GenericContext if the Decl has one.
  LLVM_READONLY
  const GenericContext *getAsGenericContext() const;

  bool hasUnderscoredNaming() const;

  bool isPrivateStdlibDecl(bool treatNonBuiltinProtocolsAsPublic = true) const;
  
  /// Check if this is a declaration defined at the top level of the Swift module
  bool isStdlibDecl() const;

  /// The effective lifetime resulting from the decorations on the declaration.
  ///
  /// Usually, this, not getLifetimeAnnotationFromAttributes should be used.
  LifetimeAnnotation getLifetimeAnnotation() const;

  /// The source-level lifetime attribute, either @_eagerMove or @_noEagerMove
  /// that the declaration bears.
  ///
  /// Usually getLifetimeAnnotation should be used.
  ///
  /// Needed to access the attributes before the AST has been fully formed, such
  /// as when printing.
  LifetimeAnnotation getLifetimeAnnotationFromAttributes() const;

  bool isNoImplicitCopy() const {
    return getAttrs().hasAttribute<NoImplicitCopyAttr>();
  }

  AvailabilityContext getAvailabilityForLinkage() const;

  /// Whether this declaration or one of its outer contexts has the
  /// @_weakLinked attribute.
  bool isAlwaysWeakImported() const;

  /// Whether this declaration is weak-imported from the given module,
  /// either because of the presence of the @_weakLinked attribute, or
  /// because of availability.
  ///
  /// Note that \p fromModule should either be the "main module" or
  /// nullptr. (This is because when it is non-null, we query the
  /// current deployment target, and not the deployment target that
  /// the module was built with.)
  ///
  /// If \p fromModule is the main module, this returns false when the
  /// declaration is part of the main module, or if the declaration is
  /// at least as available as the current deployment target.
  ///
  /// If \p fromModule is null, we instead return true if the
  /// declaration is meant to be weak linked with _some_ deployment
  /// target; that is, the presence of the @_weakLinked attribute or
  /// any kind of availability is enough, irrespective of the current
  /// deployment target.
  bool isWeakImported(ModuleDecl *fromModule) const;

  /// Returns true if the nature of this declaration allows overrides syntactically.
  ///
  /// If this returns true, the decl can be safely casted to ValueDecl.
  bool isSyntacticallyOverridable() const;

  /// Retrieve the global actor attribute that applies to this declaration,
  /// if any.
  ///
  /// This is the "raw" global actor attribute as written directly on the
  /// declaration, along with the nominal type declaration to which it refers,
  /// without any inference rules applied.
  Optional<std::pair<CustomAttr *, NominalTypeDecl *>>
  getGlobalActorAttr() const;

  /// If an alternative module name is specified for this decl, e.g. using
  /// @_originalDefinedIn attribute, this function returns this module name.
  StringRef getAlternateModuleName() const;

  // Is this Decl an SPI? It can be directly marked with @_spi or is defined in
  // an @_spi context.
  bool isSPI() const;

  bool isAvailableAsSPI() const;

  /// Determine whether this Decl has either Private or FilePrivate access,
  /// and its DeclContext does not.
  bool isOutermostPrivateOrFilePrivateScope() const;

  /// Retrieve the @available attribute that provides the OS version range that
  /// this declaration is available in.
  ///
  /// This attribute may come from an enclosing decl since availability is
  /// inherited. The second member of the returned pair is the decl that owns
  /// the attribute.
  Optional<std::pair<const AvailableAttr *, const Decl *>>
  getSemanticAvailableRangeAttr() const;

  /// Retrieve the @available attribute that makes this declaration unavailable,
  /// if any.
  ///
  /// This attribute may come from an enclosing decl since availability is
  /// inherited. The second member of the returned pair is the decl that owns
  /// the attribute.
  ///
  /// Note that this notion of unavailability is broader than that which is
  /// checked by \c AvailableAttr::isUnavailable.
  Optional<std::pair<const AvailableAttr *, const Decl *>>
  getSemanticUnavailableAttr() const;

  // List the SPI groups declared with @_spi or inherited by this decl.
  //
  // SPI groups are inherited from the parent contexts only if the local decl
  // doesn't declare any @_spi.
  ArrayRef<Identifier> getSPIGroups() const;

  /// Returns true if this declaration has any `@backDeployed` attributes.
  bool hasBackDeployedAttr() const;

  /// Emit a diagnostic tied to this declaration.
  template<typename ...ArgTypes>
  InFlightDiagnostic diagnose(
      Diag<ArgTypes...> ID,
      typename detail::PassArgument<ArgTypes>::type... Args) const {
    return getDiags().diagnose(this, ID, std::move(Args)...);
  }

  /// Retrieve the diagnostic engine for diagnostics emission.
  LLVM_READONLY
  DiagnosticEngine &getDiags() const;
};

/// Allocates memory for a Decl with the given \p baseSize. If necessary,
/// it includes additional space immediately preceding the Decl for a ClangNode.
/// \note \p baseSize does not need to include space for a ClangNode if
/// requested -- the necessary space will be added automatically.
template <typename DeclTy, typename AllocatorTy>
void *allocateMemoryForDecl(AllocatorTy &allocator, size_t baseSize,
                            bool includeSpaceForClangNode) {
  static_assert(alignof(DeclTy) >= sizeof(void *),
                "A pointer must fit in the alignment of the DeclTy!");

  size_t size = baseSize;
  if (includeSpaceForClangNode)
    size += alignof(DeclTy);

  void *mem = allocator.Allocate(size, alignof(DeclTy));
  if (includeSpaceForClangNode)
    mem = reinterpret_cast<char *>(mem) + alignof(DeclTy);
  return mem;
}

// A private class for forcing exact field layout.
class alignas(8) _GenericContext {
// Not really public. See GenericContext.
public:
  /// The state of the generic parameters.
  enum class GenericParamsState: uint8_t {
    /// The stored generic parameters represent parsed generic parameters,
    /// written in the source.
    Parsed = 0,
    /// The stored generic parameters represent generic parameters that are
    /// synthesized by the type checker but were not written in the source.
    TypeChecked = 1,
    /// The stored generic parameters represent both the parsed and
    /// type-checked generic parameters.
    ParsedAndTypeChecked = 2,
  };

  llvm::PointerIntPair<GenericParamList *, 2, GenericParamsState>
      GenericParamsAndState;

  /// The trailing where clause.
  ///
  /// Note that this is not currently serialized, because semantic analysis
  /// moves the trailing where clause into the generic parameter list.
  TrailingWhereClause *TrailingWhere = nullptr;

  /// The generic signature of this declaration.
  llvm::PointerIntPair<GenericSignature, 1, bool> GenericSigAndBit;
};

class GenericContext : private _GenericContext, public DeclContext {
  friend class GenericParamListRequest;
  friend class GenericSignatureRequest;
  
protected:
  GenericContext(DeclContextKind Kind, DeclContext *Parent,
                 GenericParamList *Params);

public:
  /// Retrieve the set of parameters to a generic context, or null if
  /// this context is not generic.
  GenericParamList *getGenericParams() const;

  /// Retrieve the generic parameters as written in source. Unlike
  /// getGenericParams() this will not synthesize generic parameters for
  /// extensions, protocols and certain type aliases.
  GenericParamList *getParsedGenericParams() const;

  /// Determine whether this context has generic parameters
  /// of its own.
  ///
  /// \code
  /// class C<T> {
  ///   func f1() {}    // isGeneric == false
  ///   func f2<T>() {} // isGeneric == true
  /// }
  ///
  /// protocol P { // isGeneric == true due to implicit Self param
  ///   func p()   // isGeneric == false
  /// }
  /// \endcode
  bool isGeneric() const { return getGenericParams() != nullptr; }
  bool hasComputedGenericSignature() const;
  bool isComputingGenericSignature() const;
  
  /// Retrieve the trailing where clause for this extension, if any.
  TrailingWhereClause *getTrailingWhereClause() const {
    return TrailingWhere;
  }

  /// Set the trailing where clause for this extension.
  void setTrailingWhereClause(TrailingWhereClause *trailingWhereClause) {
    TrailingWhere = trailingWhereClause;
  }

  /// Retrieve the generic signature for this context.
  GenericSignature getGenericSignature() const;

  /// Retrieve the generic context for this context.
  GenericEnvironment *getGenericEnvironment() const;

  /// Retrieve the innermost generic parameter types.
  TypeArrayView<GenericTypeParamType> getInnermostGenericParamTypes() const;

  /// Retrieve the generic requirements.
  ArrayRef<Requirement> getGenericRequirements() const;

  /// Set the generic signature of this context.
  void setGenericSignature(GenericSignature genericSig);

  /// Retrieve the position of any where clause for this context's
  /// generic parameters.
  SourceRange getGenericTrailingWhereClauseSourceRange() const;
};
static_assert(sizeof(_GenericContext) + sizeof(DeclContext) ==
              sizeof(GenericContext), "Please add fields to _GenericContext");

/// ImportDecl - This represents a single import declaration, e.g.:
///   import Swift
///   import typealias Swift.Int
class ImportDecl final : public Decl,
    private llvm::TrailingObjects<ImportDecl, ImportPath::Element> {
  friend TrailingObjects;
  friend class Decl;

  SourceLoc ImportLoc;
  SourceLoc KindLoc;
  /// Used to store the real module name corresponding to this import decl in
  /// case module aliasing is used. For example if '-module-alias Foo=Bar' was
  /// passed and this decl is 'import Foo', the real name 'Bar' will be stored.
  Identifier RealModuleName;

  /// The resolved module.
  ModuleDecl *Mod = nullptr;

  ImportDecl(DeclContext *DC, SourceLoc ImportLoc, ImportKind K,
             SourceLoc KindLoc, ImportPath Path);
public:
  static ImportDecl *create(ASTContext &C, DeclContext *DC,
                            SourceLoc ImportLoc, ImportKind Kind,
                            SourceLoc KindLoc,
                            ImportPath Path,
                            ClangNode ClangN = ClangNode());

  /// Returns the import kind that is most appropriate for \p VD.
  ///
  /// Note that this will never return \c Type; an imported typealias will use
  /// the more specific kind from its underlying type.
  static ImportKind getBestImportKind(const ValueDecl *VD);

  /// Returns the most appropriate import kind for the given list of decls.
  ///
  /// If the list is non-homogeneous, or if there is more than one decl that
  /// cannot be overloaded, returns None.
  static Optional<ImportKind> findBestImportKind(ArrayRef<ValueDecl *> Decls);

  ImportKind getImportKind() const {
    return static_cast<ImportKind>(Bits.ImportDecl.ImportKind);
  }

  /// Retrieves the import path as written in the source code.
  /// 
  /// \returns An \c ImportPath corresponding to this import decl. If module aliasing
  ///          was used, this will contain the aliased name of the module; for instance,
  ///          if you wrote 'import Foo' but passed '-module-alias Foo=Bar', this import
  ///          path will include 'Foo'. This return value is always owned by \c ImportDecl
  ///          (which is owned by the AST context), so it can be persisted.
  ImportPath getImportPath() const {
    return ImportPath({ getTrailingObjects<ImportPath::Element>(),
                        static_cast<size_t>(Bits.ImportDecl.NumPathElements) });
  }

  /// Retrieves the import path, replacing any module aliases with real names.
  /// 
  /// \param scratch An \c ImportPath::Builder which may, if necessary, be used to
  ///        construct the return value. It may go unused, so you should not try to
  ///        read the result from it; use the return value instead.
  /// \returns An \c ImportPath corresponding to this import decl. If module aliasing
  ///          was used, this will contain the real name of the module; for instance,
  ///          if you wrote 'import Foo' but passed '-module-alias Foo=Bar', this import
  ///          path will include 'Bar'. This return value may be owned by \p scratch,
  ///          so it should not be used after \p scratch is destroyed.
  ImportPath getRealImportPath(ImportPath::Builder &scratch) const;

  /// Retrieves the part of the import path that contains the module name,
  /// as written in the source code.
  /// 
  /// \returns A \c ImportPath::Module corresponding to this import decl. If module
  ///          aliasing was used, this will contain the aliased name of the module; for
  ///          instance, if you wrote 'import Foo' but passed '-module-alias Foo=Bar',
  ///          this module path will contain 'Foo'. This return value is always owned by
  ///          \c ImportDecl (which is owned by the AST context), so it can be persisted.
  ImportPath::Module getModulePath() const {
    return getImportPath().getModulePath(getImportKind());
  }

  /// Retrieves the part of the import path that contains the module name,
  /// replacing any module aliases with real names.
  /// 
  /// \param scratch An \c ImportPath::Builder which may, if necessary, be used to
  ///        construct the return value. It may go unused, so you should not try to
  ///        read the result from it; use the return value instead.
  /// \returns An \c ImportPath::Module corresponding to this import decl. If module
  ///          aliasing was used, this will contain the real name of the module; for
  ///          instance, if you wrote 'import Foo' but passed '-module-alias Foo=Bar',
  ///          the returned path will contain 'Bar'. This return value may be owned
  ///          by \p scratch, so it should not be used after \p scratch is destroyed.
  ImportPath::Module getRealModulePath(ImportPath::Builder &scratch) const {
    return getRealImportPath(scratch).getModulePath(getImportKind());
  }

  ImportPath::Access getAccessPath() const {
    return getImportPath().getAccessPath(getImportKind());
  }

  bool isExported() const {
    return getAttrs().hasAttribute<ExportedAttr>();
  }

  bool isTestable() const {
    return getAttrs().hasAttribute<TestableAttr>();
  }

  ModuleDecl *getModule() const { return Mod; }
  void setModule(ModuleDecl *M) { Mod = M; }

  /// For a scoped import such as 'import class Foundation.NSString', retrieve
  /// the decls it references. Otherwise, returns an empty array.
  ArrayRef<ValueDecl *> getDecls() const;

  /// Access level of this import, either explicitly declared or implicit.
  AccessLevel getAccessLevel() const;

  /// Is the access level of this import implicit, aka a default import?
  bool isAccessLevelImplicit() const;

  const clang::Module *getClangModule() const {
    return getClangNode().getClangModule();
  }

  SourceLoc getStartLoc() const { return ImportLoc; }
  SourceLoc getLocFromSource() const {
    return getImportPath().getSourceRange().Start;
  }
  SourceRange getSourceRange() const {
    return SourceRange(ImportLoc, getImportPath().getSourceRange().End);
  }
  SourceLoc getKindLoc() const { return KindLoc; }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Import;
  }
};

/// An entry in the "inherited" list of a type or extension.
struct InheritedEntry : public TypeLoc {
  /// Whether there was an @unchecked attribute.
  bool isUnchecked = false;

  InheritedEntry(const TypeLoc &typeLoc);

  InheritedEntry(const TypeLoc &typeLoc, bool isUnchecked)
    : TypeLoc(typeLoc), isUnchecked(isUnchecked) { }
};

/// ExtensionDecl - This represents a type extension containing methods
/// associated with the type.  This is not a ValueDecl and has no Type because
/// there are no runtime values of the Extension's type.  
class ExtensionDecl final : public GenericContext, public Decl,
                            public IterableDeclContext {
  SourceLoc ExtensionLoc;  // Location of 'extension' keyword.
  SourceRange Braces;

  /// The type being extended.
  TypeRepr *ExtendedTypeRepr;

  /// The nominal type being extended.
  ///
  /// The bit indicates whether binding has been attempted. The pointer can be
  /// null if either no binding was attempted or if binding could not find  the
  /// extended nominal.
  llvm::PointerIntPair<NominalTypeDecl *, 1, bool> ExtendedNominal;

  ArrayRef<InheritedEntry> Inherited;

  /// The next extension in the linked list of extensions.
  ///
  /// The bit indicates whether this extension has been resolved to refer to
  /// a known nominal type.
  llvm::PointerIntPair<ExtensionDecl *, 1, bool> NextExtension
    = {nullptr, false};

  /// Note that we have added a member into the iterable declaration context.
  void addedMember(Decl *member);

  friend class ExtensionIterator;
  friend class NominalTypeDecl;
  friend class MemberLookupTable;
  friend class ConformanceLookupTable;
  friend class IterableDeclContext;

  ExtensionDecl(SourceLoc extensionLoc, TypeRepr *extendedType,
                ArrayRef<InheritedEntry> inherited,
                DeclContext *parent,
                TrailingWhereClause *trailingWhereClause);

  /// Retrieve the conformance loader (if any), and removing it in the
  /// same operation. The caller is responsible for loading the
  /// conformances.
  std::pair<LazyMemberLoader *, uint64_t> takeConformanceLoader() {
    if (!Bits.ExtensionDecl.HasLazyConformances)
      return { nullptr, 0 };

    return takeConformanceLoaderSlow();
  }

  /// Slow path for \c takeConformanceLoader().
  std::pair<LazyMemberLoader *, uint64_t> takeConformanceLoaderSlow();

  friend class ExtendedNominalRequest;
  friend class Decl;
public:
  using Decl::getASTContext;

  /// Create a new extension declaration.
  static ExtensionDecl *create(ASTContext &ctx, SourceLoc extensionLoc,
                               TypeRepr *extendedType,
                               ArrayRef<InheritedEntry> inherited,
                               DeclContext *parent,
                               TrailingWhereClause *trailingWhereClause,
                               ClangNode clangNode = ClangNode());

  SourceLoc getStartLoc() const { return ExtensionLoc; }
  SourceLoc getLocFromSource() const { return ExtensionLoc; }
  SourceRange getSourceRange() const {
    if (!Braces.isValid())
      return SourceRange(ExtensionLoc);
    return { ExtensionLoc, Braces.End };
  }

  SourceRange getBraces() const { return Braces; }
  void setBraces(SourceRange braces) { Braces = braces; }

  bool hasBeenBound() const { return ExtendedNominal.getInt(); }

  void setExtendedNominal(NominalTypeDecl *n) {
    ExtendedNominal.setPointerAndInt(n, true);
  }

  /// Retrieve the type being extended.
  ///
  /// Only use this entry point when the complete type, as spelled in the source,
  /// is required. For most clients, \c getExtendedNominal(), which provides
  /// only the \c NominalTypeDecl, will suffice.
  Type getExtendedType() const;

  /// Retrieve the nominal type declaration that is being extended.
  /// Will  trip an assertion if the declaration has not already been computed.
  /// In order to fail fast when type checking work is attempted
  /// before extension binding has taken place.

  NominalTypeDecl *getExtendedNominal() const;

  /// Compute the nominal type declaration that is being extended.
  NominalTypeDecl *computeExtendedNominal() const;

  /// \c hasBeenBound means nothing if this extension can never been bound
  /// because it is not at the top level.
  bool canNeverBeBound() const;

  bool hasValidParent() const;

  /// Retrieve the extended type definition as written in the source, if it exists.
  ///
  /// Repr would not be available if the extension was been loaded
  /// from a serialized module.
  TypeRepr *getExtendedTypeRepr() const { return ExtendedTypeRepr; }
                              
  /// Retrieve the set of protocols that this type inherits (i.e,
  /// explicitly conforms to).
  ArrayRef<InheritedEntry> getInherited() const { return Inherited; }

  void setInherited(ArrayRef<InheritedEntry> i) { Inherited = i; }

  bool hasDefaultAccessLevel() const {
    return Bits.ExtensionDecl.DefaultAndMaxAccessLevel != 0;
  }

  uint8_t getDefaultAndMaxAccessLevelBits() const {
    return Bits.ExtensionDecl.DefaultAndMaxAccessLevel;
  }
  void setDefaultAndMaxAccessLevelBits(AccessLevel defaultAccess,
                                       AccessLevel maxAccess) {
    Bits.ExtensionDecl.DefaultAndMaxAccessLevel =
      (1 << (static_cast<unsigned>(defaultAccess) - 1)) |
      (1 << (static_cast<unsigned>(maxAccess) - 1));
  }

  AccessLevel getDefaultAccessLevel() const;
  AccessLevel getMaxAccessLevel() const;

  void setDefaultAndMaxAccess(AccessLevel defaultAccess,
                              AccessLevel maxAccess) {
    assert(!hasDefaultAccessLevel() && "default access level already set");
    assert(maxAccess >= defaultAccess);
    assert(maxAccess != AccessLevel::Private && "private not valid");
    assert(defaultAccess != AccessLevel::Private && "private not valid");
    setDefaultAndMaxAccessLevelBits(defaultAccess, maxAccess);
    assert(getDefaultAccessLevel() == defaultAccess && "not enough bits");
    assert(getMaxAccessLevel() == maxAccess && "not enough bits");
  }

  void setConformanceLoader(LazyMemberLoader *resolver, uint64_t contextData);

  /// Determine whether this is a constrained extension, which adds additional
  /// requirements beyond those of the nominal type.
  bool isConstrainedExtension() const;
  
  /// Determine whether this extension context is interchangeable with the
  /// original nominal type context.
  ///
  /// False if any of the following properties hold:
  /// - the extension is defined in a different module from the original
  ///   nominal type decl,
  /// - the extension is constrained, or
  /// - the extension is to a protocol.
  /// FIXME: In a world where protocol extensions are dynamically dispatched,
  /// "extension is to a protocol" would no longer be a reason to use the
  /// extension mangling, because an extension method implementation could be
  /// resiliently moved into the original protocol itself.
  bool isEquivalentToExtendedContext() const;

  /// True if this extension provides an implementation for an imported
  /// Objective-C \c \@interface. This implies various restrictions and special
  /// behaviors for its members.
  bool isObjCImplementation() const;

  /// Returns the name of the category specified by the \c \@_objcImplementation
  /// attribute, or \c None if the name is invalid. Do not call unless
  /// \c isObjCImplementation() returns \c true.
  Optional<Identifier> getCategoryNameForObjCImplementation() const;

  /// If this extension represents an imported Objective-C category, returns the
  /// category's name. Otherwise returns the empty identifier.
  Identifier getObjCCategoryName() const;

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Extension;
  }
  static bool classof(const DeclContext *C) {
    if (auto D = C->getAsDecl())
      return classof(D);
    return false;
  }
  static bool classof(const IterableDeclContext *C) {
    return C->getIterableContextKind() 
             == IterableDeclContextKind::ExtensionDecl;
  }

  using DeclContext::operator new;
  using DeclContext::operator delete;
};

/// Iterator that walks the extensions of a particular type.
class ExtensionIterator {
  ExtensionDecl *current;

public:
  ExtensionIterator() : current() { }
  explicit ExtensionIterator(ExtensionDecl *current) : current(current) { }

  ExtensionDecl *operator*() const { return current; }
  ExtensionDecl *operator->() const { return current; }

  ExtensionIterator &operator++() {
    current = current->NextExtension.getPointer();
    return *this;
  }

  ExtensionIterator operator++(int) {
    ExtensionIterator tmp = *this;
    ++(*this);
    return tmp;
  }

  friend bool operator==(ExtensionIterator x, ExtensionIterator y) {
    return x.current == y.current;
  }

  friend bool operator!=(ExtensionIterator x, ExtensionIterator y) {
    return x.current != y.current;
  }
};

/// Range that covers a set of extensions.
class ExtensionRange {
  ExtensionIterator first;
  ExtensionIterator last;

public:

  ExtensionRange(ExtensionIterator first, ExtensionIterator last)
    : first(first), last(last) { }

  typedef ExtensionIterator iterator;
  iterator begin() const { return first; }
  iterator end() const { return last; }
};
  
  
/// This represents one entry in a PatternBindingDecl, which are pairs of
/// Pattern and Initialization expression.  The pattern is always present, but
/// the initializer can be null if there is none.
class PatternBindingEntry {
  enum class Flags {
    Checked = 1 << 0,
    Removed = 1 << 1,
    /// Whether the contents of this initializer were subsumed by
    /// some other initialization, e.g., a lazy property's initializer
    /// gets subsumed by the getter body.
    Subsumed = 1 << 2,
  };
  llvm::PointerIntPair<Pattern *, 3, OptionSet<Flags>> PatternAndFlags;

  struct InitializerAndEqualLoc {
    // When the initializer is removed we don't actually clear the pointers
    // because we might need to get initializer's source range. Since the
    // initializer is ASTContext-allocated it is safe.
    
    /// Exactly the expr the programmer wrote
    Expr *originalInit;
    /// Might be transformed, e.g. for a property wrapper. In the absence of
    /// transformation or synthesis, holds the expr as parsed.
    Expr *initAfterSynthesis;
    /// The location of the equal '=' token.
    SourceLoc EqualLoc;
  };

  union {
    /// The initializer expression and its '=' token loc.
    InitializerAndEqualLoc InitExpr;

    /// The text of the initializer expression if deserialized from a module.
    StringRef InitStringRepresentation;
  };

  enum class PatternFlags {
    IsText = 1 << 0,
    IsFullyValidated = 1 << 1,
    IsFromDebugger = 1 << 2,
  };
  /// The initializer context used for this pattern binding entry.
  llvm::PointerIntPair<DeclContext *, 3, OptionSet<PatternFlags>>
      InitContextAndFlags;

  /// Values captured by this initializer.
  CaptureInfo Captures;

  friend class Parser;
  friend class PatternBindingInitializer;
  friend class PatternBindingDecl;
  friend class ast_scope::AbstractPatternEntryScope;
  friend class ast_scope::GenericParamScope;
  friend class ast_scope::PatternEntryDeclScope;
  friend class ast_scope::PatternEntryInitializerScope;

private:
  // FIXME: This API is transitional. Once the callers of
  // typeCheckPatternBinding are requestified, merge this bit with
  // Flags::Checked.
  friend class PatternBindingEntryRequest;

  bool isFullyValidated() const {
    return InitContextAndFlags.getInt().contains(
        PatternFlags::IsFullyValidated);
  }
  void setFullyValidated() {
    InitContextAndFlags.setInt(InitContextAndFlags.getInt() |
                               PatternFlags::IsFullyValidated);
  }

  /// Set if this pattern binding came from the debugger.
  ///
  /// Stay away unless you are \c PatternBindingDecl::createForDebugger
  void setFromDebugger() {
    InitContextAndFlags.setInt(InitContextAndFlags.getInt() |
                               PatternFlags::IsFromDebugger);
  }

public:
  /// \p E is the initializer as parsed.
  PatternBindingEntry(Pattern *P, SourceLoc EqualLoc, Expr *E,
                      DeclContext *InitContext)
      : PatternAndFlags(P, {}), InitExpr({E, E, EqualLoc}),
        InitContextAndFlags({InitContext, None}) {}

private:
  Pattern *getPattern() const { return PatternAndFlags.getPointer(); }
  void setPattern(Pattern *P) { PatternAndFlags.setPointer(P); }

  /// Whether the given pattern binding entry is initialized.
  ///
  /// \param onlyExplicit Only consider explicit initializations (rather
  /// than implicitly-generated ones).
  bool isInitialized(bool onlyExplicit = false) const;

  Expr *getInit() const {
    if (PatternAndFlags.getInt().contains(Flags::Removed) ||
        InitContextAndFlags.getInt().contains(PatternFlags::IsText))
      return nullptr;
    return InitExpr.initAfterSynthesis;
  }
  /// Retrieve the initializer if it should be executed to initialize this
  /// particular pattern binding.
  Expr *getExecutableInit() const {
    return isInitializerSubsumed() ? nullptr : getInit();
  }
  SourceRange getOriginalInitRange() const;
  void setInit(Expr *E);

  /// Gets the text of the initializer expression, stripping out inactive
  /// branches of any #ifs inside the expression.
  StringRef getInitStringRepresentation(SmallVectorImpl<char> &scratch) const;

  /// Sets the initializer string representation to the string that was
  /// deserialized from a partial module.
  void setInitStringRepresentation(StringRef str) {
    InitStringRepresentation = str;
    InitContextAndFlags.setInt(InitContextAndFlags.getInt() |
                               PatternFlags::IsText);
  }

  /// Whether this pattern entry can generate a string representation of its
  /// initializer expression.
  bool hasInitStringRepresentation() const;

  /// Retrieve the location of the equal '=' token.
  SourceLoc getEqualLoc() const {
    return InitContextAndFlags.getInt().contains(PatternFlags::IsText)
               ? SourceLoc()
               : InitExpr.EqualLoc;
  }

  /// Set the location of the equal '=' token.
  void setEqualLoc(SourceLoc equalLoc) {
    assert(!InitContextAndFlags.getInt().contains(PatternFlags::IsText) &&
           "cannot set equal loc for textual initializer");
    InitExpr.EqualLoc = equalLoc;
  }

  /// Retrieve the initializer after the =, if any, as it was written in the
  /// source.
  Expr *getOriginalInit() const;

  /// Set the initializer after the = as it was written in the source.
  void setOriginalInit(Expr *);

  bool isInitializerChecked() const {
    return PatternAndFlags.getInt().contains(Flags::Checked);
  }
  void setInitializerChecked() {
    PatternAndFlags.setInt(PatternAndFlags.getInt() | Flags::Checked);
  }

  bool isInitializerSubsumed() const {
    return PatternAndFlags.getInt().contains(Flags::Subsumed);
  }
  void setInitializerSubsumed() {
    PatternAndFlags.setInt(PatternAndFlags.getInt() | Flags::Subsumed);
  }

  /// Returns \c true if the debugger created this pattern binding entry.
  bool isFromDebugger() const {
    return InitContextAndFlags.getInt().contains(PatternFlags::IsFromDebugger);
  }

  // Return the first variable initialized by this pattern.
  VarDecl *getAnchoringVarDecl() const;

  // Retrieve the declaration context for the initializer.
  DeclContext *getInitContext() const {
    return InitContextAndFlags.getPointer();
  }

  /// Override the initializer context.
  void setInitContext(DeclContext *dc) { InitContextAndFlags.setPointer(dc); }

  SourceLoc getStartLoc() const;

  /// Retrieve the end location covered by this pattern binding entry.
  ///
  /// \param omitAccessors Whether the computation should omit the accessors
  /// from the source range.
  SourceLoc getEndLoc(bool omitAccessors = false) const;

  /// Retrieve the source range covered by this pattern binding entry.
  ///
  /// \param omitAccessors Whether the computation should omit the accessors
  /// from the source range.
  SourceRange getSourceRange(bool omitAccessors = false) const;

  CaptureInfo getCaptureInfo() const { return Captures; }
  void setCaptureInfo(CaptureInfo captures) { Captures = captures; }

private:
  SourceLoc getLastAccessorEndLoc() const;
};

/// This decl contains a pattern and optional initializer for a set
/// of one or more VarDecls declared together.
///
/// For example, in
/// \code
///   var (a, b) = foo(), (c,d) = bar()
/// \endcode
///
/// this includes two entries in the pattern list.  The first contains the
/// pattern "(a, b)" and the initializer "foo()".  The second contains the
/// pattern "(c, d)" and the initializer "bar()".
///
class PatternBindingDecl final : public Decl,
    private llvm::TrailingObjects<PatternBindingDecl, PatternBindingEntry> {
  friend TrailingObjects;
  friend class Decl;
  friend class PatternBindingEntryRequest;

  SourceLoc StaticLoc; ///< Location of the 'static/class' keyword, if present.
  SourceLoc VarLoc;    ///< Location of the 'var' keyword.

  friend class Decl;
  
  PatternBindingDecl(SourceLoc StaticLoc, StaticSpellingKind StaticSpelling,
                     SourceLoc VarLoc, unsigned NumPatternEntries,
                     DeclContext *Parent);
  SourceLoc getLocFromSource() const { return VarLoc; }
public:
  static PatternBindingDecl *create(ASTContext &Ctx, SourceLoc StaticLoc,
                                    StaticSpellingKind StaticSpelling,
                                    SourceLoc VarLoc,
                                    ArrayRef<PatternBindingEntry> PatternList,
                                    DeclContext *Parent);

  static PatternBindingDecl *create(ASTContext &Ctx, SourceLoc StaticLoc,
                                    StaticSpellingKind StaticSpelling,
                                    SourceLoc VarLoc, Pattern *Pat,
                                    SourceLoc EqualLoc, Expr *E,
                                    DeclContext *Parent);

  static PatternBindingDecl *createImplicit(ASTContext &Ctx,
                                            StaticSpellingKind StaticSpelling,
                                            Pattern *Pat, Expr *E,
                                            DeclContext *Parent,
                                            SourceLoc VarLoc = SourceLoc());

  static PatternBindingDecl *createDeserialized(
                               ASTContext &Ctx, SourceLoc StaticLoc,
                               StaticSpellingKind StaticSpelling,
                               SourceLoc VarLoc,
                               unsigned NumPatternEntries,
                               DeclContext *Parent);

  // A dedicated entrypoint that allows LLDB to create pattern bindings
  // that look implicit to the compiler but contain user code.
  static PatternBindingDecl *createForDebugger(ASTContext &Ctx,
                                               StaticSpellingKind Spelling,
                                               Pattern *Pat, Expr *E,
                                               DeclContext *Parent);

  SourceLoc getStartLoc() const {
    return StaticLoc.isValid() ? StaticLoc : VarLoc;
  }
  SourceRange getSourceRange() const;

  unsigned getNumPatternEntries() const {
    return Bits.PatternBindingDecl.NumPatternEntries;
  }
  
  ArrayRef<PatternBindingEntry> getPatternList() const {
    return const_cast<PatternBindingDecl*>(this)->getMutablePatternList();
  }

  /// Clean up walking the initializers for the pattern
  class InitIterator {

    const PatternBindingDecl &decl;
    unsigned currentPatternEntryIndex;

    void next() { ++currentPatternEntryIndex; }

  public:
    using value_type = Expr *;
    using pointer = value_type;
    using reference = value_type;
    using difference_type = unsigned;

    InitIterator(const PatternBindingDecl &decl, unsigned start = 0)
        : decl(decl), currentPatternEntryIndex(start) {}

    InitIterator &operator++() {
      next();
      return *this;
    }

    InitIterator operator++(int) {
      InitIterator newIterator(decl, currentPatternEntryIndex);
      newIterator.next();
      return newIterator;
    }

    pointer operator->() { return decl.getInit(currentPatternEntryIndex); }

    pointer operator*() { return decl.getInit(currentPatternEntryIndex); }

    difference_type operator-(const InitIterator &other) {
      return currentPatternEntryIndex - other.currentPatternEntryIndex;
    }

    bool operator==(const InitIterator &other) const {
      return &decl == &other.decl &&
             currentPatternEntryIndex == other.currentPatternEntryIndex;
    }

    bool operator!=(const InitIterator &other) const {
      return !(*this == other);
    }
  };

  InitIterator beginInits() const { return InitIterator(*this); }

  InitIterator endInits() const {
    return InitIterator(*this, getNumPatternEntries());
  }

  llvm::iterator_range<InitIterator> initializers() const {
    return llvm::make_range(beginInits(), endInits());
  }

  void setInitStringRepresentation(unsigned i, StringRef str) {
    getMutablePatternList()[i].setInitStringRepresentation(str);
  }

  /// Whether the given pattern entry is initialized.
  bool isInitialized(unsigned i) const {
    return getPatternList()[i].isInitialized();
  }

  Expr *getInit(unsigned i) const {
    return getPatternList()[i].getInit();
  }
  Expr *getExecutableInit(unsigned i) const {
    return getPatternList()[i].getExecutableInit();
  }
  Expr *getOriginalInit(unsigned i) const {
    return getPatternList()[i].getOriginalInit();
  }

  SourceRange getOriginalInitRange(unsigned i) const {
    return getPatternList()[i].getOriginalInitRange();
  }

  void setInit(unsigned i, Expr *E) {
    getMutablePatternList()[i].setInit(E);
  }

  void setOriginalInit(unsigned i, Expr *E) {
    getMutablePatternList()[i].setOriginalInit(E);
  }

  Pattern *getPattern(unsigned i) const {
    return getPatternList()[i].getPattern();
  }
  
  void setPattern(unsigned i, Pattern *Pat, DeclContext *InitContext,
                  bool isFullyValidated = false);

  bool isFullyValidated(unsigned i) const {
    return getPatternList()[i].isFullyValidated();
  }

  DeclContext *getInitContext(unsigned i) const {
    return getPatternList()[i].getInitContext();
  }

  CaptureInfo getCaptureInfo(unsigned i) const {
    return getPatternList()[i].getCaptureInfo();
  }

  void setCaptureInfo(unsigned i, CaptureInfo captures) {
    getMutablePatternList()[i].setCaptureInfo(captures);
  }

  /// Given that this PBD is the parent pattern for the specified VarDecl,
  /// return the entry of the VarDecl in our PatternList.  For example, in:
  ///
  ///   let (a,b) = foo(), (c,d) = bar()
  ///
  /// "a" and "b" will have index 0, since they correspond to the first pattern,
  /// and "c" and "d" will have index 1 since they correspond to the second one.
  unsigned getPatternEntryIndexForVarDecl(const VarDecl *VD) const;
  
  bool isInitializerChecked(unsigned i) const {
    return getPatternList()[i].isInitializerChecked();
  }

  void setInitializerChecked(unsigned i) {
    getMutablePatternList()[i].setInitializerChecked();
  }

  bool isInitializerSubsumed(unsigned i) const {
    return getPatternList()[i].isInitializerSubsumed();
  }

  void setInitializerSubsumed(unsigned i) {
    getMutablePatternList()[i].setInitializerSubsumed();
  }
  
  /// Does this binding declare something that requires storage?
  bool hasStorage() const;

  /// Determines whether this binding either has an initializer expression, or is
  /// default initialized, without performing any type checking on it.
  bool isDefaultInitializable() const {
    for (unsigned i : range(getNumPatternEntries()))
      if (!isDefaultInitializable(i))
        return false;

    return true;
  }

  /// Can the pattern at index i be default initialized?
  bool isDefaultInitializable(unsigned i) const;

  /// Can the property wrapper be used to provide default initialization?
  bool isDefaultInitializableViaPropertyWrapper(unsigned i) const;

  /// Does this pattern have a user-provided initializer expression?
  bool isExplicitlyInitialized(unsigned i) const;

  /// Whether the pattern entry at the given index can generate a string
  /// representation of its initializer expression.
  bool hasInitStringRepresentation(unsigned i) const {
    return getPatternList()[i].hasInitStringRepresentation();
  }

  SourceLoc getEqualLoc(unsigned i) const;

  /// When the pattern binding contains only a single variable with no
  /// destructuring, retrieve that variable.
  VarDecl *getSingleVar() const;

  /// Return the first variable initialized by the pattern at the given index.
  VarDecl *getAnchoringVarDecl(unsigned i) const;

  bool isStatic() const { return Bits.PatternBindingDecl.IsStatic; }
  void setStatic(bool s) { Bits.PatternBindingDecl.IsStatic = s; }
  SourceLoc getStaticLoc() const { return StaticLoc; }
  /// \returns the way 'static'/'class' was spelled in the source.
  StaticSpellingKind getStaticSpelling() const {
    return static_cast<StaticSpellingKind>(
        Bits.PatternBindingDecl.StaticSpelling);
  }
  /// \returns the way 'static'/'class' should be spelled for this declaration.
  StaticSpellingKind getCorrectStaticSpelling() const;

  /// Is the pattern binding entry for this variable  currently being computed?
  bool isComputingPatternBindingEntry(const VarDecl *vd) const;

  /// Is this an "async let" declaration?
  bool isAsyncLet() const;

  /// Gets the text of the initializer expression for the pattern entry at the
  /// given index, stripping out inactive branches of any #ifs inside the
  /// expression.
  StringRef getInitStringRepresentation(unsigned i,
                                        SmallVectorImpl<char> &scratch) const {
    return getPatternList()[i].getInitStringRepresentation(scratch);
  }

  /// Returns \c true if this pattern binding was created by the debugger.
  bool isDebuggerBinding() const { return Bits.PatternBindingDecl.IsDebugger; }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::PatternBinding;
  }

private:
  MutableArrayRef<PatternBindingEntry> getMutablePatternList() {
    // Pattern entries are tail allocated.
    return {getTrailingObjects<PatternBindingEntry>(), getNumPatternEntries()};
  }
};
  
/// TopLevelCodeDecl - This decl is used as a container for top-level
/// expressions and statements in the main module.  It is always a direct
/// child of a SourceFile.  The primary reason for building these is to give
/// top-level statements a DeclContext which is distinct from the file itself.
/// This, among other things, makes it easier to distinguish between local
/// top-level variables (which are not live past the end of the statement) and
/// global variables.
class TopLevelCodeDecl : public DeclContext, public Decl {
  BraceStmt *Body;
  SourceLoc getLocFromSource() const { return getStartLoc(); }
  friend class Decl;
public:
  TopLevelCodeDecl(DeclContext *Parent, BraceStmt *Body = nullptr)
    : DeclContext(DeclContextKind::TopLevelCodeDecl, Parent),
      Decl(DeclKind::TopLevelCode, Parent),
      Body(Body) {}

  BraceStmt *getBody() const { return Body; }
  void setBody(BraceStmt *b) { Body = b; }

  SourceLoc getStartLoc() const;
  SourceRange getSourceRange() const;

  LLVM_READONLY
  ASTContext &getASTContext() const { return DeclContext::getASTContext(); }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::TopLevelCode;
  }
  static bool classof(const DeclContext *C) {
    if (auto D = C->getAsDecl())
      return classof(D);
    return false;
  }
  
  using DeclContext::operator new;
  using DeclContext::operator delete;
};

/// SerializedTopLevelCodeDeclContext - This represents what was originally a
/// TopLevelCodeDecl during serialization. It is preserved only to maintain the
/// correct AST structure and remangling after deserialization.
class SerializedTopLevelCodeDeclContext : public SerializedLocalDeclContext {
public:
  SerializedTopLevelCodeDeclContext(DeclContext *Parent)
    : SerializedLocalDeclContext(LocalDeclContextKind::TopLevelCodeDecl,
                                 Parent) {}
  static bool classof(const DeclContext *DC) {
    if (auto LDC = dyn_cast<SerializedLocalDeclContext>(DC))
      return LDC->getLocalDeclContextKind() ==
        LocalDeclContextKind::TopLevelCodeDecl;
    return false;
  }
};

/// IfConfigDecl - This class represents #if/#else/#endif blocks.
/// Active and inactive block members are stored separately, with the intention
/// being that active members will be handed back to the enclosing context.
class IfConfigDecl : public Decl {
  /// An array of clauses controlling each of the #if/#elseif/#else conditions.
  /// The array is ASTContext allocated.
  ArrayRef<IfConfigClause> Clauses;
  SourceLoc EndLoc;
  SourceLoc getLocFromSource() const { return Clauses[0].Loc; }
  friend class Decl;
public:
  
  IfConfigDecl(DeclContext *Parent, ArrayRef<IfConfigClause> Clauses,
               SourceLoc EndLoc, bool HadMissingEnd)
    : Decl(DeclKind::IfConfig, Parent), Clauses(Clauses), EndLoc(EndLoc)
  {
    Bits.IfConfigDecl.HadMissingEnd = HadMissingEnd;
  }

  ArrayRef<IfConfigClause> getClauses() const { return Clauses; }

  /// Return the active clause, or null if there is no active one.
  const IfConfigClause *getActiveClause() const {
    for (auto &Clause : Clauses)
      if (Clause.isActive) return &Clause;
    return nullptr;
  }

  const ArrayRef<ASTNode> getActiveClauseElements() const {
    if (auto *Clause = getActiveClause())
      return Clause->Elements;
    return {};
  }
  
  SourceLoc getEndLoc() const { return EndLoc; }

  bool hadMissingEnd() const { return Bits.IfConfigDecl.HadMissingEnd; }
  
  SourceRange getSourceRange() const;
  
  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::IfConfig;
  }
};

class StringLiteralExpr;

class PoundDiagnosticDecl : public Decl {
  SourceLoc StartLoc;
  SourceLoc EndLoc;
  StringLiteralExpr *Message;
  SourceLoc getLocFromSource() const { return StartLoc; }
  friend class Decl;
public:
  PoundDiagnosticDecl(DeclContext *Parent, bool IsError, SourceLoc StartLoc,
                      SourceLoc EndLoc, StringLiteralExpr *Message)
    : Decl(DeclKind::PoundDiagnostic, Parent), StartLoc(StartLoc),
      EndLoc(EndLoc), Message(Message) {
      Bits.PoundDiagnosticDecl.IsError = IsError;
      Bits.PoundDiagnosticDecl.HasBeenEmitted = false; 
    }

  DiagnosticKind getKind() const {
    return isError() ? DiagnosticKind::Error : DiagnosticKind::Warning;
  }

  StringLiteralExpr *getMessage() const { return Message; }

  bool isError() const {
    return Bits.PoundDiagnosticDecl.IsError;
  }

  bool hasBeenEmitted() const {
    return Bits.PoundDiagnosticDecl.HasBeenEmitted;
  }

  void markEmitted() {
    Bits.PoundDiagnosticDecl.HasBeenEmitted = true;
  }
  
  SourceLoc getEndLoc() const { return EndLoc; };
  
  SourceRange getSourceRange() const {
    return SourceRange(StartLoc, EndLoc);
  }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::PoundDiagnostic;
  }
};
  
class OpaqueTypeDecl;

/// Describes the least favorable positions at which a requirement refers
/// to a given generic parameter in terms of variance, for use in the
/// is-inheritable and is-available-existential checks.
class GenericParameterReferenceInfo final {
  using OptionalTypePosition = OptionalEnum<decltype(TypePosition::Covariant)>;

public:
  /// Whether the uncurried interface type of the declaration, stripped of any
  /// optionality, is a direct reference to the generic parameter at hand. For
  /// example, "func foo(x: Int) -> () -> Self?" has a covariant 'Self' result.
  bool hasCovariantSelfResult;

  OptionalTypePosition selfRef;
  OptionalTypePosition assocTypeRef;

  /// A reference to 'Self'.
  static GenericParameterReferenceInfo forSelfRef(TypePosition position) {
    return GenericParameterReferenceInfo(false, position, llvm::None);
  }

  /// A reference to the generic parameter in covariant result position.
  static GenericParameterReferenceInfo forCovariantResult() {
    return GenericParameterReferenceInfo(true, TypePosition::Covariant,
                                         llvm::None);
  }

  /// A reference to 'Self' through an associated type.
  static GenericParameterReferenceInfo forAssocTypeRef(TypePosition position) {
    return GenericParameterReferenceInfo(false, llvm::None, position);
  }

  GenericParameterReferenceInfo &operator|=(const GenericParameterReferenceInfo &other);

  explicit operator bool() const {
    return hasCovariantSelfResult || selfRef || assocTypeRef;
  }

  GenericParameterReferenceInfo()
      : hasCovariantSelfResult(false), selfRef(llvm::None),
        assocTypeRef(llvm::None) {}

private:
  GenericParameterReferenceInfo(bool hasCovariantSelfResult, OptionalTypePosition selfRef,
                    OptionalTypePosition assocTypeRef)
      : hasCovariantSelfResult(hasCovariantSelfResult), selfRef(selfRef),
        assocTypeRef(assocTypeRef) {}
};

/// ValueDecl - All named decls that are values in the language.  These can
/// have a type, etc.
class ValueDecl : public Decl {
public:
  enum : unsigned { InvalidDiscriminator = 0xFFFF };

private:
  DeclName Name;
  SourceLoc NameLoc;
  llvm::PointerIntPair<Type, 3, OptionalEnum<AccessLevel>> TypeAndAccess;
  unsigned LocalDiscriminator = InvalidDiscriminator;

  struct {
    /// Whether the "IsObjC" bit has been computed yet.
    unsigned isObjCComputed : 1;

    /// Whether this declaration is exposed to Objective-C.
    unsigned isObjC : 1;

    /// Whether the "overridden" declarations have been computed already.
    unsigned hasOverriddenComputed : 1;

    /// Whether there are any "overridden" declarations. The actual overridden
    /// declarations are kept in a side table in the ASTContext.
    unsigned hasOverridden : 1;

    /// Whether the "isDynamic" bit has been computed yet.
    unsigned isDynamicComputed : 1;

    /// Whether this declaration is 'dynamic', meaning that all uses of
    /// the declaration will go through an extra level of indirection that
    /// allows the entity to be replaced at runtime.
    unsigned isDynamic : 1;

    /// Whether the "isFinal" bit has been computed yet.
    unsigned isFinalComputed : 1;

    /// Whether this declaration is 'final'. A final class can't be subclassed,
    /// a final class member can't be overridden.
    unsigned isFinal : 1;

    /// Whether the "isIUO" bit" has been computed yet.
    unsigned isIUOComputed : 1;

    /// Whether this declaration produces an implicitly unwrapped
    /// optional result.
    unsigned isIUO : 1;

    /// Whether the "isMoveOnly" bit has been computed yet.
    unsigned isMoveOnlyComputed : 1;

    /// Whether this declaration can not be copied and thus is move only.
    unsigned isMoveOnly : 1;
  } LazySemanticInfo = { };

  friend class DynamicallyReplacedDeclRequest;
  friend class OverriddenDeclsRequest;
  friend class IsObjCRequest;
  friend class IsFinalRequest;
  friend class IsMoveOnlyRequest;
  friend class IsDynamicRequest;
  friend class IsImplicitlyUnwrappedOptionalRequest;
  friend class InterfaceTypeRequest;
  friend class CheckRedeclarationRequest;
  friend class Decl;
  SourceLoc getLocFromSource() const { return NameLoc; }
protected:
  ValueDecl(DeclKind K,
            llvm::PointerUnion<DeclContext *, ASTContext *> context,
            DeclName name, SourceLoc NameLoc)
    : Decl(K, context), Name(name), NameLoc(NameLoc) {
    Bits.ValueDecl.AlreadyInLookupTable = false;
    Bits.ValueDecl.CheckedRedeclaration = false;
    Bits.ValueDecl.IsUserAccessible = true;
    Bits.ValueDecl.Synthesized = false;
  }

  // MemberLookupTable borrows a bit from this type
  friend class MemberLookupTable;
  bool isAlreadyInLookupTable() {
    return Bits.ValueDecl.AlreadyInLookupTable;
  }
  void setAlreadyInLookupTable(bool value = true) {
    Bits.ValueDecl.AlreadyInLookupTable = value;
  }

  /// Determine whether we have already checked whether this
  /// declaration is a redeclaration.
  bool alreadyCheckedRedeclaration() const {
    return Bits.ValueDecl.CheckedRedeclaration;
  }

  /// Set whether we have already checked this declaration as a
  /// redeclaration.
  void setCheckedRedeclaration() {
    Bits.ValueDecl.CheckedRedeclaration = true;
  }

public:
  /// Return true if this protocol member is a protocol requirement.
  ///
  /// Asserts if this is not a member of a protocol.
  bool isProtocolRequirement() const;

  /// Return true if this is a member implementation for an \c @_objcImplementation
  /// extension.
  bool isObjCMemberImplementation() const;

  void setUserAccessible(bool Accessible) {
    Bits.ValueDecl.IsUserAccessible = Accessible;
  }

  bool isUserAccessible() const {
    return Bits.ValueDecl.IsUserAccessible;
  }

  bool isSynthesized() const {
    return Bits.ValueDecl.Synthesized;
  }

  void setSynthesized(bool value = true) {
    Bits.ValueDecl.Synthesized = value;
  }

  bool hasName() const { return bool(Name); }
  bool isOperator() const { return Name.isOperator(); }

  /// Retrieve the full name of the declaration.
  DeclName getName() const { return Name; }
  void setName(DeclName name) { Name = name; }

  /// Retrieve the base name of the declaration, ignoring any argument
  /// names.
  DeclBaseName getBaseName() const { return Name.getBaseName(); }

  Identifier getBaseIdentifier() const {
    return Name.getBaseIdentifier();
  }

  /// Generates a DeclNameRef referring to this declaration with as much
  /// specificity as possible.
  DeclNameRef createNameRef() const {
    return DeclNameRef(Name);
  }

  /// Retrieve the name to use for this declaration when interoperating
  /// with the Objective-C runtime.
  ///
  /// \returns A "selector" containing the runtime name. For non-method
  /// entities (classes, protocols, properties), this operation will
  /// return a zero-parameter selector with the appropriate name in its
  /// first slot.
  Optional<ObjCSelector> getObjCRuntimeName(
                                    bool skipIsObjCResolution = false) const;

  /// Determine whether the given declaration can infer @objc, or the
  /// Objective-C name, if used to satisfy the given requirement.
  bool canInferObjCFromRequirement(ValueDecl *requirement);

  SourceLoc getNameLoc() const { return NameLoc; }

  bool isUsableFromInline() const;

  /// Returns \c true if this declaration is *not* intended to be used directly
  /// by application developers despite the visibility.
  bool shouldHideFromEditor() const;

  bool hasAccess() const {
    return TypeAndAccess.getInt().hasValue();
  }

  /// Access control is done by Requests.
  friend class AccessLevelRequest;

  /// Returns the access level specified explicitly by the user, or provided by
  /// default according to language rules.
  ///
  /// Most of the time this is not the interesting value to check; access is
  /// limited by enclosing scopes per SE-0025. Use #getFormalAccessScope to
  /// check if access control is being used consistently, and to take features
  /// such as \c \@testable and \c \@usableFromInline into account.
  ///
  /// \sa getFormalAccessScope
  /// \sa hasOpenAccess
  AccessLevel getFormalAccess() const;

  /// Returns the outermost DeclContext from which this declaration can be
  /// accessed, or null if the declaration is public.
  ///
  /// This is used when calculating if access control is being used
  /// consistently. If \p useDC is provided (the location where the value is
  /// being used), features that affect formal access such as \c \@testable are
  /// taken into account.
  ///
  /// \invariant
  /// <code>value.isAccessibleFrom(
  ///     value.getFormalAccessScope().getDeclContext())</code>
  ///
  /// If \p treatUsableFromInlineAsPublic is true, declarations marked with the
  /// \c \@usableFromInline attribute are treated as public. This is normally
  /// false for name lookup and other source language concerns, but true when
  /// computing the linkage of generated functions.
  ///
  /// \sa getFormalAccess
  /// \sa isAccessibleFrom
  /// \sa hasOpenAccess
  AccessScope
  getFormalAccessScope(const DeclContext *useDC = nullptr,
                       bool treatUsableFromInlineAsPublic = false) const;


  /// Copy the formal access level and @usableFromInline attribute from
  /// \p source.
  ///
  /// If \p sourceIsParentContext is true, an access level of \c private will
  /// be copied as \c fileprivate, to ensure that this declaration will be
  /// available everywhere \p source is.
  void copyFormalAccessFrom(const ValueDecl *source,
                            bool sourceIsParentContext = false);

  /// Returns the access level that actually controls how a declaration should
  /// be emitted and may be used.
  ///
  /// This is the access used when making optimization and code generation
  /// decisions. It should not be used at the AST or semantic level.
  AccessLevel getEffectiveAccess() const;

  void setAccess(AccessLevel access) {
    assert(!hasAccess() && "access already set");
    overwriteAccess(access);
  }

  /// Overwrite the access of this declaration.
  ///
  /// This is needed in the LLDB REPL.
  void overwriteAccess(AccessLevel access) {
    TypeAndAccess.setInt(access);
  }

  /// Returns true if this declaration is accessible from the given context.
  ///
  /// A private declaration is accessible from any DeclContext within the same
  /// source file.
  ///
  /// An internal declaration is accessible from any DeclContext within the same
  /// module.
  ///
  /// A public declaration is accessible everywhere.
  ///
  /// If \p DC is null, returns true only if this declaration is public.
  ///
  /// If \p forConformance is true, we ignore the visibility of the protocol
  /// when evaluating protocol extension members. This language rule allows a
  /// protocol extension of a private protocol to provide default
  /// implementations for the requirements of a public protocol, even when
  /// the default implementations are not visible to name lookup.
  bool isAccessibleFrom(const DeclContext *DC,
                        bool forConformance = false,
                        bool allowUsableFromInline = false) const;

  /// Returns whether this declaration should be treated as \c open from
  /// \p useDC. This is very similar to #getFormalAccess, but takes
  /// \c \@testable into account.
  ///
  /// This is mostly only useful when considering requirements on an override:
  /// if the base declaration is \c open, the override might have to be too.
  bool hasOpenAccess(const DeclContext *useDC) const;

  /// FIXME: This is deprecated.
  bool isRecursiveValidation() const;

  /// Retrieve the "interface" type of this value, which uses
  /// GenericTypeParamType if the declaration is generic. For a generic
  /// function, this will have a GenericFunctionType with a
  /// GenericSignature inside the type.
  Type getInterfaceType() const;
  bool hasInterfaceType() const;

  /// Set the interface type for the given value.
  void setInterfaceType(Type type);
  
  /// isInstanceMember - Determine whether this value is an instance member
  /// of an enum or protocol.
  bool isInstanceMember() const;

  /// Retrieve the context discriminator for this local value, which
  /// is the index of this declaration in the sequence of
  /// discriminated declarations with the same name in the current
  /// context.  Only local functions and variables with getters and
  /// setters have discriminators.
  unsigned getLocalDiscriminator() const;
  void setLocalDiscriminator(unsigned index);

  /// Whether this declaration has a local discriminator.
  bool hasLocalDiscriminator() const;

  /// Return the "raw" local discriminator, without computing it.
  unsigned getRawLocalDiscriminator() const { return LocalDiscriminator; }

  /// Retrieve the declaration that this declaration overrides, if any.
  ValueDecl *getOverriddenDecl() const;

  /// Retrieve the declarations that this declaration overrides, if any.
  llvm::TinyPtrVector<ValueDecl *> getOverriddenDecls() const;

  /// Set the declaration that this declaration overrides.
  void setOverriddenDecl(ValueDecl *overridden) {
    setOverriddenDecls(overridden);
  }

  /// Set the declarations that this declaration overrides.
  void setOverriddenDecls(ArrayRef<ValueDecl *> overridden);

  /// Whether the overridden declarations have already been computed.
  bool overriddenDeclsComputed() const;

  /// Compute the untyped overload signature for this declaration.
  OverloadSignature getOverloadSignature() const;

  /// Retrieve the type used to describe this entity for the purposes of
  /// overload resolution.
  CanType getOverloadSignatureType() const;

  /// Returns true if the decl requires Objective-C interop.
  ///
  /// This can be true even if there is no 'objc' attribute on the declaration.
  /// In that case it was inferred by the type checker and set with a call to
  /// markAsObjC().
  bool isObjC() const;

  /// Note whether this declaration is known to be exposed to Objective-C.
  void setIsObjC(bool Value);

  /// Is this declaration semantically 'final', meaning that the type checker
  /// should treat it as final even if the ABI does not?
  bool isSemanticallyFinal() const;

  /// Is this declaration 'final'?
  bool isFinal() const;

  /// Is this declaration 'moveOnly'?
  bool isMoveOnly() const;

  /// Is this declaration marked with 'dynamic'?
  bool isDynamic() const;

private:
  bool isObjCDynamic() const {
    return isObjC() && isDynamic();
  }

  bool isNativeDynamic() const {
    return !isObjC() && isDynamic();
  }

  bool isObjCDynamicInGenericClass() const;

public:
  /// Should we use Objective-C method dispatch for this decl.
  bool shouldUseObjCDispatch() const {
    return isObjCDynamic();
  }

  /// Should we use native dynamic function replacement dispatch for this decl.
  bool shouldUseNativeDynamicDispatch() const {
    return isNativeDynamic();
  }

  /// Should we use Objective-C category based function replacement for this
  /// decl.
  /// This is all `@objc dynamic` methods except for such methods in native
  /// generic classes. We can't use a category for generic classes so we use
  /// native replacement instead (this behavior is only enabled with
  /// -enable-implicit-dynamic).
  bool shouldUseObjCMethodReplacement() const;

  /// Should we use native dynamic function replacement mechanism for this decl.
  /// This is all native dynamic methods except for `@objc dynamic` methods in
  /// generic classes (see above).
  bool shouldUseNativeMethodReplacement() const;

  /// Is this a native dynamic function replacement based replacement.
  /// This is all @_dynamicReplacement(for:) of native functions and @objc
  /// dynamic methods on generic classes (see above).
  bool isNativeMethodReplacement() const;

  /// Returns if this declaration has more visible formal access than 'other'.
  bool isMoreVisibleThan(ValueDecl *other) const;

  /// Set whether this type is 'dynamic' or not.
  void setIsDynamic(bool value);

  /// Whether the 'dynamic' bit has been computed already.
  bool isDynamicComputed() const {
    return LazySemanticInfo.isDynamicComputed;
  }

  /// Returns true if this decl can be found by id-style dynamic lookup.
  bool canBeAccessedByDynamicLookup() const;

  /// Returns true if this declaration has an implicitly unwrapped optional
  /// result. The precise meaning depends on the declaration kind:
  /// - for properties, the value is IUO
  /// - for subscripts, the element type is IUO
  /// - for functions, the result type is IUO
  /// - for constructors, the failability kind is IUO
  bool isImplicitlyUnwrappedOptional() const;

  /// Should only be set on imported and deserialized declarations; parsed
  /// declarations compute this lazily via a request.
  void setImplicitlyUnwrappedOptional(bool isIUO) {
    LazySemanticInfo.isIUOComputed = 1;
    LazySemanticInfo.isIUO = isIUO;
  }

  /// Returns the protocol requirements that this decl conforms to.
  ArrayRef<ValueDecl *>
  getSatisfiedProtocolRequirements(bool Sorted = false) const;

  /// Determines the kind of access that should be performed by a
  /// DeclRefExpr or MemberRefExpr use of this value in the specified
  /// context.
  ///
  /// \param DC The declaration context.
  ///
  /// \param isAccessOnSelf Whether this is a member access on the implicit
  ///        'self' declaration of the declaration context.
  AccessSemantics getAccessSemanticsFromContext(const DeclContext *DC,
                                                bool isAccessOnSelf) const;

  /// Determines if a reference to this declaration from a nested function
  /// should be treated like a capture of a local value.
  bool isLocalCapture() const;

  /// Print a reference to the given declaration.
  std::string printRef() const;

  /// Dump a reference to the given declaration.
  void dumpRef(raw_ostream &os) const;

  /// Dump a reference to the given declaration.
  SWIFT_DEBUG_DUMPER(dumpRef());

  /// Returns true if the declaration is a static member of a type.
  ///
  /// This is not necessarily the opposite of "isInstanceMember()". Both
  /// predicates will be false for declarations that either categorically
  /// can't be "static" or are in a context where "static" doesn't make sense.
  bool isStatic() const;

  /// Retrieve the location at which we should insert a new attribute or
  /// modifier.
  SourceLoc getAttributeInsertionLoc(bool forModifier) const;

  static bool classof(const Decl *D) {
    return D->getKind() >= DeclKind::First_ValueDecl &&
           D->getKind() <= DeclKind::Last_ValueDecl;
  }
  
  /// True if this is a C function that was imported as a member of a type in
  /// Swift.
  bool isImportAsMember() const;

  /// Returns true if the declaration's interface type is a function type with a
  /// curried self parameter.
  bool hasCurriedSelf() const;

  /// Returns true if the declaration has a parameter list associated with it.
  ///
  /// Note that not all declarations with function interface types have
  /// parameter lists, for example an enum element without associated values.
  bool hasParameterList() const;

  /// Returns the number of curry levels in the declaration's interface type.
  unsigned getNumCurryLevels() const;

  /// Get the decl for this value's opaque result type, if it has one.
  OpaqueTypeDecl *getOpaqueResultTypeDecl() const;

  /// Get the representative for this value's opaque result type, if it has one.
  /// Returns a `TypeRepr` instead of an `OpaqueReturnTypeRepr` because 'some'
  /// types might appear in one or more structural positions, e.g.  (some P,
  /// some Q), or we might have a `NamedOpaqueReturnTypeRepr`.
  TypeRepr *getOpaqueResultTypeRepr() const;

  /// Get the representative for this value's result type, if it has one.
  TypeRepr *getResultTypeRepr() const;

  /// Retrieve the attribute associating this declaration with a
  /// result builder, if there is one.
  CustomAttr *getAttachedResultBuilder() const;

  /// Retrieve the @resultBuilder type attached to this declaration,
  /// if there is one.
  Type getResultBuilderType() const;

  /// If this value or its backing storage is annotated
  /// @_dynamicReplacement(for: ...), compute the original declaration
  /// that this declaration dynamically replaces.
  ValueDecl *getDynamicallyReplacedDecl() const;

  /// Find references to 'Self' in the type signature of this declaration in the
  /// context of the given existential base type.
  ///
  /// \param treatNonResultCovariantSelfAsInvariant When set, covariant 'Self'
  /// references that are not in covariant result type position are considered
  /// invariant. This position is the uncurried interface type of a declaration,
  /// stripped of any optionality. For example, this is true for 'Self' in
  /// 'func foo(Int) -> () -> Self?'.
  GenericParameterReferenceInfo findExistentialSelfReferences(
      Type baseTy, bool treatNonResultCovariantSelfAsInvariant) const;

  /// Retrieve a nominal type declaration backing given runtime discoverable
  /// attribute.
  ///
  /// FIXME: This should be a more general facility but its unclear where
  ///        to place it for maximum impact.
  NominalTypeDecl *getRuntimeDiscoverableAttrTypeDecl(CustomAttr *attr) const;

  /// Given a runtime discoverable attribute, return a generator
  /// which could be used to instantiate it for this declaration
  /// together with its result type.
  std::pair<BraceStmt *, Type>
  getRuntimeDiscoverableAttributeGenerator(CustomAttr *) const;
};

/// This is a common base class for declarations which declare a type.
class TypeDecl : public ValueDecl {
  ArrayRef<InheritedEntry> Inherited;

protected:
  TypeDecl(DeclKind K, llvm::PointerUnion<DeclContext *, ASTContext *> context,
           Identifier name, SourceLoc NameLoc,
           ArrayRef<InheritedEntry> inherited) :
    ValueDecl(K, context, name, NameLoc), Inherited(inherited) {}

public:
  Identifier getName() const { return getBaseIdentifier(); }

  /// Returns the string for the base name, or "_" if this is unnamed.
  StringRef getNameStr() const {
    return hasName() ? getBaseIdentifier().str() : "_";
  }

  /// The type of this declaration's values. For the type of the
  /// declaration itself, use getInterfaceType(), which returns a
  /// metatype.
  Type getDeclaredInterfaceType() const;

  /// Retrieve the set of protocols that this type inherits (i.e,
  /// explicitly conforms to).
  ArrayRef<InheritedEntry> getInherited() const { return Inherited; }

  void setInherited(ArrayRef<InheritedEntry> i) { Inherited = i; }

  static bool classof(const Decl *D) {
    return D->getKind() >= DeclKind::First_TypeDecl &&
           D->getKind() <= DeclKind::Last_TypeDecl;
  }

  /// Compute an ordering between two type declarations that is ABI-stable.
  static int compare(const TypeDecl *type1, const TypeDecl *type2);

  /// Compute an ordering between two type declarations that is ABI-stable.
  /// This version takes a pointer-to-a-pointer for use with
  /// llvm::array_pod_sort() and similar.
  template<typename T>
  static int compare(T * const* type1, T * const* type2) {
    return compare(*type1, *type2);
  }
};

/// A type declaration that can have generic parameters attached to it. Because
/// it has these generic parameters, it is always a DeclContext.
class GenericTypeDecl : public GenericContext, public TypeDecl {
public:
  GenericTypeDecl(DeclKind K, DeclContext *DC,
                  Identifier name, SourceLoc nameLoc,
                  ArrayRef<InheritedEntry> inherited,
                  GenericParamList *GenericParams);

  // Resolve ambiguity due to multiple base classes.
  using TypeDecl::getASTContext;
  using DeclContext::operator new;
  using DeclContext::operator delete;
  using TypeDecl::getDeclaredInterfaceType;

  static bool classof(const DeclContext *C) {
    if (auto D = C->getAsDecl())
      return classof(D);
    return false;
  }
  static bool classof(const Decl *D) {
    return D->getKind() >= DeclKind::First_GenericTypeDecl &&
           D->getKind() <= DeclKind::Last_GenericTypeDecl;
  }
};

/// OpaqueTypeDecl - This is a declaration of an opaque type. The opaque type
/// is formally equivalent to its underlying type, but abstracts it away from
/// clients of the opaque type, only exposing the type as something conforming
/// to a given set of constraints.
///
/// An `OpaqueTypeDecl` is formed implicitly when a declaration is written with
/// an opaque result type, as in the following example:
///
/// func foo() -> some SignedInteger { return 1 }
///
/// The declared type uses a special kind of archetype type to represent
/// abstracted types, e.g. `(some P, some Q)` becomes `((opaque archetype 0),
/// (opaque archetype 1))`.
class OpaqueTypeDecl final :
    public GenericTypeDecl,
    private llvm::TrailingObjects<OpaqueTypeDecl, TypeRepr *> {
  friend TrailingObjects;

public:
  /// A set of substitutions that represents a possible underlying type iff
  /// associated set of availability conditions is met.
  class ConditionallyAvailableSubstitutions;

private:
  /// The original declaration that "names" the opaque type. Although a specific
  /// opaque type cannot be explicitly named, opaque types can propagate
  /// arbitrarily through expressions, so we need to know *which* opaque type is
  /// propagated.
  ///
  /// The bit indicates whether there are any trailing
  /// OpaqueReturnTypeReprs.
  llvm::PointerIntPair<ValueDecl *, 1>
      NamingDeclAndHasOpaqueReturnTypeRepr;

  /// The generic signature of the opaque interface to the type. This is the
  /// outer generic signature with added generic parameters representing the
  /// abstracted underlying types.
  GenericSignature OpaqueInterfaceGenericSignature;

  /// If known, the underlying type and conformances of the opaque type,
  /// expressed as a SubstitutionMap for the opaque interface generic signature.
  /// This maps types in the interface generic signature to the outer generic
  /// signature of the original declaration.
  Optional<SubstitutionMap> UniqueUnderlyingType;

  /// A set of substitutions which are used based on the availability
  /// checks performed at runtime. This set of only populated if there
  /// is no single unique underlying type for this opaque type declaration.
  ///
  /// It always contains one or more conditionally available substitutions
  /// followed by a universally available type used as a fallback.
  Optional<MutableArrayRef<ConditionallyAvailableSubstitutions *>>
      ConditionallyAvailableTypes = None;

  mutable Identifier OpaqueReturnTypeIdentifier;

  OpaqueTypeDecl(ValueDecl *NamingDecl, GenericParamList *GenericParams,
                 DeclContext *DC,
                 GenericSignature OpaqueInterfaceGenericSignature,
                 ArrayRef<TypeRepr *> OpaqueReturnTypeReprs);

  unsigned getNumOpaqueReturnTypeReprs() const {
    return NamingDeclAndHasOpaqueReturnTypeRepr.getInt()
      ? getOpaqueGenericParams().size()
      : 0;
  }

  size_t numTrailingObjects(OverloadToken<OpaqueReturnTypeRepr *>) const {
    return getNumOpaqueReturnTypeReprs();
  }

public:
  static OpaqueTypeDecl *get(
      ValueDecl *NamingDecl, GenericParamList *GenericParams,
      DeclContext *DC,
      GenericSignature OpaqueInterfaceGenericSignature,
      ArrayRef<TypeRepr *> OpaqueReturnTypeReprs);

  ValueDecl *getNamingDecl() const {
    return NamingDeclAndHasOpaqueReturnTypeRepr.getPointer();
  }
  
  void setNamingDecl(ValueDecl *D) {
    assert(!getNamingDecl() && "already have naming decl");
    NamingDeclAndHasOpaqueReturnTypeRepr.setPointer(D);
  }

  /// Is this opaque type the opaque return type of the given function?
  ///
  /// This is more complex than just checking `getNamingDecl` because the
  /// function could also be the getter of a storage declaration.
  bool isOpaqueReturnTypeOfFunction(const AbstractFunctionDecl *func) const;

  /// Get the ordinal of the anonymous opaque parameter of this decl with type
  /// repr `repr`, as introduce implicitly by an occurrence of "some" in return
  /// position e.g. `func f() -> some P`. Returns -1 if `repr` is not found.
  Optional<unsigned> getAnonymousOpaqueParamOrdinal(
      TypeRepr *repr) const;

  GenericSignature getOpaqueInterfaceGenericSignature() const {
    return OpaqueInterfaceGenericSignature;
  }

  /// Retrieve the generic parameters that represent the opaque types described by this opaque
  /// type declaration.
  TypeArrayView<GenericTypeParamType> getOpaqueGenericParams() const {
    return OpaqueInterfaceGenericSignature.getInnermostGenericParams();
  }

  /// Whether the generic parameters of this opaque type declaration were
  /// explicit, i.e., for named opaque result types.
  bool hasExplicitGenericParams() const;

  /// When the generic parameters were explicit, returns the generic parameter
  /// corresponding to the given ordinal.
  ///
  /// Otherwise, returns \c nullptr.
  GenericTypeParamDecl *getExplicitGenericParam(unsigned ordinal) const;

  /// Retrieve the buffer containing the opaque return type
  /// representations that correspond to the opaque generic parameters.
  ArrayRef<TypeRepr *> getOpaqueReturnTypeReprs() const {
    return {
      getTrailingObjects<TypeRepr *>(),
      getNumOpaqueReturnTypeReprs()
    };
  }

  /// The substitutions that map the generic parameters of the opaque type to
  /// the unique underlying types, when that information is known.
  Optional<SubstitutionMap> getUniqueUnderlyingTypeSubstitutions() const {
    return UniqueUnderlyingType;
  }
  
  void setUniqueUnderlyingTypeSubstitutions(SubstitutionMap subs) {
    assert(!UniqueUnderlyingType.has_value() && "resetting underlying type?!");
    UniqueUnderlyingType = subs;
  }

  bool hasConditionallyAvailableSubstitutions() const {
    return ConditionallyAvailableTypes.has_value();
  }

  ArrayRef<ConditionallyAvailableSubstitutions *>
  getConditionallyAvailableSubstitutions() const {
    assert(ConditionallyAvailableTypes);
    return ConditionallyAvailableTypes.value();
  }

  void setConditionallyAvailableSubstitutions(
      ArrayRef<ConditionallyAvailableSubstitutions *> substitutions);

  // Opaque type decls are currently always implicit
  SourceRange getSourceRange() const { return SourceRange(); }
  
  // Get the identifier string that can be used to cross-reference unnamed
  // opaque return types across files.
  Identifier getOpaqueReturnTypeIdentifier() const;
  
  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::OpaqueType;
  }
  static bool classof(const GenericTypeDecl *D) {
    return D->getKind() == DeclKind::OpaqueType;
  }
  static bool classof(const DeclContext *C) {
    if (auto D = C->getAsDecl())
      return classof(D);
    return false;
  }

  using AvailabilityCondition = std::pair<VersionRange, bool>;

  class ConditionallyAvailableSubstitutions final
      : private llvm::TrailingObjects<
            ConditionallyAvailableSubstitutions,
            AvailabilityCondition> {
    friend TrailingObjects;

    unsigned NumAvailabilityConditions;

    SubstitutionMap Substitutions;

    /// A type with limited availability described by the provided set
    /// of availability conditions (with `and` relationship).
    ConditionallyAvailableSubstitutions(
        ArrayRef<AvailabilityCondition> availabilityContext,
        SubstitutionMap substitutions)
        : NumAvailabilityConditions(availabilityContext.size()),
          Substitutions(substitutions) {
      assert(!availabilityContext.empty());
      std::uninitialized_copy(availabilityContext.begin(),
                              availabilityContext.end(),
                              getTrailingObjects<AvailabilityCondition>());
    }

  public:
    ArrayRef<AvailabilityCondition> getAvailability() const {
      return {getTrailingObjects<AvailabilityCondition>(), NumAvailabilityConditions};
    }

    SubstitutionMap getSubstitutions() const { return Substitutions; }

    static ConditionallyAvailableSubstitutions *
    get(ASTContext &ctx, ArrayRef<AvailabilityCondition> availabilityContext,
        SubstitutionMap substitutions);
  };
};

/// TypeAliasDecl - This is a declaration of a typealias, for example:
///
///    typealias Foo = Int
///
/// TypeAliasDecl's always have 'MetatypeType' type.
///
class TypeAliasDecl : public GenericTypeDecl {
  friend class UnderlyingTypeRequest;
  
  /// The location of the 'typealias' keyword
  SourceLoc TypeAliasLoc;

  /// The location of the equal '=' token
  SourceLoc EqualLoc;

  /// The end of the type, valid even when the type cannot be parsed
  SourceLoc TypeEndLoc;

  /// The location of the right-hand side of the typealias binding
  TypeLoc UnderlyingTy;

public:
  TypeAliasDecl(SourceLoc TypeAliasLoc, SourceLoc EqualLoc, Identifier Name,
                SourceLoc NameLoc, GenericParamList *GenericParams,
                DeclContext *DC);

  SourceLoc getStartLoc() const { return TypeAliasLoc; }
  SourceRange getSourceRange() const;

  /// Returns the location of the equal '=' token
  SourceLoc getEqualLoc() const {
    return EqualLoc;
  }

  void setTypeEndLoc(SourceLoc e) { TypeEndLoc = e; }

  /// Retrieve the TypeRepr corresponding to the parsed underlying type.
  TypeRepr *getUnderlyingTypeRepr() const {
    return UnderlyingTy.getTypeRepr();
  }
  void setUnderlyingTypeRepr(TypeRepr *repr) {
    UnderlyingTy = repr;
  }
  
  /// Retrieve the interface type of the underlying type.
  Type getUnderlyingType() const;
  void setUnderlyingType(Type type);

  /// Returns the interface type of the underlying type if computed, null
  /// otherwise. Should only be used for dumping.
  Type getCachedUnderlyingType() const { return UnderlyingTy.getType(); }

  /// For generic typealiases, return the unbound generic type.
  ///
  /// Since UnboundGenericType is on its way out, so is this method. Try to
  /// avoid introducing new callers if possible. Instead of passing around
  /// an UnboundGenericType, considering passing around the Decl itself
  /// instead.
  UnboundGenericType *getUnboundGenericType() const;

  /// Retrieve a sugared interface type containing the structure of the interface
  /// type before any semantic validation has occurred.
  Type getStructuralType() const;

  /// Whether the typealias forwards perfectly to its underlying type.
  ///
  /// If true, this typealias was created by ClangImporter to preserve source
  /// compatibility with a previous language version's name for a type. Many
  /// checks in Sema look through compatibility aliases even when they would
  /// operate on other typealiases.
  ///
  /// \warning This has absolutely nothing to do with the Objective-C
  /// \c compatibility_alias keyword.
  bool isCompatibilityAlias() const {
    return Bits.TypeAliasDecl.IsCompatibilityAlias;
  }

  /// Sets whether the typealias forwards perfectly to its underlying type.
  ///
  /// Marks this typealias as having been created by ClangImporter to preserve
  /// source compatibility with a previous language version's name for a type.
  /// Many checks in Sema look through compatibility aliases even when they
  /// would operate on other typealiases.
  ///
  /// \warning This has absolutely nothing to do with the Objective-C
  /// \c compatibility_alias keyword.
  void markAsCompatibilityAlias(bool newValue = true) {
    Bits.TypeAliasDecl.IsCompatibilityAlias = newValue;
  }

  /// Is this a special debugger variable?
  bool isDebuggerAlias() const { return Bits.TypeAliasDecl.IsDebuggerAlias; }
  void markAsDebuggerAlias(bool isDebuggerAlias) {
    Bits.TypeAliasDecl.IsDebuggerAlias = isDebuggerAlias;
  }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::TypeAlias;
  }
  static bool classof(const GenericTypeDecl *D) {
    return D->getKind() == DeclKind::TypeAlias;
  }
  static bool classof(const DeclContext *C) {
    if (auto D = C->getAsDecl())
      return classof(D);
    return false;
  }
};

/// A declaration of a generic type parameter.
///
/// A generic type parameter introduces a new, named type parameter along
/// with some set of requirements on any type argument used to realize this
/// type parameter. The requirements involve conformances to specific
/// protocols or inheritance from a specific class type.
///
/// In the following example, 'T' is a generic type parameter with the
/// requirement that the type argument conform to the 'Comparable' protocol.
///
/// \code
/// func min<T : Comparable>(x : T, y : T) -> T { ... }
/// \endcode
class GenericTypeParamDecl final
    : public TypeDecl,
      private llvm::TrailingObjects<GenericTypeParamDecl, TypeRepr *,
                                    SourceLoc> {
  friend TrailingObjects;

  size_t numTrailingObjects(OverloadToken<TypeRepr *>) const {
    return isOpaqueType() ? 1 : 0;
  }

  size_t numTrailingObjects(OverloadToken<SourceLoc>) const {
    return isParameterPack() ? 1 : 0;
  }

  /// Construct a new generic type parameter.
  ///
  /// \param dc The DeclContext in which the generic type parameter's owner
  /// occurs. This should later be overwritten with the actual declaration
  /// context that owns the type parameter.
  ///
  /// \param name The name of the generic parameter.
  /// \param nameLoc The location of the name.
  /// \param eachLoc The location of the 'each' keyword for a type parameter
  ///                pack.
  /// \param depth The generic signature depth.
  /// \param index The index of the parameter in the generic signature.
  /// \param isParameterPack Whether the generic parameter is for a type
  ///                        parameter pack, denoted by \c <each T>.
  /// \param isOpaqueType Whether the generic parameter is written as an opaque
  ///                     parameter e.g 'some Collection'.
  /// \param opaqueTypeRepr The TypeRepr of an opaque generic parameter.
  ///
  GenericTypeParamDecl(DeclContext *dc, Identifier name, SourceLoc nameLoc,
                       SourceLoc eachLoc, unsigned depth, unsigned index,
                       bool isParameterPack, bool isOpaqueType,
                       TypeRepr *opaqueTypeRepr);

  /// Construct a new generic type parameter.
  ///
  /// \param dc The DeclContext in which the generic type parameter's owner
  /// occurs. This should later be overwritten with the actual declaration
  /// context that owns the type parameter.
  ///
  /// \param name The name of the generic parameter.
  /// \param nameLoc The location of the name.
  /// \param eachLoc The location of the 'each' keyword for a type parameter
  ///                pack.
  /// \param depth The generic signature depth.
  /// \param index The index of the parameter in the generic signature.
  /// \param isParameterPack Whether the generic parameter is for a type
  ///                        parameter pack, denoted by \c <each T>.
  /// \param isOpaqueType Whether the generic parameter is written as an opaque
  ///                     parameter e.g 'some Collection'.
  /// \param opaqueTypeRepr The TypeRepr of an opaque generic parameter.
  ///
  static GenericTypeParamDecl *create(DeclContext *dc, Identifier name,
                                      SourceLoc nameLoc, SourceLoc eachLoc,
                                      unsigned depth, unsigned index,
                                      bool isParameterPack, bool isOpaqueType,
                                      TypeRepr *opaqueTypeRepr);

public:
  static const unsigned InvalidDepth = 0xFFFF;

  /// Construct a new generic type parameter. This should only be used by the
  /// ClangImporter, use \c GenericTypeParamDecl::create[...] instead.
  GenericTypeParamDecl(DeclContext *dc, Identifier name, SourceLoc nameLoc,
                       SourceLoc eachLoc, unsigned depth, unsigned index,
                       bool isParameterPack)
      : GenericTypeParamDecl(dc, name, nameLoc, eachLoc, depth, index,
                             isParameterPack, /*isOpaqueType*/ false, nullptr) {
  }

  /// Construct a deserialized generic type parameter.
  ///
  /// \param dc The DeclContext in which the generic type parameter's owner
  /// occurs. This should later be overwritten with the actual declaration
  /// context that owns the type parameter.
  ///
  /// \param name The name of the generic parameter.
  /// \param depth The generic signature depth.
  /// \param index The index of the parameter in the generic signature.
  /// \param isParameterPack Whether the generic parameter is for a type
  ///                        parameter pack, denoted by \c <each T>.
  /// \param isOpaqueType Whether the generic parameter is written as an opaque
  ///                     parameter e.g 'some Collection'.
  ///
  static GenericTypeParamDecl *
  createDeserialized(DeclContext *dc, Identifier name, unsigned depth,
                     unsigned index, bool isParameterPack, bool isOpaqueType);

  /// Construct a new parsed generic type parameter.
  ///
  /// \param dc The DeclContext in which the generic type parameter's owner
  /// occurs. This should later be overwritten with the actual declaration
  /// context that owns the type parameter.
  ///
  /// \param name The name of the generic parameter.
  /// \param nameLoc The location of the name.
  /// \param eachLoc The location of the 'each' keyword for a type parameter
  ///                pack.
  /// \param index The index of the parameter in the generic signature.
  /// \param isParameterPack Whether the generic parameter is for a type
  ///                        parameter pack, denoted by \c <each T>.
  ///
  static GenericTypeParamDecl *createParsed(DeclContext *dc, Identifier name,
                                            SourceLoc nameLoc,
                                            SourceLoc eachLoc, unsigned index,
                                            bool isParameterPack);

  /// Construct a new implicit generic type parameter.
  ///
  /// \param dc The DeclContext in which the generic type parameter's owner
  /// occurs. This should later be overwritten with the actual declaration
  /// context that owns the type parameter.
  ///
  /// \param name The name of the generic parameter.
  /// \param depth The generic signature depth.
  /// \param index The index of the parameter in the generic signature.
  /// \param isParameterPack Whether the generic parameter is for a type
  ///                        parameter pack, denoted by \c <each T>.
  /// \param isOpaqueType Whether the generic parameter is written as an opaque
  ///                     parameter e.g 'some Collection'.
  /// \param opaqueTypeRepr The TypeRepr of an opaque generic parameter.
  /// \param nameLoc The location of the name.
  /// \param eachLoc The location of the 'each' keyword for a type parameter
  ///                pack.
  ///
  static GenericTypeParamDecl *
  createImplicit(DeclContext *dc, Identifier name, unsigned depth,
                 unsigned index, bool isParameterPack = false,
                 bool isOpaqueType = false, TypeRepr *opaqueTypeRepr = nullptr,
                 SourceLoc nameLoc = {}, SourceLoc eachLoc = {});

  /// The depth of this generic type parameter, i.e., the number of outer
  /// levels of generic parameter lists that enclose this type parameter.
  ///
  /// \code
  /// struct X<T> {
  ///   func f<U>() { }
  /// }
  /// \endcode
  ///
  /// Here 'T' has depth 0 and 'U' has depth 1. Both have index 0.
  unsigned getDepth() const { return Bits.GenericTypeParamDecl.Depth; }

  /// Set the depth of this generic type parameter.
  ///
  /// \sa getDepth
  void setDepth(unsigned depth) {
    Bits.GenericTypeParamDecl.Depth = depth;
    assert(Bits.GenericTypeParamDecl.Depth == depth && "Truncation");
  }

  /// Returns \c true if this generic type parameter is declared as a type
  /// parameter pack.
  ///
  /// \code
  /// func foo<each T>(_ : for each T) { }
  /// struct Foo<each T> { }
  /// \endcode
  bool isParameterPack() const { return Bits.GenericTypeParamDecl.ParameterPack; }

  /// Determine whether this generic parameter represents an opaque type.
  ///
  /// \code
  /// // "some P" is represented by a generic type parameter.
  /// func f() -> [some P] { ... }
  /// \endcode
  bool isOpaqueType() const {
    return Bits.GenericTypeParamDecl.IsOpaqueType;
  }

  /// Retrieve the opaque return type representation described by this
  /// generic parameter, or NULL if any of the following are true:
  ///   - the generic parameter does not describe an opaque type
  ///   - the opaque type was introduced via the "named opaque parameters"
  ///     extension, meaning that it was specified explicitly
  ///   - the enclosing declaration was deserialized, in which case it lost
  ///     the source location information and has no type representation.
  TypeRepr *getOpaqueTypeRepr() const {
    if (!isOpaqueType())
      return nullptr;

    return *getTrailingObjects<TypeRepr *>();
  }

  /// The index of this generic type parameter within its generic parameter
  /// list.
  ///
  /// \code
  /// struct X<T, U> {
  ///   func f<V>() { }
  /// }
  /// \endcode
  ///
  /// Here 'T' and 'U' have indexes 0 and 1, respectively. 'V' has index 0.
  unsigned getIndex() const { return Bits.GenericTypeParamDecl.Index; }

  /// Retrieve the 'each' keyword location for a type parameter pack \c each T
  SourceLoc getEachLoc() const {
    if (!isParameterPack())
      return SourceLoc();

    return *getTrailingObjects<SourceLoc>();
  }

  SourceLoc getStartLoc() const { return getNameLoc(); }
  SourceRange getSourceRange() const;

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::GenericTypeParam;
  }
};

/// A declaration of an associated type.
///
/// An associated type introduces a new, named type in a protocol that
/// can vary from one conforming type to the next. Associated types have a
/// set of requirements to which the type that replaces it much realize,
/// described via conformance to specific protocols, or inheritance from a
/// specific class type.
///
/// In the following example, 'Element' is an associated type with no
/// requirements.
///
/// \code
/// protocol Enumerator {
///   typealias Element
///   func getNext() -> Element?
/// }
/// \endcode
class AssociatedTypeDecl : public TypeDecl {
  /// The location of the initial keyword.
  SourceLoc KeywordLoc;

  /// The default definition.
  TypeRepr *DefaultDefinition;

  /// The where clause attached to the associated type.
  TrailingWhereClause *TrailingWhere;

  LazyMemberLoader *Resolver = nullptr;
  uint64_t ResolverContextData;

  friend class DefaultDefinitionTypeRequest;

public:
  AssociatedTypeDecl(DeclContext *dc, SourceLoc keywordLoc, Identifier name,
                     SourceLoc nameLoc, TypeRepr *defaultDefinition,
                     TrailingWhereClause *trailingWhere);
  AssociatedTypeDecl(DeclContext *dc, SourceLoc keywordLoc, Identifier name,
                     SourceLoc nameLoc, TrailingWhereClause *trailingWhere,
                     LazyMemberLoader *definitionResolver,
                     uint64_t resolverData);

  /// Get the protocol in which this associated type is declared.
  ProtocolDecl *getProtocol() const {
    return cast<ProtocolDecl>(getDeclContext());
  }

  /// Check if we have a default definition type.
  bool hasDefaultDefinitionType() const {
    // If we have a TypeRepr, return true immediately without kicking off
    // a request.
    return DefaultDefinition || getDefaultDefinitionType();
  }

  /// Retrieve the default definition type.
  Type getDefaultDefinitionType() const;

  /// Retrieve the default definition as written in the source.
  TypeRepr *getDefaultDefinitionTypeRepr() const {
    return DefaultDefinition;
  }

  /// Retrieve the trailing where clause for this associated type, if any.
  TrailingWhereClause *getTrailingWhereClause() const { return TrailingWhere; }

  /// Set the trailing where clause for this associated type.
  void setTrailingWhereClause(TrailingWhereClause *trailingWhereClause) {
    TrailingWhere = trailingWhereClause;
  }

  /// Retrieve the associated type "anchor", which is the associated type
  /// declaration that will be used to describe this associated type in the
  /// ABI.
  ///
  /// The associated type "anchor" is an associated type that does not
  /// override any other associated type. There may be several such associated
  /// types; select one deterministically.
  AssociatedTypeDecl *getAssociatedTypeAnchor() const;

  /// Retrieve the (first) overridden associated type declaration, if any.
  AssociatedTypeDecl *getOverriddenDecl() const {
    return cast_or_null<AssociatedTypeDecl>(
        TypeDecl::getOverriddenDecl());
  }

  /// Retrieve the set of associated types overridden by this associated
  /// type.
  llvm::TinyPtrVector<AssociatedTypeDecl *> getOverriddenDecls() const;

  SourceLoc getStartLoc() const { return KeywordLoc; }
  SourceRange getSourceRange() const;

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::AssociatedType;
  }
};

class MemberLookupTable;
class ObjCMethodLookupTable;
class ConformanceLookupTable;
  
// Kinds of pointer types.
enum PointerTypeKind : unsigned {
  PTK_UnsafeMutableRawPointer,
  PTK_UnsafeRawPointer,
  PTK_UnsafeMutablePointer,
  PTK_UnsafePointer,
  PTK_AutoreleasingUnsafeMutablePointer,
};

static inline bool isRawPointerKind(PointerTypeKind PTK) {
  switch (PTK) {
  case PTK_UnsafeMutableRawPointer:
  case PTK_UnsafeRawPointer:
    return true;
  case PTK_UnsafeMutablePointer:
  case PTK_UnsafePointer:
  case PTK_AutoreleasingUnsafeMutablePointer:
    return false;
  }

  llvm_unreachable("Unhandled PointerTypeKind in switch.");
}

// Kinds of buffer pointer types.
enum BufferPointerTypeKind : unsigned {
  BPTK_UnsafeMutableRawBufferPointer,
  BPTK_UnsafeRawBufferPointer,
  BPTK_UnsafeMutableBufferPointer,
  BPTK_UnsafeBufferPointer,
};

enum KeyPathTypeKind : unsigned char {
  KPTK_AnyKeyPath,
  KPTK_PartialKeyPath,
  KPTK_KeyPath,
  KPTK_WritableKeyPath,
  KPTK_ReferenceWritableKeyPath
};

/// NominalTypeDecl - a declaration of a nominal type, like a struct.
class NominalTypeDecl : public GenericTypeDecl, public IterableDeclContext {
  SourceRange Braces;

  /// The first extension of this type.
  ExtensionDecl *FirstExtension = nullptr;

  /// The last extension of this type, used solely for efficient
  /// insertion of new extensions.
  ExtensionDecl *LastExtension = nullptr;

  /// The generation at which we last loaded extensions.
  unsigned ExtensionGeneration;
  
  /// Prepare to traverse the list of extensions.
  void prepareExtensions();

  /// Retrieve the conformance loader (if any), and removing it in the
  /// same operation. The caller is responsible for loading the
  /// conformances.
  std::pair<LazyMemberLoader *, uint64_t> takeConformanceLoader() {
    if (!Bits.NominalTypeDecl.HasLazyConformances)
      return { nullptr, 0 };

    return takeConformanceLoaderSlow();
  }

  /// Slow path for \c takeConformanceLoader().
  std::pair<LazyMemberLoader *, uint64_t> takeConformanceLoaderSlow();

  /// A lookup table containing all of the members of this type and
  /// its extensions, together with a bit indicating if the table
  /// has been prepared.
  ///
  /// The table itself is lazily constructed and updated when
  /// lookupDirect() is called.
  llvm::PointerIntPair<MemberLookupTable *, 1, bool> LookupTable;

  /// Get the lookup table, lazily constructing an empty table if
  /// necessary.
  MemberLookupTable *getLookupTable();

  /// Prepare the lookup table to make it ready for lookups.
  /// Does nothing when called more than once.
  void prepareLookupTable();

  /// Note that we have added a member into the iterable declaration context,
  /// so that it can also be added to the lookup table (if needed).
  void addedMember(Decl *member);

  /// Note that we have added an extension into the nominal type,
  /// so that its members can eventually be added to the lookup table.
  void addedExtension(ExtensionDecl *ext);

  /// A lookup table used to find the protocol conformances of
  /// a given nominal type.
  mutable ConformanceLookupTable *ConformanceTable = nullptr;

  /// Prepare the conformance table.
  void prepareConformanceTable() const;

  /// Returns the protocol requirements that \c Member conforms to.
  ArrayRef<ValueDecl *>
    getSatisfiedProtocolRequirementsForMember(const ValueDecl *Member,
                                              bool Sorted) const;

  ObjCMethodLookupTable *ObjCMethodLookup = nullptr;

  /// Create the Objective-C method lookup table, or return \c false if this
  /// kind of type cannot have Objective-C methods.
  bool createObjCMethodLookup();

  friend class ASTContext;
  friend class MemberLookupTable;
  friend class ConformanceLookupTable;
  friend class ExtensionDecl;
  friend class DeclContext;
  friend class IterableDeclContext;
  friend class DirectLookupRequest;
  friend class LookupAllConformancesInContextRequest;
  friend ArrayRef<ValueDecl *>
  ValueDecl::getSatisfiedProtocolRequirements(bool Sorted) const;

protected:
  Type DeclaredTy;
  Type DeclaredInterfaceTy;

  NominalTypeDecl(DeclKind K, DeclContext *DC, Identifier name,
                  SourceLoc NameLoc,
                  ArrayRef<InheritedEntry> inherited,
                  GenericParamList *GenericParams) :
    GenericTypeDecl(K, DC, name, NameLoc, inherited, GenericParams),
    IterableDeclContext(IterableDeclContextKind::NominalTypeDecl)
  {
    Bits.NominalTypeDecl.AddedImplicitInitializers = false;
    ExtensionGeneration = 0;
    Bits.NominalTypeDecl.HasLazyConformances = false;
    Bits.NominalTypeDecl.IsComputingSemanticMembers = false;
  }

  friend class ProtocolType;

public:
  using GenericTypeDecl::getASTContext;

  SourceRange getBraces() const { return Braces; }
  
  void setBraces(SourceRange braces) { Braces = braces; }

  /// Should this declaration behave as if it must be accessed
  /// resiliently, even when we're building a non-resilient module?
  ///
  /// This is used for diagnostics, because we do not want a behavior
  /// change between builds with resilience enabled and disabled.
  bool isFormallyResilient() const;

  /// Do we need to use resilient access patterns outside of this type's
  /// resilience domain?
  bool isResilient() const;

  /// Do we need to use resilient access patterns when accessing this
  /// type from the given module?
  bool isResilient(ModuleDecl *M, ResilienceExpansion expansion) const;

  /// Determine whether we have already attempted to add any
  /// implicitly-defined initializers to this declaration.
  bool addedImplicitInitializers() const {
    return Bits.NominalTypeDecl.AddedImplicitInitializers;
  }

  /// Note that we have attempted to add implicit initializers.
  void setAddedImplicitInitializers() {
    Bits.NominalTypeDecl.AddedImplicitInitializers = true;
  }

  /// getDeclaredType - Retrieve the type declared by this entity, without
  /// any generic parameters bound if this is a generic type.
  ///
  /// Since UnboundGenericType is on its way out, so is this method. Try to
  /// avoid introducing new callers if possible. Instead of passing around
  /// an UnboundGenericType, considering passing around the Decl itself
  /// instead.
  Type getDeclaredType() const;

  /// getDeclaredInterfaceType - Retrieve the type declared by this entity, with
  /// generic parameters bound if this is a generic type.
  Type getDeclaredInterfaceType() const;

  /// Add a new extension to this nominal type.
  void addExtension(ExtensionDecl *extension);

  /// Add a member to this decl's lookup table.
  ///
  /// Calls "prepareLookupTable" as a side effect.
  void addMemberToLookupTable(Decl *member);

  /// Retrieve the set of extensions of this type.
  ExtensionRange getExtensions();

  /// Special-behaviour flags passed to lookupDirect()
  enum class LookupDirectFlags {
    /// Whether to include @_implements members.
    /// Used by conformance-checking to find special @_implements members.
    IncludeAttrImplements = 1 << 0,
    /// Whether to exclude members of macro expansions.
    ExcludeMacroExpansions = 1 << 1,
  };

  /// Find all of the declarations with the given name within this nominal type
  /// and its extensions.
  ///
  /// This routine does not look into superclasses, nor does it consider
  /// protocols to which the nominal type conforms. Furthermore, the resulting
  /// set of declarations has not been filtered for visibility, nor have
  /// overridden declarations been removed.
  TinyPtrVector<ValueDecl *> lookupDirect(DeclName name, SourceLoc loc = SourceLoc(),
                                          OptionSet<LookupDirectFlags> flags =
                                          OptionSet<LookupDirectFlags>());

  /// Find the distributed actor system instance of this distributed actor.
  VarDecl *getDistributedActorSystemProperty() const;

  /// Find, or potentially synthesize, the implicit 'id' property of this actor.
  VarDecl *getDistributedActorIDProperty() const;

  /// Find the 'RemoteCallTarget.init(_:)' initializer function.
  ConstructorDecl *getDistributedRemoteCallTargetInitFunction() const;

  /// Find the 'RemoteCallArgument(label:name:value:)' initializer function.
  ConstructorDecl *getDistributedRemoteCallArgumentInitFunction() const;

  /// Get the move-only `enqueue(ExecutorJob)` protocol requirement function on the `Executor` protocol.
  AbstractFunctionDecl *getExecutorOwnedEnqueueFunction() const;
  /// This method should be deprecated and removed
  /// Get the move-only `enqueue(Job)` protocol requirement function on the `Executor` protocol.
  AbstractFunctionDecl *getExecutorLegacyOwnedEnqueueFunction() const;
  /// Get the move-only `enqueue(UnownedJob)` protocol requirement function on the `Executor` protocol.
  AbstractFunctionDecl *getExecutorLegacyUnownedEnqueueFunction() const;

  /// Collect the set of protocols to which this type should implicitly
  /// conform, such as AnyObject (for classes).
  void getImplicitProtocols(SmallVectorImpl<ProtocolDecl *> &protocols);

  /// Look for conformances of this nominal type to the given
  /// protocol.
  ///
  /// \param protocol The protocol whose conformance is requested.
  /// \param conformances Will be populated with the set of protocol
  /// conformances found for this protocol.
  ///
  /// \returns true if any conformances were found. 
  bool lookupConformance(
         ProtocolDecl *protocol,
         SmallVectorImpl<ProtocolConformance *> &conformances) const;

  /// Retrieve all of the protocols that this nominal type conforms to.
  ///
  /// \param sorted Whether to sort the protocols in canonical order.
  SmallVector<ProtocolDecl *, 2> getAllProtocols(bool sorted = false) const;

  /// Retrieve all of the protocol conformances for this nominal type.
  SmallVector<ProtocolConformance *, 2> getAllConformances(
                                          bool sorted = false) const;

  /// Register an externally-created protocol conformance in the
  /// conformance lookup table.
  ///
  /// This is used by deserialization of module files to report
  /// conformances.
  void registerProtocolConformance(ProtocolConformance *conformance,
                                   bool synthesized = false);

  void setConformanceLoader(LazyMemberLoader *resolver, uint64_t contextData);

  /// Look in this type and its extensions (but not any of its protocols or
  /// superclasses) for declarations with a given Objective-C selector.
  ///
  /// Note that this can find methods, initializers, deinitializers,
  /// getters, and setters.
  ///
  /// \param selector The Objective-C selector of the method we're
  /// looking for.
  ///
  /// \param isInstance Whether we are looking for an instance method
  /// (vs. a class method).
  TinyPtrVector<AbstractFunctionDecl *> lookupDirect(ObjCSelector selector,
                                                     bool isInstance);

  /// Record the presence of an @objc method with the given selector. No-op if
  /// the type is of a kind which cannot contain @objc methods.
  void recordObjCMethod(AbstractFunctionDecl *method, ObjCSelector selector);

  /// Is this the decl for Optional<T>?
  bool isOptionalDecl() const;

  /// Is this a key path type?
  Optional<KeyPathTypeKind> getKeyPathTypeKind() const;

  /// Retrieve information about this type as a property wrapper.
  PropertyWrapperTypeInfo getPropertyWrapperTypeInfo() const;

  /// Return a collection of the stored member variables of this type.
  ArrayRef<VarDecl *> getStoredProperties() const;

  /// Return a collection of all properties with init accessors in
  /// this type.
  ArrayRef<VarDecl *> getInitAccessorProperties() const;

  /// Establish a mapping between properties that could be iniitalized
  /// via other properties by means of init accessors. This mapping is
  /// one-to-many because we allow intersecting `initializes(...)`.
  void collectPropertiesInitializableByInitAccessors(
      std::multimap<VarDecl *, VarDecl *> &result) const;

  /// Return a collection of the stored member variables of this type, along
  /// with placeholders for unimportable stored properties.
  ArrayRef<Decl *> getStoredPropertiesAndMissingMemberPlaceholders() const;

  /// Whether this nominal type qualifies as an actor, meaning that it is
  /// either an actor type or a protocol whose `Self` type conforms to the
  /// `Actor` protocol.
  bool isActor() const;

  /// Whether this nominal type qualifies as a distributed actor, meaning that
  /// it is either a distributed actor or a DistributedActor constrained protocol.
  bool isDistributedActor() const;

  /// Whether this nominal type qualifies as any actor (plain or distributed).
  bool isAnyActor() const;

  /// Whether this nominal type is the `MainActor` global actor.
  bool isMainActor() const;

  /// Return the range of semantics attributes attached to this NominalTypeDecl.
  auto getSemanticsAttrs() const
      -> decltype(getAttrs().getSemanticsAttrs()) {
    return getAttrs().getSemanticsAttrs();
  }

  bool hasSemanticsAttr(StringRef attrValue) const {
    return getAttrs().hasSemanticsAttr(attrValue);
  }

  /// Returns true if we should emit assembly vision remarks on all methods of
  /// this nominal type.
  bool shouldEmitAssemblyVisionRemarksOnMethods() const {
    return getAttrs().hasAttribute<EmitAssemblyVisionRemarksAttr>();
  }

  /// Whether this declaration has a synthesized memberwise initializer.
  bool hasMemberwiseInitializer() const;

  /// Retrieves the synthesized memberwise initializer for this declaration,
  /// or \c nullptr if it does not have one.
  ConstructorDecl *getMemberwiseInitializer() const;

  /// Retrieves the effective memberwise initializer for this declaration, or
  /// \c nullptr if it does not have one.
  ///
  /// An effective memberwise initializer is either a synthesized memberwise
  /// initializer or a user-defined initializer with the same type.
  ///
  /// The access level of the memberwise initializer is set to the minimum of:
  /// - Public, by default. This enables public nominal types to have public
  ///   memberwise initializers.
  ///   - The `public` default is important for synthesized member types, e.g.
  ///     `TangentVector` structs synthesized during `Differentiable` derived
  ///     conformances. Manually extending these types to define a public
  ///     memberwise initializer causes a redeclaration error.
  /// - The minimum access level of memberwise-initialized properties in the
  ///   nominal type declaration.
  ///
  /// Effective memberwise initializers are used only by derived conformances
  /// for `Self`-returning protocol requirements like `AdditiveArithmetic.+`.
  /// Such derived conformances require memberwise initialization.
  ConstructorDecl *getEffectiveMemberwiseInitializer();

  /// Whether this declaration has a synthesized zero parameter default
  /// initializer.
  bool hasDefaultInitializer() const;

  bool isTypeErasedGenericClass() const;

  /// Retrieves the synthesized zero parameter default initializer for this
  /// declaration, or \c nullptr if it doesn't have one.
  ConstructorDecl *getDefaultInitializer() const;

  /// Force the synthesis of all members named \c member requiring semantic
  /// analysis and install them in the member list of this nominal type.
  ///
  /// \Note The use of this method in the compiler signals an architectural
  /// problem with the caller. Use \c TypeChecker::lookup* instead of
  /// introducing new usages.
  ///
  /// FIXME: This method presents a problem with respect to the consistency
  /// and idempotency of lookups in the compiler. If we instead had a model
  /// where lookup requests would explicitly return semantic members or parsed
  /// members this function could disappear.
  void synthesizeSemanticMembersIfNeeded(DeclName member);

  /// Retrieves the static 'shared' property of a global actor type, which
  /// is used to extract the actor instance.
  ///
  /// \returns the static 'shared' property for a global actor, or \c nullptr
  /// for types that are not global actors.
  VarDecl *getGlobalActorInstance() const;

  /// Whether this type is a global actor, which can be used as an
  /// attribute to decorate declarations for inclusion in the actor-isolated
  /// state denoted by this type.
  bool isGlobalActor() const {
    return getGlobalActorInstance() != nullptr;
  }

  /// Return the `DestructorDecl` for a struct or enum's `deinit` declaration.
  /// Returns null if the type is a class, or does not have a declared `deinit`.
  DestructorDecl *getValueTypeDestructor();

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) {
    return D->getKind() >= DeclKind::First_NominalTypeDecl &&
           D->getKind() <= DeclKind::Last_NominalTypeDecl;
  }
  static bool classof(const GenericTypeDecl *D) {
    return D->getKind() >= DeclKind::First_NominalTypeDecl &&
           D->getKind() <= DeclKind::Last_NominalTypeDecl;
  }

  static bool classof(const DeclContext *C) {
    if (auto D = C->getAsDecl())
      return classof(D);
    return false;
  }
  static bool classof(const IterableDeclContext *C) {
    return C->getIterableContextKind()
             == IterableDeclContextKind::NominalTypeDecl;
  }
  static bool classof(const NominalTypeDecl *D) { return true; }
  static bool classof(const ExtensionDecl *D) { return false; }
};

/// This is the declaration of an enum.
///
/// For example:
///
/// \code
///    enum Bool {
///      case false
///      case true
///    }
///
///    enum Optional<T> {
///      case none
///      case some(T)
///    }
/// \endcode
///
/// The type of the decl itself is a MetatypeType; use getDeclaredType()
/// to get the declared type ("Bool" or "Optional" in the above example).
class EnumDecl final : public NominalTypeDecl {
  SourceLoc EnumLoc;

  enum SemanticInfoFlags : uint8_t {
    // Is the raw type valid?
    HasComputedRawType         = 1 << 0,
    // Is the complete set of (auto-incremented) raw values available?
    HasFixedRawValues          = 1 << 1,
    // Is the complete set of raw values type checked?
    HasFixedRawValuesAndTypes  = 1 << 2,
  };
  OptionSet<SemanticInfoFlags> SemanticFlags;

  friend class EnumRawValuesRequest;
  friend class EnumRawTypeRequest;

public:
  EnumDecl(SourceLoc EnumLoc, Identifier Name, SourceLoc NameLoc,
            ArrayRef<InheritedEntry> Inherited,
            GenericParamList *GenericParams, DeclContext *DC);

  SourceLoc getStartLoc() const { return EnumLoc; }
  SourceRange getSourceRange() const {
    return SourceRange(EnumLoc, getBraces().End);
  }

public:
  /// A range for iterating the elements of an enum.
  using ElementRange = DowncastFilterRange<EnumElementDecl, DeclRange>;

  /// A range for iterating the cases of an enum.
  using CaseRange = DowncastFilterRange<EnumCaseDecl, DeclRange>;

  /// Return a range that iterates over all the elements of an enum.
  ElementRange getAllElements() const {
    return ElementRange(getMembers());
  }

  unsigned getNumElements() const {
    auto eltRange = getAllElements();
    return std::distance(eltRange.begin(), eltRange.end());
  }

  /// If this enum has a unique element, return it. A unique element can
  /// either hold a value or not, and the existence of one unique element does
  /// not imply the existence or non-existence of the opposite unique element.
  EnumElementDecl *getUniqueElement(bool hasValue) const;

  /// Return a range that iterates over all the cases of an enum.
  CaseRange getAllCases() const {
    return CaseRange(getMembers());
  }

  /// Insert all of the 'case' element declarations into a DenseSet.
  void getAllElements(llvm::DenseSet<EnumElementDecl*> &elements) const {
    for (auto elt : getAllElements())
      elements.insert(elt);
  }

  /// Whether this enum has a raw value type that recursively references itself.
  bool hasCircularRawValue() const;
  
  /// Record that this enum has had all of its raw values computed.
  void setHasFixedRawValues();
  
  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Enum;
  }
  static bool classof(const GenericTypeDecl *D) {
    return D->getKind() == DeclKind::Enum;
  }
  static bool classof(const NominalTypeDecl *D) {
    return D->getKind() == DeclKind::Enum;
  }
  static bool classof(const DeclContext *C) {
    if (auto D = C->getAsDecl())
      return classof(D);
    return false;
  }
  static bool classof(const IterableDeclContext *C) {
    auto NTD = dyn_cast<NominalTypeDecl>(C);
    return NTD && classof(NTD);
  }
  
  /// Determine whether this enum declares a raw type in its inheritance clause.
  bool hasRawType() const { return (bool)getRawType(); }

  /// Retrieve the declared raw type of the enum from its inheritance clause,
  /// or null if it has none.
  Type getRawType() const;

  /// Set the raw type of the enum from its inheritance clause.
  void setRawType(Type rawType);

  /// True if none of the enum cases have associated values.
  ///
  /// Note that this is true for enums with absolutely no cases.
  bool hasOnlyCasesWithoutAssociatedValues() const;

  /// True if any of the enum cases have availability annotations.
  ///
  /// Note that this is false for enums with absolutely no cases.
  bool hasPotentiallyUnavailableCaseValue() const;

  /// True if the enum has cases.
  bool hasCases() const {
    return !getAllElements().empty();
  }

  /// True if the enum is marked 'indirect'.
  bool isIndirect() const {
    return getAttrs().hasAttribute<IndirectAttr>();
  }

  /// True if the enum can be exhaustively switched within \p useDC.
  ///
  /// Note that this property is \e not necessarily true for all children of
  /// \p useDC. In particular, an inlinable function does not get to switch
  /// exhaustively over a non-exhaustive enum declared in the same module.
  ///
  /// This is the predicate used when deciding if a switch statement needs a
  /// default case. It should not be used for optimization or code generation.
  ///
  /// \sa isEffectivelyExhaustive
  bool isFormallyExhaustive(const DeclContext *useDC) const;

  /// True if the enum can be exhaustively switched within a function defined
  /// within \p M, with \p expansion specifying whether the function is
  /// inlinable.
  ///
  /// This is the predicate used when making optimization and code generation
  /// decisions. It should not be used at the AST or semantic level.
  ///
  /// \sa isFormallyExhaustive
  bool isEffectivelyExhaustive(ModuleDecl *M,
                               ResilienceExpansion expansion) const;
};

/// StructDecl - This is the declaration of a struct, for example:
///
///    struct Complex { var R : Double, I : Double }
///
/// The type of the decl itself is a MetatypeType; use getDeclaredType()
/// to get the declared type ("Complex" in the above example).
class StructDecl final : public NominalTypeDecl {
  SourceLoc StructLoc;

  // We import C++ class templates as generic structs. Then when in Swift code
  // we want to substitute generic parameters with actual arguments, we
  // convert the arguments to C++ equivalents and ask Clang to instantiate the
  // C++ template. Then we import the C++ class template instantiation
  // as a non-generic structs with a name prefixed with `__CxxTemplateInst`.
  //
  // To reiterate:
  // 1) We need to have a C++ class template declaration in the Clang AST. This
  //    declaration is simply imported from a Clang module.
  // 2) We need a Swift generic struct in the Swift AST. This will provide
  //    template arguments to Clang.
  // 3) We produce a C++ class template instantiation in the Clang AST
  //    using 1) and 2). This declaration does not exist in the Clang module
  //    AST initially in the general case, it's added there on instantiation.
  // 4) We import the instantiation as a Swift struct, with the name prefixed
  //    with `__CxxTemplateInst`.
  //
  // This causes a problem for serialization/deserialization of the Swift
  // module. Imagine the Swift struct from 4) is used in the function return
  // type. We cannot just serialize the non generic Swift struct, because on
  // deserialization we would need to find its backing Clang declaration
  // (the C++ class template instantiation), and it won't be found in the
  // general case. Only the C++ class template from step 1) is in the Clang
  // AST.
  //
  // What we need is to serialize enough information to be
  // able to instantiate C++ class template on deserialization. It turns out
  // that all that information is conveniently covered by the BoundGenericType,
  // which we store in this field. The field is set during the typechecking at
  // the time when we instantiate the C++ class template.
  //
  // Alternative, and likely better solution long term, is to serialize the
  // C++ class template instantiation into a synthetic Clang module, and load
  // this Clang module on deserialization.
  Type TemplateInstantiationType = Type();

public:
  StructDecl(SourceLoc StructLoc, Identifier Name, SourceLoc NameLoc,
             ArrayRef<InheritedEntry> Inherited,
             GenericParamList *GenericParams, DeclContext *DC);

  SourceLoc getStartLoc() const { return StructLoc; }
  SourceRange getSourceRange() const {
    return SourceRange(StructLoc, getBraces().End);
  }

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Struct;
  }
  static bool classof(const GenericTypeDecl *D) {
    return D->getKind() == DeclKind::Struct;
  }
  static bool classof(const NominalTypeDecl *D) {
    return D->getKind() == DeclKind::Struct;
  }
  static bool classof(const DeclContext *C) {
    if (auto D = C->getAsDecl())
      return classof(D);
    return false;
  }
  static bool classof(const IterableDeclContext *C) {
    auto NTD = dyn_cast<NominalTypeDecl>(C);
    return NTD && classof(NTD);
  }

  /// Does this struct contain unreferenceable storage, such as C fields that
  /// cannot be represented in Swift?
  bool hasUnreferenceableStorage() const {
    return Bits.StructDecl.HasUnreferenceableStorage;
  }

  void setHasUnreferenceableStorage(bool v) {
    Bits.StructDecl.HasUnreferenceableStorage = v;
  }

  /// Does this struct represent a non-trivial (for the purpose of calls, as
  /// defined by Itanium ABI) C++ record. A C++ record is considered non-trivial
  /// for the purpose of calls if either its constructor, copy-constructor, or
  /// destructor is non-trivial. As such, a C++ record with a non-trivial
  /// copy-assignment operator but other trivial members is considered to be
  /// trivial.
  bool isCxxNonTrivial() const { return Bits.StructDecl.IsCxxNonTrivial; }

  void setIsCxxNonTrivial(bool v) { Bits.StructDecl.IsCxxNonTrivial = v; }

  bool isNonTrivialPtrAuth() const {
    return Bits.StructDecl.IsNonTrivialPtrAuth;
  }

  void setHasNonTrivialPtrAuth(bool v) {
    Bits.StructDecl.IsNonTrivialPtrAuth = v;
  }

  Type getTemplateInstantiationType() const { return TemplateInstantiationType; }
  void setTemplateInstantiationType(Type t) { TemplateInstantiationType = t; }
};

/// This is the base type for AncestryOptions. Each flag describes possible
/// interesting kinds of superclasses that a class may have.
enum class AncestryFlags : uint8_t {
  /// The class or one of its superclasses is @objc.
  ObjC = (1<<0),

  /// The class or one of its superclasses is @objcMembers.
  ObjCMembers = (1<<1),

  /// The class or one of its superclasses is generic.
  Generic = (1<<2),

  /// The class or one of its superclasses is resilient.
  Resilient = (1<<3),

  /// The class or one of its superclasses has resilient metadata and is in a
  /// different resilience domain.
  ResilientOther = (1<<4),

  /// The class or one of its superclasses is imported from Clang.
  ClangImported = (1<<5),

  /// The class or one of its superclasses requires stored property initializers.
  RequiresStoredPropertyInits = (1<<6),

  /// The class uses the ObjC object model (reference counting,
  /// isa encoding, etc.).
  ObjCObjectModel = (1<<7),
};

/// Return type of ClassDecl::checkAncestry(). Describes a set of interesting
/// kinds of superclasses that a class may have.
using AncestryOptions = OptionSet<AncestryFlags>;

/// ClassDecl - This is the declaration of a class, for example:
///
///    class Complex { var R : Double, I : Double }
///
/// The type of the decl itself is a MetatypeType; use getDeclaredType()
/// to get the declared type ("Complex" in the above example).
class ClassDecl final : public NominalTypeDecl {
  SourceLoc ClassLoc;

  struct {
    /// The superclass decl and a bit to indicate whether the
    /// superclass was computed yet or not.
    llvm::PointerIntPair<ClassDecl *, 1, bool> SuperclassDecl;

    /// The superclass type and a bit to indicate whether the
    /// superclass was computed yet or not.
    llvm::PointerIntPair<Type, 1, bool> SuperclassType;
  } LazySemanticInfo;

  Optional<bool> getCachedInheritsSuperclassInitializers() const {
    if (Bits.ClassDecl.ComputedInheritsSuperclassInits)
      return Bits.ClassDecl.InheritsSuperclassInits;

    return None;
  }

  Optional<bool> getCachedHasMissingDesignatedInitializers() const {
    if (!Bits.ClassDecl.ComputedHasMissingDesignatedInitializers) {
      // Force loading all the members, which will add this attribute if any of
      // members are determined to be missing while loading.
      auto mutableThis = const_cast<ClassDecl *>(this);
      (void)mutableThis->lookupDirect(DeclBaseName::createConstructor(),
                                      getStartLoc());
    }

    if (Bits.ClassDecl.ComputedHasMissingDesignatedInitializers)
      return Bits.ClassDecl.HasMissingDesignatedInitializers;

    return None;
  }

  void setHasMissingDesignatedInitializers(bool value) {
    Bits.ClassDecl.HasMissingDesignatedInitializers = value;
    Bits.ClassDecl.ComputedHasMissingDesignatedInitializers = true;
  }

  /// Marks that this class inherits convenience initializers from its
  /// superclass.
  void setInheritsSuperclassInitializers(bool value) {
    Bits.ClassDecl.InheritsSuperclassInits = value;
    Bits.ClassDecl.ComputedInheritsSuperclassInits = true;
  }

  friend class SuperclassDeclRequest;
  friend class SuperclassTypeRequest;
  friend class ABIMembersRequest;
  friend class HasMissingDesignatedInitializersRequest;
  friend class InheritsSuperclassInitializersRequest;

public:
  ClassDecl(SourceLoc ClassLoc, Identifier Name, SourceLoc NameLoc,
            ArrayRef<InheritedEntry> Inherited,
            GenericParamList *GenericParams, DeclContext *DC,
            bool isActor);

  SourceLoc getStartLoc() const { return ClassLoc; }
  SourceRange getSourceRange() const {
    return SourceRange(ClassLoc, getBraces().End);
  }

  /// Determine whether the member area of this class's metadata (which consists
  /// of field offsets and vtable entries) is to be considered opaque by clients.
  ///
  /// Note that even @_fixed_layout classes have resilient metadata if they are
  /// in a resilient module.
  bool hasResilientMetadata() const;

  /// Determine whether this class has resilient metadata when accessed from the
  /// given module and resilience expansion.
  bool hasResilientMetadata(ModuleDecl *M, ResilienceExpansion expansion) const;

  /// Determine whether this class has a superclass.
  bool hasSuperclass() const { return (bool)getSuperclassDecl(); }

  /// Retrieve the superclass of this class, or null if there is no superclass.
  Type getSuperclass() const;

  /// Retrieve the ClassDecl for the superclass of this class, or null if there
  /// is no superclass.
  ClassDecl *getSuperclassDecl() const;

  /// Check if this class is a superclass or equal to the given class.
  bool isSuperclassOf(const ClassDecl *other) const;

  /// Set the superclass of this class.
  void setSuperclass(Type superclass);

  /// Walk this class and all of the superclasses of this class, transitively,
  /// invoking the callback function for each class.
  ///
  /// \param fn The callback function that will be invoked for each superclass.
  /// It can return \c Continue to continue the traversal. Returning
  /// \c SkipChildren halts the search and returns \c false, while returning
  /// \c Stop halts the search and returns \c true.
  ///
  /// \returns \c true if \c fn returned \c Stop for any class, \c false
  /// otherwise.
  bool walkSuperclasses(
      llvm::function_ref<TypeWalker::Action(ClassDecl *)> fn) const;

  //// Whether this class requires all of its stored properties to
  //// have initializers in the class definition.
  bool requiresStoredPropertyInits() const {
    return checkAncestry(AncestryFlags::RequiresStoredPropertyInits);
  }

  /// \see getForeignClassKind
  enum class ForeignKind : uint8_t {
    /// A normal Swift or Objective-C class.
    Normal = 0,
    /// An imported Core Foundation type. These are AnyObject-compatible but
    /// do not have runtime metadata.
    CFType,
    /// An imported Objective-C type whose class and metaclass symbols are not
    /// both available at link-time but can be accessed through the Objective-C
    /// runtime.
    RuntimeOnly
  };

  /// Whether this class is "foreign", meaning that it is implemented
  /// by a runtime that Swift does not have first-class integration
  /// with.  This generally means that:
  ///   - class data is either abstracted or cannot be made to
  ///     fit with Swift's metatype schema, and/or
  ///   - there is no facility for subclassing or adding polymorphic
  ///     methods to the class.
  ///
  /// We may find ourselves wanting to break this bit into more
  /// precise chunks later.
  ForeignKind getForeignClassKind() const {
    return static_cast<ForeignKind>(Bits.ClassDecl.RawForeignKind);
  }
  void setForeignClassKind(ForeignKind kind) {
    Bits.ClassDecl.RawForeignKind = static_cast<unsigned>(kind);
  }

  /// Returns true if this class is any kind of "foreign class".
  ///
  /// \see getForeignClassKind
  bool isForeign() const {
    return getForeignClassKind() != ForeignKind::Normal ||
      const_cast<ClassDecl *>(this)->isForeignReferenceType();
  }

  /// Whether the class is (known to be) a default actor.
  bool isDefaultActor() const;
  bool isDefaultActor(ModuleDecl *M, ResilienceExpansion expansion) const;

  /// Whether the class is known to be a *root* default actor,
  /// i.e. the first class in its hierarchy that is a default actor.
  bool isRootDefaultActor() const;
  bool isRootDefaultActor(ModuleDecl *M, ResilienceExpansion expansion) const;

  /// It is a `distributed actor` with a custom executor.
  bool isNonDefaultExplicitDistributedActor() const;
  bool isNonDefaultExplicitDistributedActor(ModuleDecl *M, ResilienceExpansion expansion) const;

  /// Whether the class was explicitly declared with the `actor` keyword.
  bool isExplicitActor() const { return Bits.ClassDecl.IsActor; }

  /// Whether the class was explicitly declared with the `distributed actor` keywords.
  bool isExplicitDistributedActor() const {
    return isExplicitActor() &&
           getAttrs().hasAttribute<DistributedActorAttr>();
  }

  /// Get the closest-to-root superclass that's an actor class.
  const ClassDecl *getRootActorClass() const;

  /// Fetch this class's unownedExecutor property, if it has one.
  const VarDecl *getUnownedExecutorProperty() const;

  /// Is this the NSObject class type?
  bool isNSObject() const;

  /// Whether the class directly inherits from NSObject but should use
  /// Swift's native object model.
  bool isNativeNSObjectSubclass() const;

  /// Whether the class uses the ObjC object model (reference counting,
  /// allocation, etc.), the Swift model, or has no reference counting at all.
  ReferenceCounting getObjectModel() const;

  LayoutConstraintKind getLayoutConstraintKind() const {
    if (getObjectModel() == ReferenceCounting::ObjC)
      return LayoutConstraintKind::Class;

    return LayoutConstraintKind::NativeClass;
  }

  /// Returns true if the class has designated initializers that are not listed
  /// in its members.
  ///
  /// This can occur, for example, if the class is an Objective-C class with
  /// initializers that cannot be represented in Swift.
  bool hasMissingDesignatedInitializers() const;

  /// Returns true if the class has missing members that require vtable entries.
  ///
  /// In this case, the class cannot be subclassed, because we cannot construct
  /// the vtable for the subclass.
  bool hasMissingVTableEntries() const;

  void setHasMissingVTableEntries(bool newValue = true) {
    Bits.ClassDecl.ComputedHasMissingVTableEntries = 1;
    Bits.ClassDecl.HasMissingVTableEntries = newValue;
  }

  /// Returns true if this class cannot be used with weak or unowned
  /// references.
  /// 
  /// Note that this is true if this class or any of its ancestor classes
  /// are marked incompatible.
  bool isIncompatibleWithWeakReferences() const;

  void setIsIncompatibleWithWeakReferences(bool newValue = true) {
    Bits.ClassDecl.IsIncompatibleWithWeakReferences = newValue;
  }

  /// Find a method of a class that overrides a given method.
  /// Return nullptr, if no such method exists.
  AbstractFunctionDecl *findOverridingDecl(
      const AbstractFunctionDecl *method) const;

  /// Find a method implementation which will be used when a given method
  /// is invoked on an instance of this class. This implementation may stem
  /// either from a class itself or its direct or indirect superclasses.
  AbstractFunctionDecl *findImplementingMethod(
      const AbstractFunctionDecl *method) const;
  
  /// Retrieve the destructor for this class.
  DestructorDecl *getDestructor() const;

  /// Determine whether this class inherits the convenience initializers
  /// from its superclass.
  bool inheritsSuperclassInitializers() const;

  /// Walks the class hierarchy starting from this class, checking various
  /// conditions.
  AncestryOptions checkAncestry() const;

  /// Check if the class has ancestry of the given kind.
  bool checkAncestry(AncestryFlags flag) const {
    return checkAncestry().contains(flag);
  }

  /// The type of metaclass to use for a class.
  enum class MetaclassKind : uint8_t {
    ObjC,
    SwiftStub,
  };

  /// Determine which sort of metaclass to use for this class
  MetaclassKind getMetaclassKind() const;

  /// Retrieve the name to use for this class when interoperating with
  /// the Objective-C runtime.
  StringRef getObjCRuntimeName(llvm::SmallVectorImpl<char> &buffer) const;

  /// Return the imported declaration for the category with the given name; this
  /// will always be an Objective-C-backed \c ExtensionDecl or, if \p name is
  /// empty, \c ClassDecl. Returns \c nullptr if the class was not imported from
  /// Objective-C or does not have an imported category by that name.
  IterableDeclContext *getImportedObjCCategory(Identifier name) const;

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Class;
  }
  static bool classof(const GenericTypeDecl *D) {
    return D->getKind() == DeclKind::Class;
  }
  static bool classof(const NominalTypeDecl *D) {
    return D->getKind() == DeclKind::Class;
  }
  static bool classof(const DeclContext *C) {
    if (auto D = C->getAsDecl())
      return classof(D);
    return false;
  }
  static bool classof(const IterableDeclContext *C) {
    auto NTD = dyn_cast<NominalTypeDecl>(C);
    return NTD && classof(NTD);
  }
  
  /// Returns true if the decl uses the Objective-C generics model.
  ///
  /// This is true of imported Objective-C classes.
  bool isTypeErasedGenericClass() const {
    return hasClangNode() && isGenericContext() && isObjC();
  }
  
  /// True if the class is known to be implemented in Swift.
  bool hasKnownSwiftImplementation() const {
    return !hasClangNode();
  }

  /// Used to determine if this class decl is a foreign reference type. I.e., a
  /// non-reference-counted swift reference type that was imported from a C++
  /// record.
  bool isForeignReferenceType() const;

  bool hasRefCountingAnnotations() const;
};

/// The set of known protocols for which derived conformances are supported.
enum class KnownDerivableProtocolKind : uint8_t {
  RawRepresentable,
  OptionSet,
  CaseIterable,
  Comparable,
  Equatable,
  Hashable,
  BridgedNSError,
  CodingKey,
  Encodable,
  Decodable,
  AdditiveArithmetic,
  Differentiable,
  Identifiable,
  Actor,
  DistributedActor,
  DistributedActorSystem,
};

using PrimaryAssociatedTypeName = std::pair<Identifier, SourceLoc>;

/// ProtocolDecl - A declaration of a protocol, for example:
///
///   protocol Drawable {
///     func draw()
///   }
///
/// Every protocol has an implicitly-created 'Self' generic parameter that
/// stands for a type that conforms to the protocol. For example,
///
///   protocol Clonable {
///     func clone() -> Self
///   }
///
class ProtocolDecl final : public NominalTypeDecl {
  SourceLoc ProtocolLoc;

  ArrayRef<PrimaryAssociatedTypeName> PrimaryAssociatedTypeNames;
  ArrayRef<ProtocolDecl *> InheritedProtocols;
  ArrayRef<AssociatedTypeDecl *> AssociatedTypes;
  ArrayRef<ValueDecl *> ProtocolRequirements;

  struct {
    /// The superclass decl and a bit to indicate whether the
    /// superclass was computed yet or not.
    llvm::PointerIntPair<ClassDecl *, 1, bool> SuperclassDecl;

    /// The superclass type and a bit to indicate whether the
    /// superclass was computed yet or not.
    llvm::PointerIntPair<Type, 1, bool> SuperclassType;
  } LazySemanticInfo;

  /// The generic signature representing exactly the new requirements introduced
  /// by this protocol.
  Optional<RequirementSignature> RequirementSig;

  /// Returns the cached result of \c requiresClass or \c None if it hasn't yet
  /// been computed.
  Optional<bool> getCachedRequiresClass() const {
    if (Bits.ProtocolDecl.RequiresClassValid)
      return Bits.ProtocolDecl.RequiresClass;

    return None;
  }

  /// Caches the result of \c requiresClass
  void setCachedRequiresClass(bool requiresClass) {
    Bits.ProtocolDecl.RequiresClassValid = true;
    Bits.ProtocolDecl.RequiresClass = requiresClass;
  }

  /// Returns the cached result of \c existentialConformsToSelf or \c None if it
  /// hasn't yet been computed.
  Optional<bool> getCachedExistentialConformsToSelf() const {
    if (Bits.ProtocolDecl.ExistentialConformsToSelfValid)
      return Bits.ProtocolDecl.ExistentialConformsToSelf;

    return None;
  }

  /// Caches the result of \c existentialConformsToSelf
  void setCachedExistentialConformsToSelf(bool result) {
    Bits.ProtocolDecl.ExistentialConformsToSelfValid = true;
    Bits.ProtocolDecl.ExistentialConformsToSelf = result;
  }

  /// Returns the cached result of \c hasSelfOrAssociatedTypeRequirements or
  /// \c None if it hasn't yet been computed.
  Optional<bool> getCachedHasSelfOrAssociatedTypeRequirements() {
    if (Bits.ProtocolDecl.HasSelfOrAssociatedTypeRequirementsValid)
      return Bits.ProtocolDecl.HasSelfOrAssociatedTypeRequirements;

    return None;
  }

  /// Caches the result of \c hasSelfOrAssociatedTypeRequirements
  void setCachedHasSelfOrAssociatedTypeRequirements(bool value) {
    Bits.ProtocolDecl.HasSelfOrAssociatedTypeRequirementsValid = true;
    Bits.ProtocolDecl.HasSelfOrAssociatedTypeRequirements = value;
  }

  bool hasLazyRequirementSignature() const {
    return Bits.ProtocolDecl.HasLazyRequirementSignature;
  }

  bool hasLazyPrimaryAssociatedTypes() const {
    return Bits.ProtocolDecl.HasLazyPrimaryAssociatedTypes;
  }

  friend class SuperclassDeclRequest;
  friend class SuperclassTypeRequest;
  friend class StructuralRequirementsRequest;
  friend class TypeAliasRequirementsRequest;
  friend class ProtocolDependenciesRequest;
  friend class RequirementSignatureRequest;
  friend class ProtocolRequiresClassRequest;
  friend class ExistentialConformsToSelfRequest;
  friend class HasSelfOrAssociatedTypeRequirementsRequest;
  friend class InheritedProtocolsRequest;
  friend class PrimaryAssociatedTypesRequest;
  friend class ProtocolRequirementsRequest;
  
public:
  ProtocolDecl(DeclContext *DC, SourceLoc ProtocolLoc, SourceLoc NameLoc,
               Identifier Name,
               ArrayRef<PrimaryAssociatedTypeName> PrimaryAssociatedTypeNames,
               ArrayRef<InheritedEntry> Inherited,
               TrailingWhereClause *TrailingWhere);

  using Decl::getASTContext;

  /// Retrieve the set of protocols inherited from this protocol.
  ArrayRef<ProtocolDecl *> getInheritedProtocols() const;

  /// Determine whether this protocol has a superclass.
  bool hasSuperclass() const { return (bool)getSuperclassDecl(); }

  /// Retrieve the superclass of this protocol, or null if there is no superclass.
  Type getSuperclass() const;

  /// Retrieve the ClassDecl for the superclass of this protocol, or null if there
  /// is no superclass.
  ClassDecl *getSuperclassDecl() const;

  /// Set the superclass of this protocol.
  void setSuperclass(Type superclass);

  /// Retrieve the set of AssociatedTypeDecl members of this protocol; this
  /// saves loading the set of members in cases where there's no possibility of
  /// a protocol having nested types (ObjC protocols).
  ArrayRef<AssociatedTypeDecl *> getAssociatedTypeMembers() const;

  /// Returns the list of primary associated type names. These are the associated
  /// types that is parametrized with same-type requirements in a
  /// parametrized protocol type of the form SomeProtocol<Arg1, Arg2...>.
  ArrayRef<PrimaryAssociatedTypeName> getPrimaryAssociatedTypeNames() const {
    return PrimaryAssociatedTypeNames;
  }

  /// Returns the list of primary associated types. These are the associated
  /// types that is parametrized with same-type requirements in a
  /// parametrized protocol type of the form SomeProtocol<Arg1, Arg2...>.
  ArrayRef<AssociatedTypeDecl *> getPrimaryAssociatedTypes() const;

  /// Returns the list of all requirements (associated type and value)
  /// associated with this protocol.
  ArrayRef<ValueDecl *> getProtocolRequirements() const;

  /// Returns a protocol requirement with the given name, or nullptr if the
  /// name has multiple overloads, or no overloads at all.
  ValueDecl *getSingleRequirement(DeclName name) const;

  /// Returns an associated type with the given name, or nullptr if one does
  /// not exist.
  AssociatedTypeDecl *getAssociatedType(Identifier name) const;

  /// Returns the existential type for this protocol.
  Type getDeclaredExistentialType() const {
    return ExistentialType::get(getDeclaredInterfaceType());
  }

  /// Walk this protocol and all of the protocols inherited by this protocol,
  /// transitively, invoking the callback function for each protocol.
  ///
  /// \param fn The callback function that will be invoked for each inherited
  /// protocol. It can return \c Continue to continue the traversal,
  /// \c SkipChildren to avoid visiting the children of the given protocol
  /// but continue the search, and \c Stop to halt the search.
  ///
  /// \returns \c true if \c fn returned \c Stop for any protocol, \c false
  /// otherwise.
  bool walkInheritedProtocols(
               llvm::function_ref<TypeWalker::Action(ProtocolDecl *)> fn) const;

  /// Determine whether this protocol inherits from the given ("super")
  /// protocol.
  bool inheritsFrom(const ProtocolDecl *Super) const;
  
  SourceLoc getStartLoc() const { return ProtocolLoc; }
  SourceRange getSourceRange() const {
    return SourceRange(ProtocolLoc, getBraces().End);
  }

  /// True if this protocol can only be conformed to by class types.
  bool requiresClass() const;

  /// Determine whether an existential conforming to this protocol can be
  /// matched with a generic type parameter constrained to this protocol.
  /// This is only permitted if there is nothing "non-trivial" that we
  /// can do with the metatype, which means the protocol must not have
  /// any static methods and must be declared @objc.
  bool existentialConformsToSelf() const;

  /// Does this protocol require a self-conformance witness table?
  bool requiresSelfConformanceWitnessTable() const;

  /// Determine whether this protocol has `Self` or associated type
  /// requirements.
  ///
  /// This is true if one of the following conditions is met for this protocol
  /// or an inherited protocol:
  /// - The protocol has an associated type requirement.
  /// - `Self` appears in non-covariant position in the type signature of a
  ///   value requirement.
  bool hasSelfOrAssociatedTypeRequirements() const;

  /// Determine whether an existential type constrained by this protocol must
  /// be written using `any` syntax.
  ///
  /// \Note This method takes language feature state into account.
  bool existentialRequiresAny() const;

  /// Returns a list of protocol requirements that must be assessed to
  /// determine a concrete's conformance effect polymorphism kind.
  PolymorphicEffectRequirementList getPolymorphicEffectRequirements(
      EffectKind kind) const;
  bool hasPolymorphicEffect(EffectKind kind) const;

  /// Determine whether this is a "marker" protocol, meaning that is indicates
  /// semantics but has no corresponding witness table.
  bool isMarkerProtocol() const;

private:
  void computeKnownProtocolKind() const;

  bool areInheritedProtocolsValid() const {
    return Bits.ProtocolDecl.InheritedProtocolsValid;
  }
  void setInheritedProtocolsValid() {
    Bits.ProtocolDecl.InheritedProtocolsValid = true;
  }

  bool areProtocolRequirementsValid() const {
    return Bits.ProtocolDecl.ProtocolRequirementsValid;
  }
  void setProtocolRequirementsValid() {
    Bits.ProtocolDecl.ProtocolRequirementsValid = true;
  }

public:
  /// If this is known to be a compiler-known protocol, returns the kind.
  /// Otherwise returns None.
  Optional<KnownProtocolKind> getKnownProtocolKind() const {
    if (Bits.ProtocolDecl.KnownProtocol == 0)
      computeKnownProtocolKind();

    if (Bits.ProtocolDecl.KnownProtocol == 1)
      return None;
    
    return static_cast<KnownProtocolKind>(Bits.ProtocolDecl.KnownProtocol - 2);
  }

  Optional<KnownDerivableProtocolKind> getKnownDerivableProtocolKind() const;

  /// Check whether this protocol is of a specific, known protocol kind.
  bool isSpecificProtocol(KnownProtocolKind kind) const {
    if (auto knownKind = getKnownProtocolKind())
      return *knownKind == kind;

    return false;
  }

  /// Whether this protocol has a circular reference in its list of inherited
  /// protocols.
  bool hasCircularInheritedProtocols() const;

  /// Returns true if the protocol has requirements that are not listed in its
  /// members.
  ///
  /// This can occur, for example, if the protocol is an Objective-C protocol
  /// with requirements that cannot be represented in Swift.
  bool hasMissingRequirements() const {
    (void)getMembers();
    return Bits.ProtocolDecl.HasMissingRequirements;
  }

  void setHasMissingRequirements(bool newValue) {
    Bits.ProtocolDecl.HasMissingRequirements = newValue;
  }

  /// Returns the default type witness for an associated type, or a null
  /// type if there is no default.
  Type getDefaultTypeWitness(AssociatedTypeDecl *assocType) const;

  /// Set the default type witness for an associated type.
  void setDefaultTypeWitness(AssociatedTypeDecl *assocType, Type witness);

  /// Returns the default witness for a requirement, or nullptr if there is
  /// no default.
  Witness getDefaultWitness(ValueDecl *requirement) const;

  /// Record the default witness for a requirement.
  void setDefaultWitness(ValueDecl *requirement, Witness witness);

  /// Returns the default associated conformance witness for an associated
  /// type, or \c None if there is no default.
  ProtocolConformanceRef
  getDefaultAssociatedConformanceWitness(CanType association,
                                         ProtocolDecl *requirement) const;

  /// Set the default associated conformance witness for the given
  /// associated conformance.
  void setDefaultAssociatedConformanceWitness(
                                            CanType association,
                                            ProtocolDecl *requirement,
                                            ProtocolConformanceRef conformance);

  /// Retrieve the name to use for this protocol when interoperating
  /// with the Objective-C runtime.
  StringRef getObjCRuntimeName(llvm::SmallVectorImpl<char> &buffer) const;

  /// Retrieve the original requirements written in source, as structural types.
  ///
  /// The requirement machine builds the requirement signature from structural
  /// requirements. Almost everywhere else should use getRequirementSignature()
  /// instead.
  ArrayRef<StructuralRequirement> getStructuralRequirements() const;

  /// Retrieve same-type requirements implied by protocol typealiases with the
  /// same name as associated types, and diagnose cases that are better expressed
  /// via a 'where' clause.
  ArrayRef<Requirement> getTypeAliasRequirements() const;

  /// Get the list of protocols appearing on the right hand side of conformance
  /// requirements. Computed from the structural requirements, above.
  ArrayRef<ProtocolDecl *> getProtocolDependencies() const;

  /// Retrieve the requirements that describe this protocol from the point of
  /// view of the generic system; see RequirementSignature.h for details.
  RequirementSignature getRequirementSignature() const;

  /// Is the requirement signature currently being computed?
  bool isComputingRequirementSignature() const;

  /// Has the requirement signature been computed yet?
  bool isRequirementSignatureComputed() const {
    return RequirementSig.has_value();
  }

  void setRequirementSignature(RequirementSignature requirementSig);

  void setLazyRequirementSignature(LazyMemberLoader *lazyLoader,
                                   uint64_t requirementSignatureData);

  void setLazyAssociatedTypeMembers(LazyMemberLoader *lazyLoader,
                                    uint64_t associatedTypesData);

  void setLazyPrimaryAssociatedTypeMembers(LazyMemberLoader *lazyLoader,
                                           uint64_t associatedTypesData);

public:
  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Protocol;
  }
  static bool classof(const GenericTypeDecl *D) {
    return D->getKind() == DeclKind::Protocol;
  }
  static bool classof(const NominalTypeDecl *D) {
    return D->getKind() == DeclKind::Protocol;
  }
  static bool classof(const DeclContext *C) {
    if (auto D = C->getAsDecl())
      return classof(D);
    return false;
  }
  static bool classof(const IterableDeclContext *C) {
    auto NTD = dyn_cast<NominalTypeDecl>(C);
    return NTD && classof(NTD);
  }
};

/// This is the special singleton Builtin.TheTupleType. It is not directly
/// visible in the source language, but we use it to attach extensions
/// and conformances for tuple types.
///
/// - The declared interface type is the special TheTupleType singleton.
/// - The generic parameter list has one pack generic parameter, <Elements...>
/// - The generic signature has no requirements, <Elements...>
/// - The self interface type is the tuple type containing a single pack
///   expansion, (Elements...).
class BuiltinTupleDecl final : public NominalTypeDecl {
  TupleType *TupleSelfType = nullptr;

public:
  BuiltinTupleDecl(Identifier Name, DeclContext *Parent);

  SourceRange getSourceRange() const {
    return SourceRange();
  }

  TupleType *getTupleSelfType() const;

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::BuiltinTuple;
  }
  static bool classof(const GenericTypeDecl *D) {
    return D->getKind() == DeclKind::BuiltinTuple;
  }
  static bool classof(const NominalTypeDecl *D) {
    return D->getKind() == DeclKind::BuiltinTuple;
  }
  static bool classof(const DeclContext *C) {
    if (auto D = C->getAsDecl())
      return classof(D);
    return false;
  }
  static bool classof(const IterableDeclContext *C) {
    auto NTD = dyn_cast<NominalTypeDecl>(C);
    return NTD && classof(NTD);
  }
};

/// AbstractStorageDecl - This is the common superclass for VarDecl and
/// SubscriptDecl, representing potentially settable memory locations.
class AbstractStorageDecl : public ValueDecl {
  friend class SetterAccessLevelRequest;
  friend class IsGetterMutatingRequest;
  friend class IsSetterMutatingRequest;
  friend class OpaqueReadOwnershipRequest;
  friend class StorageImplInfoRequest;
  friend class RequiresOpaqueAccessorsRequest;
  friend class RequiresOpaqueModifyCoroutineRequest;
  friend class SynthesizeAccessorRequest;

public:
  static const size_t MaxNumAccessors = 255;
private:
  /// A record of the accessors for the declaration.
  class alignas(1 << 3) AccessorRecord final :
      private llvm::TrailingObjects<AccessorRecord, AccessorDecl*> {
    friend TrailingObjects;

    using AccessorIndex = uint8_t;
    static const AccessorIndex InvalidIndex = 0;

    /// The range of the braces around the accessor clause.
    SourceRange Braces;

    /// The number of accessors currently stored in this record.
    AccessorIndex NumAccessors;

    /// The storage capacity of this record for accessors.  Always includes
    /// enough space for adding opaque accessors to the record, which are the
    /// only accessors that should ever be added retroactively; hence this
    /// field is only here for the purposes of safety checks.
    AccessorIndex AccessorsCapacity;

    /// Either 0, meaning there is no registered accessor of the given kind,
    /// or the index+1 of the accessor in the accessors array.
    AccessorIndex AccessorIndices[NumAccessorKinds];

    AccessorRecord(SourceRange braces,
                   ArrayRef<AccessorDecl*> accessors,
                   AccessorIndex accessorsCapacity);
  public:
    static AccessorRecord *create(ASTContext &ctx, SourceRange braces,
                                  ArrayRef<AccessorDecl*> accessors);

    SourceRange getBracesRange() const { return Braces; }

    inline AccessorDecl *getAccessor(AccessorKind kind) const;

    ArrayRef<AccessorDecl *> getAllAccessors() const {
      return { getTrailingObjects<AccessorDecl*>(), NumAccessors };
    }

    void addOpaqueAccessor(AccessorDecl *accessor);

  private:
    MutableArrayRef<AccessorDecl *> getAccessorsBuffer() {
      return { getTrailingObjects<AccessorDecl*>(), NumAccessors };
    }

    bool registerAccessor(AccessorDecl *accessor, AccessorIndex index);
  };

  llvm::PointerIntPair<AccessorRecord*, 3, OptionalEnum<AccessLevel>> Accessors;

  struct {
    unsigned IsGetterMutatingComputed : 1;
    unsigned IsGetterMutating : 1;
    unsigned IsSetterMutatingComputed : 1;
    unsigned IsSetterMutating : 1;
    unsigned OpaqueReadOwnershipComputed : 1;
    unsigned OpaqueReadOwnership : 2;
    unsigned ImplInfoComputed : 1;
    unsigned RequiresOpaqueAccessorsComputed : 1;
    unsigned RequiresOpaqueAccessors : 1;
    unsigned RequiresOpaqueModifyCoroutineComputed : 1;
    unsigned RequiresOpaqueModifyCoroutine : 1;
  } LazySemanticInfo = { };

  /// The implementation info for the accessors.
  StorageImplInfo ImplInfo;

  /// Add a synthesized accessor.
  void setSynthesizedAccessor(AccessorKind kind, AccessorDecl *getter);

protected:
  AbstractStorageDecl(DeclKind Kind, bool IsStatic, DeclContext *DC,
                      DeclName Name, SourceLoc NameLoc,
                      StorageIsMutable_t supportsMutation)
    : ValueDecl(Kind, DC, Name, NameLoc),
      ImplInfo(StorageImplInfo::getSimpleStored(supportsMutation)) {
    Bits.AbstractStorageDecl.IsStatic = IsStatic;
  }

public:

  /// Should this declaration be treated as if annotated with transparent
  /// attribute.
  bool isTransparent() const;

  /// Is this a type ('static') variable?
  bool isStatic() const {
    return Bits.AbstractStorageDecl.IsStatic;
  }
  void setStatic(bool IsStatic) {
    Bits.AbstractStorageDecl.IsStatic = IsStatic;
  }
  bool isCompileTimeConst() const;

  /// \returns the way 'static'/'class' should be spelled for this declaration.
  StaticSpellingKind getCorrectStaticSpelling() const;

  /// Return the interface type of the stored value.
  Type getValueInterfaceType() const;

  /// Determine how this storage is implemented.
  StorageImplInfo getImplInfo() const;

  /// Overwrite the registered implementation-info.  This should be
  /// used carefully.
  void setImplInfo(StorageImplInfo implInfo);

  /// Cache the implementation-info, for use by the request-evaluator.
  void cacheImplInfo(StorageImplInfo implInfo);

  ReadImplKind getReadImpl() const {
    return getImplInfo().getReadImpl();
  }
  WriteImplKind getWriteImpl() const {
    return getImplInfo().getWriteImpl();
  }
  ReadWriteImplKind getReadWriteImpl() const {
    return getImplInfo().getReadWriteImpl();
  }


  /// Return true if this is a VarDecl that has storage associated with
  /// it.
  bool hasStorage() const;

  /// Return true if this storage has the basic accessors/capability
  /// to be mutated.  This is generally constant after the accessors are
  /// installed by the parser/importer/whatever.
  ///
  /// Note that this is different from the mutability of the declaration
  /// in the user language: sometimes we can assign to declarations that
  /// don't support mutation (e.g. to initialize them), and sometimes we
  /// can't mutate things that do support mutation (e.g. because their
  /// setter is private).
  StorageIsMutable_t supportsMutation() const {
    return getImplInfo().supportsMutation();
  }

  /// Determine whether references to this storage declaration may appear
  /// on the left-hand side of an assignment, as the operand of a
  /// `&` or 'inout' operator, or as a component in a writable key path.
  bool isSettable(const DeclContext *UseDC,
                  const DeclRefExpr *base = nullptr) const;

  /// Determine whether references to this storage declaration in Swift may
  /// appear on the left-hand side of an assignment, as the operand of a
  /// `&` or 'inout' operator, or as a component in a writable key path.
  ///
  /// This method is equivalent to \c isSettable with the exception of
  /// 'optional' storage requirements, which lack support for direct writes
  /// in Swift.
  bool isSettableInSwift(const DeclContext *UseDC,
                         const DeclRefExpr *base = nullptr) const;

  /// Does this storage declaration have explicitly-defined accessors
  /// written in the source?
  bool hasParsedAccessors() const;

  /// Return the ownership of values opaquely read from this storage.
  OpaqueReadOwnership getOpaqueReadOwnership() const;
  void setOpaqueReadOwnership(OpaqueReadOwnership ownership) {
    LazySemanticInfo.OpaqueReadOwnership = unsigned(ownership);
    LazySemanticInfo.OpaqueReadOwnershipComputed = true;
  }

  /// Return true if reading this storage requires the ability to
  /// modify the base value.
  bool isGetterMutating() const;
  void setIsGetterMutating(bool isMutating) {
    LazySemanticInfo.IsGetterMutating = isMutating;
    LazySemanticInfo.IsGetterMutatingComputed = true;
  }
  
  /// Return true if modifying this storage requires the ability to
  /// modify the base value.
  bool isSetterMutating() const;
  void setIsSetterMutating(bool isMutating) {
    LazySemanticInfo.IsSetterMutating = isMutating;
    LazySemanticInfo.IsSetterMutatingComputed = true;
  }

  AccessorDecl *getAccessor(AccessorKind kind) const {
    if (auto info = Accessors.getPointer())
      return info->getAccessor(kind);
    return nullptr;
  }

  ArrayRef<AccessorDecl*> getAllAccessors() const {
    if (const auto *info = Accessors.getPointer())
      return info->getAllAccessors();
    return {};
  }

  /// This is the primary mechanism by which we can easily determine whether
  /// this storage decl has any effects.
  ///
  /// \returns the getter decl iff this decl has only one accessor that is
  ///          a 'get' with an effect (i.e., 'async', 'throws', or both).
  ///          Otherwise returns nullptr.
  AccessorDecl *getEffectfulGetAccessor() const;

  /// Performs a "limit check" on an effect possibly exhibited by this storage
  /// decl with respect to some other storage decl that serves as the "limit."
  /// This check says that \c this is less effectful than \c other if
  /// \c this either does not exhibit the effect, or if it does, then \c other
  /// also exhibits the effect. Thus, it is conceptually equivalent to
  /// a less-than-or-equal (≤) check like so:
  ///
  /// \verbatim
  ///
  ///           this->hasEffect(E) ≤ other->hasEffect(E)
  ///
  /// \endverbatim
  ///
  /// \param kind the single effect we are performing a check for.
  ///
  /// \returns true iff \c this decl either does not exhibit the effect,
  ///          or \c other also exhibits the effect.
  bool isLessEffectfulThan(AbstractStorageDecl const* other,
                           EffectKind kind) const;

  /// Return an accessor that this storage is expected to have, synthesizing
  /// one if necessary. Note that will always synthesize one, even if the
  /// accessor is not part of the expected opaque set for the storage, so use
  /// with caution.
  AccessorDecl *getSynthesizedAccessor(AccessorKind kind) const;

  /// Return an accessor part of the set of opaque accessors dictated by the
  /// requirements of the ABI.
  ///
  /// This will synthesize the accessor if one is required but not specified
  /// in source; for example, most of the time a mutable property is required
  /// to have a 'modify' accessor, but if the property was only written with
  /// 'get' and 'set' accessors, 'modify' will be synthesized to call 'get'
  /// followed by 'set'.
  ///
  /// If the accessor is not needed for ABI reasons, this returns nullptr.
  /// To ensure an accessor is always returned, use getSynthesizedAccessor().
  AccessorDecl *getOpaqueAccessor(AccessorKind kind) const;

  /// Collect all opaque accessors.
  ArrayRef<AccessorDecl*>
    getOpaqueAccessors(llvm::SmallVectorImpl<AccessorDecl*> &scratch) const;

  /// Return an accessor that was written in source. Returns null if the
  /// accessor was not explicitly defined by the user.
  AccessorDecl *getParsedAccessor(AccessorKind kind) const;

  /// Visit all parsed accessors.
  void visitParsedAccessors(llvm::function_ref<void (AccessorDecl*)>) const;

  /// Visit all opaque accessor kinds.
  void visitExpectedOpaqueAccessors(
                            llvm::function_ref<void (AccessorKind)>) const;

  /// Visit all opaque accessors.
  void visitOpaqueAccessors(llvm::function_ref<void (AccessorDecl*)>) const;

  /// Visit all eagerly emitted accessors.
  ///
  /// This is the union of the parsed and opaque sets.
  void visitEmittedAccessors(llvm::function_ref<void (AccessorDecl*)>) const;

  void setAccessors(SourceLoc lbraceLoc, ArrayRef<AccessorDecl*> accessors,
                    SourceLoc rbraceLoc);

  /// Add a setter to an existing Computed var.
  ///
  /// This should only be used by the ClangImporter.
  void setComputedSetter(AccessorDecl *Set);

  /// Does this storage require opaque accessors of any kind?
  bool requiresOpaqueAccessors() const;

  /// Does this storage require an opaque accessor of the given kind?
  bool requiresOpaqueAccessor(AccessorKind kind) const;

  /// Does this storage require a 'get' accessor in its opaque-accessors set?
  bool requiresOpaqueGetter() const {
    return getOpaqueReadOwnership() != OpaqueReadOwnership::Borrowed;
  }

  /// Does this storage require a 'read' accessor in its opaque-accessors set?
  bool requiresOpaqueReadCoroutine() const {
    return getOpaqueReadOwnership() != OpaqueReadOwnership::Owned;
  }

  /// Does this storage require a 'set' accessor in its opaque-accessors set?
  bool requiresOpaqueSetter() const { return supportsMutation(); }

  /// Does this storage require a 'modify' accessor in its opaque-accessors set?
  bool requiresOpaqueModifyCoroutine() const;

  /// Does this storage have any explicit observers (willSet or didSet) attached
  /// to it?
  bool hasObservers() const {
    return getParsedAccessor(AccessorKind::WillSet) ||
           getParsedAccessor(AccessorKind::DidSet);
  }

  SourceRange getBracesRange() const {
    if (auto info = Accessors.getPointer())
      return info->getBracesRange();
    return SourceRange();
  }

  AccessLevel getSetterFormalAccess() const;

  AccessScope
  getSetterFormalAccessScope(const DeclContext *useDC = nullptr,
                             bool treatUsableFromInlineAsPublic = false) const;

  void setSetterAccess(AccessLevel accessLevel) {
    assert(!Accessors.getInt().hasValue());
    overwriteSetterAccess(accessLevel);
  }

  void overwriteSetterAccess(AccessLevel accessLevel);

  /// Given that this is an Objective-C property or subscript declaration,
  /// produce its getter selector.
  ObjCSelector
  getObjCGetterSelector(Identifier preferredName = Identifier()) const;

  /// Given that this is an Objective-C property or subscript declaration,
  /// produce its setter selector.
  ObjCSelector
  getObjCSetterSelector(Identifier preferredName = Identifier()) const;

  AbstractStorageDecl *getOverriddenDecl() const {
    return cast_or_null<AbstractStorageDecl>(ValueDecl::getOverriddenDecl());
  }

  /// Returns the location of 'override' keyword, if any.
  SourceLoc getOverrideLoc() const;

  /// Returns true if this declaration has a setter accessible from the given
  /// context.
  ///
  /// If \p DC is null, returns true only if the setter is public.
  ///
  /// See \c isAccessibleFrom for a discussion of the \p forConformance
  /// parameter.
  bool isSetterAccessibleFrom(const DeclContext *DC,
                              bool forConformance=false) const;

  /// Determine how this storage declaration should actually be accessed.
  AccessStrategy getAccessStrategy(AccessSemantics semantics,
                                   AccessKind accessKind,
                                   ModuleDecl *module,
                                   ResilienceExpansion expansion) const;

  /// Should this declaration behave as if it must be accessed
  /// resiliently, even when we're building a non-resilient module?
  ///
  /// This is used for diagnostics, because we do not want a behavior
  /// change between builds with resilience enabled and disabled.
  bool isFormallyResilient() const;

  /// Do we need to use resilient access patterns outside of this
  /// property's resilience domain?
  bool isResilient() const;

  /// Do we need to use resilient access patterns when accessing this
  /// property from the given module?
  bool isResilient(ModuleDecl *M, ResilienceExpansion expansion) const;

  /// True if the storage can be referenced by a keypath directly.
  /// Otherwise, its override must be referenced.
  bool isValidKeyPathComponent() const;

  /// True if the storage exports a property descriptor for key paths in
  /// other modules.
  bool exportsPropertyDescriptor() const;

  /// True if any of the accessors to the storage is private or fileprivate.
  bool hasPrivateAccessor() const;

  bool hasDidSetOrWillSetDynamicReplacement() const;

  bool hasAnyNativeDynamicAccessors() const;

  /// Does this have a 'distributed' modifier?
  bool isDistributed() const;

  /// Return a distributed thunk if this computed property is marked as
  /// 'distributed' and and nullptr otherwise.
  FuncDecl *getDistributedThunk() const;

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) {
    return D->getKind() >= DeclKind::First_AbstractStorageDecl &&
           D->getKind() <= DeclKind::Last_AbstractStorageDecl;
  }
};

/// Describes which synthesized property for a property with an attached
/// wrapper is being referenced.
enum class PropertyWrapperSynthesizedPropertyKind {
  /// The backing storage property, which is a stored property of the
  /// wrapper type.
  Backing,
  /// A projection (e.g., `$foo`), which is a computed property to access the
  /// wrapper instance's \c projectedValue property.
  Projection,
};

/// VarDecl - 'var' and 'let' declarations.
class VarDecl : public AbstractStorageDecl {
  friend class NamingPatternRequest;
  NamedPattern *NamingPattern = nullptr;

public:
  enum class Introducer : uint8_t {
    Let = 0,
    Var = 1,
    InOut = 2,
  };

protected:
  PointerUnion<PatternBindingDecl *,
               Stmt *,
               VarDecl *,
               CaptureListExpr *> Parent;

  VarDecl(DeclKind kind, bool isStatic, Introducer introducer,
          SourceLoc nameLoc, Identifier name, DeclContext *dc,
          StorageIsMutable_t supportsMutation);

protected:
  // Only \c ParamDecl::setSpecifier is allowed to flip this - and it's also
  // on the way out of that business.
  void setIntroducer(Introducer value) {
    Bits.VarDecl.Introducer = uint8_t(value);
  }

public:
  VarDecl(bool isStatic, Introducer introducer,
          SourceLoc nameLoc, Identifier name, DeclContext *dc)
    : VarDecl(DeclKind::Var, isStatic, introducer, nameLoc,
              name, dc, StorageIsMutable_t(introducer == Introducer::Var)) {}

  SourceRange getSourceRange() const;

  Identifier getName() const { return getBaseIdentifier(); }

  /// Returns the string for the base name, or "_" if this is unnamed.
  StringRef getNameStr() const {
    return hasName() ? getBaseIdentifier().str() : "_";
  }

  /// Get the type of the variable within its context. If the context is generic,
  /// this will use archetypes.
  Type getType() const;

  /// Retrieve the source range of the variable type, or an invalid range if the
  /// variable's type is not explicitly written in the source.
  ///
  /// Only for use in diagnostics.  It is not always possible to always
  /// precisely point to the variable type because of type aliases.
  SourceRange getTypeSourceRangeForDiagnostics() const;

  /// Returns whether the var is settable in the specified context: this
  /// is either because it is a stored var, because it has a custom setter, or
  /// is a let member in an initializer.
  ///
  /// Pass a null context and null base to check if it's always settable.
  bool isSettable(const DeclContext *UseDC,
                  const DeclRefExpr *base = nullptr) const;

  /// Return the parent pattern binding that may provide an initializer for this
  /// VarDecl.  This returns null if there is none associated with the VarDecl.
  PatternBindingDecl *getParentPatternBinding() const {
    if (!Parent)
      return nullptr;
    return Parent.dyn_cast<PatternBindingDecl *>();
  }
  void setParentPatternBinding(PatternBindingDecl *PBD) {
    assert(PBD);
    Parent = PBD;
  }

  /// Return the Pattern involved in initializing this VarDecl.  However, recall
  /// that the Pattern may be involved in initializing more than just this one
  /// vardecl.  For example, if this is a VarDecl for "x", the pattern may be
  /// "(x, y)" and the initializer on the PatternBindingDecl may be "(1,2)" or
  /// "foo()".
  ///
  /// If this has no parent pattern binding decl or statement associated, it
  /// returns null.
  ///
  Pattern *getParentPattern() const;

  /// Returns the parsed type of this variable declaration.  For parameters, this
  /// is the parsed type the user explicitly wrote.  For variables, this is the
  /// type the user wrote in the typed pattern that binds this variable.
  ///
  /// Note that there are many cases where the user may elide types.  This will
  /// return null in those cases.
  TypeRepr *getTypeReprOrParentPatternTypeRepr() const;

  /// Return the statement that owns the pattern associated with this VarDecl,
  /// if one exists.
  ///
  /// NOTE: After parsing and before type checking, all VarDecls from
  /// CaseLabelItem's Patterns return their CaseStmt. After type checking, we
  /// will have constructed the CaseLabelItem VarDecl linked list implying this
  /// will return nullptr. After type checking, if one wishes to find a parent
  /// pattern of a VarDecl of a CaseStmt, \see getRecursiveParentPatternStmt
  /// instead.
  Stmt *getParentPatternStmt() const {
    if (!Parent)
      return nullptr;
    return Parent.dyn_cast<Stmt *>();
  }

  void setParentPatternStmt(Stmt *s) {
    assert(s);
    Parent = s;
  }

  /// Look for the parent pattern stmt of this var decl, recursively
  /// looking through var decl pointers and then through any
  /// fallthroughts.
  Stmt *getRecursiveParentPatternStmt() const;

  /// Returns the var decl that this var decl is an implicit reference to if
  /// such a var decl exists.
  VarDecl *getParentVarDecl() const {
    if (!Parent)
      return nullptr;
    return Parent.dyn_cast<VarDecl *>();
  }

  /// Set \p v to be the pattern produced VarDecl that is the parent of this
  /// var decl.
  void setParentVarDecl(VarDecl *v) {
    assert(v && v != this);
    Parent = v;
  }

  NamedPattern *getNamingPattern() const;
  void setNamingPattern(NamedPattern *Pat);

  /// If this is a VarDecl that does not belong to a CaseLabelItem's pattern,
  /// return this. Otherwise, this VarDecl must belong to a CaseStmt's
  /// CaseLabelItem. In that case, return the first case label item of the first
  /// case stmt in a sequence of case stmts that fallthrough into each other.
  ///
  /// NOTE: During type checking, we emit an error if we have a single case
  /// label item with a pattern that has multiple var decls of the same
  /// name. This means that during type checking and before type checking, we
  /// may have a _malformed_ switch stmt var decl linked list since var decls in
  /// the same case label item that have the same name will point at the same
  /// canonical var decl, namely the first var decl with the name in the
  /// canonical case label item's var decl list. This is ok, since we are going
  /// to emit the error, but it requires us to be more careful/cautious before
  /// type checking has been complete when relying on canonical var decls
  /// matching up.
  VarDecl *getCanonicalVarDecl() const;

  /// If this is a case stmt var decl, return the var decl that corresponds to
  /// this var decl in the first case label item of the case stmt. Returns
  /// nullptr if this isn't a VarDecl that is part of a case stmt.
  NullablePtr<VarDecl> getCorrespondingFirstCaseLabelItemVarDecl() const;

  /// If this is a case stmt var decl, return the case body var decl that this
  /// var decl maps to.
  NullablePtr<VarDecl> getCorrespondingCaseBodyVariable() const;

  /// Return true if this var decl is an implicit var decl belonging to a case
  /// stmt's body.
  bool isCaseBodyVariable() const;

  /// True if the global stored property requires lazy initialization.
  bool isLazilyInitializedGlobal() const;

  /// Return the initializer involved in this VarDecl.  Recall that the
  /// initializer may be involved in initializing more than just this one
  /// vardecl though.  For example, if this is a VarDecl for "x", the pattern
  /// may be "(x, y)" and the initializer on the PatternBindingDecl may be
  /// "(1,2)" or "foo()".
  ///
  /// If this has no parent pattern binding decl associated, or if that pattern
  /// binding has no initial value, this returns null.
  ///
  Expr *getParentInitializer() const {
    if (auto *PBD = getParentPatternBinding()) {
      const auto i = PBD->getPatternEntryIndexForVarDecl(this);
      return PBD->getInit(i);
    }
    return nullptr;
  }

  /// Whether there exists an initializer for this \c VarDecl.
  bool isParentInitialized() const {
    if (auto *PBD = getParentPatternBinding()) {
      const auto i = PBD->getPatternEntryIndexForVarDecl(this);
      return PBD->isInitialized(i);
    }
    return false;
  }

  /// Return the initializer that will initializer this VarDecl at runtime.
  /// This is equivalent to `getParentInitializer()`, but returns `null` if the
  /// initializer itself was subsumed, e.g., by a macro or property wrapper.
  Expr *getParentExecutableInitializer() const;

  /// Whether this variable has an initializer that will be code-generated.
  bool isParentExecutabledInitialized() const {
    return getParentExecutableInitializer() != nullptr;
  }

  // Return whether this VarDecl has an initial value, either by checking
  // if it has an initializer in its parent pattern binding or if it has
  // the @_hasInitialValue attribute.
  bool hasInitialValue() const {
    return getAttrs().hasAttribute<HasInitialValueAttr>() ||
           isParentInitialized();
  }

  VarDecl *getOverriddenDecl() const {
    return cast_or_null<VarDecl>(AbstractStorageDecl::getOverriddenDecl());
  }
  
  /// Is this an immutable 'let' property?
  ///
  /// For \c ParamDecl instances, using \c isImmutable is preferred.
  bool isLet() const;

  /// Is this an "async let" property?
  bool isAsyncLet() const;

  /// Is this var known to be a "local" distributed actor,
  /// if so the implicit throwing ans some isolation checks can be skipped.
  bool isKnownToBeLocal() const;

  /// Is this a stored property that will _not_ trigger any user-defined code
  /// upon any kind of access?
  bool isOrdinaryStoredProperty() const;

  Introducer getIntroducer() const {
    return Introducer(Bits.VarDecl.Introducer);
  }

  CaptureListExpr *getParentCaptureList() const {
    if (!Parent)
      return nullptr;
    return Parent.dyn_cast<CaptureListExpr *>();
  }

  /// Set \p v to be the pattern produced VarDecl that is the parent of this
  /// var decl.
  void setParentCaptureList(CaptureListExpr *expr) {
    assert(expr != nullptr);
    Parent = expr;
  }
  /// Is this an element in a capture list?
  bool isCaptureList() const {
    return getParentCaptureList() != nullptr;
  }
    
  /// Is this a capture of the self param?
  bool isSelfParamCapture() const { return Bits.VarDecl.IsSelfParamCapture; }
  void setIsSelfParamCapture(bool IsSelfParamCapture = true) {
      Bits.VarDecl.IsSelfParamCapture = IsSelfParamCapture;
  }

  /// Check whether this capture of the self param is actor-isolated.
  bool isSelfParamCaptureIsolated() const;

  /// Determines if this var has an initializer expression that should be
  /// exposed to clients.
  ///
  /// There's a very narrow case when we would: if the decl is an instance
  /// member with an initializer expression and the parent type is
  /// @frozen and resides in a resilient module.
  bool isInitExposedToClients() const;

  /// Determines if this var is exposed as part of the layout of a
  /// @frozen struct.
  ///
  /// From the standpoint of access control and exportability checking, this
  /// var will behave as if it was public, even if it is internal or private.
  bool isLayoutExposedToClients() const;

  /// Is this a special debugger variable?
  bool isDebuggerVar() const { return Bits.VarDecl.IsDebuggerVar; }
  void setDebuggerVar(bool IsDebuggerVar) {
    Bits.VarDecl.IsDebuggerVar = IsDebuggerVar;
  }

  /// Is this the synthesized storage for a 'lazy' property?
  bool isLazyStorageProperty() const {
    return Bits.VarDecl.IsLazyStorageProperty;
  }
  void setLazyStorageProperty(bool IsLazyStorage) {
    Bits.VarDecl.IsLazyStorageProperty = IsLazyStorage;
  }

  /// True if this is a top-level global variable from the main source file.
  bool isTopLevelGlobal() const { return Bits.VarDecl.IsTopLevelGlobal; }
  void setTopLevelGlobal(bool b) { Bits.VarDecl.IsTopLevelGlobal = b; }
  
  /// Retrieve the custom attributes that attach property wrappers to this
  /// property. The returned list contains all of the attached property wrapper
  /// attributes in source order, which means the outermost wrapper attribute
  /// is provided first.
  llvm::TinyPtrVector<CustomAttr *> getAttachedPropertyWrappers() const;

  /// Retrieve the outermost property wrapper attribute associated with
  /// this declaration. For example:
  ///
  /// \code
  /// @A @B @C var <name>: Bool = ...
  /// \endcode
  ///
  /// The outermost attribute in this case is `@A` and it has
  /// complete wrapper type `A<B<C<Bool>>>`.
  CustomAttr *getOutermostAttachedPropertyWrapper() const {
    auto wrappers = getAttachedPropertyWrappers();
    return wrappers.empty() ? nullptr : wrappers.front();
  }

  /// Whether this property has any attached property wrappers.
  bool hasAttachedPropertyWrapper() const;

  /// Whether this var has an implicit property wrapper attribute.
  bool hasImplicitPropertyWrapper() const;

  /// Whether this var is a parameter with an attached property wrapper
  /// that has an external effect on the function.
  bool hasExternalPropertyWrapper() const;

  /// Whether this property has any attached runtime metadata attributes.
  bool hasRuntimeMetadataAttributes() const;

  /// Whether all of the attached property wrappers have an init(wrappedValue:)
  /// initializer.
  bool allAttachedPropertyWrappersHaveWrappedValueInit() const;
  
  /// Retrieve the type of the attached property wrapper as a contextual
  /// type.
  ///
  /// \param index Which property wrapper type is being computed, where 0
  /// indicates the first (outermost) attached property wrapper.
  ///
  /// \returns a NULL type for properties without attached wrappers,
  /// an error type when the property wrapper type itself is erroneous,
  /// or the wrapper type itself, which may involve unbound generic
  /// types.
  Type getAttachedPropertyWrapperType(unsigned index) const;

  /// Retrieve information about the attached property wrapper type.
  ///
  /// \param i Which attached property wrapper type is being queried, where 0 is the outermost (first)
  /// attached property wrapper type.
  PropertyWrapperTypeInfo getAttachedPropertyWrapperTypeInfo(unsigned i) const;

  /// Retrieve the fully resolved attached property wrapper type.
  ///
  /// This type will be the fully-resolved form of
  /// \c getAttachedPropertyWrapperType(0), which will not contain any
  /// unbound generic types. It will be the type of the backing property.
  Type getPropertyWrapperBackingPropertyType() const;

  /// If there is an attached property wrapper, retrieve the synthesized
  /// auxiliary variables.
  PropertyWrapperAuxiliaryVariables
      getPropertyWrapperAuxiliaryVariables() const;

  /// If there is an attached property wrapper, retrieve information about
  /// how to initialize the backing property.
  PropertyWrapperInitializerInfo
      getPropertyWrapperInitializerInfo() const;

  /// Retrieve information about the mutability of the composed
  /// property wrappers.
  Optional<PropertyWrapperMutability>
      getPropertyWrapperMutability() const;

  /// Returns whether this property is the backing storage property or a storage
  /// wrapper for wrapper instance's projectedValue. If this property is
  /// neither, then it returns `None`.
  Optional<PropertyWrapperSynthesizedPropertyKind>
  getPropertyWrapperSynthesizedPropertyKind() const;

  /// Retrieve the backing storage property for a property that has an
  /// attached property wrapper.
  ///
  /// The backing storage property will be a stored property of the
  /// wrapper's type. This will be equivalent to
  /// \c getAttachedPropertyWrapperType(0) when it is fully-specified;
  /// if \c getAttachedPropertyWrapperType(0) involves an unbound
  /// generic type, the backing storage property will be the appropriate
  /// bound generic version.
  VarDecl *getPropertyWrapperBackingProperty() const;

  /// Retrieve the projection var for a property that has an attached
  /// property wrapper with a \c projectedValue .
  VarDecl *getPropertyWrapperProjectionVar() const;

  /// Retrieve the local wrapped value var for a parameter that has
  /// an attached property wrapper.
  VarDecl *getPropertyWrapperWrappedValueVar() const;

  /// Return true if this property either has storage or has an attached property
  /// wrapper that has storage.
  bool hasStorageOrWrapsStorage() const;

  /// Visit all auxiliary declarations to this VarDecl.
  ///
  /// An auxiliary declaration is a declaration synthesized by the compiler to support
  /// this VarDecl, such as synthesized property wrapper variables.
  ///
  /// \note this function only visits auxiliary decls that are not part of the AST.
  void visitAuxiliaryDecls(llvm::function_ref<void(VarDecl *)>) const;

  /// Retrieve the backing storage property for a lazy property.
  VarDecl *getLazyStorageProperty() const;

  /// Whether the memberwise initializer parameter for a property with a
  /// property wrapper type uses the wrapped type. This will occur, for example,
  /// when there is an explicitly-specified initializer like:
  ///
  /// \code
  /// @Lazy var i = 17
  /// \endcode
  ///
  /// Or when there is no initializer but each composed property wrapper has
  /// a suitable `init(wrappedValue:)`.
  bool isPropertyMemberwiseInitializedWithWrappedType() const;

  /// Return the interface type of the value used for the 'wrappedValue:'
  /// parameter when initializing a property wrapper.
  ///
  /// If the property has an attached property wrapper and the 'wrappedValue:'
  /// parameter is an autoclosure, return a function type returning the stored
  /// value. Otherwise, return the interface type of the stored value.
  Type getPropertyWrapperInitValueInterfaceType() const;

  /// If this property is the backing storage for a property with an attached
  /// property wrapper, return the original property.
  ///
  /// \param kind If not \c None, only returns the original property when
  /// \c this property is the specified synthesized property.
  VarDecl *getOriginalWrappedProperty(
      Optional<PropertyWrapperSynthesizedPropertyKind> kind = None) const;

  /// Set the property that wraps to this property as it's backing
  /// property.
  void setOriginalWrappedProperty(VarDecl *originalProperty);

  /// Return the Objective-C runtime name for this property.
  Identifier getObjCPropertyName() const;

  /// Retrieve the default Objective-C selector for the getter of a
  /// property of the given name.
  static ObjCSelector getDefaultObjCGetterSelector(ASTContext &ctx,
                                                   Identifier propertyName);

  /// Retrieve the default Objective-C selector for the setter of a
  /// property of the given name.
  static ObjCSelector getDefaultObjCSetterSelector(ASTContext &ctx,
                                                   Identifier propertyName);

  /// If this is a simple 'let' constant, emit a note with a fixit indicating
  /// that it can be rewritten to a 'var'.  This is used in situations where the
  /// compiler detects obvious attempts to mutate a constant.
  void emitLetToVarNoteIfSimple(DeclContext *UseDC) const;

  /// Returns true if the name is the self identifier and is implicit.
  bool isSelfParameter() const;

  /// Check whether the variable is the "self" of an actor method.
  bool isActorSelf() const;

  /// Determine whether this property will be part of the implicit memberwise
  /// initializer.
  ///
  /// \param preferDeclaredProperties When encountering a `lazy` property
  /// or a property that has an attached property wrapper, prefer the
  /// actual declared property (which may or may not be considered "stored"
  /// as the moment) to the backing storage property. Otherwise, the stored
  /// backing property will be treated as the member-initialized property.
  bool isMemberwiseInitialized(bool preferDeclaredProperties) const;

  /// Return the range of semantics attributes attached to this VarDecl.
  auto getSemanticsAttrs() const
      -> decltype(getAttrs().getAttributes<SemanticsAttr>()) {
    return getAttrs().getAttributes<SemanticsAttr>();
  }

  /// Returns true if this VarDecl has the string \p attrValue as a semantics
  /// attribute.
  bool hasSemanticsAttr(StringRef attrValue) const {
    return llvm::any_of(getSemanticsAttrs(), [&](const SemanticsAttr *attr) {
      return attrValue.equals(attr->Value);
    });
  }

  clang::PointerAuthQualifier getPointerAuthQualifier() const;

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) { 
    return D->getKind() == DeclKind::Var || D->getKind() == DeclKind::Param; 
  }
};

/// A function parameter declaration.
class ParamDecl : public VarDecl {
  friend class DefaultArgumentInitContextRequest;
  friend class DefaultArgumentExprRequest;
  friend class DefaultArgumentTypeRequest;

  enum class ArgumentNameFlags : uint8_t {
    /// Whether or not this parameter is destructed.
    Destructured = 1 << 0,

    /// Whether or not this parameter is '_const'.
    IsCompileTimeConst = 1 << 1,
  };

  llvm::PointerIntPair<Identifier, 2, OptionSet<ArgumentNameFlags>>
      ArgumentNameAndFlags;
  SourceLoc ParameterNameLoc;
  SourceLoc ArgumentNameLoc;
  SourceLoc SpecifierLoc;

  TypeRepr *TyRepr = nullptr;

  struct alignas(1 << StoredDefaultArgumentAlignInBits) StoredDefaultArgument {
    PointerUnion<Expr *, VarDecl *> DefaultArg;

    /// The type of the default argument expression.
    Type ExprType;

    /// Stores the context for the default argument as well as a bit to
    /// indicate whether the default expression has been type-checked.
    llvm::PointerIntPair<Initializer *, 1, bool> InitContextAndIsTypeChecked;

    StringRef StringRepresentation;
    CaptureInfo Captures;
  };

  /// Retrieve the cached initializer context for the parameter's default
  /// argument without triggering a request.
  Optional<Initializer *> getCachedDefaultArgumentInitContext() const;

  enum class Flags : uint8_t {
    /// Whether or not this parameter is vargs.
    IsVariadic = 1 << 0,

    /// Whether or not this parameter is `@autoclosure`.
    IsAutoClosure = 1 << 1,

    /// Whether or not this parameter is 'isolated'.
    IsIsolated = 1 << 2,
  };

  /// The default value, if any, along with flags.
  llvm::PointerIntPair<StoredDefaultArgument *, 3, OptionSet<Flags>>
      DefaultValueAndFlags;

  friend class ParamSpecifierRequest;

public:
  ParamDecl(SourceLoc specifierLoc, SourceLoc argumentNameLoc,
            Identifier argumentName, SourceLoc parameterNameLoc,
            Identifier parameterName, DeclContext *dc);

  /// Create a new ParamDecl identical to the first except without the interface type.
  static ParamDecl *cloneWithoutType(const ASTContext &Ctx, ParamDecl *PD);

  /// Create a an identical copy of this ParamDecl.
  static ParamDecl *clone(const ASTContext &Ctx, ParamDecl *PD);

  static ParamDecl *
  createImplicit(ASTContext &Context, SourceLoc specifierLoc,
                 SourceLoc argumentNameLoc, Identifier argumentName,
                 SourceLoc parameterNameLoc, Identifier parameterName,
                 Type interfaceType, DeclContext *Parent,
                 ParamSpecifier specifier = ParamSpecifier::Default);

  static ParamDecl *
  createImplicit(ASTContext &Context, Identifier argumentName,
                 Identifier parameterName, Type interfaceType,
                 DeclContext *Parent,
                 ParamSpecifier specifier = ParamSpecifier::Default);

  /// Retrieve the argument (API) name for this function parameter.
  Identifier getArgumentName() const {
    return ArgumentNameAndFlags.getPointer();
  }

  /// Retrieve the parameter (local) name for this function parameter.
  Identifier getParameterName() const { return getName(); }

  /// Retrieve the source location of the argument (API) name.
  ///
  /// The resulting source location will be valid if the argument name
  /// was specified separately from the parameter name.
  SourceLoc getArgumentNameLoc() const { return ArgumentNameLoc; }

  SourceLoc getParameterNameLoc() const { return ParameterNameLoc; }

  SourceLoc getSpecifierLoc() const { return SpecifierLoc; }

  /// Retrieve the TypeRepr corresponding to the parsed type of the parameter, if it exists.
  TypeRepr *getTypeRepr() const { return TyRepr; }
  void setTypeRepr(TypeRepr *repr) { TyRepr = repr; }

  bool isDestructured() const {
    auto flags = ArgumentNameAndFlags.getInt();
    return flags.contains(ArgumentNameFlags::Destructured);
  }

  void setDestructured(bool repr) {
    auto flags = ArgumentNameAndFlags.getInt();
    flags = repr ? flags | ArgumentNameFlags::Destructured
                 : flags - ArgumentNameFlags::Destructured;
    ArgumentNameAndFlags.setInt(flags);
  }

  DefaultArgumentKind getDefaultArgumentKind() const {
    return static_cast<DefaultArgumentKind>(Bits.ParamDecl.defaultArgumentKind);
  }
  bool isDefaultArgument() const {
    return getDefaultArgumentKind() != DefaultArgumentKind::None;
  }
  void setDefaultArgumentKind(DefaultArgumentKind K) {
    Bits.ParamDecl.defaultArgumentKind = static_cast<unsigned>(K);
  }

  void setDefaultArgumentKind(ArgumentAttrs K) {
    setDefaultArgumentKind(K.argumentKind);
  }

  /// Whether this parameter has a default argument expression available.
  ///
  /// Note that this will return false for deserialized declarations, which only
  /// have a textual representation of their default expression.
  bool hasDefaultExpr() const;

  /// Whether this parameter has a caller-side default argument expression
  /// such as the magic literal \c #function.
  bool hasCallerSideDefaultExpr() const;

  /// Retrieve the fully type-checked default argument expression for this
  /// parameter, or \c nullptr if there is no default expression.
  ///
  /// Note that while this will produce a type-checked expression for
  /// caller-side default arguments such as \c #function, this is done purely to
  /// check whether the code is valid. Such default arguments get re-created
  /// at the call site in order to have the correct context information.
  Expr *getTypeCheckedDefaultExpr() const;

  /// Retrieve the potentially un-type-checked default argument expression for
  /// this parameter, which can be queried for information such as its source
  /// range and textual representation. Returns \c nullptr if there is no
  /// default expression.
  Expr *getStructuralDefaultExpr() const {
    if (auto stored = DefaultValueAndFlags.getPointer())
      return stored->DefaultArg.dyn_cast<Expr *>();
    return nullptr;
  }

  /// Retrieve the type of the default expression (if any) associated with
  /// this parameter declaration.
  Type getTypeOfDefaultExpr() const;

  VarDecl *getStoredProperty() const {
    if (auto stored = DefaultValueAndFlags.getPointer())
      return stored->DefaultArg.dyn_cast<VarDecl *>();
    return nullptr;
  }

  /// Sets a new default argument expression for this parameter. This should
  /// only be called internally by ParamDecl and AST walkers.
  ///
  /// \param E The new default argument.
  /// \param isTypeChecked Whether this argument should be used as the
  /// parameter's fully type-checked default argument.
  void setDefaultExpr(Expr *E, bool isTypeChecked);

  /// Sets a type of default expression associated with this parameter.
  /// This should only be called by deserialization.
  void setDefaultExprType(Type type);

  void setStoredProperty(VarDecl *var);

  /// Retrieve the initializer context for the parameter's default argument.
  Initializer *getDefaultArgumentInitContext() const;

  void setDefaultArgumentInitContext(Initializer *initContext);

  CaptureInfo getDefaultArgumentCaptureInfo() const {
    assert(DefaultValueAndFlags.getPointer());
    return DefaultValueAndFlags.getPointer()->Captures;
  }

  void setDefaultArgumentCaptureInfo(CaptureInfo captures);

  /// Extracts the text of the default argument attached to the provided
  /// ParamDecl, removing all inactive #if clauses and providing only the
  /// text of active #if clauses.
  ///
  /// For example, the default argument:
  /// ```
  /// {
  ///   #if false
  ///   print("false")
  ///   #else
  ///   print("true")
  ///   #endif
  /// }
  /// ```
  /// will return
  /// ```
  /// {
  ///   print("true")
  /// }
  /// ```
  /// \sa getDefaultValue
  StringRef getDefaultValueStringRepresentation(
    SmallVectorImpl<char> &scratch) const;

  void setDefaultValueStringRepresentation(StringRef stringRepresentation);

  /// Whether or not this parameter is old-style variadic.
  bool isVariadic() const;
  void setVariadic(bool value = true) {
    auto flags = DefaultValueAndFlags.getInt();
    DefaultValueAndFlags.setInt(value ? flags | Flags::IsVariadic
                                      : flags - Flags::IsVariadic);
  }

  /// Whether or not this parameter is marked with `@autoclosure`.
  bool isAutoClosure() const {
    return DefaultValueAndFlags.getInt().contains(Flags::IsAutoClosure);
  }
  void setAutoClosure(bool value = true) {
    auto flags = DefaultValueAndFlags.getInt();
    DefaultValueAndFlags.setInt(value ? flags | Flags::IsAutoClosure
                                      : flags - Flags::IsAutoClosure);
  }

  /// Whether or not this parameter is marked with 'isolated'.
  bool isIsolated() const {
    return DefaultValueAndFlags.getInt().contains(Flags::IsIsolated);
  }

  void setIsolated(bool value = true) {
    auto flags = DefaultValueAndFlags.getInt();
    DefaultValueAndFlags.setInt(value ? flags | Flags::IsIsolated
                                      : flags - Flags::IsIsolated);
  }

  /// Whether or not this parameter is marked with '_const'.
  bool isCompileTimeConst() const {
    return ArgumentNameAndFlags.getInt().contains(
        ArgumentNameFlags::IsCompileTimeConst);
  }

  void setCompileTimeConst(bool value = true) {
    auto flags = ArgumentNameAndFlags.getInt();
    flags = value ? flags | ArgumentNameFlags::IsCompileTimeConst
                  : flags - ArgumentNameFlags::IsCompileTimeConst;
    ArgumentNameAndFlags.setInt(flags);
  }

  /// Does this parameter reject temporary pointer conversions?
  bool isNonEphemeral() const;

  /// Attempt to apply an implicit `@_nonEphemeral` attribute to this parameter.
  void setNonEphemeralIfPossible();

  /// Remove the type of this varargs element designator, without the array
  /// type wrapping it.  A parameter like "Int..." will have formal parameter
  /// type of "[Int]" and this returns "Int".
  static Type getVarargBaseTy(Type VarArgT);
  
  /// Remove the type of this varargs element designator, without the array
  /// type wrapping it.
  Type getVarargBaseTy() const {
    assert(isVariadic());
    return getVarargBaseTy(getInterfaceType());
  }

  /// Determine whether this declaration is an anonymous closure parameter.
  bool isAnonClosureParam() const;

  using Specifier = ParamSpecifier;

  Optional<Specifier> getCachedSpecifier() const {
    if (Bits.ParamDecl.OwnershipSpecifier != 0)
      return Specifier(Bits.ParamDecl.OwnershipSpecifier - 1);

    return None;
  }

  /// Return the raw specifier value for this parameter.
  Specifier getSpecifier() const;
  void setSpecifier(Specifier Spec);

  LifetimeAnnotation getLifetimeAnnotation() const;

  /// Is the type of this parameter 'inout'?
  bool isInOut() const { return getSpecifier() == Specifier::InOut; }

  bool isImmutableInFunctionBody() const {
    return isSpecifierImmutableInFunctionBody(getSpecifier());
  }
  static bool isSpecifierImmutableInFunctionBody(Specifier sp) {
    switch (sp) {
    case Specifier::Default:
    case Specifier::Borrowing:
    case Specifier::LegacyShared:
    case Specifier::LegacyOwned:
      return true;
    case Specifier::Consuming:
    case Specifier::InOut:
      return false;
    }
    llvm_unreachable("unhandled specifier");
  }

  ValueOwnership getValueOwnership() const {
    return getValueOwnershipForSpecifier(getSpecifier());
  }

  static ValueOwnership getValueOwnershipForSpecifier(Specifier specifier) {
    switch (specifier) {
    case ParamSpecifier::InOut:
      return ValueOwnership::InOut;
    case ParamSpecifier::Borrowing:
    case ParamSpecifier::LegacyShared:
      return ValueOwnership::Shared;
    case ParamSpecifier::Consuming:
    case ParamSpecifier::LegacyOwned:
      return ValueOwnership::Owned;
    case ParamSpecifier::Default:
      return ValueOwnership::Default;
    }
    llvm_unreachable("invalid ParamSpecifier");
  }

  static Specifier
  getParameterSpecifierForValueOwnership(ValueOwnership ownership) {
    // TODO: switch over to consuming/borrowing once they're fully supported
    switch (ownership) {
    case ValueOwnership::Default:
      return Specifier::Default;
    case ValueOwnership::Shared:
      return Specifier::LegacyShared; // should become Borrowing
    case ValueOwnership::InOut:
      return Specifier::InOut;
    case ValueOwnership::Owned:
      return Specifier::LegacyOwned; // should become Consuming
    }
    llvm_unreachable("unhandled ownership");
  }

  SourceRange getSourceRange() const;

  AnyFunctionType::Param toFunctionParam(Type type = Type()) const;

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) { 
    return D->getKind() == DeclKind::Param;
  }
  
  /// Get the source code spelling of a parameter specifier value as a string.
  static StringRef getSpecifierSpelling(Specifier spec);
};
  
inline ValueOwnership
ParameterTypeFlags::getValueOwnership() const {
  return ParamDecl::getValueOwnershipForSpecifier(getOwnershipSpecifier());
}
  
inline ValueOwnership
YieldTypeFlags::getValueOwnership() const {
  return ParamDecl::getValueOwnershipForSpecifier(getOwnershipSpecifier());
}
  
/// Describes the kind of subscripting used in Objective-C.
enum class ObjCSubscriptKind {
  /// Objective-C indexed subscripting, which is based on an integral
  /// index.
  Indexed,
  /// Objective-C keyed subscripting, which is based on an object
  /// argument or metatype thereof.
  Keyed
};

/// Declares a subscripting operator for a type.
///
/// A subscript declaration is defined as a get/set pair that produces a
/// specific type. For example:
///
/// \code
/// subscript (i : Int) -> String {
///   get { /* return ith String */ }
///   set { /* set ith string to value */ }
/// }
/// \endcode
///
/// A type with a subscript declaration can be used as the base of a subscript
/// expression a[i], where a is of the subscriptable type and i is the type
/// of the index. A subscript can have multiple indices:
///
/// \code
/// struct Matrix {
///   subscript (i : Int, j : Int) -> Double {
///     get { /* return element at position (i, j) */ }
///     set { /* set element at position (i, j) */ }
///   }
/// }
/// \endcode
///
/// A given type can have multiple subscript declarations, so long as the
/// signatures (indices and element type) are distinct.
///
class SubscriptDecl : public GenericContext, public AbstractStorageDecl {
  friend class ResultTypeRequest;

  SourceLoc StaticLoc;
  SourceLoc ArrowLoc;
  SourceLoc EndLoc;
  ParameterList *Indices;
  TypeLoc ElementTy;

  void setElementInterfaceType(Type type);

  SubscriptDecl(DeclName Name,
                SourceLoc StaticLoc, StaticSpellingKind StaticSpelling,
                SourceLoc SubscriptLoc, ParameterList *Indices,
                SourceLoc ArrowLoc, TypeRepr *ElementTyR, DeclContext *Parent,
                GenericParamList *GenericParams)
    : GenericContext(DeclContextKind::SubscriptDecl, Parent, GenericParams),
      AbstractStorageDecl(DeclKind::Subscript,
                          StaticSpelling != StaticSpellingKind::None,
                          Parent, Name, SubscriptLoc,
                          /*will be overwritten*/ StorageIsNotMutable),
      StaticLoc(StaticLoc), ArrowLoc(ArrowLoc),
      Indices(nullptr), ElementTy(ElementTyR) {
    Bits.SubscriptDecl.StaticSpelling = static_cast<unsigned>(StaticSpelling);
    setIndices(Indices);
  }

public:
  /// Factory function only for use by deserialization.
  static SubscriptDecl *createDeserialized(ASTContext &Context, DeclName Name,
                                           StaticSpellingKind StaticSpelling,
                                           Type ElementTy, DeclContext *Parent,
                                           GenericParamList *GenericParams);

  static SubscriptDecl *create(ASTContext &Context, DeclName Name,
                               SourceLoc StaticLoc,
                               StaticSpellingKind StaticSpelling,
                               SourceLoc SubscriptLoc, ParameterList *Indices,
                               SourceLoc ArrowLoc, TypeRepr *ElementTyR,
                               DeclContext *Parent,
                               GenericParamList *GenericParams);

  static SubscriptDecl *create(ASTContext &Context, DeclName Name,
                               SourceLoc StaticLoc,
                               StaticSpellingKind StaticSpelling,
                               SourceLoc SubscriptLoc, ParameterList *Indices,
                               SourceLoc ArrowLoc, Type ElementTy,
                               DeclContext *Parent,
                               GenericParamList *GenericParams);

  static SubscriptDecl *createImported(ASTContext &Context, DeclName Name,
                                       SourceLoc SubscriptLoc,
                                       ParameterList *Indices,
                                       SourceLoc ArrowLoc, Type ElementTy,
                                       DeclContext *Parent, ClangNode ClangN);
  
  /// \returns the way 'static'/'class' was spelled in the source.
  StaticSpellingKind getStaticSpelling() const {
    return static_cast<StaticSpellingKind>(Bits.SubscriptDecl.StaticSpelling);
  }
  
  SourceLoc getStaticLoc() const { return StaticLoc; }
  SourceLoc getSubscriptLoc() const { return getNameLoc(); }
  
  SourceLoc getStartLoc() const {
    return getStaticLoc().isValid() ? getStaticLoc() : getSubscriptLoc();
  }
  SourceLoc getEndLoc() const { return EndLoc; }

  void setEndLoc(SourceLoc sl) { EndLoc = sl; }
  SourceRange getSourceRange() const;
  SourceRange getSignatureSourceRange() const;

  /// Retrieve the indices for this subscript operation.
  ParameterList *getIndices() { return Indices; }
  const ParameterList *getIndices() const { return Indices; }
  void setIndices(ParameterList *p);

  /// Retrieve the type of the element referenced by a subscript
  /// operation.
  Type getElementInterfaceType() const;

  TypeRepr *getElementTypeRepr() const { return ElementTy.getTypeRepr(); }
  SourceRange getElementTypeSourceRange() const {
    return ElementTy.getSourceRange();
  }

  /// Determine the kind of Objective-C subscripting this declaration
  /// implies.
  ObjCSubscriptKind getObjCSubscriptKind() const;

  SubscriptDecl *getOverriddenDecl() const {
    return cast_or_null<SubscriptDecl>(
                                  AbstractStorageDecl::getOverriddenDecl());
  }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Subscript;
  }
  
  static bool classof(const DeclContext *DC) {
    if (auto D = DC->getAsDecl())
      return classof(D);
    return false;
  }

  using DeclContext::operator new;
  using DeclContext::operator delete;
  using Decl::getASTContext;
};

/// Encodes imported-as-member status for C functions that get imported
/// as methods.
class ImportAsMemberStatus {
  friend class AbstractFunctionDecl;

  // non-0 denotes import-as-member. 1 denotes no self index. n+2 denotes self
  // index of n
  uint8_t rawValue;

public:
  ImportAsMemberStatus(uint8_t rawValue = 0) : rawValue(rawValue) {}

  uint8_t getRawValue() const { return rawValue; }

  bool isImportAsMember() const { return rawValue != 0; }
  bool isInstance() const { return rawValue >= 2; }
  bool isStatic() const { return rawValue == 1; }
  uint8_t getSelfIndex() const {
    assert(isInstance() && "not set");
    return rawValue - 2;
  }
  void setStatic() {
    assert(!isStatic() && "already set");
    rawValue = 1;
  }
  void setSelfIndex(uint8_t idx) {
    assert(!isImportAsMember() && "already set");
    assert(idx <= UINT8_MAX-2 && "out of bounds");
    rawValue = idx + 2;
  }
};

class BodyAndFingerprint {
  llvm::PointerIntPair<BraceStmt *, 1, bool> BodyAndHasFp;
  Fingerprint Fp;

public:
  BodyAndFingerprint(BraceStmt *body, Optional<Fingerprint> fp)
      : BodyAndHasFp(body, fp.has_value()),
        Fp(fp.has_value() ? *fp : Fingerprint::ZERO()) {}
  BodyAndFingerprint() : BodyAndFingerprint(nullptr, None) {}

  BraceStmt *getBody() const { return BodyAndHasFp.getPointer(); }

  Optional<Fingerprint> getFingerprint() const {
    if (BodyAndHasFp.getInt())
      return Fp;
    else
      return None;
  }

  void setFingerprint(Optional<Fingerprint> fp) {
    if (fp.has_value()) {
      Fp = *fp;
      BodyAndHasFp.setInt(true);
    } else {
      Fp = Fingerprint::ZERO();
      BodyAndHasFp.setInt(false);
    }
  }
};

void simple_display(llvm::raw_ostream &out, BodyAndFingerprint value);

/// Base class for function-like declarations.
class AbstractFunctionDecl : public GenericContext, public ValueDecl {
  friend class NeedsNewVTableEntryRequest;

public:
  /// records the kind of SILGen-synthesized body this decl represents
  enum class SILSynthesizeKind {
    None,
    MemberwiseInitializer,
    DistributedActorFactory

    // This enum currently needs to fit in a 2-bit bitfield.
  };

  enum class BodyKind {
    /// The function did not have a body in the source code file.
    None,

    /// Function body is delayed, to be parsed later.
    Unparsed,

    /// Function body is parsed and available as an AST subtree.
    Parsed,

    /// Function body is not available, although it was written in the source.
    Skipped,

    /// Function body will be synthesized on demand.
    Synthesize,

    /// Function body is present and type-checked.
    TypeChecked,

    // Function body will be synthesized by SILGen.
    SILSynthesize,

    /// Function body text was deserialized from a .swiftmodule.
    Deserialized

    // This enum currently needs to fit in a 3-bit bitfield.
  };

  BodyKind getBodyKind() const {
    return BodyKind(Bits.AbstractFunctionDecl.BodyKind);
  }

  struct BodySynthesizer {
    std::pair<BraceStmt *, bool> (* Fn)(AbstractFunctionDecl *, void *);
    void *Context;
  };

private:
  ParameterList *Params;

private:
  /// The generation at which we last loaded derivative function configurations.
  unsigned DerivativeFunctionConfigGeneration = 0;
  /// Prepare to traverse the list of derivative function configurations.
  void prepareDerivativeFunctionConfigurations();

  /// A uniqued list of derivative function configurations.
  /// - `@differentiable` and `@derivative` attribute type-checking is
  ///   responsible for populating derivative function configurations specified
  ///   in the current module.
  /// - Module loading is responsible for populating derivative function
  ///   configurations from imported modules.
  struct DerivativeFunctionConfigurationList;
  DerivativeFunctionConfigurationList *DerivativeFunctionConfigs = nullptr;

public:
  /// Get all derivative function configurations.
  ArrayRef<AutoDiffConfig>
  getDerivativeFunctionConfigurations();

  /// Add the given derivative function configuration.
  void addDerivativeFunctionConfiguration(const AutoDiffConfig &config);

protected:
  // If a function has a body at all, we have either a parsed body AST node or
  // we have saved the end location of the unparsed body.
  union {
    /// This enum member is active if getBodyKind() is BodyKind::Parsed or
    /// BodyKind::TypeChecked.
    BodyAndFingerprint BodyAndFP;

    /// This enum member is active if getBodyKind() is BodyKind::Deserialized.
    StringRef BodyStringRepresentation;

    /// This enum member is active if getBodyKind() == BodyKind::Synthesize.
    BodySynthesizer Synthesizer;

    /// The location of the function body when the body is delayed or skipped.
    ///
    /// This enum member is active if getBodyKind() is BodyKind::Unparsed or
    /// BodyKind::Skipped.
    SourceRange BodyRange;
  };

  friend class ParseAbstractFunctionBodyRequest;
  friend class TypeCheckFunctionBodyRequest;

  CaptureInfo Captures;

  /// Location of the 'async' token.
  SourceLoc AsyncLoc;

  /// Location of the 'throws' token.
  SourceLoc ThrowsLoc;

  struct {
    unsigned NeedsNewVTableEntryComputed : 1;
    unsigned NeedsNewVTableEntry : 1;
  } LazySemanticInfo = { };

  AbstractFunctionDecl(DeclKind Kind, DeclContext *Parent, DeclName Name,
                       SourceLoc NameLoc, bool Async, SourceLoc AsyncLoc,
                       bool Throws, SourceLoc ThrowsLoc,
                       bool HasImplicitSelfDecl,
                       GenericParamList *GenericParams)
      : GenericContext(DeclContextKind::AbstractFunctionDecl, Parent,
                       GenericParams),
        ValueDecl(Kind, Parent, Name, NameLoc), BodyAndFP(), AsyncLoc(AsyncLoc),
        ThrowsLoc(ThrowsLoc) {
    setBodyKind(BodyKind::None);
    Bits.AbstractFunctionDecl.HasImplicitSelfDecl = HasImplicitSelfDecl;
    Bits.AbstractFunctionDecl.Overridden = false;
    Bits.AbstractFunctionDecl.Async = Async;
    Bits.AbstractFunctionDecl.Throws = Throws;
    Bits.AbstractFunctionDecl.HasSingleExpressionBody = false;
    Bits.AbstractFunctionDecl.HasNestedTypeDeclarations = false;
    Bits.AbstractFunctionDecl.DistributedThunk = false;
  }

  void setBodyKind(BodyKind K) {
    Bits.AbstractFunctionDecl.BodyKind = unsigned(K);
  }

  void setSILSynthesizeKind(SILSynthesizeKind K) {
    Bits.AbstractFunctionDecl.SILSynthesizeKind = unsigned(K);
  }

  SILSynthesizeKind getSILSynthesizeKind() const {
    return SILSynthesizeKind(Bits.AbstractFunctionDecl.SILSynthesizeKind);
  }

public:
  void setHasSingleExpressionBody(bool Has = true) { 
    Bits.AbstractFunctionDecl.HasSingleExpressionBody = Has;
  }

  bool hasSingleExpressionBody() const {
    return Bits.AbstractFunctionDecl.HasSingleExpressionBody;
  }

  Expr *getSingleExpressionBody() const;
  void setSingleExpressionBody(Expr *NewBody);

  /// Returns the string for the base name, or "_" if this is unnamed.
  StringRef getNameStr() const {
    assert(!getName().isSpecial() && "Cannot get string for special names");
    return hasName() ? getBaseIdentifier().str() : "_";
  }

  /// Should this declaration be treated as if annotated with transparent
  /// attribute.
  bool isTransparent() const;

  // Expose our import as member status
  ImportAsMemberStatus getImportAsMemberStatus() const {
    return ImportAsMemberStatus(Bits.AbstractFunctionDecl.IAMStatus);
  }
  bool isImportAsMember() const {
    return getImportAsMemberStatus().isImportAsMember();
  }
  bool isImportAsInstanceMember() const {
    return getImportAsMemberStatus().isInstance();
  }
  bool isImportAsStaticMember() const {
    return getImportAsMemberStatus().isStatic();
  }
  uint8_t getSelfIndex() const {
    return getImportAsMemberStatus().getSelfIndex();
  }

  void setImportAsStaticMember() {
    auto newValue = getImportAsMemberStatus();
    newValue.setStatic();
    Bits.AbstractFunctionDecl.IAMStatus = newValue.getRawValue();
  }
  void setSelfIndex(uint8_t idx) {
    auto newValue = getImportAsMemberStatus();
    newValue.setSelfIndex(idx);
    Bits.AbstractFunctionDecl.IAMStatus = newValue.getRawValue();
  }

  /// Retrieve the location of the 'async' keyword, if present.
  SourceLoc getAsyncLoc() const { return AsyncLoc; }

  /// Retrieve the location of the 'throws' keyword, if present.
  SourceLoc getThrowsLoc() const { return ThrowsLoc; }

  /// Returns true if the function is marked as `async`. The
  /// type of the function will be `async` as well.
  bool hasAsync() const { return Bits.AbstractFunctionDecl.Async; }

  /// Determine whether the given function is concurrent.
  ///
  /// A function is concurrent if it has the @Sendable attribute.
  bool isSendable() const;

  /// Returns true if the function is a suitable 'async' context.
  ///
  /// Functions that are an 'async' context can make calls to 'async' functions.
  bool isAsyncContext() const {
    return hasAsync();
  }

  /// Returns true if the function body throws.
  bool hasThrows() const { return Bits.AbstractFunctionDecl.Throws; }

  /// Returns if the function throws or is async.
  bool hasEffect(EffectKind kind) const;

  /// Returns if the function is 'rethrows' or 'reasync'.
  bool hasPolymorphicEffect(EffectKind kind) const;

  /// Returns 'true' if the function is distributed.
  bool isDistributed() const;

  /// Is this a thunk function used to access a distributed method
  /// or computed property outside of its actor isolation context?
  bool isDistributedThunk() const {
    return Bits.AbstractFunctionDecl.DistributedThunk;
  }

  void setDistributedThunk(bool isThunk) {
    Bits.AbstractFunctionDecl.DistributedThunk = isThunk;
  }

  /// For a 'distributed' target (func or computed property),
  /// get the 'thunk' responsible for performing the 'remoteCall'.
  ///
  /// \return the synthesized thunk, or null if the base of the call has
  ///         diagnosed errors during type checking.
  FuncDecl *getDistributedThunk() const;

  PolymorphicEffectKind getPolymorphicEffectKind(EffectKind kind) const;

  // FIXME: Hack that provides names with keyword arguments for accessors.
  DeclName getEffectiveFullName() const;

  /// Returns true if the function has a body written in the source file.
  ///
  /// Note that a true return value does not imply that the body was actually
  /// parsed.
  bool hasBody() const {
    return getBodyKind() != BodyKind::None &&
           getBodyKind() != BodyKind::Skipped;
  }

  /// Returns true if the text of this function's body can be retrieved either
  /// by extracting the text from the source buffer or reading the inlinable
  /// body from a deserialized swiftmodule.
  bool hasInlinableBodyText() const;

  /// Returns the function body, if it was parsed, or nullptr otherwise.
  ///
  /// Note that a null return value does not imply that the source code did not
  /// have a body for this function.
  ///
  /// \sa hasBody()
  BraceStmt *getBody(bool canSynthesize = true) const;

  /// Retrieve the type-checked body of the given function, or \c nullptr if
  /// there's no body available.
  BraceStmt *getTypecheckedBody() const;

  /// Set a new body for the function.
  void setBody(BraceStmt *S, BodyKind NewBodyKind);

  /// Note that the body was skipped for this function. Function body
  /// cannot be attached after this call.
  void setBodySkipped(SourceRange bodyRange) {
    // FIXME: Remove 'Parsed' from this list once we can always delay
    //        parsing bodies. The -experimental-skip-*-function-bodies options
    //        do currently skip parsing, unless disabled through other means in
    //        SourceFile::hasDelayedBodyParsing.
    assert(getBodyKind() == BodyKind::None ||
           getBodyKind() == BodyKind::Unparsed ||
           getBodyKind() == BodyKind::Parsed);
    assert(bodyRange.isValid());
    BodyRange = bodyRange;
    setBodyKind(BodyKind::Skipped);
  }

  /// Note that parsing for the body was delayed.
  void setBodyDelayed(SourceRange bodyRange) {
    assert(getBodyKind() == BodyKind::None);
    assert(bodyRange.isValid());
    BodyRange = bodyRange;
    setBodyKind(BodyKind::Unparsed);
  }

  void setBodyToBeReparsed(SourceRange bodyRange);

  /// Provide the parsed body for the function.
  void setBodyParsed(BraceStmt *S, Optional<Fingerprint> fp = None) {
    setBody(S, BodyKind::Parsed);
    BodyAndFP.setFingerprint(fp);
  }

  /// Was there a nested type declaration detected when parsing this
  /// function was skipped?
  bool hasNestedTypeDeclarations() const {
    return Bits.AbstractFunctionDecl.HasNestedTypeDeclarations;
  }

  void setHasNestedTypeDeclarations(bool value) {
    Bits.AbstractFunctionDecl.HasNestedTypeDeclarations = value;
  }

  /// Note that parsing for the body was delayed.
  ///
  /// The function should return the body statement and a flag indicating
  /// whether that body is already type-checked.
  void setBodySynthesizer(
      std::pair<BraceStmt *, bool> (* fn)(AbstractFunctionDecl *, void *),
      void *context = nullptr) {
    assert(getBodyKind() == BodyKind::None);
    Synthesizer = {fn, context};
    setBodyKind(BodyKind::Synthesize);
  }

  /// Note that this is a memberwise initializer and thus the body will be
  /// generated by SILGen.
  void setIsMemberwiseInitializer() {
    assert(getBodyKind() == BodyKind::None);
    assert(isa<ConstructorDecl>(this));
    setBodyKind(BodyKind::SILSynthesize);
    setSILSynthesizeKind(SILSynthesizeKind::MemberwiseInitializer);
  }

  /// Mark that the body should be filled in to be a factory method for creating
  /// a distributed actor.
  void setDistributedActorFactory() {
    assert(getBodyKind() == BodyKind::None);
    assert(isa<FuncDecl>(this));
    setBodyKind(BodyKind::SILSynthesize);
    setSILSynthesizeKind(SILSynthesizeKind::DistributedActorFactory);
  }

  /// Gets the body of this function, stripping the unused portions of #if
  /// configs inside the body. If this function was not deserialized from a
  /// .swiftmodule, this body is reconstructed from the original
  /// source buffer.
  StringRef getInlinableBodyText(SmallVectorImpl<char> &scratch) const;

  void setBodyStringRepresentation(StringRef body) {
    assert(getBodyKind() == BodyKind::None);
    setBodyKind(BodyKind::Deserialized);
    BodyStringRepresentation = body;
  }

  bool isBodyTypeChecked() const {
    return getBodyKind() == BodyKind::TypeChecked;
  }

  bool isBodySILSynthesize() const {
    return getBodyKind() == BodyKind::SILSynthesize;
  }

  bool isBodySkipped() const {
    return getBodyKind() == BodyKind::Skipped;
  }

  bool isMemberwiseInitializer() const {
    return getBodyKind() == BodyKind::SILSynthesize
        && getSILSynthesizeKind() == SILSynthesizeKind::MemberwiseInitializer;
  }

  /// Determines whether this function represents a distributed actor
  /// initialization factory. Such functions do not have a body that is
  /// representable in the AST, so it must be synthesized during SILGen.
  bool isDistributedActorFactory() const {
    return getBodyKind() == BodyKind::SILSynthesize &&
           getSILSynthesizeKind() == SILSynthesizeKind::DistributedActorFactory;
  }

  /// Determines whether this function is a 'remoteCall' function,
  /// which is used as ad-hoc protocol requirement by the
  /// 'DistributedActorSystem' protocol.
  bool isDistributedActorSystemRemoteCall(bool isVoidReturn) const;

  /// Determines whether this function is a 'makeInvocationEncoder' function,
  /// which is used as ad-hoc protocol requirement by the
  /// 'DistributedActorSystem' protocol.
  bool isDistributedActorSystemMakeInvocationEncoder() const;

  /// Determines if this function is a 'recordGenericSubstitution' function,
  /// which is used as ad-hoc protocol requirement by the
  /// 'DistributedTargetInvocationEncoder' protocol.
  bool isDistributedTargetInvocationEncoderRecordGenericSubstitution() const;

  /// Determines if this function is a 'recordArgument' function,
  /// which is used as ad-hoc protocol requirement by the
  /// 'DistributedTargetInvocationEncoder' protocol.
  bool isDistributedTargetInvocationEncoderRecordArgument() const;

  /// Determines if this function is a 'recordReturnType' function,
  /// which is used as ad-hoc protocol requirement by the
  /// 'DistributedTargetInvocationEncoder' protocol.
  bool isDistributedTargetInvocationEncoderRecordReturnType() const;

  /// Determines if this function is a 'recordErrorType' function,
  /// which is used as ad-hoc protocol requirement by the
  /// 'DistributedTargetInvocationEncoder' protocol.
  bool isDistributedTargetInvocationEncoderRecordErrorType() const;

  /// Determines if this function is a 'decodeNextArgument' function,
  /// which is used as ad-hoc protocol requirement by the
  /// 'DistributedTargetInvocationDecoder' protocol.
  bool isDistributedTargetInvocationDecoderDecodeNextArgument() const;

  /// Determines if this function is a 'onReturn' function,
  /// which is used as ad-hoc protocol requirement by the
  /// 'DistributedTargetInvocationResultHandler' protocol.
  bool isDistributedTargetInvocationResultHandlerOnReturn() const;

  /// For a method of a class, checks whether it will require a new entry in the
  /// vtable.
  bool needsNewVTableEntry() const;

  /// True if the decl is a method which introduces a new witness table entry.
  bool requiresNewWitnessTableEntry() const {
    return getOverriddenDecls().empty();
  }

public:
  /// Retrieve the source range of the function body.
  SourceRange getBodySourceRange() const;

  /// Keep current \c getBodySourceRange() as the "original" body source range
  /// iff the this method hasn't been called on this object. The current body
  /// source range must be in the same buffer as the location of the declaration
  /// itself.
  void keepOriginalBodySourceRange();

  /// Retrieve the fingerprint of the body. Note that this is not affected by
  /// the body of the local functions or the members of the local types in this
  /// function.
  Optional<Fingerprint> getBodyFingerprint() const;

  /// Retrieve the fingerprint of the body including the local type members and
  /// the local function bodies.
  Optional<Fingerprint> getBodyFingerprintIncludingLocalTypeMembers() const;

  /// Retrieve the source range of the *original* function body.
  ///
  /// This may be different from \c getBodySourceRange() that returns the source
  /// range of the *current* body. It happens when the body is parsed from other
  /// source buffers for e.g. code-completion.
  SourceRange getOriginalBodySourceRange() const;

  /// Retrieve the source range of the function declaration name + patterns.
  SourceRange getSignatureSourceRange() const;

  CaptureInfo getCaptureInfo() const { return Captures; }
  void setCaptureInfo(CaptureInfo captures) { Captures = captures; }

  /// Retrieve the Objective-C selector that names this method.
  ObjCSelector getObjCSelector(DeclName preferredName = DeclName(),
                               bool skipIsObjCResolution = false) const;

  /// Determine whether the given method would produce an Objective-C
  /// instance method.
  bool isObjCInstanceMethod() const;

  /// Determine whether the name of an argument is an API name by default
  /// depending on the function context.
  bool argumentNameIsAPIByDefault() const;

  /// Retrieve the function's parameter list, not including 'self' if present.
  ParameterList *getParameters() {
    return Params;
  }
  const ParameterList *getParameters() const {
    return Params;
  }

  void setParameters(ParameterList *Params);

  bool hasImplicitSelfDecl() const {
    return Bits.AbstractFunctionDecl.HasImplicitSelfDecl;
  }

  ParamDecl **getImplicitSelfDeclStorage();

  /// Retrieve the implicit 'self' parameter for methods. Returns nullptr for
  /// free functions.
  const ParamDecl *getImplicitSelfDecl(bool createIfNeeded=true) const {
    return const_cast<AbstractFunctionDecl*>(this)
        ->getImplicitSelfDecl(createIfNeeded);
  }
  ParamDecl *getImplicitSelfDecl(bool createIfNeeded=true);

  /// Retrieve the declaration that this method overrides, if any.
  AbstractFunctionDecl *getOverriddenDecl() const {
    return cast_or_null<AbstractFunctionDecl>(ValueDecl::getOverriddenDecl());
  }

  /// Whether the declaration is later overridden in the module
  ///
  /// Overrides are resolved during type checking; only query this field after
  /// the whole module has been checked
  bool isOverridden() const { return Bits.AbstractFunctionDecl.Overridden; }

  /// The declaration has been overridden in the module
  ///
  /// Resolved during type checking
  void setIsOverridden() { Bits.AbstractFunctionDecl.Overridden = true; }

  /// Set information about the foreign error convention used by this
  /// declaration.
  void setForeignErrorConvention(const ForeignErrorConvention &convention);

  /// Get information about the foreign error convention used by this
  /// declaration, given that it is @objc and 'throws'.
  Optional<ForeignErrorConvention> getForeignErrorConvention() const;

  /// If this is a foreign C function imported as a method, get the index of
  /// the foreign parameter imported as `self`. If the function is imported
  /// as a static method, `-1` is returned to represent the `self` parameter
  /// being dropped altogether. `None` is returned for a normal function
  /// or method.
  Optional<int> getForeignFunctionAsMethodSelfParameterIndex() const;

  /// Set information about the foreign async convention used by this
  /// declaration.
  void setForeignAsyncConvention(const ForeignAsyncConvention &convention);

  /// Get information about the foreign async convention used by this
  /// declaration, given that it is @objc and 'async'.
  Optional<ForeignAsyncConvention> getForeignAsyncConvention() const;

  static bool classof(const Decl *D) {
    return D->getKind() >= DeclKind::First_AbstractFunctionDecl &&
           D->getKind() <= DeclKind::Last_AbstractFunctionDecl;
  }

  static bool classof(const DeclContext *DC) {
    if (auto D = DC->getAsDecl())
      return classof(D);
    return false;
  }
  
  /// True if the declaration is forced to be statically dispatched.
  bool hasForcedStaticDispatch() const;

  /// Get the type of this declaration without the Self clause.
  /// Asserts if not in type context.
  Type getMethodInterfaceType() const;

  /// Tests if this is a function returning a DynamicSelfType, or a
  /// constructor.
  bool hasDynamicSelfResult() const;

  /// The async function marked as the alternative to this function, if any.
  AbstractFunctionDecl *getAsyncAlternative() const;

  /// If \p asyncAlternative is set, then compare its parameters to this
  /// (presumed synchronous) function's parameters to find the index of the
  /// completion handler parameter. This should be the only missing
  /// parameter in \p asyncAlternative, ignoring defaulted parameters if they
  /// have the same label. It must have a void-returning function type and be
  /// attributed with @escaping but not @autoclosure.
  ///
  /// Returns the last index of the parameter that looks like a completion
  /// handler if \p asyncAlternative is not set (with the same conditions on
  /// its type as above).
  Optional<unsigned> findPotentialCompletionHandlerParam(
      const AbstractFunctionDecl *asyncAlternative = nullptr) const;

  using DeclContext::operator new;
  using DeclContext::operator delete;
  using Decl::getASTContext;
};

class OperatorDecl;

/// FuncDecl - 'func' declaration.
class FuncDecl : public AbstractFunctionDecl {
  friend class AbstractFunctionDecl;
  friend class SelfAccessKindRequest;
  friend class IsStaticRequest;
  friend class ResultTypeRequest;

  SourceLoc StaticLoc;  // Location of the 'static' token or invalid.
  SourceLoc FuncLoc;    // Location of the 'func' token.

  TypeLoc FnRetType;

protected:
  FuncDecl(DeclKind Kind,
           SourceLoc StaticLoc, StaticSpellingKind StaticSpelling,
           SourceLoc FuncLoc,
           DeclName Name, SourceLoc NameLoc,
           bool Async, SourceLoc AsyncLoc,
           bool Throws, SourceLoc ThrowsLoc,
           bool HasImplicitSelfDecl,
           GenericParamList *GenericParams, DeclContext *Parent)
    : AbstractFunctionDecl(Kind, Parent,
                           Name, NameLoc,
                           Async, AsyncLoc,
                           Throws, ThrowsLoc,
                           HasImplicitSelfDecl, GenericParams),
      StaticLoc(StaticLoc), FuncLoc(FuncLoc) {
    assert(!Name.getBaseName().isSpecial());

    Bits.FuncDecl.StaticSpelling = static_cast<unsigned>(StaticSpelling);

    Bits.FuncDecl.ForcedStaticDispatch = false;
    Bits.FuncDecl.SelfAccess =
      static_cast<unsigned>(SelfAccessKind::NonMutating);
    Bits.FuncDecl.SelfAccessComputed = false;
    Bits.FuncDecl.IsStaticComputed = false;
    Bits.FuncDecl.IsStatic = false;
    Bits.FuncDecl.HasTopLevelLocalContextCaptures = false;
  }

  void setResultInterfaceType(Type type);

private:
  static FuncDecl *createImpl(ASTContext &Context, SourceLoc StaticLoc,
                              StaticSpellingKind StaticSpelling,
                              SourceLoc FuncLoc,
                              DeclName Name, SourceLoc NameLoc,
                              bool Async, SourceLoc AsyncLoc,
                              bool Throws, SourceLoc ThrowsLoc,
                              GenericParamList *GenericParams,
                              DeclContext *Parent,
                              ClangNode ClangN);

  Optional<SelfAccessKind> getCachedSelfAccessKind() const {
    if (Bits.FuncDecl.SelfAccessComputed)
      return static_cast<SelfAccessKind>(Bits.FuncDecl.SelfAccess);

    return None;
  }

  Optional<bool> getCachedIsStatic() const {
    if (Bits.FuncDecl.IsStaticComputed)
      return Bits.FuncDecl.IsStatic;

    return None;
  }

public:
  /// Factory function only for use by deserialization.
  static FuncDecl *createDeserialized(ASTContext &Context,
                                      StaticSpellingKind StaticSpelling,
                                      DeclName Name, bool Async, bool Throws,
                                      GenericParamList *GenericParams,
                                      Type FnRetType, DeclContext *Parent);

  static FuncDecl *create(ASTContext &Context, SourceLoc StaticLoc,
                          StaticSpellingKind StaticSpelling, SourceLoc FuncLoc,
                          DeclName Name, SourceLoc NameLoc, bool Async,
                          SourceLoc AsyncLoc, bool Throws, SourceLoc ThrowsLoc,
                          GenericParamList *GenericParams,
                          ParameterList *BodyParams, TypeRepr *ResultTyR,
                          DeclContext *Parent);

  static FuncDecl *createImplicit(ASTContext &Context,
                                  StaticSpellingKind StaticSpelling,
                                  DeclName Name, SourceLoc NameLoc, bool Async,
                                  bool Throws, GenericParamList *GenericParams,
                                  ParameterList *BodyParams, Type FnRetType,
                                  DeclContext *Parent);

  static FuncDecl *createImported(ASTContext &Context, SourceLoc FuncLoc,
                                  DeclName Name, SourceLoc NameLoc, bool Async,
                                  bool Throws, ParameterList *BodyParams,
                                  Type FnRetType,
                                  GenericParamList *GenericParams,
                                  DeclContext *Parent, ClangNode ClangN);

  bool isStatic() const;

  /// \returns the way 'static'/'class' was spelled in the source.
  StaticSpellingKind getStaticSpelling() const {
    return static_cast<StaticSpellingKind>(Bits.FuncDecl.StaticSpelling);
  }
  /// \returns the way 'static'/'class' should be spelled for this declaration.
  StaticSpellingKind getCorrectStaticSpelling() const;
  void setStatic(bool IsStatic = true) {
    Bits.FuncDecl.IsStaticComputed = true;
    Bits.FuncDecl.IsStatic = IsStatic;
  }

  bool isMutating() const {
    return getSelfAccessKind() == SelfAccessKind::Mutating;
  }
  bool isCallAsFunctionMethod() const;

  bool isMainTypeMainMethod() const;

  SelfAccessKind getSelfAccessKind() const;

  LifetimeAnnotation getLifetimeAnnotation() const;

  void setSelfAccessKind(SelfAccessKind mod) {
    Bits.FuncDecl.SelfAccess = static_cast<unsigned>(mod);
    Bits.FuncDecl.SelfAccessComputed = true;
  }

  SourceLoc getStaticLoc() const { return StaticLoc; }
  SourceLoc getFuncLoc() const { return FuncLoc; }

  SourceLoc getStartLoc() const {
    return StaticLoc.isValid() && !isa<AccessorDecl>(this)
              ? StaticLoc : FuncLoc;
  }
  SourceRange getSourceRange() const;

  TypeRepr *getResultTypeRepr() const { return FnRetType.getTypeRepr(); }
  SourceRange getResultTypeSourceRange() const {
    return FnRetType.getSourceRange();
  }

  /// Retrieve the result interface type of this function.
  Type getResultInterfaceType() const;

  /// isUnaryOperator - Determine whether this is a unary operator
  /// implementation.  This check is a syntactic rather than type-based check,
  /// which looks at the number of parameters specified, in order to allow
  /// for the definition of unary operators on tuples, as in:
  ///
  ///   prefix func + (param : (a:Int, b:Int))
  ///
  /// This also allows the unary-operator-ness of a func decl to be determined
  /// prior to type checking.
  bool isUnaryOperator() const;
  
  /// isBinaryOperator - Determine whether this is a binary operator
  /// implementation.  This check is a syntactic rather than type-based check,
  /// which looks at the number of parameters specified, in order to allow
  /// distinguishing a binary operator from a unary operator on tuples, as in:
  ///
  ///   prefix func + (_:(a:Int, b:Int)) // unary operator +(1,2)
  ///   infix func  + (a:Int, b:Int)     // binary operator 1 + 2
  ///
  /// This also allows the binary-operator-ness of a func decl to be determined
  /// prior to type checking.
  bool isBinaryOperator() const;

  void getLocalCaptures(SmallVectorImpl<CapturedValue> &Result) const {
    return getCaptureInfo().getLocalCaptures(Result);
  }

  ParamDecl **getImplicitSelfDeclStorage();

  /// Get the supertype method this method overrides, if any.
  FuncDecl *getOverriddenDecl() const {
    return cast_or_null<FuncDecl>(AbstractFunctionDecl::getOverriddenDecl());
  }

  OperatorDecl *getOperatorDecl() const;

  /// Returns true if the function is forced to be statically dispatched.
  bool hasForcedStaticDispatch() const {
    return Bits.FuncDecl.ForcedStaticDispatch;
  }
  void setForcedStaticDispatch(bool flag) {
    Bits.FuncDecl.ForcedStaticDispatch = flag;
  }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Func ||
           D->getKind() == DeclKind::Accessor;
  }
  static bool classof(const AbstractFunctionDecl *D) {
    return classof(static_cast<const Decl*>(D));
  }
  static bool classof(const DeclContext *DC) {
    if (auto D = DC->getAsDecl())
      return classof(D);
    return false;
  }

  /// True if the function is a defer body.
  bool isDeferBody() const;

  /// Perform basic checking to determine whether the @IBAction or
  /// @IBSegueAction attribute can be applied to this function.
  bool isPotentialIBActionTarget() const;

  bool hasTopLevelLocalContextCaptures() const {
    return Bits.FuncDecl.HasTopLevelLocalContextCaptures;
  }

  void setHasTopLevelLocalContextCaptures(bool hasCaptures=true);
};

/// This represents an accessor function, such as a getter or setter.
class AccessorDecl final : public FuncDecl {
  /// Location of the accessor keyword, e.g. 'set'.
  SourceLoc AccessorKeywordLoc;

  AbstractStorageDecl *Storage;

  AccessorDecl(SourceLoc declLoc, SourceLoc accessorKeywordLoc,
               AccessorKind accessorKind, AbstractStorageDecl *storage,
               SourceLoc staticLoc, StaticSpellingKind staticSpelling,
               bool async, SourceLoc asyncLoc, bool throws, SourceLoc throwsLoc,
               bool hasImplicitSelfDecl, DeclContext *parent)
      : FuncDecl(DeclKind::Accessor, staticLoc, staticSpelling,
                 /*func loc*/ declLoc,
                 /*name*/ Identifier(), /*name loc*/ declLoc, async, asyncLoc,
                 throws, throwsLoc, hasImplicitSelfDecl,
                 /*genericParams*/ nullptr, parent),
        AccessorKeywordLoc(accessorKeywordLoc), Storage(storage) {
    assert(!async || accessorKind == AccessorKind::Get
           && "only get accessors can be async");
    Bits.AccessorDecl.AccessorKind = unsigned(accessorKind);
  }

  static AccessorDecl *
  createImpl(ASTContext &ctx, SourceLoc declLoc, SourceLoc accessorKeywordLoc,
             AccessorKind accessorKind, AbstractStorageDecl *storage,
             SourceLoc staticLoc, StaticSpellingKind staticSpelling, bool async,
             SourceLoc asyncLoc, bool throws, SourceLoc throwsLoc,
             DeclContext *parent, ClangNode clangNode);

  Optional<bool> getCachedIsTransparent() const {
    if (Bits.AccessorDecl.IsTransparentComputed)
      return Bits.AccessorDecl.IsTransparent;
    return None;
  }

  friend class IsAccessorTransparentRequest;

public:
  static AccessorDecl *createDeserialized(ASTContext &ctx,
                                          AccessorKind accessorKind,
                                          AbstractStorageDecl *storage,
                                          StaticSpellingKind staticSpelling,
                                          bool async, bool throws,
                                          Type fnRetType, DeclContext *parent);

  static AccessorDecl *
  create(ASTContext &ctx, SourceLoc declLoc, SourceLoc accessorKeywordLoc,
         AccessorKind accessorKind, AbstractStorageDecl *storage,
         SourceLoc staticLoc, StaticSpellingKind staticSpelling, bool async,
         SourceLoc asyncLoc, bool throws, SourceLoc throwsLoc,
         ParameterList *parameterList, Type fnRetType, DeclContext *parent,
         ClangNode clangNode = ClangNode());

  SourceLoc getAccessorKeywordLoc() const { return AccessorKeywordLoc; }

  AbstractStorageDecl *getStorage() const {
    return Storage;
  }

  AccessorKind getAccessorKind() const {
    return AccessorKind(Bits.AccessorDecl.AccessorKind);
  }

  bool isGetter() const { return getAccessorKind() == AccessorKind::Get; }
  bool isSetter() const { return getAccessorKind() == AccessorKind::Set; }
  bool isAnyAddressor() const {
    auto kind = getAccessorKind();
    return kind == AccessorKind::Address
        || kind == AccessorKind::MutableAddress;
  }

  /// isGetterOrSetter - Determine whether this is specifically a getter or
  /// a setter, as opposed to some other kind of accessor.
  ///
  /// For example, only getters and setters can be exposed to Objective-C.
  bool isGetterOrSetter() const { return isGetter() || isSetter(); }

  bool isObservingAccessor() const {
    switch (getAccessorKind()) {
#define OBSERVING_ACCESSOR(ID, KEYWORD) \
    case AccessorKind::ID: return true;
#define ACCESSOR(ID) \
    case AccessorKind::ID: return false;
#include "swift/AST/AccessorKinds.def"
    }
    llvm_unreachable("bad accessor kind");
  }

  bool isInitAccessor() const {
    return (getAccessorKind() == AccessorKind::Init);
  }

  /// \returns true if this is non-mutating due to applying a 'mutating'
  /// attribute. For example a "mutating set" accessor.
  bool isExplicitNonMutating() const;

  /// Is the accessor one of the kinds that's assumed nonmutating by default?
  bool isAssumedNonMutating() const;

  /// Is this accessor one of the kinds that's implicitly a coroutine?
  bool isCoroutine() const {
    switch (getAccessorKind()) {
#define COROUTINE_ACCESSOR(ID, KEYWORD) \
    case AccessorKind::ID: return true;
#define ACCESSOR(ID) \
    case AccessorKind::ID: return false;
#include "swift/AST/AccessorKinds.def"
    }
    llvm_unreachable("bad accessor kind");
  }

  bool isImplicitGetter() const {
    return isGetter() && getAccessorKeywordLoc().isInvalid();
  }

  /// Is this accessor a "simple" didSet? A "simple" didSet does not
  /// use the implicit oldValue parameter in its body or does not have
  /// an explicit parameter in its parameter list.
  bool isSimpleDidSet() const;

  void setIsTransparent(bool transparent) {
    Bits.AccessorDecl.IsTransparent = transparent;
    Bits.AccessorDecl.IsTransparentComputed = 1;
  }

  /// A representation of the name to be displayed to users. \c getNameStr
  /// for anything other than a getter or setter.
  void printUserFacingName(llvm::raw_ostream &out) const;

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Accessor;
  }
  static bool classof(const AbstractFunctionDecl *D) {
    return classof(static_cast<const Decl*>(D));
  }
  static bool classof(const DeclContext *DC) {
    if (auto D = DC->getAsDecl())
      return classof(D);
    return false;
  }
};

inline AccessorDecl *
AbstractStorageDecl::AccessorRecord::getAccessor(AccessorKind kind) const {
  if (auto optIndex = AccessorIndices[unsigned(kind)]) {
    auto accessor = getAllAccessors()[optIndex - 1];
    assert(accessor && accessor->getAccessorKind() == kind);
    return accessor;
  }
  return nullptr;
}
  
/// This represents a 'case' declaration in an 'enum', which may declare
/// one or more individual comma-separated EnumElementDecls.
class EnumCaseDecl final : public Decl,
    private llvm::TrailingObjects<EnumCaseDecl, EnumElementDecl *> {
  friend TrailingObjects;
  friend class Decl;
  SourceLoc CaseLoc;
  
  EnumCaseDecl(SourceLoc CaseLoc,
               ArrayRef<EnumElementDecl *> Elements,
               DeclContext *DC)
    : Decl(DeclKind::EnumCase, DC),
      CaseLoc(CaseLoc)
  {
    Bits.EnumCaseDecl.NumElements = Elements.size();
    std::uninitialized_copy(Elements.begin(), Elements.end(),
                            getTrailingObjects<EnumElementDecl *>());
  }
  SourceLoc getLocFromSource() const { return CaseLoc; }

public:
  static EnumCaseDecl *create(SourceLoc CaseLoc,
                              ArrayRef<EnumElementDecl*> Elements,
                              DeclContext *DC);
  
  /// Get the list of elements declared in this case.
  ArrayRef<EnumElementDecl *> getElements() const {
    return {getTrailingObjects<EnumElementDecl *>(),
            Bits.EnumCaseDecl.NumElements};
  }
  SourceRange getSourceRange() const;

  /// Returns the first of the member elements or null if there are no elements.
  /// The attributes written with an EnumCaseDecl will be attached to each of
  /// the elements instead so inspecting the attributes of the first element is
  /// often useful.
  EnumElementDecl *getFirstElement() const {
    auto elements = getElements();
    return elements.empty() ? nullptr : elements.front();
  }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::EnumCase;
  }
};

/// This represents a single case of an 'enum' declaration.
///
/// For example, the X, Y, and Z in this enum:
///
/// \code
///   enum V {
///     case X(Int), Y(Int)
///     case Z
///   }
/// \endcode
///
/// The type of an EnumElementDecl is always the EnumType for the containing
/// enum. EnumElementDecls are represented in the AST as members of their
/// parent EnumDecl, although syntactically they are subordinate to the
/// EnumCaseDecl.
class EnumElementDecl : public DeclContext, public ValueDecl {
  friend class EnumRawValuesRequest;
  
  /// This is the type specified with the enum element, for
  /// example 'Int' in 'case Y(Int)'.  This is null if there is no type
  /// associated with this element, as in 'case Z' or in all elements of enum
  /// definitions.
  ParameterList *Params;
  
  SourceLoc EqualsLoc;
  
  /// The raw value literal for the enum element, or null.
  LiteralExpr *RawValueExpr;

public:
  EnumElementDecl(SourceLoc IdentifierLoc, DeclName Name,
                  ParameterList *Params,
                  SourceLoc EqualsLoc,
                  LiteralExpr *RawValueExpr,
                  DeclContext *DC);

  /// Returns the string for the base name, or "_" if this is unnamed.
  StringRef getNameStr() const {
    assert(!getName().isSpecial() && "Cannot get string for special names");
    return hasName() ? getBaseIdentifier().str() : "_";
  }

  Type getArgumentInterfaceType() const;

  void setParameterList(ParameterList *params);
  ParameterList *getParameterList() const { return Params; }

  /// Retrieves a fully typechecked raw value expression associated
  /// with this enum element, if it exists.
  LiteralExpr *getRawValueExpr() const;
  
  /// Retrieves a "structurally" checked raw value expression associated
  /// with this enum element, if it exists.
  ///
  /// The structural raw value may or may not have a type set, but it is
  /// guaranteed to be suitable for retrieving any non-semantic information
  /// like digit text for an integral raw value or user text for a string raw value.
  LiteralExpr *getStructuralRawValueExpr() const;
  
  /// Reset the raw value expression.
  void setRawValueExpr(LiteralExpr *e);

  /// Return the containing EnumDecl.
  EnumDecl *getParentEnum() const {
    return cast<EnumDecl>(getDeclContext());
  }
  
  /// Return the containing EnumCaseDecl.
  EnumCaseDecl *getParentCase() const;

  SourceLoc getStartLoc() const {
    return getNameLoc();
  }
  SourceRange getSourceRange() const;
  
  bool hasAssociatedValues() const {
    return getParameterList() != nullptr;
  }

  /// True if the case is marked 'indirect'.
  bool isIndirect() const {
    return getAttrs().hasAttribute<IndirectAttr>();
  }

  /// Do not call this!
  /// It exists to let the AST walkers get the raw value without forcing a request.
  LiteralExpr *getRawValueUnchecked() const { return RawValueExpr; }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::EnumElement;
  }

  static bool classof(const DeclContext *DC) {
    if (auto D = DC->getAsDecl())
      return classof(D);
    return false;
  }

  using DeclContext::operator new;
  using DeclContext::operator delete;
  using Decl::getASTContext;
};
  
inline SourceRange EnumCaseDecl::getSourceRange() const {
  auto subRange = getElements().back()->getSourceRange();
  if (subRange.isValid())
    return {CaseLoc, subRange.End};
  return {};
}

/// Describes the kind of initializer.
enum class CtorInitializerKind {
  /// A designated initializer is an initializer responsible for initializing
  /// the stored properties of the current class and chaining to a superclass's
  /// designated initializer (for non-root classes).
  ///
  /// Designated initializers are never inherited.
  Designated,

  /// A convenience initializer is an initializer that initializes a complete
  /// object by delegating to another initializer (eventually reaching a
  /// designated initializer).
  ///
  /// Convenience initializers are inherited into subclasses that override
  /// all of their superclass's designated initializers.
  Convenience,

  /// A convenience factory initializer is a convenience initializer introduced
  /// by an imported Objective-C factory method.
  ///
  /// Convenience factory initializers cannot be expressed directly in
  /// Swift; rather, they are produced by the Clang importer when importing
  /// an instancetype factory method from Objective-C.
  ConvenienceFactory,

  /// A factory initializer is an initializer that is neither designated nor
  /// convenience: it can be used to create an object of the given type, but
  /// cannot be chained to via "super.init" nor is it inherited.
  ///
  /// A factory initializer is written with a return type of the class name
  /// itself. FIXME: However, this is only a presentation form, and at present
  /// the only factory initializers are produced by importing an Objective-C
  /// factory method that does not return instancetype.
  ///
  /// FIXME: Arguably, structs and enums only have factory initializers, and
  /// using designated initializers for them is a misnomer.
  Factory
};

/// Specifies the kind of initialization call performed within the body
/// of the constructor, e.g., self.init or super.init.
enum class BodyInitKind {
  /// There are no calls to self.init or super.init.
  None,
  /// There is a call to self.init, which delegates to another (peer)
  /// initializer.
  Delegating,
  /// There is a call to super.init, which chains to a superclass initializer.
  Chained,
  /// There are no calls to self.init or super.init explicitly in the body of
  /// the constructor, but a 'super.init' call will be implicitly added
  /// by semantic analysis.
  ImplicitChained
};

struct BodyInitKindAndExpr {
  BodyInitKind initKind;
  ApplyExpr *initExpr;

  BodyInitKindAndExpr() : initKind(BodyInitKind::None), initExpr(nullptr) {}

  BodyInitKindAndExpr(BodyInitKind initKind, ApplyExpr *initExpr)
      : initKind(initKind), initExpr(initExpr) {}

  friend bool operator==(BodyInitKindAndExpr lhs, BodyInitKindAndExpr rhs) {
    return (lhs.initKind == rhs.initKind &&
            lhs.initExpr == rhs.initExpr);
  }
};

/// ConstructorDecl - Declares a constructor for a type.  For example:
///
/// \code
/// struct X {
///   var x : Int
///   init(i : Int) {
///      x = i
///   }
/// }
/// \endcode
class ConstructorDecl : public AbstractFunctionDecl {
  /// The location of the '!' or '?' for a failable initializer.
  SourceLoc FailabilityLoc;

  ParamDecl *SelfDecl;

  /// The interface type of the initializing constructor.
  Type InitializerInterfaceType;

  /// The typechecked call to super.init expression, which needs to be
  /// inserted at the end of the initializer by SILGen.
  Expr *CallToSuperInit = nullptr;

public:
  ConstructorDecl(DeclName Name, SourceLoc ConstructorLoc, 
                  bool Failable, SourceLoc FailabilityLoc,
                  bool Async, SourceLoc AsyncLoc,
                  bool Throws, SourceLoc ThrowsLoc,
                  ParameterList *BodyParams,
                  GenericParamList *GenericParams, 
                  DeclContext *Parent);

  static ConstructorDecl *
  createImported(ASTContext &ctx, ClangNode clangNode, DeclName name,
                 SourceLoc constructorLoc, 
                 bool failable, SourceLoc failabilityLoc, 
                 bool async, SourceLoc asyncLoc,
                 bool throws, SourceLoc throwsLoc,
                 ParameterList *bodyParams, GenericParamList *genericParams,
                 DeclContext *parent);

  SourceLoc getConstructorLoc() const { return getNameLoc(); }
  SourceLoc getStartLoc() const { return getConstructorLoc(); }
  SourceRange getSourceRange() const;

  /// Get the interface type of the constructed object.
  Type getResultInterfaceType() const;

  /// Get the interface type of the initializing constructor.
  Type getInitializerInterfaceType();

  /// Get the typechecked call to super.init expression, which needs to be
  /// inserted at the end of the initializer by SILGen.
  Expr *getSuperInitCall() { return CallToSuperInit; }
  void setSuperInitCall(Expr *CallExpr) { CallToSuperInit = CallExpr; }

  ParamDecl **getImplicitSelfDeclStorage() { return &SelfDecl; }

  /// Determine whether the body of this constructor contains any delegating
  /// or superclass initializations (\c self.init or \c super.init,
  /// respectively) within its body.
  BodyInitKindAndExpr getDelegatingOrChainedInitKind() const;
  void clearCachedDelegatingOrChainedInitKind();

  /// Whether this constructor is required.
  bool isRequired() const {
    return getAttrs().hasAttribute<RequiredAttr>();
  }

  /// Determine the kind of initializer this is.
  CtorInitializerKind getInitKind() const;

  /// Whether this is a designated initializer.
  bool isDesignatedInit() const {
    return getInitKind() == CtorInitializerKind::Designated;
  }

  /// Whether this is a convenience initializer.
  bool isConvenienceInit() const {
    return getInitKind() == CtorInitializerKind::Convenience ||
           getInitKind() == CtorInitializerKind::ConvenienceFactory;
  }

  /// Whether this is a factory initializer.
  bool isFactoryInit() const {
    switch (getInitKind()) {
    case CtorInitializerKind::Designated:
    case CtorInitializerKind::Convenience:
      return false;
        
    case CtorInitializerKind::Factory:
    case CtorInitializerKind::ConvenienceFactory:
      return true;
    }
    llvm_unreachable("bad CtorInitializerKind");
  }

  /// Determine whether this initializer is inheritable.
  bool isInheritable() const {
    switch (getInitKind()) {
    case CtorInitializerKind::Designated:
    case CtorInitializerKind::Factory:
      return false;

    case CtorInitializerKind::Convenience:
    case CtorInitializerKind::ConvenienceFactory:
      return true;
    }
    llvm_unreachable("bad CtorInitializerKind");
  }

  /// Determine if this is a failable initializer.
  bool isFailable() const {
    return Bits.ConstructorDecl.Failable;
  }

  /// Retrieve the location of the '!' or '?' in a failable initializer.
  SourceLoc getFailabilityLoc() const { return FailabilityLoc; }

  /// Whether the implementation of this method is a stub that traps at runtime.
  bool hasStubImplementation() const {
    return Bits.ConstructorDecl.HasStubImplementation;
  }

  /// Set whether the implementation of this method is a stub that
  /// traps at runtime.
  void setStubImplementation(bool stub) {
    Bits.ConstructorDecl.HasStubImplementation = stub;
  }

  ConstructorDecl *getOverriddenDecl() const {
    return cast_or_null<ConstructorDecl>(
        AbstractFunctionDecl::getOverriddenDecl());
  }

  /// Determine whether this initializer falls into the special case for
  /// Objective-C initializers with selectors longer than "init", e.g.,
  /// \c initForMemory.
  ///
  /// In such cases, one can write the Swift initializer
  /// with a single parameter of type '()', e.g,
  ///
  /// \code
  /// @objc init(forMemory: ())
  /// \endcode
  bool isObjCZeroParameterWithLongSelector() const;

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Constructor;
  }
  static bool classof(const AbstractFunctionDecl *D) {
    return classof(static_cast<const Decl*>(D));
  }
  static bool classof(const DeclContext *DC) {
    if (auto D = DC->getAsDecl())
      return classof(D);
    return false;
  }
};

/// DestructorDecl - Declares a destructor for a type.  For example:
///
/// \code
/// struct X {
///   var fd : Int
///   deinit {
///      close(fd)
///   }
/// }
/// \endcode
class DestructorDecl : public AbstractFunctionDecl {
  ParamDecl *SelfDecl;

public:
  DestructorDecl(SourceLoc DestructorLoc, DeclContext *Parent);

  ParamDecl **getImplicitSelfDeclStorage() { return &SelfDecl; }

  SourceLoc getDestructorLoc() const { return getNameLoc(); }
  SourceLoc getStartLoc() const { return getDestructorLoc(); }
  SourceRange getSourceRange() const;

  /// Retrieve the Objective-C selector for destructors.
  ObjCSelector getObjCSelector() const;

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Destructor;
  }
  static bool classof(const AbstractFunctionDecl *D) {
    return classof(static_cast<const Decl*>(D));
  }
  static bool classof(const DeclContext *DC) {
    if (auto D = DC->getAsDecl())
      return classof(D);
    return false;
  }
};

/// Declares a precedence group.  For example:
///
/// \code
/// precedencegroup MultiplicativePrecedence {
///   associativity: right
///   higherThan: AdditivePrecedence
/// }
/// \endcode
class PrecedenceGroupDecl : public Decl {
public:
  struct Relation {
    SourceLoc NameLoc;
    Identifier Name;
    PrecedenceGroupDecl *Group;
  };

private:
  SourceLoc PrecedenceGroupLoc, NameLoc, LBraceLoc, RBraceLoc;
  SourceLoc AssociativityKeywordLoc, AssociativityValueLoc;
  SourceLoc AssignmentKeywordLoc, AssignmentValueLoc;
  SourceLoc HigherThanLoc, LowerThanLoc;
  Identifier Name;
  unsigned NumHigherThan, NumLowerThan;
  // Tail-allocated array of Relations

  Relation *getHigherThanBuffer() {
    return reinterpret_cast<Relation*>(this + 1);
  }
  const Relation *getHigherThanBuffer() const {
    return reinterpret_cast<const Relation*>(this + 1);
  }
  Relation *getLowerThanBuffer() {
    return getHigherThanBuffer() + NumHigherThan;
  }
  const Relation *getLowerThanBuffer() const {
    return getHigherThanBuffer() + NumHigherThan;
  }

  PrecedenceGroupDecl(DeclContext *DC,
                      SourceLoc precedenceGroupLoc,
                      SourceLoc nameLoc, Identifier name,
                      SourceLoc lbraceLoc,
                      SourceLoc associativityKeywordLoc,
                      SourceLoc associativityValueLoc,
                      Associativity associativity,
                      SourceLoc assignmentKeywordLoc,
                      SourceLoc assignmentValueLoc,
                      bool isAssignment,
                      SourceLoc higherThanLoc, ArrayRef<Relation> higherThan,
                      SourceLoc lowerThanLoc, ArrayRef<Relation> lowerThan,
                      SourceLoc rbraceLoc);
  friend class Decl;
  SourceLoc getLocFromSource() const { return NameLoc; }
public:
  static PrecedenceGroupDecl *create(DeclContext *dc,
                                     SourceLoc precedenceGroupLoc,
                                     SourceLoc nameLoc,
                                     Identifier name,
                                     SourceLoc lbraceLoc,
                                     SourceLoc associativityKeywordLoc,
                                     SourceLoc associativityValueLoc,
                                     Associativity associativity,
                                     SourceLoc assignmentKeywordLoc,
                                     SourceLoc assignmentValueLoc,
                                     bool isAssignment,
                                     SourceLoc higherThanLoc,
                                     ArrayRef<Relation> higherThan,
                                     SourceLoc lowerThanLoc,
                                     ArrayRef<Relation> lowerThan,
                                     SourceLoc rbraceLoc);


  SourceRange getSourceRange() const {
    return { PrecedenceGroupLoc, RBraceLoc };
  }

  /// Return the location of 'precedencegroup' in:
  ///   precedencegroup MultiplicativePrecedence { ... }
  SourceLoc getPrecedenceGroupLoc() const { return PrecedenceGroupLoc; }

  /// Return the location of 'MultiplicativePrecedence' in:
  ///   precedencegroup MultiplicativePrecedence { ... }
  SourceLoc getNameLoc() const {
    return NameLoc;
  }

  Identifier getName() const {
    return Name;
  }

  // This is needed to allow templated code to work with both ValueDecls and
  // PrecedenceGroupDecls.
  DeclBaseName getBaseName() const { return Name; }

  SourceLoc getLBraceLoc() const { return LBraceLoc; }
  SourceLoc getRBraceLoc() const { return RBraceLoc; }

  bool isAssociativityImplicit() const {
    return AssociativityKeywordLoc.isInvalid();
  }

  /// Return the location of 'associativity' in:
  ///   associativity: left
  SourceLoc getAssociativityKeywordLoc() const {
    return AssociativityKeywordLoc;
  }

  /// Return the location of 'right' in:
  ///   associativity: right
  SourceLoc getAssociativityValueLoc() const {
    return AssociativityValueLoc;
  }

  Associativity getAssociativity() const {
    return Associativity(Bits.PrecedenceGroupDecl.Associativity);
  }
  bool isLeftAssociative() const {
    return getAssociativity() == Associativity::Left;
  }
  bool isRightAssociative() const {
    return getAssociativity() == Associativity::Right;
  }
  bool isNonAssociative() const {
    return getAssociativity() == Associativity::None;
  }

  bool isAssignmentImplicit() const {
    return AssignmentKeywordLoc.isInvalid();
  }

  /// Return the location of 'assignment' in:
  ///   assignment: true
  SourceLoc getAssignmentKeywordLoc() const {
    return AssignmentKeywordLoc;
  }

  /// Return the location of 'assignment' in:
  ///   assignment: true
  SourceLoc getAssignmentValueLoc() const {
    return AssignmentValueLoc;
  }

  bool isAssignment() const {
    return Bits.PrecedenceGroupDecl.IsAssignment;
  }

  bool isHigherThanImplicit() const {
    return HigherThanLoc.isInvalid();
  }

  /// Return the location of 'higherThan' in:
  ///   higherThan: AdditivePrecedence
  SourceLoc getHigherThanLoc() const {
    return HigherThanLoc;
  }

  /// Retrieve the array of \c Relation objects containing those precedence
  /// groups with higher precedence than this precedence group.
  ///
  /// The elements of this array may be invalid, in which case they will have
  /// null \c PrecedenceGroupDecl elements.
  ArrayRef<Relation> getHigherThan() const {
    return { getHigherThanBuffer(), NumHigherThan };
  }
  MutableArrayRef<Relation> getMutableHigherThan() {
    return { getHigherThanBuffer(), NumHigherThan };
  }

  bool isLowerThanImplicit() const {
    return LowerThanLoc.isInvalid();
  }

  /// Return the location of 'lowerThan' in:
  ///   lowerThan: MultiplicativePrecedence
  SourceLoc getLowerThanLoc() const {
    return LowerThanLoc;
  }

  /// Retrieve the array of \c Relation objects containing those precedence
  /// groups with lower precedence than this precedence group.
  ///
  /// The elements of this array may be invalid, in which case they will have
  /// null \c PrecedenceGroupDecl elements.
  ArrayRef<Relation> getLowerThan() const {
    return { getLowerThanBuffer(), NumLowerThan };
  }
  MutableArrayRef<Relation> getMutableLowerThan() {
    return { getLowerThanBuffer(), NumLowerThan };
  }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::PrecedenceGroup;
  }
};

/// The fixity of an OperatorDecl.
enum class OperatorFixity : uint8_t {
  Infix,
  Prefix,
  Postfix
};

inline void simple_display(llvm::raw_ostream &out, OperatorFixity fixity) {
  switch (fixity) {
  case OperatorFixity::Infix:
    out << "infix";
    return;
  case OperatorFixity::Prefix:
    out << "prefix";
    return;
  case OperatorFixity::Postfix:
    out << "postfix";
    return;
  }
  llvm_unreachable("Unhandled case in switch");
}

/// Abstract base class of operator declarations.
class OperatorDecl : public Decl {
  SourceLoc OperatorLoc, NameLoc;
  
  Identifier name;

  SourceLoc getLocFromSource() const { return NameLoc; }
  friend class Decl;
public:
  OperatorDecl(DeclKind kind, DeclContext *DC, SourceLoc OperatorLoc,
               Identifier Name, SourceLoc NameLoc)
      : Decl(kind, DC), OperatorLoc(OperatorLoc), NameLoc(NameLoc), name(Name)
  {}

  /// Retrieve the operator's fixity, corresponding to the concrete subclass
  /// of the OperatorDecl.
  OperatorFixity getFixity() const {
    switch (getKind()) {
#define DECL(Id, Name) case DeclKind::Id: llvm_unreachable("Not an operator!");
#define OPERATOR_DECL(Id, Name)
#include "swift/AST/DeclNodes.def"
    case DeclKind::InfixOperator:
      return OperatorFixity::Infix;
    case DeclKind::PrefixOperator:
      return OperatorFixity::Prefix;
    case DeclKind::PostfixOperator:
      return OperatorFixity::Postfix;
    }
    llvm_unreachable("invalid decl kind");
  }

  SourceLoc getOperatorLoc() const { return OperatorLoc; }
  SourceLoc getNameLoc() const { return NameLoc; }
  Identifier getName() const { return name; }

  // This is needed to allow templated code to work with both ValueDecls and
  // OperatorDecls.
  DeclBaseName getBaseName() const { return name; }

  static bool classof(const Decl *D) {
    // Workaround: http://llvm.org/PR35906
    if (DeclKind::Last_Decl == DeclKind::Last_OperatorDecl)
      return D->getKind() >= DeclKind::First_OperatorDecl;
    return D->getKind() >= DeclKind::First_OperatorDecl
        && D->getKind() <= DeclKind::Last_OperatorDecl;
  }
};

/// Declares the behavior of an infix operator. For example:
///
/// \code
/// infix operator /+/ : AdditionPrecedence, Numeric
/// \endcode
class InfixOperatorDecl : public OperatorDecl {
  SourceLoc ColonLoc, PrecedenceGroupLoc;
  Identifier PrecedenceGroupName;

public:
  InfixOperatorDecl(DeclContext *DC, SourceLoc operatorLoc, Identifier name,
                    SourceLoc nameLoc, SourceLoc colonLoc,
                    Identifier precedenceGroupName,
                    SourceLoc precedenceGroupLoc)
      : OperatorDecl(DeclKind::InfixOperator, DC, operatorLoc, name, nameLoc),
        ColonLoc(colonLoc), PrecedenceGroupLoc(precedenceGroupLoc),
        PrecedenceGroupName(precedenceGroupName) {}

  SourceLoc getEndLoc() const {
    if (getPrecedenceGroupLoc().isValid())
      return getPrecedenceGroupLoc();

    return getNameLoc();
  }

  SourceRange getSourceRange() const {
    return { getOperatorLoc(), getEndLoc() };
  }

  SourceLoc getColonLoc() const { return ColonLoc; }

  Identifier getPrecedenceGroupName() const { return PrecedenceGroupName; }
  SourceLoc getPrecedenceGroupLoc() const { return PrecedenceGroupLoc; }
  PrecedenceGroupDecl *getPrecedenceGroup() const;

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::InfixOperator;
  }
};
  
/// Declares the behavior of a prefix operator. For example:
///
/// \code
/// prefix operator /+/ {}
/// \endcode
class PrefixOperatorDecl : public OperatorDecl {
public:
  PrefixOperatorDecl(DeclContext *DC, SourceLoc OperatorLoc, Identifier Name,
                     SourceLoc NameLoc)
      : OperatorDecl(DeclKind::PrefixOperator, DC, OperatorLoc, Name, NameLoc)
  {}

  SourceRange getSourceRange() const {
    return { getOperatorLoc(), getNameLoc() };
  }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::PrefixOperator;
  }
};
  
/// Declares the behavior of a postfix operator. For example:
///
/// \code
/// postfix operator /+/ {}
/// \endcode
class PostfixOperatorDecl : public OperatorDecl {
public:
  PostfixOperatorDecl(DeclContext *DC, SourceLoc OperatorLoc, Identifier Name,
                      SourceLoc NameLoc)
      : OperatorDecl(DeclKind::PostfixOperator, DC, OperatorLoc, Name, NameLoc)
  {}

  SourceRange getSourceRange() const {
    return { getOperatorLoc(), getNameLoc() };
  }
  
  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::PostfixOperator;
  }
};

class MacroExpansionDecl;

/// Represents a missing declaration in the source code.
///
/// This is used for parser recovery, e.g. when parsing a floating
/// attribute list, and to represent placeholders for unexpanded
/// declarations generated by macros.
class MissingDecl : public Decl {
  /// If this missing decl represents an unexpanded peer generated by a macro,
  /// \c unexpandedMacro contains the macro reference and the base declaration
  /// where the macro expansion applies.
  struct {
    llvm::PointerUnion<MacroExpansionDecl *, CustomAttr *> macroRef;
    Decl *baseDecl;
  } unexpandedMacro;

  /// The location that the decl would be if it wasn't missing.
  SourceLoc Loc;

  MissingDecl(DeclContext *DC, SourceLoc loc)
      : Decl(DeclKind::Missing, DC), Loc(loc) {
    setImplicit();
  }

  friend class Decl;
  SourceLoc getLocFromSource() const { return Loc; }

public:
  static MissingDecl *create(ASTContext &ctx, DeclContext *DC, SourceLoc loc) {
    return new (ctx) MissingDecl(DC, loc);
  }

  SourceRange getSourceRange() const { return SourceRange(Loc); }

  static MissingDecl *
  forUnexpandedMacro(
      llvm::PointerUnion<MacroExpansionDecl *, CustomAttr *> macroRef,
      Decl *baseDecl) {
    auto &ctx = baseDecl->getASTContext();
    auto *dc = baseDecl->getDeclContext();
    auto *missing = new (ctx) MissingDecl(dc, SourceLoc());

    missing->unexpandedMacro.macroRef = macroRef;
    missing->unexpandedMacro.baseDecl = baseDecl;

    return missing;
  }

  using MacroExpandedDeclCallback = llvm::function_ref<void(ValueDecl *)>;
  void forEachMacroExpandedDecl(MacroExpandedDeclCallback callback);

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Missing;
  }
};

/// Represents a hole where a declaration should have been.
///
/// Among other things, these are used to keep vtable layout consistent.
class MissingMemberDecl : public Decl {
  DeclName Name;

  MissingMemberDecl(DeclContext *DC, DeclName name,
                    unsigned vtableEntries,
                    unsigned fieldOffsetVectorEntries)
      : Decl(DeclKind::MissingMember, DC), Name(name) {
    Bits.MissingMemberDecl.NumberOfVTableEntries = vtableEntries;
    assert(getNumberOfVTableEntries() == vtableEntries && "not enough bits");
    Bits.MissingMemberDecl.NumberOfFieldOffsetVectorEntries =
      fieldOffsetVectorEntries;
    assert(getNumberOfFieldOffsetVectorEntries() == fieldOffsetVectorEntries
           && "not enough bits");
    setImplicit();
  }
  friend class Decl;
  SourceLoc getLocFromSource() const {
    return SourceLoc();
  }
public:
  static MissingMemberDecl *
  create(ASTContext &ctx, DeclContext *DC, DeclName name,
         unsigned numVTableEntries, bool hasStorage) {
    assert(!numVTableEntries || isa<ProtocolDecl>(DC) || isa<ClassDecl>(DC) &&
           "Only classes and protocols have vtable/witness table entries");
    assert(!hasStorage || !isa<ProtocolDecl>(DC) &&
           "Protocols cannot have missing stored properties");

    return new (ctx) MissingMemberDecl(DC, name, numVTableEntries, hasStorage);
  }

  DeclName getName() const {
    return Name;
  }

  unsigned getNumberOfVTableEntries() const {
    return Bits.MissingMemberDecl.NumberOfVTableEntries;
  }

  unsigned getNumberOfFieldOffsetVectorEntries() const {
    return Bits.MissingMemberDecl.NumberOfFieldOffsetVectorEntries;
  }

  SourceRange getSourceRange() const {
    return SourceRange();
  }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::MissingMember;
  }
};

/// Provides a declaration of a macro.
///
/// Macros are declared within the source code with the `macro` introducer.
///
/// Macros are defined externally via conformances to the 'Macro' type
/// that is part of swift-syntax, and are introduced into the compiler via
/// various mechanisms (e.g., in-process macros provided as builtins or
/// loaded via shared library, and so on).
class MacroDecl : public GenericContext, public ValueDecl {
public:
  /// The location of the 'macro' keyword.
  SourceLoc macroLoc;

  /// The parameter list.
  ParameterList *parameterList;

  /// Where the '->' is located, if present.
  SourceLoc arrowLoc;

  /// The result type.
  TypeLoc resultType;

  /// The macro definition, which should always be a
  /// \c MacroExpansionExpr in well-formed code.
  Expr *definition;

  MacroDecl(SourceLoc macroLoc, DeclName name, SourceLoc nameLoc,
            GenericParamList *genericParams,
            ParameterList *parameterList,
            SourceLoc arrowLoc,
            TypeRepr *resultType,
            Expr *definition,
            DeclContext *parent);

  SourceRange getSourceRange() const;

  /// Retrieve the interface type produced when expanding this macro.
  Type getResultInterfaceType() const;

  /// Determine the contexts in which this macro can be applied.
  MacroRoles getMacroRoles() const;

  /// Retrieve the attribute that declared the given macro role.
  const MacroRoleAttr *getMacroRoleAttr(MacroRole role) const;

  /// Populate the \c names vector with the decl names introduced
  /// by a given role of this macro.
  void getIntroducedNames(MacroRole role, ValueDecl *attachedTo,
                          SmallVectorImpl<DeclName> &names) const;

  /// Returns a DeclName that represents arbitrary names.
  static DeclName getArbitraryName() {
    return DeclName();
  }

  /// Returns a DeclName that acts as a stand-in for all unique names that
  /// are manufactured by the macro expansion context's `makeUniqueName`.
  static DeclName getUniqueNamePlaceholder(ASTContext &ctx);

  /// Determine whether the given name is the unique-name placeholder
  /// produced by `getUniqueNamePlaceholder`.
  static bool isUniqueNamePlaceholder(DeclName name);

  /// Determine whether the given name is one of the unique names manufactured
  /// by the macro expansion context's `makeUniqueName`.
  static bool isUniqueMacroName(StringRef name);

  /// Determine whether the given name is one of the unique names manufactured
  /// by the macro expansion context's `makeUniqueName`.
  static bool isUniqueMacroName(DeclBaseName name);

  /// Retrieve the definition of this macro.
  MacroDefinition getDefinition() const;

  /// Set the definition of this macro
  void setDefinition(MacroDefinition definition);

  /// Retrieve the parameter list of this macro.
  ParameterList *getParameterList() const { return parameterList; }

  /// Retrieve the builtin macro kind for this macro, or \c None if it is a
  /// user-defined macro with no special semantics.
  Optional<BuiltinMacroKind> getBuiltinKind() const;

  static bool classof(const DeclContext *C) {
    if (auto D = C->getAsDecl())
      return classof(D);
    return false;
  }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::Macro;
  }

  using DeclContext::operator new;
  using DeclContext::operator delete;
  using Decl::getASTContext;
};

class MacroExpansionDecl : public Decl, public FreestandingMacroExpansion {

public:
  enum : unsigned { InvalidDiscriminator = 0xFFFF };

  MacroExpansionDecl(DeclContext *dc, MacroExpansionInfo *info);

  static MacroExpansionDecl *create(DeclContext *dc, SourceLoc poundLoc,
                                    DeclNameRef macro, DeclNameLoc macroLoc,
                                    SourceLoc leftAngleLoc,
                                    ArrayRef<TypeRepr *> genericArgs,
                                    SourceLoc rightAngleLoc,
                                    ArgumentList *args);

  DeclContext *getDeclContext() const { return Decl::getDeclContext(); }

  SourceRange getSourceRange() const {
    return getExpansionInfo()->getSourceRange();
  }
  SourceLoc getLocFromSource() const { return getExpansionInfo()->SigilLoc; }

  /// Enumerate the nodes produced by expanding this macro expansion.
  void forEachExpandedNode(llvm::function_ref<void(ASTNode)> callback) const;

  /// Returns a discriminator which determines this macro expansion's index
  /// in the sequence of macro expansions within the current function.
  unsigned getDiscriminator() const;

  /// Retrieve the raw discriminator, which may not have been computed yet.
  ///
  /// Only use this for queries that are checking for (e.g.) reentrancy or
  /// intentionally do not want to initiate verification.
  unsigned getRawDiscriminator() const {
    return Bits.MacroExpansionDecl.Discriminator;
  }

  void setDiscriminator(unsigned discriminator) {
    assert(getRawDiscriminator() == InvalidDiscriminator);
    assert(discriminator != InvalidDiscriminator);
    Bits.MacroExpansionDecl.Discriminator = discriminator;
  }

  static bool classof(const Decl *D) {
    return D->getKind() == DeclKind::MacroExpansion;
  }
  static bool classof(const FreestandingMacroExpansion *expansion) {
    return expansion->getFreestandingMacroKind() == FreestandingMacroKind::Decl;
  }
};

/// Find references to the given generic parameter in the type signature of the
/// given declaration using the given generic signature.
///
/// \param skipParamIndex If the value is a function or subscript declaration,
/// specifies the index of the parameter that shall be skipped.
GenericParameterReferenceInfo findGenericParameterReferences(
    const ValueDecl *value,
    CanGenericSignature sig, GenericTypeParamType *genericParam,
    bool treatNonResultCovarianceAsInvariant,
    Optional<unsigned> skipParamIndex);

inline bool AbstractStorageDecl::isSettable(const DeclContext *UseDC,
                                            const DeclRefExpr *base) const {
  if (auto vd = dyn_cast<VarDecl>(this))
    return vd->isSettable(UseDC, base);

  auto sd = cast<SubscriptDecl>(this);
  return sd->supportsMutation();
}

inline bool
AbstractStorageDecl::isSettableInSwift(const DeclContext *UseDC,
                                       const DeclRefExpr *base) const {
  // TODO: Writing to an optional storage requirement is not supported in Swift.
  if (getAttrs().hasAttribute<OptionalAttr>()) {
    return false;
  }

  return isSettable(UseDC, base);
}

inline void
AbstractStorageDecl::overwriteSetterAccess(AccessLevel accessLevel) {
  Accessors.setInt(accessLevel);
  if (auto setter = getAccessor(AccessorKind::Set))
    setter->overwriteAccess(accessLevel);
  if (auto modify = getAccessor(AccessorKind::Modify))
    modify->overwriteAccess(accessLevel);
  if (auto mutableAddressor = getAccessor(AccessorKind::MutableAddress))
    mutableAddressor->overwriteAccess(accessLevel);
}

/// Constructors and destructors always have a 'self' parameter,
/// which is stored in an instance member. Functions only have a
/// 'self' if they are declared inside of a nominal type or extension,
/// in which case we tail-allocate storage for it.
inline ParamDecl **AbstractFunctionDecl::getImplicitSelfDeclStorage() {
  switch (getKind()) {
  default: llvm_unreachable("Unknown AbstractFunctionDecl!");
  case DeclKind::Constructor:
    return cast<ConstructorDecl>(this)->getImplicitSelfDeclStorage();
  case DeclKind::Destructor:
    return cast<DestructorDecl>(this)->getImplicitSelfDeclStorage();
  case DeclKind::Func:
  case DeclKind::Accessor:
    return cast<FuncDecl>(this)->getImplicitSelfDeclStorage();
  }
}

inline ParamDecl **FuncDecl::getImplicitSelfDeclStorage() {
  if (!hasImplicitSelfDecl())
    return nullptr;

  if (!isa<AccessorDecl>(this)) {
    assert(getKind() == DeclKind::Func && "no new kinds of functions");
    return reinterpret_cast<ParamDecl **>(this+1);
  }
  return reinterpret_cast<ParamDecl **>(static_cast<AccessorDecl*>(this)+1);
}

inline DeclIterator &DeclIterator::operator++() {
  Current = Current->NextDecl;
  return *this;
}

inline bool AbstractFunctionDecl::hasForcedStaticDispatch() const {
  if (auto func = dyn_cast<FuncDecl>(this))
    return func->hasForcedStaticDispatch();
  return false;
}

inline bool ValueDecl::isStatic() const {
  // Currently, only storage and function decls can be static/class.
  if (auto storage = dyn_cast<AbstractStorageDecl>(this))
    return storage->isStatic();
  if (auto func = dyn_cast<FuncDecl>(this))
    return func->isStatic();
  return false;
}

inline bool ValueDecl::isImportAsMember() const {
  if (auto func = dyn_cast<AbstractFunctionDecl>(this))
    return func->isImportAsMember();
  return false;
}

inline bool ValueDecl::hasCurriedSelf() const {
  if (auto *afd = dyn_cast<AbstractFunctionDecl>(this))
    return afd->hasImplicitSelfDecl();
  if (isa<EnumElementDecl>(this))
    return true;
  return false;
}

inline bool ValueDecl::hasParameterList() const {
  if (auto *eed = dyn_cast<EnumElementDecl>(this))
    return eed->hasAssociatedValues();
  if (auto *macro = dyn_cast<MacroDecl>(this))
    return macro->parameterList != nullptr;
  return isa<AbstractFunctionDecl>(this) || isa<SubscriptDecl>(this);
}

inline unsigned ValueDecl::getNumCurryLevels() const {
  unsigned curryLevels = 0;
  if (hasParameterList())
    curryLevels++;
  if (hasCurriedSelf())
    curryLevels++;
  return curryLevels;
}

inline bool Decl::isSyntacticallyOverridable() const {
  if (isa<VarDecl>(this) ||
      isa<SubscriptDecl>(this) ||
      isa<FuncDecl>(this) ||
      isa<DestructorDecl>(this)) {
    if (static_cast<const ValueDecl*>(this)->isFinal()) {
      return false;
    }
    auto classDecl = getDeclContext()->getSelfClassDecl();
    return classDecl && !classDecl->isActor() && !classDecl->isFinal();
  } else {
    return false;
  }
}

inline GenericParamKey::GenericParamKey(const GenericTypeParamDecl *d)
    : ParameterPack(d->isParameterPack()), Depth(d->getDepth()),
      Index(d->getIndex()) {}

inline const GenericContext *Decl::getAsGenericContext() const {
  switch (getKind()) {
  default: return nullptr;
#define DECL(Id, Parent) // See previous line
#define GENERIC_DECL(Id, Parent) \
  case DeclKind::Id: \
    return static_cast<const Id##Decl*>(this);
#include "swift/AST/DeclNodes.def"
  }
}

inline bool DeclContext::classof(const Decl *D) {
  switch (D->getKind()) { //
  default: return false;
#define DECL(ID, PARENT) // See previous line
#define CONTEXT_DECL(ID, PARENT) \
  case DeclKind::ID: return true;
#include "swift/AST/DeclNodes.def"
  }
}

inline DeclContext *DeclContext::castDeclToDeclContext(const Decl *D) {
  // XXX -- ModuleDecl is not defined in Decl.h, but because DeclContexts
  // preface decls in memory, any DeclContext type will due.
  const DeclContext *DC = static_cast<const ExtensionDecl*>(D);
  switch (D->getKind()) {
  default: llvm_unreachable("Not a DeclContext");
#define DECL(ID, PARENT) // See previous line
#define CONTEXT_DECL(ID, PARENT) \
  case DeclKind::ID:
#include "swift/AST/DeclNodes.def"
    return const_cast<DeclContext *>(DC);
  }
}

inline EnumElementDecl *EnumDecl::getUniqueElement(bool hasValue) const {
  EnumElementDecl *result = nullptr;
  bool found = false;
  for (auto elt : getAllElements()) {
    if (elt->hasAssociatedValues() == hasValue) {
      if (found)
        return nullptr;
      found = true;
      result = elt;
    }
  }
  return result;
}

/// Retrieve the parameter list for a given declaration, or nullptr if there
/// is none.
ParameterList *getParameterList(ValueDecl *source);

/// Retrieve the parameter list for a given declaration context, or nullptr if
/// there is none.
ParameterList *getParameterList(DeclContext *source);

/// Retrieve parameter declaration from the given source at given index, or
/// nullptr if the source does not have a parameter list.
const ParamDecl *getParameterAt(ConcreteDeclRef declRef, unsigned index);

/// Retrieve parameter declaration from the given source at given index, or
/// nullptr if the source does not have a parameter list.
const ParamDecl *getParameterAt(const ValueDecl *source, unsigned index);

/// Retrieve parameter declaration from the given source at given index, or
/// nullptr if the source does not have a parameter list.
const ParamDecl *getParameterAt(const DeclContext *source, unsigned index);

void simple_display(llvm::raw_ostream &out,
                    OptionSet<NominalTypeDecl::LookupDirectFlags> options);

/// Display Decl subclasses.
void simple_display(llvm::raw_ostream &out, const Decl *decl);

/// Display ValueDecl subclasses.
void simple_display(llvm::raw_ostream &out, const ValueDecl *decl);

/// Display ExtensionDecls.
inline void simple_display(llvm::raw_ostream &out, const ExtensionDecl *decl) {
  simple_display(out, static_cast<const Decl *>(decl));
}

/// Display NominalTypeDecls.
inline void simple_display(llvm::raw_ostream &out,
                           const NominalTypeDecl *decl) {
  simple_display(out, static_cast<const Decl *>(decl));
}

inline void simple_display(llvm::raw_ostream &out,
                           const AssociatedTypeDecl *decl) {
  simple_display(out, static_cast<const Decl *>(decl));
}

/// Display GenericContext.
///
/// The template keeps this sorted down in the overload set relative to the
/// more concrete overloads with Decl pointers thereby breaking a potential ambiguity.
template <typename T>
inline typename std::enable_if<std::is_same<T, GenericContext>::value>::type
simple_display(llvm::raw_ostream &out, const T *GC) {
  simple_display(out, GC->getAsDecl());
}

/// Display GenericParamList.
void simple_display(llvm::raw_ostream &out, const GenericParamList *GPL);

/// Extract the source location from the given declaration.
SourceLoc extractNearestSourceLoc(const Decl *decl);

/// Extract the source location from the given declaration.
inline SourceLoc extractNearestSourceLoc(const ExtensionDecl *ext) {
  return extractNearestSourceLoc(static_cast<const Decl *>(ext));
}

/// Extract the source location from the given declaration.
inline SourceLoc extractNearestSourceLoc(const GenericTypeDecl *type) {
  return extractNearestSourceLoc(static_cast<const Decl *>(type));
}

/// Extract the source location from the given declaration.
inline SourceLoc extractNearestSourceLoc(const NominalTypeDecl *type) {
  return extractNearestSourceLoc(static_cast<const Decl *>(type));
}

/// Extract the source location from the given declaration.
inline SourceLoc extractNearestSourceLoc(const AbstractFunctionDecl *func) {
  return extractNearestSourceLoc(static_cast<const Decl *>(func));
}

} // end namespace swift

#endif
