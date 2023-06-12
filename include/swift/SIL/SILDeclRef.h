//===--- SILDeclRef.h - Defines the SILDeclRef struct -----------*- C++ -*-===//
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
// This file defines the SILDeclRef struct, which is used to identify a SIL
// global identifier that can be used as the operand of a FunctionRefInst
// instruction or that can have a SIL Function associated with it.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_SILDeclRef_H
#define SWIFT_SIL_SILDeclRef_H

#include "swift/AST/Availability.h"
#include "swift/AST/ClangNode.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/TypeAlignments.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/Support/PrettyStackTrace.h"

namespace llvm {
  class raw_ostream;
}

namespace swift {
  enum class EffectsKind : uint8_t;
  class AbstractFunctionDecl;
  class AbstractClosureExpr;
  class AutoDiffDerivativeFunctionIdentifier;
  class ValueDecl;
  class FuncDecl;
  class ClosureExpr;
  class AutoClosureExpr;
  class ASTContext;
  class ClassDecl;
  class EffectsAttr;
  class FileUnit;
  class SILFunctionType;
  enum IsSerialized_t : unsigned char;
  enum class SubclassScope : unsigned char;
  class SILModule;
  class SILLocation;
  enum class SILLinkage : uint8_t;
  class AnyFunctionRef;
  class GenericSignature;
  class CustomAttr;

/// How a method is dispatched.
enum class MethodDispatch {
  // The method implementation can be referenced statically.
  Static,
  // The method implementation uses class_method dispatch.
  Class,
};

/// Get the method dispatch mechanism for a method.
MethodDispatch getMethodDispatch(AbstractFunctionDecl *method);

/// True if calling the given method or property should use ObjC dispatch.
bool requiresForeignEntryPoint(ValueDecl *vd);

/// True if the entry point is natively foreign.
bool requiresForeignToNativeThunk(ValueDecl *vd);

enum ForDefinition_t : bool {
  NotForDefinition = false,
  ForDefinition = true
};

/// A key for referencing a Swift declaration in SIL.
///
/// This can currently be either a reference to a ValueDecl for functions,
/// methods, constructors, and other named entities, or a reference to a
/// AbstractClosureExpr for an anonymous function.  In addition to the AST
/// reference, there are discriminators for referencing different
/// implementation-level entities associated with a single language-level
/// declaration, such as uncurry levels of a function, the allocating and
/// initializing entry points of a constructor, etc.
struct SILDeclRef {
  /// The type of AST node location being stored.
  enum LocKind {
    Decl,
    Closure,
    File
  };
  using Loc = llvm::PointerUnion<ValueDecl *, AbstractClosureExpr *,
                                 FileUnit *>;

  /// Represents the "kind" of the SILDeclRef. For some Swift decls there
  /// are multiple SIL entry points, and the kind is used to distinguish them.
  enum class Kind : unsigned {
    /// This constant references the FuncDecl or AbstractClosureExpr
    /// in loc.
    Func,

    /// Allocator - this constant references the allocating constructor
    /// entry point of a class ConstructorDecl or the constructor of a value
    /// ConstructorDecl.
    Allocator,

    /// Initializer - this constant references the initializing constructor
    /// entry point of the class ConstructorDecl in loc.
    Initializer,
    
    /// EnumElement - this constant references the injection function for
    /// an EnumElementDecl.
    EnumElement,
    
    /// Destroyer - this constant references the destroying destructor for the
    /// DestructorDecl in loc.
    Destroyer,

    /// Deallocator - this constant references the deallocating
    /// destructor for the DestructorDecl in loc.
    Deallocator,
    
    /// GlobalAccessor - this constant references the lazy-initializing
    /// accessor for the global VarDecl in loc.
    GlobalAccessor,

    /// References the generator for a default argument of a function.
    DefaultArgGenerator,

    /// References the initializer expression for a stored property
    /// of a nominal type.
    StoredPropertyInitializer,

    /// References the ivar initializer for the ClassDecl in loc.
    ///
    /// Only classes that are allocated using Objective-C's allocation
    /// routines have an ivar initializer, which is emitted as
    /// .cxx_construct.
    IVarInitializer,

    /// References the ivar destroyer for the ClassDecl in loc.
    ///
    /// Only classes that are allocated using Objective-C's allocation
    /// routines have an ivar destroyer, which is emitted as
    /// .cxx_destruct.
    IVarDestroyer,

    /// References the wrapped value injection function used to initialize
    /// the backing storage property from a wrapped value.
    PropertyWrapperBackingInitializer,

    /// References the function used to initialize a property wrapper storage
    /// instance from a projected value.
    PropertyWrapperInitFromProjectedValue,

    /// The main entry-point function. This may reference a SourceFile for a
    /// top-level main, or a decl for e.g an @main decl.
    EntryPoint,

    /// The asynchronous main entry-point function.
    AsyncEntryPoint,

    /// This constant references the generator function used to instantiate
    /// attribute value associated with a particular declaration.
    RuntimeAttributeGenerator,
  };

  /// Represents the variants of a back deployable function.
  enum class BackDeploymentKind : unsigned {
    /// Default value. If a SILDecRef references a function that has been back
    /// deployed and has this back deployment kind, then it references the
    /// original ABI stable function.
    None,
    /// The thunk variant of a function that calls either the original function
    /// or the fallback variant if the original is unavailable. This thunk will
    /// be emitted with PublicNonABI linkage.
    Thunk,
    /// The fallback variant of the function. This function will be emitted with
    /// PublicNonABI linkage.
    Fallback,
  };

  /// The AST node represented by this SILDeclRef.
  Loc loc;
  /// The Kind of this SILDeclRef.
  Kind kind : 5;
  /// True if this references a foreign entry point for the referenced decl.
  unsigned isForeign : 1;
  /// True if this references a distributed function.
  unsigned isDistributed : 1;
  /// True if this references a distributed function, but it is known to be local
  unsigned isKnownToBeLocal : 1;
  /// True is this reference to function that could be looked up via a special
  /// runtime API.
  unsigned isRuntimeAccessible : 1;
  /// The BackDeploymentKind of this SILDeclRef.
  BackDeploymentKind backDeploymentKind : 2;
  /// The default argument index for a default argument getter.
  unsigned defaultArgIndex : 10;

  PointerUnion<AutoDiffDerivativeFunctionIdentifier *,
               const GenericSignatureImpl *, CustomAttr *>
      pointer;

  /// Returns the type of AST node location being stored by the SILDeclRef.
  LocKind getLocKind() const {
    if (loc.is<ValueDecl *>())
      return LocKind::Decl;
    if (loc.is<AbstractClosureExpr *>())
      return LocKind::Closure;
    if (loc.is<FileUnit *>())
      return LocKind::File;
    llvm_unreachable("Unhandled location kind!");
  }

  /// The derivative function identifier.
  AutoDiffDerivativeFunctionIdentifier * getDerivativeFunctionIdentifier() const {
    if (!pointer.is<AutoDiffDerivativeFunctionIdentifier *>())
      return nullptr;
    return pointer.get<AutoDiffDerivativeFunctionIdentifier *>();
  }

  GenericSignature getSpecializedSignature() const {
    if (!pointer.is<const GenericSignatureImpl *>())
      return GenericSignature();
    else
      return GenericSignature(pointer.get<const GenericSignatureImpl *>());
  }

  /// Produces a null SILDeclRef.
  SILDeclRef()
      : loc(), kind(Kind::Func), isForeign(0), 
        isDistributed(0), isKnownToBeLocal(0), isRuntimeAccessible(0),
        backDeploymentKind(BackDeploymentKind::None), defaultArgIndex(0) {}

  /// Produces a SILDeclRef of the given kind for the given decl.
  explicit SILDeclRef(
      ValueDecl *decl, Kind kind,
      bool isForeign = false,
      bool isDistributed = false,
      bool isDistributedKnownToBeLocal = false,
      bool isRuntimeAccessible = false,
      BackDeploymentKind backDeploymentKind = BackDeploymentKind::None,
      AutoDiffDerivativeFunctionIdentifier *derivativeId = nullptr);

  /// Produces a SILDeclRef for the given ValueDecl or
  /// AbstractClosureExpr:
  /// - If 'loc' is a func or closure, this returns a Func SILDeclRef.
  /// - If 'loc' is a ConstructorDecl, this returns the Allocator SILDeclRef
  ///   for the constructor.
  /// - If 'loc' is an EnumElementDecl, this returns the EnumElement
  ///   SILDeclRef for the enum element.
  /// - If 'loc' is a DestructorDecl, this returns the Destructor SILDeclRef
  ///   for the containing ClassDecl.
  /// - If 'loc' is a global VarDecl, this returns its GlobalAccessor
  ///   SILDeclRef.
  explicit SILDeclRef(
      Loc loc,
      bool isForeign = false,
      bool isDistributed = false,
      bool isDistributedLocal = false);

  /// See above put produces a prespecialization according to the signature.
  explicit SILDeclRef(Loc loc, GenericSignature prespecializationSig);

  /// Produce a SIL constant for a default argument generator.
  static SILDeclRef getDefaultArgGenerator(Loc loc, unsigned defaultArgIndex);

  /// Produces a SILDeclRef for a synthetic main entry-point such as @main.
  static SILDeclRef getMainDeclEntryPoint(ValueDecl *decl);

  /// Produces a SILDeclRef for the synthesized async main entry-point
  static SILDeclRef getAsyncMainDeclEntryPoint(ValueDecl *decl);

  /// Produces a SILDeclRef for the entry-point of a main FileUnit.
  static SILDeclRef getMainFileEntryPoint(FileUnit *file);

  /// Produces a SILDeclRef for the entry-point of an async main FileUnit.
  static SILDeclRef getAsyncMainFileEntryPoint(FileUnit *file);

  static SILDeclRef getRuntimeAttributeGenerator(CustomAttr *attr,
                                                 ValueDecl *decl);

  bool isNull() const { return loc.isNull(); }
  explicit operator bool() const { return !isNull(); }

  bool hasDecl() const { return loc.is<ValueDecl *>(); }
  bool hasFileUnit() const { return loc.is<FileUnit *>(); }
  bool hasClosureExpr() const;
  bool hasAutoClosureExpr() const;
  bool hasFuncDecl() const;

  ValueDecl *getDecl() const { return loc.get<ValueDecl *>(); }
  AbstractClosureExpr *getAbstractClosureExpr() const {
    return loc.dyn_cast<AbstractClosureExpr *>();
  }
  ClosureExpr *getClosureExpr() const;
  AutoClosureExpr *getAutoClosureExpr() const;
  FuncDecl *getFuncDecl() const;
  AbstractFunctionDecl *getAbstractFunctionDecl() const;
  FileUnit *getFileUnit() const {
    return loc.get<FileUnit *>();
  }

  /// Get ModuleDecl that contains the SILDeclRef
  ModuleDecl *getModuleContext() const;

  /// Retrieves the ASTContext from the underlying AST node being stored.
  ASTContext &getASTContext() const;

  /// Retrieve the innermost declaration context corresponding to the underlying
  /// node, which will either be the node itself (if it's also a declaration
  /// context) or its parent context.
  DeclContext *getInnermostDeclContext() const;

  llvm::Optional<AnyFunctionRef> getAnyFunctionRef() const;

  SILLocation getAsRegularLocation() const;

  enum class ManglingKind {
    Default,
    DynamicThunk,
  };

  /// Produce a mangled form of this constant.
  std::string mangle(ManglingKind MKind = ManglingKind::Default) const;

  /// True if the SILDeclRef references a function.
  bool isFunc() const {
    return kind == Kind::Func;
  }

  /// True if the SILDeclRef references a setter function.
  bool isSetter() const;

  /// True if the SILDeclRef references a constructor entry point.
  bool isConstructor() const {
    return kind == Kind::Allocator || kind == Kind::Initializer;
  }
  /// True if the SILDeclRef references a destructor entry point.
  bool isDestructor() const {
    return kind == Kind::Destroyer || kind == Kind::Deallocator;
  }
  /// True if the SILDeclRef references an enum entry point.
  bool isEnumElement() const {
    return kind == Kind::EnumElement;
  }
  /// True if the SILDeclRef references a global variable accessor.
  bool isGlobal() const {
    return kind == Kind::GlobalAccessor;
  }
  /// True if the SILDeclRef references the generator for a default argument of
  /// a function.
  bool isDefaultArgGenerator() const {
    return kind == Kind::DefaultArgGenerator;
  }
  /// True if the SILDeclRef references the initializer for a stored property
  /// of a nominal type.
  bool isStoredPropertyInitializer() const {
    return kind == Kind::StoredPropertyInitializer;
  }
  /// True if the SILDeclRef references the initializer for the backing storage
  /// of a property wrapper.
  bool isPropertyWrapperBackingInitializer() const {
    return (kind == Kind::PropertyWrapperBackingInitializer ||
            kind == Kind::PropertyWrapperInitFromProjectedValue);
  }

  /// True if the SILDeclRef references the ivar initializer or deinitializer of
  /// a class.
  bool isIVarInitializerOrDestroyer() const {
    return kind == Kind::IVarInitializer || kind == Kind::IVarDestroyer;
  }

  /// True if the SILDeclRef references an allocating or deallocating entry
  /// point.
  bool isInitializerOrDestroyer() const {
    return kind == Kind::Initializer || kind == Kind::Destroyer;
  }

  /// True if the SILDeclRef references an init accessor declaration.
  bool isInitAccessor() const;

  /// True if the function should be treated as transparent.
  bool isTransparent() const;
  /// True if the function should have its body serialized.
  IsSerialized_t isSerialized() const;
  /// True if the function has noinline attribute.
  bool isNoinline() const;
  /// True if the function has __always inline attribute.
  bool isAlwaysInline() const;
  /// True if the function has the @backDeployed attribute.
  bool isBackDeployed() const;

  /// Return the expected linkage for a definition of this declaration.
  SILLinkage getDefinitionLinkage() const;

  /// Return the expected linkage of this declaration.
  SILLinkage getLinkage(ForDefinition_t forDefinition) const;

  /// Return the hash code for the SIL declaration.
  friend llvm::hash_code hash_value(const SILDeclRef &ref) {
    return llvm::hash_combine(ref.loc.getOpaqueValue(),
                              static_cast<int>(ref.kind),
                              ref.isForeign, ref.isDistributed,
                              ref.defaultArgIndex);
  }

  bool operator==(SILDeclRef rhs) const {
    return loc.getOpaqueValue() == rhs.loc.getOpaqueValue() &&
           kind == rhs.kind && isForeign == rhs.isForeign &&
           isDistributed == rhs.isDistributed &&
           backDeploymentKind == rhs.backDeploymentKind &&
           defaultArgIndex == rhs.defaultArgIndex &&
           pointer == rhs.pointer;
  }
  bool operator!=(SILDeclRef rhs) const {
    return !(*this == rhs);
  }
  
  void print(llvm::raw_ostream &os) const;
  void dump() const;

  unsigned getParameterListCount() const;

  /// Returns the foreign (or native) entry point corresponding to the same
  /// decl.
  SILDeclRef asForeign(bool foreign = true) const {
    return SILDeclRef(loc.getOpaqueValue(), kind,
                      /*foreign=*/foreign,
                      /*distributed=*/false,
                      /*knownToBeLocal=*/false,
                      /*runtimeAccessible=*/false,
                      backDeploymentKind,
                      defaultArgIndex,
                      pointer.get<AutoDiffDerivativeFunctionIdentifier *>());
  }
  /// Returns the distributed entry point corresponding to the same decl.
  SILDeclRef asDistributed(bool distributed = true) const {
    return SILDeclRef(loc.getOpaqueValue(), kind,
                      /*foreign=*/false,
                      /*distributed=*/distributed,
                      /*knownToBeLocal=*/false,
                      isRuntimeAccessible,
                      backDeploymentKind,
                      defaultArgIndex,
                      pointer.get<AutoDiffDerivativeFunctionIdentifier *>());
  }

  /// Returns the distributed known-to-be-local entry point corresponding to
  /// the same decl.
  SILDeclRef asDistributedKnownToBeLocal(bool isLocal = true) const {
    return SILDeclRef(loc.getOpaqueValue(), kind,
                      /*foreign=*/false,
                      /*distributed=*/false,
                      /*distributedKnownToBeLocal=*/isLocal,
                      isRuntimeAccessible,
                      backDeploymentKind,
                      defaultArgIndex,
                      pointer.get<AutoDiffDerivativeFunctionIdentifier *>());
  }

  /// Returns the runtime accessible entry point corresponding to the same decl.
  SILDeclRef asRuntimeAccessible(bool accessible = true) const {
    SILDeclRef newRef = *this;
    newRef.isRuntimeAccessible = accessible;
    return newRef;
  }

  /// Returns a copy of the decl with the given back deployment kind.
  SILDeclRef asBackDeploymentKind(BackDeploymentKind backDeploymentKind) const {
    return SILDeclRef(loc.getOpaqueValue(), kind,
                      isForeign,
                      isDistributed,
                      isKnownToBeLocal,
                      isRuntimeAccessible,
                      backDeploymentKind,
                      defaultArgIndex,
                      pointer.get<AutoDiffDerivativeFunctionIdentifier *>());
  }

  /// Returns the entry point for the corresponding autodiff derivative
  /// function.
  SILDeclRef asAutoDiffDerivativeFunction(
      AutoDiffDerivativeFunctionIdentifier *derivativeId) const {
    assert(derivativeId);
    SILDeclRef declRef = *this;
    declRef.pointer = derivativeId;
    return declRef;
  }

  /// Returns the entry point for the original function corresponding to an
  /// autodiff derivative function.
  SILDeclRef asAutoDiffOriginalFunction() const {
    assert(pointer.get<AutoDiffDerivativeFunctionIdentifier *>());
    SILDeclRef declRef = *this;
    declRef.pointer = (AutoDiffDerivativeFunctionIdentifier *)nullptr;
    return declRef;
  }

  /// Returns this `SILDeclRef` replacing `loc` with `decl`.
  SILDeclRef withDecl(ValueDecl *decl) const {
    SILDeclRef result = *this;
    result.loc = decl;
    return result;
  }

  /// True if the decl ref references a thunk from a natively foreign
  /// declaration to Swift calling convention.
  bool isForeignToNativeThunk() const;

  /// True if the decl ref references a thunk from a natively Swift declaration
  /// to foreign C or ObjC calling convention.
  bool isNativeToForeignThunk() const;

  /// True if the decl ref references a thunk handling potentially distributed actor functions
  bool isDistributedThunk() const;

  /// True if the decl ref references a thunk handling a call to a function that
  /// supports back deployment.
  bool isBackDeploymentThunk() const;

  /// True if the decl ref references a function that is the back deployment
  /// fallback for an original function which may be unavailable at runtime.
  bool isBackDeploymentFallback() const;

  /// True if the decl ref references a function that could be looked up
  /// at runtime using special API.
  bool isRuntimeAccessibleFunction() const;

  /// True if the decl ref references a method which introduces a new vtable
  /// entry.
  bool requiresNewVTableEntry() const;

  /// True if the decl ref references a method which introduces a new witness
  /// table entry.
  bool requiresNewWitnessTableEntry() const;

  /// Return a SILDeclRef to the declaration overridden by this one, or
  /// a null SILDeclRef if there is no override.
  SILDeclRef getOverridden() const;
  
  /// Return a SILDeclRef to the declaration whose vtable entry this declaration
  /// overrides. This may be different from "getOverridden" because some
  /// declarations do not always have vtable entries.
  SILDeclRef getNextOverriddenVTableEntry() const;

  /// Return the most derived override which requires a new vtable entry.
  /// If the method does not override anything or no override is vtable
  /// dispatched, will return the least derived method.
  SILDeclRef getOverriddenVTableEntry() const;

  /// Return the original protocol requirement that introduced the witness table
  /// entry overridden by this method.
  SILDeclRef getOverriddenWitnessTableEntry() const;

  /// Return the original protocol requirement that introduced the witness table
  /// entry overridden by this method.
  static AbstractFunctionDecl *getOverriddenWitnessTableEntry(
                                                    AbstractFunctionDecl *func);

  /// Returns the availability of the decl for computing linkage.
  Optional<AvailabilityContext> getAvailabilityForLinkage() const;

  /// True if the referenced entity is some kind of thunk.
  bool isThunk() const;

  /// True if the referenced entity is emitted by Swift on behalf of the Clang
  /// importer.
  bool isClangImported() const;

  /// True if the referenced entity is emitted by Clang on behalf of the Clang
  /// importer.
  bool isClangGenerated() const;
  static bool isClangGenerated(ClangNode node);

  bool isImplicit() const;

  /// Whether the referenced function contains user code. This is generally true
  /// for a non-implicit decls, but may also be true for implicit decls if
  /// explicitly written code has been spliced into the body. This is the case
  /// for e.g a lazy variable getter.
  bool hasUserWrittenCode() const;

  /// Return the scope in which the parent class of a method (i.e. class
  /// containing this declaration) can be subclassed, returning NotApplicable if
  /// this is not a method, there is no such class, or the class cannot be
  /// subclassed.
  SubclassScope getSubclassScope() const;

  /// For a SILDeclRef that describes a variable initializer or backing
  /// initializer, retrieves the expression that will be emitted for that
  /// initialization. Otherwise, returns \c nullptr.
  Expr *getInitializationExpr() const;

  bool isDynamicallyReplaceable() const;

  bool canBeDynamicReplacement() const;

  bool isAutoDiffDerivativeFunction() const {
    return pointer.is<AutoDiffDerivativeFunctionIdentifier *>() &&
           pointer.get<AutoDiffDerivativeFunctionIdentifier *>() != nullptr;
  }

  AutoDiffDerivativeFunctionIdentifier *
  getAutoDiffDerivativeFunctionIdentifier() const {
    assert(isAutoDiffDerivativeFunction());
    return pointer.get<AutoDiffDerivativeFunctionIdentifier *>();
  }
  
  bool hasAsync() const;

private:
  friend struct llvm::DenseMapInfo<swift::SILDeclRef>;
  /// Produces a SILDeclRef from an opaque value.
  explicit SILDeclRef(void *opaqueLoc, Kind kind,
                      bool isForeign,
                      bool isDistributed,
                      bool isKnownToBeLocal,
                      bool isRuntimeAccessible,
                      BackDeploymentKind backDeploymentKind,
                      unsigned defaultArgIndex,
                      AutoDiffDerivativeFunctionIdentifier *derivativeId)
      : loc(Loc::getFromOpaqueValue(opaqueLoc)), kind(kind),
        isForeign(isForeign),
        isDistributed(isDistributed),
        isKnownToBeLocal(isKnownToBeLocal),
        isRuntimeAccessible(isRuntimeAccessible),
        backDeploymentKind(backDeploymentKind),
        defaultArgIndex(defaultArgIndex),
        pointer(derivativeId) {}
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, SILDeclRef C) {
  C.print(OS);
  return OS;
}

} // end swift namespace

namespace llvm {

// DenseMap key support for SILDeclRef.
template<> struct DenseMapInfo<swift::SILDeclRef> {
  using SILDeclRef = swift::SILDeclRef;
  using Kind = SILDeclRef::Kind;
  using BackDeploymentKind = SILDeclRef::BackDeploymentKind;
  using Loc = SILDeclRef::Loc;
  using PointerInfo = DenseMapInfo<void*>;
  using UnsignedInfo = DenseMapInfo<unsigned>;

  static SILDeclRef getEmptyKey() {
    return SILDeclRef(PointerInfo::getEmptyKey(), Kind::Func, false, false,
                      false, false, BackDeploymentKind::None, 0, nullptr);
  }
  static SILDeclRef getTombstoneKey() {
    return SILDeclRef(PointerInfo::getTombstoneKey(), Kind::Func, false, false,
                      false, false, BackDeploymentKind::None, 0, nullptr);
  }
  static unsigned getHashValue(swift::SILDeclRef Val) {
    unsigned h1 = PointerInfo::getHashValue(Val.loc.getOpaqueValue());
    unsigned h2 = UnsignedInfo::getHashValue(unsigned(Val.kind));
    unsigned h3 = (Val.kind == Kind::DefaultArgGenerator)
                    ? UnsignedInfo::getHashValue(Val.defaultArgIndex)
                    : 0;
    unsigned h4 = UnsignedInfo::getHashValue(Val.isForeign);
    unsigned h5 = PointerInfo::getHashValue(Val.pointer.getOpaqueValue());
    unsigned h6 = UnsignedInfo::getHashValue(Val.isDistributed);
    unsigned h7 = UnsignedInfo::getHashValue(unsigned(Val.backDeploymentKind));
    unsigned h8 = UnsignedInfo::getHashValue(Val.isKnownToBeLocal);
    unsigned h9 = UnsignedInfo::getHashValue(Val.isRuntimeAccessible);
    return h1 ^ (h2 << 4) ^ (h3 << 9) ^ (h4 << 7) ^ (h5 << 11) ^ (h6 << 8) ^
      (h7 << 10) ^ (h8 << 13) ^ (h9 << 15);
  }
  static bool isEqual(swift::SILDeclRef const &LHS,
                      swift::SILDeclRef const &RHS) {
    return LHS == RHS;
  }
};

} // end llvm namespace

#endif
