//===--- DeclContext.h - Swift Language Context ASTs ------------*- C++ -*-===//
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
// This file defines the DeclContext class.  A DeclContext is the semantic
// construct that a declaration belongs to, such as the enclosing
// AbstractClosureExpr or declaration.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_DECLCONTEXT_H
#define SWIFT_DECLCONTEXT_H

#include "swift/AST/ASTAllocated.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/LookupKinds.h"
#include "swift/AST/ResilienceExpansion.h"
#include "swift/AST/TypeAlignments.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/Fingerprint.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/SourceLoc.h"
#include "llvm/ADT/PointerEmbeddedInt.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/Support/raw_ostream.h"

#include <type_traits>

namespace llvm {
  class raw_ostream;
}

namespace swift {
  class AbstractFunctionDecl;
  class GenericEnvironment;
  class ASTContext;
  class ASTWalker;
  class CanType;
  class Decl;
  class DeclContext;
  class EnumDecl;
  class ExtensionDecl;
  class Expr;
  class GenericParamList;
  class LazyMemberLoader;
  class GenericContext;
  class GenericSignature;
  class GenericTypeParamDecl;
  class GenericTypeParamType;
  class InfixOperatorDecl;
  class InfixOperatorLookupResult;
  enum class PlatformKind: uint8_t;
  class PrecedenceGroupDecl;
  class ProtocolDecl;
  class Requirement;
  class SourceFile;
  class Type;
  class ModuleDecl;
  class PackageUnit;
  class GenericTypeDecl;
  class NominalTypeDecl;
  class PrecedenceGroupLookupResult;
  class PostfixOperatorDecl;
  class PrefixOperatorDecl;
  class ProtocolConformance;
  class ValueDecl;
  class VarDecl;
  class Initializer;
  class ClassDecl;
  class SerializedAbstractClosureExpr;
  class SerializedPatternBindingInitializer;
  class SerializedDefaultArgumentInitializer;
  class SerializedTopLevelCodeDecl;
  class StructDecl;

namespace serialization {
using DeclID = llvm::PointerEmbeddedInt<unsigned, 31>;
}

enum class DeclContextKind : unsigned {
  AbstractClosureExpr,
  Initializer,
  TopLevelCodeDecl,
  SubscriptDecl,
  EnumElementDecl,
  AbstractFunctionDecl,
  SerializedLocal,
  MacroDecl,
  Last_LocalDeclContextKind = MacroDecl,
  Package,
  Module,
  FileUnit,
  GenericTypeDecl,
  ExtensionDecl,
  Last_DeclContextKind = ExtensionDecl
};

/// Kinds of DeclContexts after deserialization.
///
/// \see SerializedLocalDeclContext.
enum class LocalDeclContextKind : uint8_t {
  AbstractClosure,
  PatternBindingInitializer,
  DefaultArgumentInitializer,
  TopLevelCodeDecl
};

/// Describes the kind of a particular conformance.
///
/// The following code involves conformances of the three different kinds:
/// \code
/// protocol P { }
/// protocol P2 : P { }
///
/// class Super : P2 { }
/// class Sub : Super { }
/// \endcode
///
/// \c Super conforms to \c P2 via an explicit conformance,
/// specified on the class declaration itself.
///
/// \c Super conforms to \c P via an implied conformance, whose
/// origin is the explicit conformance to \c P2.
///
/// \c Sub conforms to \c P2 and \c P via inherited conformances,
/// which link back to the conformances described above.
///
/// The enumerators are ordered in terms of decreasing preference:
/// an inherited conformance is best, followed by explicit
/// conformances, then synthesized and implied conformances. Earlier
/// conformance kinds supersede later conformance kinds, possibly with a
/// diagnostic (e.g., if an inherited conformance supersedes an
/// explicit conformance).
enum class ConformanceEntryKind : unsigned {
  /// Inherited from a superclass conformance.
  Inherited,

  /// Explicitly specified.
  Explicit,

  /// Implicitly synthesized.
  Synthesized,

  /// Implied by an explicitly-specified conformance.
  Implied,
};

/// Describes the kind of conformance lookup desired.
enum class ConformanceLookupKind : unsigned {
  /// All conformances.
  All,
  /// Only the explicit conformance.
  OnlyExplicit,
  /// All conformances except for inherited ones.
  NonInherited,
  /// All conformances except structurally-derived conformances, of which
  /// Sendable is the only one.
  NonStructural,
};

/// Describes a diagnostic for a conflict between two protocol
/// conformances.
struct ConformanceDiagnostic {
  /// The protocol with conflicting conformances.
  ProtocolDecl *Protocol;

  /// The location at which the diagnostic should occur.
  SourceLoc Loc;

  /// The kind of conformance that was superseded.
  ConformanceEntryKind Kind;

  /// The explicitly-specified protocol whose conformance implied the
  /// conflicting conformance.
  ProtocolDecl *ExplicitProtocol;

  /// The declaration context that declares the existing conformance.
  DeclContext *ExistingDC;

  /// The kind of existing conformance.
  ConformanceEntryKind ExistingKind;

  /// The explicitly-specified protocol whose conformance implied the
  /// existing conflicting conformance.
  ProtocolDecl *ExistingExplicitProtocol;
};

/// Used in diagnostic %selects.
struct FragileFunctionKind {
  enum Kind : unsigned {
    Transparent,
    Inlinable,
    AlwaysEmitIntoClient,
    DefaultArgument,
    PropertyInitializer,
    BackDeploy,
    None
  };

  Kind kind = None;
  bool allowUsableFromInline = false;

  friend bool operator==(FragileFunctionKind lhs, FragileFunctionKind rhs) {
    return (lhs.kind == rhs.kind &&
            lhs.allowUsableFromInline == rhs.allowUsableFromInline);
  }

  /// Casts to `unsigned` for diagnostic %selects.
  unsigned getSelector() { return static_cast<unsigned>(kind); }
};

/// A DeclContext is an AST object which acts as a semantic container
/// for declarations.  As a policy matter, we currently define
/// contexts broadly: a lambda expression in a function is a new
/// DeclContext, but a new brace statement is not.  There's no
/// particular mandate for this, though.
///
/// Please note that DeclContext assumes that it prefaces AST type hierarchies
/// and therefore can safely access trailing memory. If you need to create a
/// macro context, please see GenericContext for how to minimize new entries in
/// the ASTHierarchy enum below.
///
/// The hierarchy between DeclContext subclasses is set in their ctors. For
/// example, FileUnit ctor takes ModuleDecl as its parent DeclContext. The
/// hierarchy from the most to least restrictive order is:
/// decl/expr (e.g. ClassDecl) -> FileUnit -> ModuleDecl -> PackageUnit -> nullptr
///
/// There's an exception, however; the parent of ModuleDecl is set nullptr, not
/// set to PackageUnit; ModuleDecl has a pointer to PackageUnit as its field,
/// and it is treated as the enclosing scope of ModuleDecl.
class alignas(1 << DeclContextAlignInBits) DeclContext
    : public ASTAllocated<DeclContext> {
  enum class ASTHierarchy : unsigned {
    Decl,
    Expr,
    FileUnit,
    Package,
    Initializer,
    SerializedLocal,
    // If you add a new AST hierarchies, then update the static_assert() below.
  };
  static_assert(unsigned(ASTHierarchy::SerializedLocal) <
                (1 << DeclContextAlignInBits),
                "ASTHierarchy exceeds bits available");

  llvm::PointerIntPair<DeclContext*, 3, ASTHierarchy> ParentAndKind;

  /// Change the parent of this context.  This should only be used
  /// very carefully.
  void setParent(DeclContext *parent) { ParentAndKind.setPointer(parent); }
  friend class Initializer; // uses setParent
  friend class AutoClosureExpr; // uses setParent
  friend class AbstractClosureExpr; // uses setParent
  
  template<class A, class B, class C>
  friend struct ::llvm::CastInfo;
  
  // See swift/AST/Decl.h
  static DeclContext *castDeclToDeclContext(const Decl *D);

  /// If this DeclContext is a GenericType declaration or an
  /// extension thereof, return the GenericTypeDecl.
  GenericTypeDecl *getSelfTypeDecl() const;

  static ASTHierarchy getASTHierarchyFromKind(DeclContextKind Kind) {
    switch (Kind) {
    case DeclContextKind::AbstractClosureExpr:
      return ASTHierarchy::Expr;
    case DeclContextKind::Initializer:
      return ASTHierarchy::Initializer;
    case DeclContextKind::SerializedLocal:
      return ASTHierarchy::SerializedLocal;
    case DeclContextKind::FileUnit:
      return ASTHierarchy::FileUnit;
    case DeclContextKind::Package:
      return ASTHierarchy::Package;
    case DeclContextKind::Module:
    case DeclContextKind::TopLevelCodeDecl:
    case DeclContextKind::AbstractFunctionDecl:
    case DeclContextKind::SubscriptDecl:
    case DeclContextKind::EnumElementDecl:
    case DeclContextKind::GenericTypeDecl:
    case DeclContextKind::ExtensionDecl:
    case DeclContextKind::MacroDecl:
      return ASTHierarchy::Decl;
    }
    llvm_unreachable("Unhandled DeclContextKind");
  }

public:
  LLVM_READONLY
  Decl *getAsDecl() {
    return ParentAndKind.getInt() == ASTHierarchy::Decl ?
      reinterpret_cast<Decl*>(this + 1) : nullptr;
  }
  const Decl *getAsDecl() const {
    return const_cast<DeclContext*>(this)->getAsDecl();
  }

  DeclContext(DeclContextKind Kind, DeclContext *Parent)
      : ParentAndKind(Parent, getASTHierarchyFromKind(Kind)) {
    // if Module kind, it may (or may not) have Package as its parent
    if (Kind != DeclContextKind::Package && Kind != DeclContextKind::Module)
      assert(Parent != nullptr && "DeclContext must have a parent context");
  }

  /// Returns the kind of context this is.
  DeclContextKind getContextKind() const;

  /// Returns whether this context asynchronous
  bool isAsyncContext() const;

  /// Returns whether this context has value semantics.
  bool hasValueSemantics() const;

  /// Returns whether this context is an extension constrained to a class type.
  bool isClassConstrainedProtocolExtension() const;

  /// Determines whether this context is itself a local scope in a
  /// code block.  A context that appears in such a scope, like a
  /// local type declaration, does not itself become a local context.
  bool isLocalContext() const {
    return getContextKind() <= DeclContextKind::Last_LocalDeclContextKind;
  }

  /// \returns true if this is a package context
  LLVM_READONLY
  bool isPackageContext() const; // see swift/AST/Module.h

  /// isModuleContext - Return true if this is a subclass of Module.
  LLVM_READONLY
  bool isModuleContext() const; // see swift/AST/Module.h

  /// \returns true if this is a context with module-wide scope, e.g. a module
  /// or a source file.
  LLVM_READONLY
  bool isModuleScopeContext() const; // see swift/AST/Module.h

  /// \returns true if this is a type context, e.g., a struct, a class, an
  /// enum, a protocol, or an extension.
  LLVM_READONLY
  bool isTypeContext() const;

  /// If this DeclContext is a NominalType declaration or an
  /// extension thereof, return the NominalTypeDecl.
  LLVM_READONLY
  NominalTypeDecl *getSelfNominalTypeDecl() const;

  /// If this DeclContext is a class, or an extension on a class, return the
  /// ClassDecl, otherwise return null.
  LLVM_READONLY
  ClassDecl *getSelfClassDecl() const;

  /// If this DeclContext is an enum, or an extension on an enum, return the
  /// EnumDecl, otherwise return null.
  LLVM_READONLY
  EnumDecl *getSelfEnumDecl() const;

  /// If this DeclContext is a struct, or an extension on a struct, return the
  /// StructDecl, otherwise return null.
  LLVM_READONLY
  StructDecl *getSelfStructDecl() const;

  /// If this DeclContext is a protocol, or an extension on a
  /// protocol, return the ProtocolDecl, otherwise return null.
  LLVM_READONLY
  ProtocolDecl *getSelfProtocolDecl() const;

  /// If this DeclContext is a protocol extension, return the extended protocol.
  LLVM_READONLY
  ProtocolDecl *getExtendedProtocolDecl() const;

  /// If this DeclContext is the initializer expression of a global or instance
  /// property, return the VarDecl, otherwise return null.
  LLVM_READONLY
  VarDecl *getNonLocalVarDecl() const;

  /// Retrieve the generic parameter 'Self' from a protocol or
  /// protocol extension.
  ///
  /// Only valid if \c getSelfProtocolDecl().
  GenericTypeParamType *getProtocolSelfType() const;

  /// Gets the type being declared by this context.
  ///
  /// - Generic types return a bound generic type using archetypes.
  /// - Non-type contexts return a null type.
  Type getDeclaredTypeInContext() const;
  
  /// Gets the type being declared by this context.
  ///
  /// - Generic types return a bound generic type using interface types.
  /// - Non-type contexts return a null type.
  Type getDeclaredInterfaceType() const;

  /// Get the type of `self` in this context.
  ///
  /// - Protocol types return the `Self` archetype.
  /// - Everything else falls back on getDeclaredTypeInContext().
  Type getSelfTypeInContext() const;

  /// Get the type of `self` in this context.
  ///
  /// - Protocol types return the `Self` interface type.
  /// - Everything else falls back on getDeclaredInterfaceType().
  Type getSelfInterfaceType() const;

  /// Visit the generic parameter list of every outer context, innermost first.
  void forEachGenericContext(
    llvm::function_ref<void (GenericParamList *)> fn) const;

  /// Returns the depth of this generic context, or in other words,
  /// the number of nested generic contexts minus one.
  ///
  /// This is (unsigned)-1 if none of the outer contexts are generic.
  unsigned getGenericContextDepth() const;

  /// Retrieve the innermost generic signature of this context or any
  /// of its parents.
  GenericSignature getGenericSignatureOfContext() const;

  /// Retrieve the innermost archetypes of this context or any
  /// of its parents.
  GenericEnvironment *getGenericEnvironmentOfContext() const;

  /// Map an interface type to a contextual type within this context.
  Type mapTypeIntoContext(Type type) const;

  /// Returns this or the first local parent context, or nullptr if it is not
  /// contained in one.
  LLVM_READONLY
  DeclContext *getLocalContext();
  const DeclContext *getLocalContext() const {
    return const_cast<DeclContext*>(this)->getLocalContext();
  }

  /// Returns the innermost method context.
  ///
  /// This routine looks through closure, initializer, and local function
  /// contexts to find the innermost method (including constructors and
  /// destructors).
  ///
  /// \returns the innermost method, or null if there is no such method.
  LLVM_READONLY
  AbstractFunctionDecl *getInnermostMethodContext();
  const AbstractFunctionDecl *getInnermostMethodContext() const {
    return const_cast<DeclContext*>(this)->getInnermostMethodContext();
  }

  /// Returns the innermost type context.
  ///
  /// This routine looks through closure, initializer, and local function
  /// contexts to find the innermost type context -- nominal type or
  /// extension.
  LLVM_READONLY
  DeclContext *getInnermostTypeContext();
  const DeclContext *getInnermostTypeContext() const {
    return const_cast<DeclContext *>(this)->getInnermostTypeContext();
  }

  /// Returns the innermost context that is a declaration.
  ///
  /// This routine looks through contexts to find the innermost
  /// declaration context that is itself a declaration.
  LLVM_READONLY
  Decl *getInnermostDeclarationDeclContext();
  const Decl *getInnermostDeclarationDeclContext() const {
    return
        const_cast<DeclContext *>(this)->getInnermostDeclarationDeclContext();
  }

  /// Returns the topmost context that is a declaration, excluding ModuleDecl.
  ///
  /// This may return itself.
  LLVM_READONLY
  Decl *getTopmostDeclarationDeclContext();
  const Decl *getTopmostDeclarationDeclContext() const {
    return const_cast<DeclContext *>(this)->getTopmostDeclarationDeclContext();
  }

  /// Returns the innermost context that is an AbstractFunctionDecl whose
  /// body has been skipped.
  LLVM_READONLY
  DeclContext *getInnermostSkippedFunctionContext();
  const DeclContext *getInnermostSkippedFunctionContext() const {
    return
        const_cast<DeclContext *>(this)->getInnermostSkippedFunctionContext();
  }

  /// Returns the semantic parent of this context.  A context has a
  /// parent if and only if it is not a module context.
  DeclContext *getParent() const {
    return ParentAndKind.getPointer();
  }

  /// Returns the semantic parent for purposes of name lookup.
  DeclContext *getParentForLookup() const;

  /// Return true if this is a child of the specified other decl context.
  bool isChildContextOf(const DeclContext *Other) const {
    if (this == Other) return false;

    for (const DeclContext *CurContext = this; CurContext;
         CurContext = CurContext->getParent())
      if (CurContext == Other)
        return true;
    return false;
  }

  /// Returns the package unit of this context.
  /// \p lookupIfNotCurrent If the current decl context is not PackageUnit, look
  /// it up via parent module
  LLVM_READONLY
  PackageUnit *getPackageContext(bool lookupIfNotCurrent = false) const;

  /// Returns the module context that contains this context.
  LLVM_READONLY
  ModuleDecl *getParentModule() const;

  /// Returns the module scope context that contains this context.
  ///
  /// This is either a \c Module or a \c FileUnit.
  LLVM_READONLY
  DeclContext *getModuleScopeContext() const;

  /// If this DeclContext is an \c \@_objcImplementation extension, returns the
  /// \c DeclContext for the Objective-C declaration it implements. Otherwise
  /// returns \c this.
  DeclContext *getImplementedObjCContext() const;

  /// Returns the source file that contains this context, or null if this
  /// is not within a source file.
  LLVM_READONLY
  SourceFile *getParentSourceFile() const;

  /// Returns the "outermost" source file that contains this context,
  /// looking through any source files for generated code, such as
  /// macro expansions.
  LLVM_READONLY
  SourceFile *getOutermostParentSourceFile() const;

  /// Determine whether this declaration context is generic, meaning that it or
  /// any of its parents have generic parameters.
  bool isGenericContext() const;

  /// Determine whether the innermost context is generic.
  bool isInnermostContextGeneric() const;

  /// Determine whether this or any parent context is a `@_specialize` extension
  /// context.
  bool isInSpecializeExtensionContext() const;

  /// Get the most optimal resilience expansion for code in this context.
  /// If the body is able to be inlined into functions in other resilience
  /// domains, this ensures that only sufficiently-conservative access patterns
  /// are used.
  ResilienceExpansion getResilienceExpansion() const;

  /// Get the fragile function kind for the code in this context, which
  /// is used for diagnostics.
  FragileFunctionKind getFragileFunctionKind() const;

  /// Returns true if this context may possibly contain members visible to
  /// AnyObject dynamic lookup.
  bool mayContainMembersAccessedByDynamicLookup() const;

  /// Extensions are only allowed at the level in a file
  /// FIXME: do this for Protocols, too someday
  bool canBeParentOfExtension() const;

  /// Look for the set of declarations with the given name within a type,
  /// its extensions and, optionally, its supertypes.
  ///
  /// This routine performs name lookup within a given type, its extensions
  /// and, optionally, its supertypes and their extensions, from the perspective
  /// of the current DeclContext. It can eliminate non-visible, hidden, and
  /// overridden declarations from the result set. It does not, however, perform
  /// any filtering based on the semantic usefulness of the results.
  ///
  /// \param type The type to look into.
  ///
  /// \param member The member to search for.
  ///
  /// \param options Options that control name lookup, based on the
  /// \c NL_* constants in \c NameLookupOptions.
  ///
  /// \param[out] decls Will be populated with the declarations found by name
  /// lookup.
  ///
  /// \returns true if anything was found.
  bool lookupQualified(Type type, DeclNameRef member,
                       SourceLoc loc, NLOptions options,
                       SmallVectorImpl<ValueDecl *> &decls) const;

  /// Look for the set of declarations with the given name within the
  /// given set of nominal type declarations.
  ///
  /// \param types The type declarations to look into.
  ///
  /// \param member The member to search for.
  ///
  /// \param options Options that control name lookup, based on the
  /// \c NL_* constants in \c NameLookupOptions.
  ///
  /// \param[out] decls Will be populated with the declarations found by name
  /// lookup.
  ///
  /// \returns true if anything was found.
  bool lookupQualified(ArrayRef<NominalTypeDecl *> types, DeclNameRef member,
                       SourceLoc loc, NLOptions options,
                       SmallVectorImpl<ValueDecl *> &decls) const;

  /// Perform qualified lookup for the given member in the given module.
  bool lookupQualified(ModuleDecl *module, DeclNameRef member,
                       SourceLoc loc, NLOptions options,
                       SmallVectorImpl<ValueDecl *> &decls) const;

  /// Look up all Objective-C methods with the given selector visible
  /// in the enclosing module.
  void lookupAllObjCMethods(
         ObjCSelector selector,
         SmallVectorImpl<AbstractFunctionDecl *> &results) const;

  /// Looks up an infix operator with a given \p name.
  ///
  /// This returns a vector of results, as it's possible to find multiple infix
  /// operators with different precedence groups.
  InfixOperatorLookupResult lookupInfixOperator(Identifier name) const;

  /// Looks up an prefix operator with a given \p name.
  ///
  /// If multiple results are found, one is chosen in a stable manner, as
  /// prefix operator decls cannot differ other than in name. If no results are
  /// found, returns \c nullptr.
  PrefixOperatorDecl *lookupPrefixOperator(Identifier name) const;

  /// Looks up an postfix operator with a given \p name.
  ///
  /// If multiple results are found, one is chosen in a stable manner, as
  /// postfix operator decls cannot differ other than in name. If no results are
  /// found, returns \c nullptr.
  PostfixOperatorDecl *lookupPostfixOperator(Identifier name) const;

  /// Looks up a precedence group with a given \p name.
  PrecedenceGroupLookupResult lookupPrecedenceGroup(Identifier name) const;

  /// Return the ASTContext for a specified DeclContext by
  /// walking up to the enclosing module and returning its ASTContext.
  LLVM_READONLY
  ASTContext &getASTContext() const;

  /// Retrieves a list of separately imported overlays which are shadowing
  /// \p declaring. If any \p overlays are returned, qualified lookups into
  /// \p declaring should be performed into \p overlays instead; since they
  /// are overlays, they will re-export \p declaring, but will also augment it
  /// with additional symbols.
  void getSeparatelyImportedOverlays(
      ModuleDecl *declaring, SmallVectorImpl<ModuleDecl *> &overlays) const;

  /// Retrieve the syntactic depth of this declaration context, i.e.,
  /// the number of non-module-scoped contexts.
  ///
  /// For an extension of a nested type, the extension is depth 1.
  unsigned getSyntacticDepth() const;

  /// Retrieve the semantic depth of this declaration context, i.e.,
  /// the number of non-module-scoped contexts.
  ///
  /// For an extension of a nested type, the depth of the nested type itself
  /// is also included.
  unsigned getSemanticDepth() const;

  /// Returns if this extension is always available on the current deployment
  /// target. Used for conformance lookup disambiguation.
  bool isAlwaysAvailableConformanceContext() const;

  /// \returns true if traversal was aborted, false otherwise.
  bool walkContext(ASTWalker &Walker);

  SWIFT_DEBUG_DUMPER(dumpContext());
  unsigned printContext(llvm::raw_ostream &OS, unsigned indent = 0,
                        bool onlyAPartialLine = false) const;
  
  // Some Decls are DeclContexts, but not all. See swift/AST/Decl.h
  static bool classof(const Decl *D);
};

/// SerializedLocalDeclContext - the base class for DeclContexts that were
/// serialized to preserve AST structure and accurate mangling after
/// deserialization.
class SerializedLocalDeclContext : public DeclContext {
private:
  unsigned LocalKind : 3;

protected:
  unsigned SpareBits : 29;

public:
  SerializedLocalDeclContext(LocalDeclContextKind LocalKind,
                             DeclContext *Parent)
    : DeclContext(DeclContextKind::SerializedLocal, Parent),
      LocalKind(static_cast<unsigned>(LocalKind)) {}

  LocalDeclContextKind getLocalDeclContextKind() const {
    return static_cast<LocalDeclContextKind>(LocalKind);
  }

  static bool classof(const DeclContext *DC) {
    return DC->getContextKind() == DeclContextKind::SerializedLocal;
  }
};

/// An iterator that walks through a list of declarations stored
/// within some iterable declaration context.
class DeclIterator {
  Decl *Current;

public: 
  typedef std::forward_iterator_tag iterator_category;
  typedef Decl *value_type;
  typedef Decl *reference;
  typedef Decl *pointer; // Non-standard but convenient
  typedef std::ptrdiff_t difference_type;

  DeclIterator(Decl *current = nullptr) : Current(current) { }

  reference operator*() const { return Current; }
  pointer operator->() const { return Current; }

  DeclIterator &operator++();

  DeclIterator operator++(int) {
    DeclIterator old = *this;
    ++*this;
    return old;
  }

  friend bool operator==(DeclIterator lhs, DeclIterator rhs) {
    return lhs.Current == rhs.Current; 
  }

  friend bool operator!=(DeclIterator lhs, DeclIterator rhs) {
    return !(lhs == rhs);
  }
};

/// The range of declarations stored within an iterable declaration
/// context.
using DeclRange = iterator_range<DeclIterator>;

/// The kind of an \c IterableDeclContext.
enum class IterableDeclContextKind : uint8_t {  
  NominalTypeDecl = 0,
  ExtensionDecl,
};

/// A declaration context that tracks the declarations it (directly)
/// owns and permits iteration over them.
///
/// Note that an iterable declaration context must inherit from both
/// \c IterableDeclContext and \c DeclContext.
class IterableDeclContext {
  /// The first declaration in this context along with a bit indicating whether
  /// the members of this context will be lazily produced.
  mutable llvm::PointerIntPair<Decl *, 1, bool> FirstDeclAndLazyMembers;

  /// The last declaration in this context, used for efficient insertion,
  /// along with the kind of iterable declaration context.
  mutable llvm::PointerIntPair<Decl *, 1, IterableDeclContextKind>
    LastDeclAndKind;

  /// The DeclID this IDC was deserialized from, if any. Used for named lazy
  /// member loading, as a key when doing lookup in this IDC.
  serialization::DeclID SerialID;

  /// Whether we have already added the parsed members into the context.
  unsigned AddedParsedMembers : 1;

  /// Whether delayed parsing detected a possible operator definition
  /// while skipping the body of this context.
  unsigned HasOperatorDeclarations : 1;

  /// Whether delayed parsing detect a possible nested class definition
  /// while skipping the body of this context.
  unsigned HasNestedClassDeclarations : 1;

  template<class A, class B, class C>
  friend struct ::llvm::CastInfo;

  static IterableDeclContext *castDeclToIterableDeclContext(const Decl *D);

  friend class LookupAllConformancesInContextRequest;

  /// Retrieve the \c ASTContext in which this iterable context occurs.
  ASTContext &getASTContext() const;

public:
  IterableDeclContext(IterableDeclContextKind kind)
    : LastDeclAndKind(nullptr, kind) {
    AddedParsedMembers = 0;
    HasOperatorDeclarations = 0;
    HasNestedClassDeclarations = 0;
  }

  /// Determine the kind of iterable context we have.
  IterableDeclContextKind getIterableContextKind() const {
    return LastDeclAndKind.getInt();
  }

  bool hasUnparsedMembers() const;

  bool maybeHasOperatorDeclarations() const {
    return HasOperatorDeclarations;
  }

  void setMaybeHasOperatorDeclarations() {
    assert(hasUnparsedMembers());
    HasOperatorDeclarations = 1;
  }

  bool maybeHasNestedClassDeclarations() const {
    return HasNestedClassDeclarations;
  }

  void setMaybeHasNestedClassDeclarations() {
    assert(hasUnparsedMembers());
    HasNestedClassDeclarations = 1;
  }

  /// Retrieve the set of members in this context.
  DeclRange getMembers() const;

  /// Get the members that were syntactically present in the source code,
  /// and will not contain any members that are implicitly synthesized by
  /// the implementation.
  ArrayRef<Decl *> getParsedMembers() const;

  /// Get all of the members within this context that can affect ABI, including
  /// any implicitly-synthesized members.
  ///
  /// The resulting list of members will be stable across translation units.
  ArrayRef<Decl *> getABIMembers() const;

  /// Get all of the members within this context, including any
  /// implicitly-synthesized members.
  ///
  /// The resulting list of members will be stable across translation units.
  ArrayRef<Decl *> getAllMembers() const;

  /// Retrieve the set of members in this context without loading any from the
  /// associated lazy loader; this should only be used as part of implementing
  /// abstractions on top of member loading, such as a name lookup table.
  DeclRange getCurrentMembersWithoutLoading() const;

  /// Return the context that contains the actual implemented members. This
  /// is \em usually just \c this, but if \c this is an imported class or
  /// category, it may be a Swift extension instead.
  IterableDeclContext *getImplementationContext();

  /// Add a member to this context.
  ///
  /// If the hint decl is specified, the new decl is inserted immediately
  /// after the hint.
  ///
  /// If insertAtHead is set, the new decl is inserted at the beginning of
  /// the list.
  ///
  /// Otherwise, it is inserted at the end.
  void addMember(Decl *member, Decl *hint = nullptr, bool insertAtHead = false);

  /// Add a member in the right place to preserve source order. This should
  /// only be called from the code completion delayed parsing path.
  void addMemberPreservingSourceOrder(Decl *member);

  /// Check whether there are lazily-loaded members.
  bool hasLazyMembers() const {
    return FirstDeclAndLazyMembers.getInt();
  }

  /// Setup the loader for lazily-loaded members.
  void setMemberLoader(LazyMemberLoader *loader, uint64_t contextData);

  /// Externally tell this context that it has no more lazy members, i.e. all lazy member loading is complete.
  void setHasLazyMembers(bool hasLazyMembers) const;

  /// Load all of the members of this context.
  void loadAllMembers() const;

  /// Determine whether this was deserialized (and thus SerialID is
  /// valid).
  bool wasDeserialized() const;

  /// Retrieve the set of protocols whose conformances will be
  /// associated with this declaration context.
  ///
  /// This function differs from \c getLocalConformances() in that it
  /// returns protocol declarations, not protocol conformances, and
  /// therefore does not require the protocol conformances to be
  /// formed.
  ///
  /// \param lookupKind The kind of lookup to perform.
  SmallVector<ProtocolDecl *, 2>
  getLocalProtocols(ConformanceLookupKind lookupKind
                      = ConformanceLookupKind::All) const;

  /// Retrieve the set of protocol conformances associated with this
  /// declaration context.
  ///
  /// \param lookupKind The kind of lookup to perform.
  SmallVector<ProtocolConformance *, 2>
  getLocalConformances(ConformanceLookupKind lookupKind
                         = ConformanceLookupKind::All) const;

  /// Retrieve diagnostics discovered while expanding conformances for this
  /// declaration context. This operation then removes those diagnostics from
  /// consideration, so subsequent calls to this function with the same
  /// declaration context that have not had any new extensions bound
  /// will see an empty array.
  SmallVector<ConformanceDiagnostic, 4> takeConformanceDiagnostics() const;

  /// Return 'this' as a \c Decl.
  const Decl *getDecl() const;

  /// Return 'this' as a \c GenericContext.
  GenericContext *getAsGenericContext();
  const GenericContext *getAsGenericContext() const {
    return const_cast<IterableDeclContext *>(this)->getAsGenericContext();
  }

  /// Get the DeclID this Decl was deserialized from.
  serialization::DeclID getDeclID() const {
    assert(wasDeserialized());
    return SerialID;
  }

  /// Set the DeclID this Decl was deserialized from.
  void setDeclID(serialization::DeclID d) {
    assert(wasDeserialized());
    SerialID = d;
  }

  // Some Decls are IterableDeclContexts, but not all.
  static bool classof(const Decl *D);

  /// Return a hash of all tokens in the body for dependency analysis, if
  /// available.
  Optional<Fingerprint> getBodyFingerprint() const;

private:
  /// Add a member to the list for iteration purposes, but do not notify the
  /// subclass that we have done so.
  void addMemberSilently(Decl *member, Decl *hint, bool insertAtHead) const;
};

/// Define simple_display for DeclContexts but not for subclasses in order to
/// avoid ambiguities with Decl* arguments.
template <typename ParamT, typename = typename std::enable_if<
                               std::is_same<ParamT, DeclContext>::value>::type>
void simple_display(llvm::raw_ostream &out, const ParamT *dc) {
  if (dc)
    dc->printContext(out, 0, true);
  else
    out << "(null)";
}

void simple_display(llvm::raw_ostream &out, const IterableDeclContext *idc);

/// Extract the source location from the given declaration context.
SourceLoc extractNearestSourceLoc(const DeclContext *dc);

/// Extract the source location from the given declaration context.
SourceLoc extractNearestSourceLoc(const IterableDeclContext *idc);

} // end namespace swift

namespace llvm {
  template<class FromTy>
  struct CastInfo<::swift::DeclContext, FromTy, std::enable_if_t<is_simple_type<FromTy>::value>>
      : public CastIsPossible<::swift::DeclContext, FromTy>,
        public DefaultDoCastIfPossible<::swift::DeclContext *, FromTy,
                                       CastInfo<::swift::DeclContext, FromTy>> {
    static inline ::swift::DeclContext *castFailed() { return nullptr; }

    static inline ::swift::DeclContext *doCast(const FromTy &val) {
      return ::swift::DeclContext::castDeclToDeclContext(val);
    }
  };

  template<class FromTy>
  struct CastInfo<::swift::IterableDeclContext, FromTy, std::enable_if_t<is_simple_type<FromTy>::value>>
      : public CastIsPossible<::swift::IterableDeclContext, FromTy>,
        public DefaultDoCastIfPossible<::swift::IterableDeclContext *, FromTy,
                                       CastInfo<::swift::IterableDeclContext, FromTy>> {
    static inline ::swift::IterableDeclContext *castFailed() { return nullptr; }

    static inline ::swift::IterableDeclContext *doCast(const FromTy &val) {
      return ::swift::IterableDeclContext::castDeclToIterableDeclContext(val);
    }
  };
}

#endif
