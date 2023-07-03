//===--- SILGenFunction.h - Function Specific AST lower context -*- C++ -*-===//
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

#ifndef SWIFT_SILGEN_SILGENFUNCTION_H
#define SWIFT_SILGEN_SILGENFUNCTION_H

#include "FormalEvaluation.h"
#include "Initialization.h"
#include "InitializeDistActorIdentity.h"
#include "JumpDest.h"
#include "RValue.h"
#include "SGFContext.h"
#include "SILGen.h"
#include "SILGenBuilder.h"
#include "swift/AST/AnyFunctionRef.h"
#include "swift/Basic/ProfileCounter.h"
#include "swift/Basic/Statistic.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILType.h"
#include "llvm/ADT/PointerIntPair.h"

namespace swift {

class ParameterList;
class ProfileCounterRef;

namespace Lowering {

class ArgumentSource;
class Condition;
class Conversion;
class ConsumableManagedValue;
class LogicalPathComponent;
class LValue;
class ManagedValue;
class PreparedArguments;
class RValue;
class CalleeTypeInfo;
class ResultPlan;
using ResultPlanPtr = std::unique_ptr<ResultPlan>;
class ArgumentScope;
class Scope;
class ExecutorBreadcrumb;

struct LValueOptions {
  bool IsNonAccessing = false;

  /// Derive options for accessing the base of an l-value, given that
  /// applying the derived component might touch the memory.
  LValueOptions forComputedBaseLValue() const {
    auto copy = *this;

    // Assume we're going to access the base.
    copy.IsNonAccessing = false;

    return copy;
  }

  /// Derive options for accessing the base of an l-value, given that
  /// applying the derived component will not touch the memory.
  LValueOptions forProjectedBaseLValue() const {
    auto copy = *this;
    return copy;
  }
};

class PatternMatchContext;

/// A formal section of the function.  This is a SILGen-only concept,
/// meant to improve locality.  It's only reflected in the generated
/// SIL implicitly.
enum class FunctionSection : bool {
  /// The section of the function dedicated to ordinary control flow.
  Ordinary,

  /// The section of the function dedicated to error-handling and
  /// similar things.
  Postmatter,
};

/// Parameter to \c SILGenFunction::emitCaptures that indicates what the
/// capture parameters are being emitted for.
enum class CaptureEmission {
  /// Captures are being emitted for immediate application to a local function.
  ImmediateApplication,
  /// Captures are being emitted for partial application to form a closure
  /// value.
  PartialApplication,
  /// Captures are being emitted for partial application of a local property
  /// wrapper setter for assign_by_wrapper. Captures are guaranteed to not
  /// escape, because assign_by_wrapper will not use the setter if the captured
  /// variable is not initialized.
  AssignByWrapper,
};

/// Different ways in which an l-value can be emitted.
enum class SGFAccessKind : uint8_t {
  /// The access is a read whose result will be ignored.
  IgnoredRead,

  /// The access is a read that would prefer the address of a borrowed value.
  /// This should only be used when it is semantically acceptable to borrow
  /// the value, not just because the caller would benefit from a borrowed
  /// value.  See shouldEmitSelfAsRValue.
  ///
  /// The caller will be calling emitAddressOfLValue or emitLoadOfLValue
  /// on the l-value.  The latter may be less efficient than an access
  /// would be if the l-value had been emitted with an owned-read kind.
  BorrowedAddressRead,

  /// The access is a read that would prefer a loaded borrowed value.
  /// This should only be used when it is semantically acceptable to borrow
  /// the value, not just because the caller would benefit from a borrowed
  /// value.  See shouldEmitSelfAsRValue.
  ///
  /// There isn't yet a way to emit the access that takes advantage of this.
  BorrowedObjectRead,

  /// The access is a read that would prefer the address of an owned value.
  ///
  /// The caller will be calling emitAddressOfLValue or emitLoadOfLValue
  /// on the l-value.
  OwnedAddressRead,

  /// The access is a read that would prefer a loaded owned value.
  ///
  /// The caller will be calling emitLoadOfLValue on the l-value.
  OwnedObjectRead,

  /// The access is an assignment (or maybe an initialization).
  ///
  /// The caller will be calling emitAssignToLValue on the l-value.
  Write,

  /// The access is a read-modify-write.
  ///
  /// The caller will be calling emitAddressOfLValue on the l-value.
  ReadWrite,

  /// The access is a consuming operation that would prefer a loaded address
  /// value. The lvalue will subsequently be left in an uninitialized state.
  ///
  /// The caller will be calling emitAddressOfLValue and then load from the
  /// l-value.
  OwnedAddressConsume,

  /// The access is a consuming operation that would prefer a loaded owned
  /// value. The lvalue will subsequently be left in an uninitialized state.
  ///
  /// The caller will be calling emitAddressOfLValue and then load from the
  /// l-value.
  OwnedObjectConsume,
};

static inline bool isReadAccess(SGFAccessKind kind) {
  return uint8_t(kind) <= uint8_t(SGFAccessKind::OwnedObjectRead);
}

static inline bool isConsumeAccess(SGFAccessKind kind) {
  switch (kind) {
  case SGFAccessKind::IgnoredRead:
  case SGFAccessKind::BorrowedAddressRead:
  case SGFAccessKind::BorrowedObjectRead:
  case SGFAccessKind::OwnedAddressRead:
  case SGFAccessKind::OwnedObjectRead:
  case SGFAccessKind::Write:
  case SGFAccessKind::ReadWrite:
    return false;
  case SGFAccessKind::OwnedAddressConsume:
  case SGFAccessKind::OwnedObjectConsume:
    return true;
  }
}

/// Given a read access kind, does it require an owned result?
static inline bool isReadAccessResultOwned(SGFAccessKind kind) {
  assert(isReadAccess(kind));
  return uint8_t(kind) >= uint8_t(SGFAccessKind::OwnedAddressRead);
}

/// Given a read access kind, does it require an address result?
static inline bool isReadAccessResultAddress(SGFAccessKind kind) {
  assert(isReadAccess(kind));
  return kind == SGFAccessKind::BorrowedAddressRead ||
         kind == SGFAccessKind::OwnedAddressRead;
}

/// Return an address-preferring version of the given access kind.
static inline SGFAccessKind getAddressAccessKind(SGFAccessKind kind) {
  switch (kind) {
  case SGFAccessKind::BorrowedObjectRead:
    return SGFAccessKind::BorrowedAddressRead;
  case SGFAccessKind::OwnedObjectRead:
    return SGFAccessKind::OwnedAddressRead;
  case SGFAccessKind::OwnedObjectConsume:
    return SGFAccessKind::OwnedAddressConsume;
  case SGFAccessKind::IgnoredRead:
  case SGFAccessKind::BorrowedAddressRead:
  case SGFAccessKind::OwnedAddressRead:
  case SGFAccessKind::OwnedAddressConsume:
  case SGFAccessKind::Write:
  case SGFAccessKind::ReadWrite:
    return kind;
  }
  llvm_unreachable("bad kind");
}

static inline AccessKind getFormalAccessKind(SGFAccessKind kind) {
  switch (kind) {
  case SGFAccessKind::IgnoredRead:
  case SGFAccessKind::BorrowedAddressRead:
  case SGFAccessKind::BorrowedObjectRead:
  case SGFAccessKind::OwnedAddressRead:
  case SGFAccessKind::OwnedObjectRead:
    return AccessKind::Read;
  case SGFAccessKind::Write:
    return AccessKind::Write;

  // TODO: Do we need our own AccessKind here?
  case SGFAccessKind::OwnedAddressConsume:
  case SGFAccessKind::OwnedObjectConsume:
  case SGFAccessKind::ReadWrite:
    return AccessKind::ReadWrite;
  }
  llvm_unreachable("bad kind");
}

/// Parameter to \c SILGenFunction::emitAddressOfLValue that indicates
/// what kind of instrumentation should be emitted when compiling under
/// Thread Sanitizer.
enum class TSanKind : bool {
  None = 0,

  /// Instrument the LValue access as an inout access.
  InoutAccess
};

/// Represents an LValue opened for mutating access.
///
/// This is used by LogicalPathComponent::projectAsBase().
struct MaterializedLValue {
  ManagedValue temporary;

  // Only set if a callback is required
  CanType origSelfType;
  CanGenericSignature genericSig;
  SILValue callback;
  SILValue callbackStorage;

  MaterializedLValue() {}
  explicit MaterializedLValue(ManagedValue temporary)
    : temporary(temporary) {}
  MaterializedLValue(ManagedValue temporary,
                     CanType origSelfType,
                     CanGenericSignature genericSig,
                     SILValue callback,
                     SILValue callbackStorage)
    : temporary(temporary),
      origSelfType(origSelfType),
      genericSig(genericSig),
      callback(callback),
      callbackStorage(callbackStorage) {}
};

/// SILGenFunction - an ASTVisitor for producing SIL from function bodies.
class LLVM_LIBRARY_VISIBILITY SILGenFunction
  : public ASTVisitor<SILGenFunction>
{ // style violation because Xcode <rdar://problem/13065676>
public:
  /// The SILGenModule this function belongs to.
  SILGenModule &SGM;
    
  /// The SILFunction being constructed.
  SILFunction &F;

  /// The SILModuleConventions for this SIL module.
  SILModuleConventions silConv;

  bool useLoweredAddresses() const { return silConv.useLoweredAddresses(); }

  /// The DeclContext corresponding to the function currently being emitted.
  DeclContext * const FunctionDC;

  /// The name of the function currently being emitted, as presented to user
  /// code by #function.
  DeclName MagicFunctionName;
  std::string MagicFunctionString;
  
  /// The abstraction pattern against which the function is being lowered.
  llvm::Optional<AbstractionPattern> OrigFnType;

  ASTContext &getASTContext() const { return SGM.M.getASTContext(); }

  /// The first block in the postmatter section of the function, if
  /// anything has been built there.
  ///
  /// (This field must precede B because B's initializer calls
  /// createBasicBlock().)
  SILFunction::iterator StartOfPostmatter;

  /// The current section of the function that we're emitting code in.
  ///
  /// The postmatter section is a part of the function intended for
  /// things like error-handling that don't need to be mixed into the
  /// normal code sequence.
  ///
  /// If the current function section is Ordinary, and
  /// StartOfPostmatter does not point to the function end, the current
  /// insertion block should be ordered before that.
  ///
  /// If the current function section is Postmatter, StartOfPostmatter
  /// does not point to the function end and the current insertion block is
  /// ordered after that (inclusive).
  ///
  /// (This field must precede B because B's initializer calls
  /// createBasicBlock().)
  FunctionSection CurFunctionSection = FunctionSection::Ordinary;

  /// Does this function require a non-void direct return?
  bool NeedsReturn = false;

  /// Is emission currently within a formal modification?
  bool isInFormalEvaluationScope() const {
    return FormalEvalContext.isInFormalEvaluationScope();
  }

  /// Is emission currently within an inout conversion?
  bool InInOutConversionScope = false;

  /// The SILGenBuilder used to construct the SILFunction.  It is what maintains
  /// the notion of the current block being emitted into.
  SILGenBuilder B;

  struct BreakContinueDest {
    LabeledStmt *Target;
    JumpDest BreakDest;
    JumpDest ContinueDest;
  };
  
  std::vector<BreakContinueDest> BreakContinueDestStack;
  std::vector<PatternMatchContext*> SwitchStack;

  /// Information for a parent SingleValueStmtExpr initialization.
  struct SingleValueStmtInitialization {
    /// The target expressions to be used for initialization.
    SmallPtrSet<Expr *, 4> Exprs;
    Initialization *Init;

    SingleValueStmtInitialization(Initialization *init) : Init(init) {}
  };

  /// A stack of active SingleValueStmtExpr initializations that may be
  /// initialized by the branches of a statement.
  std::vector<SingleValueStmtInitialization> SingleValueStmtInitStack;

  SourceFile *SF;
  SourceLoc LastSourceLoc;
  using ASTScopeTy = ast_scope::ASTScopeImpl;
  const ASTScopeTy *FnASTScope = nullptr;
  using VarDeclScopeMapTy =
      llvm::SmallDenseMap<ValueDecl *, const ASTScopeTy *, 8>;
  /// The ASTScope each variable declaration belongs to.
  VarDeclScopeMapTy VarDeclScopeMap;
  /// Caches one SILDebugScope for each ASTScope.
  llvm::SmallDenseMap<std::pair<const ASTScopeTy *, const SILDebugScope *>,
                      const SILDebugScope *, 16>
      ScopeMap;
  /// Caches one toplevel inline SILDebugScope for each macro BufferID.
  llvm::SmallDenseMap<unsigned, const SILDebugScope *, 16> InlinedScopeMap;

  /// The cleanup depth and BB for when the operand of a
  /// BindOptionalExpr is a missing value.
  SmallVector<JumpDest, 2> BindOptionalFailureDests;

  /// The cleanup depth and epilog BB for "return" statements.
  JumpDest ReturnDest = JumpDest::invalid();
  /// The cleanup depth and epilog BB for "fail" statements.
  JumpDest FailDest = JumpDest::invalid();

  /// The destination for throws.  The block will always be in the
  /// postmatter and takes a BB argument of the exception type.
  JumpDest ThrowDest = JumpDest::invalid();

  /// The destination for coroutine unwinds.  The block will always
  /// be in the postmatter.
  JumpDest CoroutineUnwindDest = JumpDest::invalid();
    
  /// This records information about the currently active cleanups.
  CleanupManager Cleanups;

  /// The current context where formal evaluation cleanups are managed.
  FormalEvaluationContext FormalEvalContext;

  /// VarLoc - representation of an emitted local variable or constant.  There
  /// are four scenarios here:
  ///
  ///  1) This could be a simple copyable "var" or "let" emitted into an
  ///     alloc_box.  In this case, 'value' contains a pointer (it is always an
  ///     address) to the value, and 'box' contains a pointer to the retain
  ///     count for the box.
  ///  2) This could be a simple non-address-only "let" represented directly. In
  ///     this case, 'value' is the value of the let and is never of address
  ///     type.  'box' is always nil.
  ///  3) This could be an address-only "let" emitted into an alloc_stack, or
  ///     passed in from somewhere else that has guaranteed lifetime (e.g. an
  ///     incoming argument of 'in_guaranteed' convention).  In this case,
  ///     'value' is a pointer to the memory (and thus, its type is always an
  ///     address) and the 'box' is nil.
  ///  4) This could be a noncopyable "var" or "let" emitted into an
  ///     alloc_box. In this case, 'value' is nil and the 'box' contains the box
  ///     itself. The user must always reproject from the box and insert an
  ///     access marker/must_must_check as appropriate.
  ///
  /// Generally, code shouldn't be written to enumerate these four cases, it
  /// should just handle the case of "box or not" or "address or not", depending
  /// on what the code cares about.
  struct VarLoc {
    /// value - the value of the variable, or the address the variable is
    /// stored at (if "value.getType().isAddress()" is true).
    ///
    /// It may be invalid if we are supposed to lazily project out an address
    /// from a box.
    SILValue value;

    /// box - This is the retainable box for something emitted to an alloc_box.
    /// It may be invalid if no box was made for the value (e.g., because it was
    /// an inout value, or constant emitted to an alloc_stack).
    SILValue box;

    static VarLoc get(SILValue value, SILValue box = SILValue()) {
      VarLoc Result;
      Result.value = value;
      Result.box = box;
      return Result;
    }
  };
  
  /// VarLocs - Entries in this map are generated when a PatternBindingDecl is
  /// emitted. The map is queried to produce the lvalue for a DeclRefExpr to
  /// a local variable.
  llvm::DenseMap<ValueDecl*, VarLoc> VarLocs;

  /// The local auxiliary declarations for the parameters of this function that
  /// need to be emitted inside the next brace statement.
  llvm::SmallVector<VarDecl *, 2> LocalAuxiliaryDecls;

  /// The mappings between instance properties referenced by this init
  /// accessor (via initializes/accesses attributes) and and argument
  /// declarations synthesized to access them in the body.
  llvm::DenseMap<VarDecl *, ParamDecl *> InitAccessorArgumentMappings;

  // Context information for tracking an `async let` child task.
  struct AsyncLetChildTask {
    SILValue asyncLet; // RawPointer to the async let state
    SILValue resultBuf; // RawPointer to the result buffer
    bool isThrowing; // true if task can throw
  };
  
  /// Mapping from each async let clause to the AsyncLet repr that contains the
  /// AsyncTask that will produce the initializer value for that clause and a
  /// Boolean value indicating whether the task can throw.
  llvm::SmallDenseMap<std::pair<PatternBindingDecl *, unsigned>,
                      AsyncLetChildTask>
      AsyncLetChildTasks;

  /// Indicates whether this function is a distributed actor's designated
  /// initializer, providing the needed clean-up to emit an identity
  /// assignment after initializing the actorSystem property.
  llvm::Optional<InitializeDistActorIdentity> DistActorCtorContext;

  /// When rebinding 'self' during an initializer delegation, we have to be
  /// careful to preserve the object at 1 retain count during the delegation
  /// because of assumptions in framework code. This enum tracks the state of
  /// 'self' during the delegation.
  enum SelfInitDelegationStates {
    // 'self' is a normal variable.
    NormalSelf,

    /// 'self' needs to be shared borrowed next time self is used.
    ///
    /// At this point we do not know if:
    ///
    /// 1. 'self' is used at all. In such a case, the borrow scope for self will
    ///     end before the delegating init call and we will overwrite the value
    ///     in
    ///     the self box.
    ///
    /// 2. If there is a consuming self use, will self be borrowed in an
    ///    exclusive manner or a shared manner. If we need to perform an
    ///    exclusive borrow, we will transition to WillExclusiveBorrowSelf in
    ///    SILGenApply.
    WillSharedBorrowSelf,

    /// 'self' needs to be exclusively borrowed next time self is used.
    ///
    /// We only advance to this state in SILGenApply when we know that we are
    /// going to be passing self to a delegating initializer that will consume
    /// it. We will always evaluate self before any other uses of self in the
    /// self.init call, so we know that we will never move from
    /// WillExclusiveBorrowSelf to WillSharedBorrowSelf.
    ///
    /// Once we are in this point, all other uses of self must be borrows until
    /// we use self in the delegating init call. All of the borrow scopes /must/
    /// end before the delegating init call.
    WillExclusiveBorrowSelf,

    /// 'self' was shared borrowed to compute the self argument of the
    /// delegating init call.
    ///
    /// This means that the delegating init uses a metatype or the like as its
    /// self argument instead of 'self'. Thus we are able to perform a shared
    /// borrow of self to compute that value and end the shared borrow scope
    /// before the delegating initializer apply.
    DidSharedBorrowSelf,

    // 'self' was exclusively borrowed for the delegating init call. All further
    // uses of self until the actual delegating init must be done via shared
    // borrows that end strictly before the delegating init call.
    DidExclusiveBorrowSelf,
  };
  SelfInitDelegationStates SelfInitDelegationState = NormalSelf;
  ManagedValue InitDelegationSelf;
  SILValue InitDelegationSelfBox;
  llvm::Optional<SILLocation> InitDelegationLoc;
  ManagedValue SuperInitDelegationSelf;

  RValue emitRValueForSelfInDelegationInit(SILLocation loc, CanType refType,
                                           SILValue result, SGFContext C);

  /// A version of emitRValueForSelfInDelegationInit that uses formal evaluation
  /// operations instead of normal scoped operations.
  RValue emitFormalEvaluationRValueForSelfInDelegationInit(SILLocation loc,
                                                           CanType refType,
                                                           SILValue addr,
                                                           SGFContext C);
  /// The metatype argument to an allocating constructor, if we're emitting one.
  SILValue AllocatorMetatype;

  /// If set, the current function is an async function which is formally
  /// isolated to the given executor, and hop_to_executor instructions must
  /// be inserted at the begin of the function and after all suspension
  /// points.
  SILValue ExpectedExecutor;

  struct ActivePackExpansion {
    GenericEnvironment *OpenedElementEnv;
    SILValue ExpansionIndex;

    /// Mapping from temporary pack expressions to their values. These
    /// are evaluated once, with their elements projected in a dynamic
    /// pack loop.
    llvm::SmallDenseMap<MaterializePackExpr *, SILValue>
      MaterializedPacks;

    ActivePackExpansion(GenericEnvironment *OpenedElementEnv)
        : OpenedElementEnv(OpenedElementEnv) {}
  };

  /// The innermost active pack expansion.
  ActivePackExpansion *InnermostPackExpansion = nullptr;

  ActivePackExpansion *getInnermostPackExpansion() const {
    assert(InnermostPackExpansion && "not inside a pack expansion!");
    return InnermostPackExpansion;
  }

  /// True if 'return' without an operand or falling off the end of the current
  /// function is valid.
  bool allowsVoidReturn() const { return ReturnDest.getBlock()->args_empty(); }

  /// Emit code to increment a counter for profiling.
  void emitProfilerIncrement(ASTNode Node);

  /// Emit code to increment a counter for profiling.
  void emitProfilerIncrement(ProfileCounterRef Ref);

  /// Load the profiled execution count corresponding to \p Node, if one is
  /// available.
  ProfileCounter loadProfilerCount(ASTNode Node) const;

  /// Get the PGO node's parent.
  llvm::Optional<ASTNode> getPGOParent(ASTNode Node) const;

  /// Tracer object for counting SIL (and other events) caused by this instance.
  FrontendStatsTracer StatsTracer;

  SILGenFunction(SILGenModule &SGM, SILFunction &F, DeclContext *DC);
  ~SILGenFunction();
  
  /// Return a stable reference to the current cleanup.
  CleanupsDepth getCleanupsDepth() const {
    return Cleanups.getCleanupsDepth();
  }
  CleanupHandle getTopCleanup() const {
    return Cleanups.getTopCleanup();
  }
  
  SILFunction &getFunction() { return F; }
  const SILFunction &getFunction() const { return F; }
  SILModule &getModule() { return F.getModule(); }
  SILGenBuilder &getBuilder() { return B; }
  const SILOptions &getOptions() { return getModule().getOptions(); }

  // Returns the type expansion context for types in this function.
  TypeExpansionContext getTypeExpansionContext() const {
    return TypeExpansionContext(getFunction());
  }

  const TypeLowering &getTypeLowering(AbstractionPattern orig, Type subst) {
    return F.getTypeLowering(orig, subst);
  }
  const TypeLowering &getTypeLowering(Type t) {
    return F.getTypeLowering(t);
  }
  CanSILFunctionType getSILFunctionType(TypeExpansionContext context,
                                        AbstractionPattern orig,
                                        CanFunctionType substFnType) {
    return SGM.Types.getSILFunctionType(context, orig, substFnType);
  }
  SILType getLoweredType(AbstractionPattern orig,
                         Type subst) {
    return F.getLoweredType(orig, subst);
  }
  SILType getLoweredType(Type t) {
    return F.getLoweredType(t);
  }
  SILType getLoweredType(AbstractionPattern orig, Type subst,
                         SILValueCategory category) {
    return SILType::getPrimitiveType(F.getLoweredRValueType(orig, subst),
                                     category);
  }
  SILType getLoweredType(Type t, SILValueCategory category) {
    return SILType::getPrimitiveType(F.getLoweredRValueType(t), category);
  }
  CanType getLoweredRValueType(AbstractionPattern orig,
                               Type subst) {
    return F.getLoweredRValueType(orig, subst);
  }
  CanType getLoweredRValueType(Type t) {
    return F.getLoweredRValueType(t);
  }
  SILType getLoweredTypeForFunctionArgument(Type t) {
    auto typeForConv =
        SGM.Types.getLoweredType(t, TypeExpansionContext::minimal());
    return getLoweredType(t).getCategoryType(typeForConv.getCategory());
  }

  SILType getLoweredLoadableType(Type t) {
    return F.getLoweredLoadableType(t);
  }
  const TypeLowering &getTypeLowering(SILType type) {
    return F.getTypeLowering(type);
  }

  SILType getSILInterfaceType(SILParameterInfo param) const {
    return silConv.getSILType(param, CanSILFunctionType(),
                              getTypeExpansionContext());
  }
  SILType getSILInterfaceType(SILResultInfo result) const {
    return silConv.getSILType(result, CanSILFunctionType(),
                              getTypeExpansionContext());
  }

  SILType getSILType(SILParameterInfo param, CanSILFunctionType fnTy) const {
    return silConv.getSILType(param, fnTy, getTypeExpansionContext());
  }
  SILType getSILType(SILResultInfo result, CanSILFunctionType fnTy) const {
    return silConv.getSILType(result, fnTy, getTypeExpansionContext());
  }

  SILType getSILTypeInContext(SILResultInfo result, CanSILFunctionType fnTy) {
    auto t = F.mapTypeIntoContext(getSILType(result, fnTy));
    return getTypeLowering(t).getLoweredType().getCategoryType(t.getCategory());
  }

  SILType getSILTypeInContext(SILParameterInfo param, CanSILFunctionType fnTy) {
    auto t = F.mapTypeIntoContext(getSILType(param, fnTy));
    return getTypeLowering(t).getLoweredType().getCategoryType(t.getCategory());
  }

  const SILConstantInfo &getConstantInfo(TypeExpansionContext context,
                                         SILDeclRef constant) {
    return SGM.Types.getConstantInfo(context, constant);
  }

  llvm::Optional<SILAccessEnforcement>
  getStaticEnforcement(VarDecl *var = nullptr);
  llvm::Optional<SILAccessEnforcement>
  getDynamicEnforcement(VarDecl *var = nullptr);
  llvm::Optional<SILAccessEnforcement>
  getUnknownEnforcement(VarDecl *var = nullptr);

  SourceManager &getSourceManager() { return SGM.M.getASTContext().SourceMgr; }
  std::string getMagicFileIDString(SourceLoc loc);
  StringRef getMagicFilePathString(SourceLoc loc);
  StringRef getMagicFunctionString();

  SILDebugLocation
  getSILDebugLocation(SILBuilder &B, SILLocation Loc,
                      llvm::Optional<SILLocation> CurDebugLocOverride,
                      bool ForMetaInstruction);

  const SILDebugScope *getScopeOrNull(SILLocation Loc,
                                      bool ForMetaInstruction = false);

private:
  const SILDebugScope *getOrCreateScope(SourceLoc SLoc);
  const SILDebugScope *getMacroScope(SourceLoc SLoc);
  const SILDebugScope *
  getOrCreateScope(const ast_scope::ASTScopeImpl *ASTScope,
                   const SILDebugScope *FnScope,
                   const SILDebugScope *InlinedAt = nullptr);

public:
  /// Enter the debug scope for \p Loc, creating it if necessary.
  ///
  /// \param isBindingScope If true, this is a scope for the bindings introduced
  /// by a let expression. This scope ends when the next innermost BraceStmt
  /// ends.
  void enterDebugScope(SILLocation Loc, bool isBindingScope = false);

  /// Return to the previous debug scope.
  void leaveDebugScope();

  std::unique_ptr<Initialization>
  prepareIndirectResultInit(SILLocation loc,
                            AbstractionPattern origResultType,
                            CanType formalResultType,
                            SmallVectorImpl<SILValue> &directResultsBuffer,
                            SmallVectorImpl<CleanupHandle> &cleanups);

  /// Check to see if an initalization for a SingleValueStmtExpr is active, and
  /// if the provided expression is for one of its branches. If so, returns the
  /// initialization to use for the expression. Otherwise returns \c nullptr.
  Initialization *getSingleValueStmtInit(Expr *E);

  //===--------------------------------------------------------------------===//
  // Entry points for codegen
  //===--------------------------------------------------------------------===//
  
  /// Generates code for a FuncDecl.
  void emitFunction(FuncDecl *fd);
  /// Emits code for a ClosureExpr.
  void emitClosure(AbstractClosureExpr *ce);

  /// Generates code for a class destroying destructor. This
  /// emits the body code from the DestructorDecl, calls the base class
  /// destructor, then implicitly releases the elements of the class.
  void emitDestroyingDestructor(DestructorDecl *dd);

  /// Generates code for an artificial top-level function that starts an
  /// application based on a main type and optionally a main type.
  void emitArtificialTopLevel(Decl *mainDecl);

  /// Generate code into @main for starting the async main on the main thread.
  void emitAsyncMainThreadStart(SILDeclRef entryPoint);

  /// Generates code for class/move only deallocating destructor. This calls the
  /// destroying destructor and then deallocates 'self'.
  void emitDeallocatingDestructor(DestructorDecl *dd);

  /// Generates code for a class deallocating destructor. This
  /// calls the destroying destructor and then deallocates 'self'.
  void emitDeallocatingClassDestructor(DestructorDecl *dd);

  /// Generates code for the deinit of the move only type and destroys all of
  /// the fields.
  void emitDeallocatingMoveOnlyDestructor(DestructorDecl *dd);

  /// Generates code for a struct constructor.
  /// This allocates the new 'self' value, emits the
  /// body code, then returns the final initialized 'self'.
  void emitValueConstructor(ConstructorDecl *ctor);
  /// Generates code for an enum case constructor.
  /// This allocates the new 'self' value, injects the enum case,
  /// then returns the final initialized 'self'.
  void emitEnumConstructor(EnumElementDecl *element);
  /// Generates code for a class constructor's
  /// allocating entry point. This allocates the new 'self' value, passes it to
  /// the initializer entry point, then returns the initialized 'self'.
  void emitClassConstructorAllocator(ConstructorDecl *ctor);
  /// Generates code for a class constructor's
  /// initializing entry point. This takes 'self' and the constructor arguments
  /// as parameters and executes the constructor body to initialize 'self'.
  void emitClassConstructorInitializer(ConstructorDecl *ctor);
  /// Generates code to initialize instance variables from their
  /// initializers.
  ///
  /// \param dc The DeclContext containing the current function.
  /// \param selfDecl The 'self' declaration within the current function.
  /// \param nominal The type whose members are being initialized.
  void emitMemberInitializers(DeclContext *dc, VarDecl *selfDecl,
                              NominalTypeDecl *nominal);

  /// Emit a method that initializes the ivars of a class.
  void emitIVarInitializer(SILDeclRef ivarInitializer);

  /// Emit a method that destroys the ivars of a class.
  void emitIVarDestroyer(SILDeclRef ivarDestroyer);

  /// Generates code for the given init accessor represented by AccessorDecl.
  /// This emits the body code and replaces all `self.<property>` references
  /// with either argument (if property appears in `acesses` list`) or result
  /// value assignment.
  void emitInitAccessor(AccessorDecl *accessor);

  /// Generates code to destroy the instance variables of a class.
  ///
  /// \param selfValue The 'self' value.
  /// \param cd The class declaration whose members are being destroyed.
  /// \param finishBB If set, used as the basic block after members have been
  ///                 destroyed, and we're ready to perform final cleanups
  ///                 before returning.
  void emitClassMemberDestruction(ManagedValue selfValue, ClassDecl *cd,
                                  CleanupLocation cleanupLoc,
                                  SILBasicBlock* finishBB);

  /// Generates code to destroy the instance variables of a move only non-class
  /// nominal type.
  ///
  /// \param selfValue The 'self' value.
  /// \param nd The nominal declaration whose members are being destroyed.
  void emitMoveOnlyMemberDestruction(SILValue selfValue, NominalTypeDecl *nd,
                                     CleanupLocation cleanupLoc);

  /// Generates code to destroy linearly recursive data structures, without
  /// building up the call stack.
  ///
  /// E.x.: In the following we want to deinit next without recursing into next.
  ///
  /// class Node<A> {
  ///   let value: A
  ///   let next: Node<A>?
  /// }
  ///
  /// \param selfValue The 'self' value.
  /// \param cd The class declaration whose members are being destroyed.
  /// \param recursiveLink The property that forms the recursive structure.
  void emitRecursiveChainDestruction(ManagedValue selfValue,
                                ClassDecl *cd,
                                VarDecl* recursiveLink,
                                CleanupLocation cleanupLoc);

  /// Generates a thunk from a foreign function to the native Swift convention.
  void emitForeignToNativeThunk(SILDeclRef thunk);
  /// Generates a thunk from a native function to foreign conventions.
  void emitNativeToForeignThunk(SILDeclRef thunk);
  /// Generates a stub that launches a detached task for running the NativeToForeignThunk of an
  /// async native method.
  ///
  /// Returns the SILFunction created for the closure implementation function that is enqueued on the
  /// new task.
  SILFunction *emitNativeAsyncToForeignThunk(SILDeclRef thunk);

  /// Generate a nullary function that returns the given value.
  /// If \p emitProfilerIncrement is set, emit a profiler increment for
  /// \p value.
  void emitGeneratorFunction(SILDeclRef function, Expr *value,
                             bool emitProfilerIncrement = false);

  /// Generate a nullary function that returns the value of the given variable's
  /// expression initializer.
  void emitGeneratorFunction(SILDeclRef function, VarDecl *var);

  /// Generate a nullary function that has the given result interface type and
  /// body.
  void emitGeneratorFunction(
      SILDeclRef function, Type resultInterfaceType, BraceStmt *body,
      llvm::Optional<AbstractionPattern> pattern = llvm::None);

  /// Generate an ObjC-compatible destructor (-dealloc).
  void emitObjCDestructor(SILDeclRef dtor);

  /// Generate code to obtain the address of the given global variable.
  ManagedValue emitGlobalVariableRef(SILLocation loc, VarDecl *var,
                                     llvm::Optional<ActorIsolation> actorIso);

  /// Generate a lazy global initializer.
  void emitLazyGlobalInitializer(PatternBindingDecl *binding,
                                 unsigned pbdEntry);
  
  /// Generate a global accessor, using the given initializer token and
  /// function
  void emitGlobalAccessor(VarDecl *global,
                          SILGlobalVariable *onceToken,
                          SILFunction *onceFunc);

  /// Generate a protocol witness entry point, invoking 'witness' at the
  /// abstraction level of 'requirement'.
  ///
  /// This is used for both concrete witness thunks and default witness
  /// thunks.
  void emitProtocolWitness(AbstractionPattern reqtOrigTy,
                           CanAnyFunctionType reqtSubstTy,
                           SILDeclRef requirement, SubstitutionMap reqtSubs,
                           SILDeclRef witness, SubstitutionMap witnessSubs,
                           IsFreeFunctionWitness_t isFree,
                           bool isSelfConformance,
                           llvm::Optional<ActorIsolation> enterIsolation);

  /// Generates subscript arguments for keypath. This function handles lowering
  /// of all index expressions including default arguments.
  ///
  /// \returns Lowered index arguments.
  /// \param subscript - The subscript decl who's arguments are being lowered.
  /// \param subs - Used to get subscript function type and to substitute generic args.
  /// \param argList - The argument list of the subscript.
  SmallVector<ManagedValue, 4>
  emitKeyPathSubscriptOperands(SILLocation loc,
                               SubscriptDecl *subscript,
                               SubstitutionMap subs,
                               ArgumentList *argList);

  /// Convert a block to a native function with a thunk.
  ManagedValue emitBlockToFunc(SILLocation loc,
                               ManagedValue block,
                               CanAnyFunctionType blockTy,
                               CanAnyFunctionType funcTy,
                               CanSILFunctionType loweredFuncTy);

  /// Convert a native function to a block with a thunk.
  ManagedValue emitFuncToBlock(SILLocation loc,
                               ManagedValue block,
                               CanAnyFunctionType funcTy,
                               CanAnyFunctionType blockTy,
                               CanSILFunctionType loweredBlockTy);

  /// Thunk with the signature of a base class method calling a derived class
  /// method.
  ///
  /// \param inputOrigType Abstraction pattern of base class method
  /// \param inputSubstType Formal AST type of base class method
  /// \param outputSubstType Formal AST type of derived class method
  /// \param baseLessVisibleThanDerived If true, the thunk does a
  /// double dispatch to the derived method's vtable entry, so that if
  /// the derived method has an override that cannot access the base,
  /// calls to the base dispatch to the correct method.
  void emitVTableThunk(SILDeclRef base,
                       SILDeclRef derived,
                       SILFunction *implFn,
                       AbstractionPattern inputOrigType,
                       CanAnyFunctionType inputSubstType,
                       CanAnyFunctionType outputSubstType,
                       bool baseLessVisibleThanDerived);
    
  //===--------------------------------------------------------------------===//
  // Control flow
  //===--------------------------------------------------------------------===//

  /// emitCondition - Emit a boolean expression as a control-flow condition.
  ///
  /// \param E - The expression to be evaluated as a condition.
  /// \param invertValue - true if this routine should invert the value before
  ///        testing true/false.
  /// \param contArgs - the types of the arguments to the continuation BB.
  ///        Matching argument values must be passed to exitTrue and exitFalse
  ///        of the resulting Condition object.
  /// \param NumTrueTaken - The number of times the condition evaluates to true.
  /// \param NumFalseTaken - The number of times the condition evaluates to
  /// false.
  ///
  /// If `contArgs` is nonempty, then both Condition::exitTrue() and
  /// Condition::exitFalse() must be called.
  Condition emitCondition(Expr *E, bool invertValue = false,
                          ArrayRef<SILType> contArgs = {},
                          ProfileCounter NumTrueTaken = ProfileCounter(),
                          ProfileCounter NumFalseTaken = ProfileCounter());

  Condition emitCondition(SILValue V, SILLocation Loc, bool invertValue = false,
                          ArrayRef<SILType> contArgs = {},
                          ProfileCounter NumTrueTaken = ProfileCounter(),
                          ProfileCounter NumFalseTaken = ProfileCounter());

  /// Create a new basic block.
  ///
  /// The block can be explicitly placed after a particular block.
  /// Otherwise, if the current insertion point is valid, it will be
  /// placed immediately after it.  Otherwise, it will be placed at the
  /// end of the current function section.
  ///
  /// Because basic blocks are generally constructed with an insertion
  /// point active, users should be aware that this behavior leads to
  /// an emergent LIFO ordering: if code generation requires multiple
  /// blocks, the second block created will be positioned before the
  /// first block.  (This is clearly desirable behavior when blocks
  /// are created by different emissions; it's just a little
  /// counter-intuitive within a single emission.)
  SILBasicBlock *createBasicBlock();
  SILBasicBlock *createBasicBlock(llvm::StringRef debugName);
  SILBasicBlock *createBasicBlockAfter(SILBasicBlock *afterBB);
  SILBasicBlock *createBasicBlockBefore(SILBasicBlock *beforeBB);

  /// Create a new basic block at the end of the given function
  /// section.
  SILBasicBlock *createBasicBlock(FunctionSection section);

  SILBasicBlock *createBasicBlockAndBranch(SILLocation loc,
                                           SILBasicBlock *destBB);

  /// Erase a basic block that was speculatively created and turned
  /// out to be unneeded.
  ///
  /// This should be called instead of eraseFromParent() in order to
  /// keep SILGen's internal bookkeeping consistent. 
  ///
  /// The block should be empty and have no predecessors.
  void eraseBasicBlock(SILBasicBlock *block);

  void mergeCleanupBlocks();

  //===--------------------------------------------------------------------===//
  // Concurrency
  //===--------------------------------------------------------------------===//

  /// Generates code to obtain the executor for the given actor isolation,
  /// as-needed, and emits a \c hop_to_executor to that executor.
  ///
  /// \returns an \c ExecutorBreadcrumb that saves the information necessary to hop
  /// back to what was previously the current executor after the actor-isolated
  /// region ends. Invoke \c emit on the breadcrumb to
  /// restore the previously-active executor.
  ExecutorBreadcrumb
  emitHopToTargetActor(SILLocation loc, llvm::Optional<ActorIsolation> actorIso,
                       llvm::Optional<ManagedValue> actorSelf);

  /// Emit a hop to the target executor, returning a breadcrumb with enough
  /// enough information to hop back.
  ExecutorBreadcrumb emitHopToTargetExecutor(SILLocation loc,
                                             SILValue executor);

  /// Generate a hop directly to a dynamic actor instance. This can only be done
  /// inside an async actor-independent function. No hop-back is expected.
  void emitHopToActorValue(SILLocation loc, ManagedValue actor);

  /// Return true if the function being emitted is an async function
  /// that unsafely inherits its executor.
  bool unsafelyInheritsExecutor();

  /// A version of `emitHopToTargetActor` that is specialized to the needs
  /// of various types of ConstructorDecls, like class or value initializers,
  /// because their prolog emission is not the same as for regular functions.
  ///
  /// This function emits the appropriate hop_to_executor for a constructor's
  /// prologue.
  ///
  /// NOTE: this does not support actor initializers!
  void emitConstructorPrologActorHop(SILLocation loc,
                                     llvm::Optional<ActorIsolation> actorIso);

  /// Set the given global actor as the isolation for this function
  /// (generally a thunk) and hop to it.
  void emitPrologGlobalActorHop(SILLocation loc, Type globalActor);

  /// Emit the executor for the given actor isolation.
  llvm::Optional<SILValue> emitExecutor(SILLocation loc,
                                        ActorIsolation isolation,
                                        llvm::Optional<ManagedValue> maybeSelf);

  /// Emit the executor value that corresponds to the generic (concurrent)
  /// executor.
  SILValue emitGenericExecutor(SILLocation loc);

  /// Emit the executor value that corresponds to the main actor.
  SILValue emitMainExecutor(SILLocation loc);

  /// Emit a precondition check to ensure that the function is executing in
  /// the expected isolation context.
  void emitPreconditionCheckExpectedExecutor(
      SILLocation loc, SILValue executor);

  /// Gets a reference to the current executor for the task.
  /// \returns a value of type Builtin.Executor
  SILValue emitGetCurrentExecutor(SILLocation loc);
  
  /// Generates code to obtain an actor's executor given a reference
  /// to the actor.
  /// \returns a value which can be used with hop_to_executor
  SILValue emitLoadActorExecutor(SILLocation loc, ManagedValue actor);

  /// Generates the code to obtain the executor for the shared instance 
  /// of the \p globalActor based on the type.
  /// \returns a value which can be used with hop_to_executor
  SILValue emitLoadGlobalActorExecutor(Type globalActor);

  //===--------------------------------------------------------------------===//
  // Memory management
  //===--------------------------------------------------------------------===//

  /// Emit debug info for the artificial error inout argument.
  void emitErrorArgument(SILLocation Loc, unsigned ArgNo);

  /// emitProlog - Generates prolog code to allocate and clean up mutable
  /// storage for closure captures and local arguments.
  void
  emitProlog(CaptureInfo captureInfo, ParameterList *paramList,
             ParamDecl *selfParam, DeclContext *DC, Type resultType,
             bool throws, SourceLoc throwsLoc,
             llvm::Optional<AbstractionPattern> origClosureType = llvm::None);
  /// A simpler version of emitProlog
  /// \returns the number of variables in paramPatterns.
  uint16_t emitBasicProlog(
      ParameterList *paramList, ParamDecl *selfParam, Type resultType,
      DeclContext *DC, bool throws, SourceLoc throwsLoc,
      unsigned numIgnoredTrailingParameters,
      llvm::Optional<AbstractionPattern> origClosureType = llvm::None);

  /// Create SILArguments in the entry block that bind a single value
  /// of the given parameter suitably for being forwarded.
  void bindParameterForForwarding(ParamDecl *param,
                                  SmallVectorImpl<SILValue> &parameters);

  /// Create SILArguments in the entry block that bind all the values
  /// of the given parameter list suitably for being forwarded.
  void bindParametersForForwarding(const ParameterList *params,
                                   SmallVectorImpl<SILValue> &parameters);

  /// Create (but do not emit) the epilog branch, and save the
  /// current cleanups depth as the destination for return statement branches.
  ///
  /// \param directResultType  If given a value, the epilog block will be
  ///                    created with arguments for each direct result of this
  ///                    function, corresponding to the formal return type.
  /// \param isThrowing  If true, create an error epilog block.
  /// \param L           The SILLocation which should be associated with
  ///                    cleanup instructions.
  void prepareEpilog(llvm::Optional<Type> directResultType, bool isThrowing,
                     CleanupLocation L);
  void prepareRethrowEpilog(CleanupLocation l);
  void prepareCoroutineUnwindEpilog(CleanupLocation l);
  
  /// Branch to and emit the epilog basic block. This will fuse
  /// the epilog to the current basic block if the epilog bb has no predecessor.
  /// The insertion point will be moved into the epilog block if it is
  /// reachable.
  ///
  /// \param TopLevelLoc The location of the top level AST node for which we are
  ///            constructing the epilog, such as a AbstractClosureExpr.
  /// \returns None if the epilog block is unreachable. Otherwise, returns
  ///          the epilog block's return value argument, or a null SILValue if
  ///          the epilog doesn't take a return value. Also returns the location
  ///          of the return instruction if the epilog block is supposed to host
  ///          the ReturnLocation (This happens in case the predecessor block is
  ///          merged with the epilog block.)
  std::pair<llvm::Optional<SILValue>, SILLocation>
  emitEpilogBB(SILLocation TopLevelLoc);

  /// Emits a standard epilog which runs top-level cleanups then returns
  /// the function return value, if any.  This can be customized by clients, who
  /// set UsesCustomEpilog to true, and optionally inject their own code into
  /// the epilog block before calling this.  If they do this, their code is run
  /// before the top-level cleanups, and the epilog block to continue is
  /// returned as the insertion point of this function.  They must provide the
  /// final exit sequence for the block as well.
  ///
  /// \param TopLevelLoc The location of the top-level expression during whose
  ///        evaluation the epilog is being produced, for example, the
  ///        AbstractClosureExpr.
  /// \param UsesCustomEpilog True if the client wants to manage its own epilog
  ///        logic.
  SILLocation emitEpilog(SILLocation TopLevelLoc,bool UsesCustomEpilog = false);

  /// Emits the standard rethrow epilog using a Swift error result.
  void emitRethrowEpilog(SILLocation topLevelLoc);

  /// Emits the coroutine-unwind epilog.
  void emitCoroutineUnwindEpilog(SILLocation topLevelLoc);

  /// emitSelfDecl - Emit a SILArgument for 'self', register it in varlocs, set
  /// up debug info, etc.  This returns the 'self' value.
  ///
  /// This is intended to only be used for destructors.
  SILValue emitSelfDeclForDestructor(VarDecl *selfDecl);

  /// Emits a temporary allocation that will be deallocated automatically at the
  /// end of the current scope. Returns the address of the allocation.
  ///
  /// \p isLexical if set to true, this is a temporary that we are using for a
  /// local let that we need to mark with the lexical flag.
  SILValue emitTemporaryAllocation(SILLocation loc, SILType ty,
                                   bool hasDynamicLifetime = false,
                                   bool isLexical = false,
                                   bool generateDebugInfo = true);

  /// Emits a temporary allocation for a pack that will be deallocated
  /// automatically at the end of the current scope.  Returns the address
  /// of the allocation.
  SILValue emitTemporaryPackAllocation(SILLocation loc, SILType packTy);

  /// Prepares a buffer to receive the result of an expression, either using the
  /// 'emit into' initialization buffer if available, or allocating a temporary
  /// allocation if not.
  ///
  /// The caller should call manageBufferForExprResult at the instant
  /// that the buffer has been initialized.
  SILValue getBufferForExprResult(SILLocation loc, SILType ty, SGFContext C);

  /// Flag that the buffer for an expression result has been properly
  /// initialized.
  ///
  /// Returns an empty value if the buffer was taken from the context.
  ManagedValue manageBufferForExprResult(SILValue buffer,
                                         const TypeLowering &bufferTL,
                                         SGFContext C);
  
  //===--------------------------------------------------------------------===//
  // Type conversions for expr emission and thunks
  //===--------------------------------------------------------------------===//

  ManagedValue emitInjectEnum(SILLocation loc,
                              ArgumentSource &&payload,
                              SILType enumTy,
                              EnumElementDecl *element,
                              SGFContext C);

  ManagedValue emitInjectOptional(SILLocation loc,
                                  const TypeLowering &expectedTL,
                                  SGFContext ctxt,
                       llvm::function_ref<ManagedValue(SGFContext)> generator);

  /// Initialize a memory location with an optional value.
  ///
  /// \param loc   The location to use for the resulting optional.
  /// \param value The value to inject into an optional.
  /// \param dest  The uninitialized memory in which to store the result value.
  /// \param optTL Type lowering information for the optional to create.
  void emitInjectOptionalValueInto(SILLocation loc,
                                   ArgumentSource &&value,
                                   SILValue dest,
                                   const TypeLowering &optTL);

  /// Initialize a memory location with an optional "nothing"
  /// value.
  ///
  /// \param loc   The location to use for the resulting optional.
  /// \param dest  The uninitialized memory in which to store the result value.
  /// \param optTL Type lowering information for the optional to create.
  void emitInjectOptionalNothingInto(SILLocation loc,
                                     SILValue dest,
                                     const TypeLowering &optTL);

  /// Return a value for an optional ".None" of the specified type. This only
  /// works for loadable enum types.
  SILValue getOptionalNoneValue(SILLocation loc, const TypeLowering &optTL);

  /// Return a value for an optional ".Some(x)" of the specified type. This only
  /// works for loadable enum types.
  ManagedValue getOptionalSomeValue(SILLocation loc, ManagedValue value,
                                    const TypeLowering &optTL);


  struct SourceLocArgs {
    ManagedValue filenameStartPointer,
                 filenameLength,
                 filenameIsAscii,
                 line,
                 column;
  };

  /// Emit raw lowered arguments for a runtime diagnostic to report the given
  /// source location:
  /// - The first three arguments are the components necessary to construct
  ///   a StaticString for the filename: start pointer, length, and
  ///   "is ascii" bit.
  /// - The fourth argument is the line number.
  SourceLocArgs
  emitSourceLocationArgs(SourceLoc loc, SILLocation emitLoc);

  /// Emit a 'String' literal for the passed 'text'.
  ///
  /// See also: 'emitLiteral' which works with various types of literals,
  /// however requires an expression to base the creation on.
  ManagedValue
  emitStringLiteral(SILLocation loc,
                    StringRef text,
                    StringLiteralExpr::Encoding encoding = StringLiteralExpr::Encoding::UTF8,
                    SGFContext ctx = SGFContext());

  /// Emit a call to the library intrinsic _doesOptionalHaveValue.
  ///
  /// The result is a Builtin.Int1.
  SILValue emitDoesOptionalHaveValue(SILLocation loc, SILValue addrOrValue);

  /// Emit a switch_enum to call the library intrinsic
  /// _diagnoseUnexpectedNilOptional if the optional has no value. Return the
  /// MangedValue resulting from the success case.
  ManagedValue emitPreconditionOptionalHasValue(SILLocation loc,
                                                ManagedValue optional,
                                                bool isImplicitUnwrap);

  /// Emit a call to the library intrinsic _getOptionalValue
  /// given the address of the optional, which checks that an optional contains
  /// some value and either returns the value or traps if there is none.
  ManagedValue emitCheckedGetOptionalValueFrom(SILLocation loc,
                                               ManagedValue addr,
                                               bool isImplicitUnwrap,
                                               const TypeLowering &optTL,
                                               SGFContext C);
  
  /// Extract the value from an optional, which must be known to contain
  /// a value.
  ManagedValue emitUncheckedGetOptionalValueFrom(SILLocation loc,
                                                 ManagedValue addrOrValue,
                                                 const TypeLowering &optTL,
                                                 SGFContext C = SGFContext());

  typedef llvm::function_ref<ManagedValue(SILGenFunction &SGF,
                                    SILLocation loc,
                                    ManagedValue input,
                                    SILType loweredResultTy,
                                    SGFContext context)> ValueTransformRef;

  /// Emit a transformation on the value of an optional type.
  ManagedValue emitOptionalToOptional(SILLocation loc,
                                      ManagedValue input,
                                      SILType loweredResultTy,
                                      ValueTransformRef transform,
                                      SGFContext C = SGFContext());

  ManagedValue emitOptionalSome(SILLocation loc, SILType optionalTy,
                                ValueProducerRef injector,
                                SGFContext C = SGFContext());

  /// Emit a reinterpret-cast from one pointer type to another, using a library
  /// intrinsic.
  RValue emitPointerToPointer(SILLocation loc,
                              ManagedValue input,
                              CanType inputTy,
                              CanType outputTy,
                              SGFContext C = SGFContext());

  ManagedValue emitClassMetatypeToObject(SILLocation loc,
                                         ManagedValue v,
                                         SILType resultTy);

  ManagedValue emitExistentialMetatypeToObject(SILLocation loc,
                                               ManagedValue v,
                                               SILType resultTy);

  ManagedValue emitProtocolMetatypeToObject(SILLocation loc,
                                            CanType inputTy,
                                            SILType resultTy);

  ManagedValue manageOpaqueValue(ManagedValue value,
                                 SILLocation loc,
                                 SGFContext C);

  /// Open up the given existential value and project its payload.
  ///
  /// \param existentialValue The existential value.
  /// \param loweredOpenedType The lowered type of the projection, which in
  /// practice will be the openedArchetype, possibly wrapped in a metatype.
  ManagedValue emitOpenExistential(SILLocation loc,
                                   ManagedValue existentialValue,
                                   SILType loweredOpenedType,
                                   AccessKind accessKind);

  /// Wrap the given value in an existential container.
  ///
  /// \param concreteFormalType AST type of value.
  /// \param concreteTL Type lowering of value.
  /// \param existentialTL Type lowering of existential type.
  /// \param F Function reference to emit the existential contents with the
  /// given context.
  ManagedValue emitExistentialErasure(
                            SILLocation loc,
                            CanType concreteFormalType,
                            const TypeLowering &concreteTL,
                            const TypeLowering &existentialTL,
                            ArrayRef<ProtocolConformanceRef> conformances,
                            SGFContext C,
                            llvm::function_ref<ManagedValue (SGFContext)> F,
                            bool allowEmbeddedNSError = true);

  RValue emitCollectionConversion(SILLocation loc,
                                  FuncDecl *fn,
                                  CanType fromCollection,
                                  CanType toCollection,
                                  ManagedValue mv,
                                  SGFContext C);

  //===--------------------------------------------------------------------===//
  // Recursive entry points
  //===--------------------------------------------------------------------===//

  using ASTVisitorType::visit;
  
  //===--------------------------------------------------------------------===//
  // Statements
  //===--------------------------------------------------------------------===//

  void visit(Stmt *S) = delete;

  void emitStmt(Stmt *S);

  void emitBreakOutOf(SILLocation loc, Stmt *S);

  void emitCatchDispatch(DoCatchStmt *S, ManagedValue exn,
                         ArrayRef<CaseStmt *> clauses,
                         JumpDest catchFallthroughDest);

  /// Emit code for the throw expr. If \p emitWillThrow is set then emit a
  /// call to swift_willThrow, that will allow the debugger to place a
  /// breakpoint on throw sites.
  void emitThrow(SILLocation loc, ManagedValue exn, bool emitWillThrow = false);
  
  //===--------------------------------------------------------------------===//
  // Patterns
  //===--------------------------------------------------------------------===//

  SILValue emitOSVersionRangeCheck(SILLocation loc, const VersionRange &range);
  void emitStmtCondition(StmtCondition Cond, JumpDest FalseDest, SILLocation loc,
                         ProfileCounter NumTrueTaken = ProfileCounter(),
                         ProfileCounter NumFalseTaken = ProfileCounter());

  void emitConditionalPBD(PatternBindingDecl *PBD, SILBasicBlock *FailBB);

  void usingImplicitVariablesForPattern(Pattern *pattern, CaseStmt *stmt,
                                        const llvm::function_ref<void(void)> &f);
  void emitSwitchStmt(SwitchStmt *S);
  void emitSwitchFallthrough(FallthroughStmt *S);

  //===--------------------------------------------------------------------===//
  // Expressions
  //===--------------------------------------------------------------------===//
 
  RValue visit(Expr *E) = delete;
 
  /// Generate SIL for the given expression, storing the final result into the
  /// specified Initialization buffer(s). This avoids an allocation and copy if
  /// the result would be allocated into temporary memory normally.
  /// The location defaults to \c E.
  void emitExprInto(Expr *E, Initialization *I,
                    llvm::Optional<SILLocation> L = llvm::None);

  /// Emit the given expression as an r-value.
  RValue emitRValue(Expr *E, SGFContext C = SGFContext());

  /// Emit the given expression as a +1 r-value.
  ///
  /// *NOTE* This creates the +1 r-value and then pushes that +1 r-value through
  /// a scope. So all temporaries resulting will be cleaned up.
  ///
  /// *NOTE* +0 vs +1 is ignored by this function. The only reason to use the
  /// SGFContext argument is to pass in an initialization.
  RValue emitPlusOneRValue(Expr *E, SGFContext C = SGFContext());

  /// Emit the given expression as a +0 r-value.
  ///
  /// *NOTE* This does not scope the creation of the +0 r-value. The reason why
  /// this is done is that +0 r-values can not be pushed through scopes.
  RValue emitPlusZeroRValue(Expr *E);

  /// Emit the given expression as an r-value with the given conversion
  /// context.  This may be more efficient --- and, in some cases,
  /// semantically different --- than emitting the expression and then
  /// converting the result.
  ///
  /// \param C a context into which to emit the converted result
  ManagedValue emitConvertedRValue(Expr *E, const Conversion &conversion,
                                   SGFContext C = SGFContext());
  ManagedValue emitConvertedRValue(SILLocation loc,
                                   const Conversion &conversion,
                                   SGFContext C,
                                   ValueProducerRef produceValue);

  /// Emit the given expression as an r-value that follows the
  /// abstraction patterns of the original type.
  ManagedValue emitRValueAsOrig(Expr *E, AbstractionPattern origPattern,
                                const TypeLowering &origTL,
                                SGFContext C = SGFContext());

  /// Emit an r-value into temporary memory and return the managed address.
  ManagedValue
  emitMaterializedRValueAsOrig(Expr *E, AbstractionPattern origPattern);
  
  /// Emit the given expression, ignoring its result.
  void emitIgnoredExpr(Expr *E);
  
  /// Emit the given expression as an r-value, then (if it is a tuple), combine
  /// it together into a single ManagedValue.
  ManagedValue emitRValueAsSingleValue(Expr *E, SGFContext C = SGFContext());

  /// Emit 'undef' in a particular formal type.
  ManagedValue emitUndef(Type type);
  ManagedValue emitUndef(SILType type);
  RValue emitUndefRValue(SILLocation loc, Type type);
  
  std::pair<ManagedValue, SILValue>
  emitUninitializedArrayAllocation(Type ArrayTy,
                                   SILValue Length,
                                   SILLocation Loc);

  CleanupHandle enterDeallocateUninitializedArrayCleanup(SILValue array);
  void emitUninitializedArrayDeallocation(SILLocation loc, SILValue array);
  ManagedValue emitUninitializedArrayFinalization(SILLocation loc,
                                                  ManagedValue array);

  /// Emit a cleanup for an owned value that should be written back at end of
  /// scope if the value is not forwarded.
  CleanupHandle enterOwnedValueWritebackCleanup(SILLocation loc,
                                                SILValue address,
                                                SILValue newValue);

  SILValue emitConversionToSemanticRValue(SILLocation loc, SILValue value,
                                          const TypeLowering &valueTL);

  ManagedValue emitConversionToSemanticRValue(SILLocation loc,
                                              ManagedValue value,
                                              const TypeLowering &valueTL);

  /// Emit the empty tuple value by emitting
  SILValue emitEmptyTuple(SILLocation loc);
  /// "Emit" an RValue representing an empty tuple.
  RValue emitEmptyTupleRValue(SILLocation loc, SGFContext C);

  /// Returns a reference to a constant in global context. For local func decls
  /// this returns the function constant with unapplied closure context.
  SILValue emitGlobalFunctionRef(SILLocation loc, SILDeclRef constant) {
    return emitGlobalFunctionRef(
        loc, constant, getConstantInfo(getTypeExpansionContext(), constant));
  }
  SILValue
  emitGlobalFunctionRef(SILLocation loc, SILDeclRef constant,
                        SILConstantInfo constantInfo,
                        bool callPreviousDynamicReplaceableImpl = false);

  /// Returns a reference to a function value that dynamically dispatches
  /// the function in a runtime-modifiable way.
  ManagedValue emitDynamicMethodRef(SILLocation loc, SILDeclRef constant,
                                    CanSILFunctionType constantTy);

  /// Returns a reference to a vtable-dispatched method.
  SILValue emitClassMethodRef(SILLocation loc, SILValue selfPtr,
                              SILDeclRef constant,
                              CanSILFunctionType constantTy);

  /// Given that a variable is a local stored variable, return its address.
  ManagedValue emitAddressOfLocalVarDecl(SILLocation loc, VarDecl *var,
                                         CanType formalRValueType,
                                         SGFAccessKind accessKind);

  // FIXME: demote this to private state.
  ManagedValue maybeEmitValueOfLocalVarDecl(
      VarDecl *var, AccessKind accessKind);

  /// Produce an RValue for a reference to the specified declaration,
  /// with the given type and in response to the specified expression.  Try to
  /// emit into the specified SGFContext to avoid copies (when provided).
  RValue emitRValueForDecl(SILLocation loc, ConcreteDeclRef decl, Type ty,
                           AccessSemantics semantics,
                           SGFContext C = SGFContext());

  /// Produce a singular RValue for a load from the specified property.
  ///
  /// This is designed to work with RValue ManagedValue bases that are either +0
  /// or +1.
  ///
  /// \arg isBaseGuaranteed This should /only/ be set to true if we know that
  /// the base value will stay alive as long as the returned RValue implying
  /// that it is safe to load/use values as +0.
  RValue emitRValueForStorageLoad(SILLocation loc,
                                  ManagedValue base,
                                  CanType baseFormalType,
                                  bool isSuper, AbstractStorageDecl *storage,
                                  PreparedArguments &&indices,
                                  SubstitutionMap substitutions,
                                  AccessSemantics semantics, Type propTy,
                                  SGFContext C,
                                  bool isBaseGuaranteed = false);

  void emitCaptures(SILLocation loc,
                    SILDeclRef closure,
                    CaptureEmission purpose,
                    SmallVectorImpl<ManagedValue> &captures);

  /// Produce a reference to a function, which may be a local function
  /// with captures. If the function is generic, substitutions must be
  /// given. The result is re-abstracted to the given expected type.
  ManagedValue emitClosureValue(SILLocation loc,
                                SILDeclRef function,
                                CanType expectedType,
                                SubstitutionMap subs,
                                bool alreadyConverted);

  PreparedArguments prepareSubscriptIndices(SILLocation loc,
                                            SubscriptDecl *subscript,
                                            SubstitutionMap subs,
                                            AccessStrategy strategy,
                                            ArgumentList *argList);

  ArgumentSource prepareAccessorBaseArg(SILLocation loc, ManagedValue base,
                                        CanType baseFormalType,
                                        SILDeclRef accessor);

  RValue emitGetAccessor(
      SILLocation loc, SILDeclRef getter, SubstitutionMap substitutions,
      ArgumentSource &&optionalSelfValue, bool isSuper,
      bool isDirectAccessorUse, PreparedArguments &&optionalSubscripts,
      SGFContext C, bool isOnSelfParameter,
      llvm::Optional<ActorIsolation> implicitActorHopTarget = llvm::None);

  void emitSetAccessor(SILLocation loc, SILDeclRef setter,
                       SubstitutionMap substitutions,
                       ArgumentSource &&optionalSelfValue,
                       bool isSuper, bool isDirectAccessorUse,
                       PreparedArguments &&optionalSubscripts,
                       ArgumentSource &&value,
                       bool isOnSelfParameter);

  ManagedValue emitAsyncLetStart(SILLocation loc,
                                 SILValue taskOptions,
                                 AbstractClosureExpr *asyncLetEntryPoint,
                                 SILValue resultBuf);

  void emitFinishAsyncLet(SILLocation loc, SILValue asyncLet, SILValue resultBuf);

  ManagedValue emitReadAsyncLetBinding(SILLocation loc, VarDecl *var);
  
  ManagedValue emitCancelAsyncTask(SILLocation loc, SILValue task);

  bool maybeEmitMaterializeForSetThunk(ProtocolConformanceRef conformance,
                                       SILLinkage linkage,
                                       Type selfInterfaceType, Type selfType,
                                       GenericEnvironment *genericEnv,
                                       AccessorDecl *requirement,
                                       AccessorDecl *witness,
                                       SubstitutionMap witnessSubs);

  ManagedValue emitAddressorAccessor(
      SILLocation loc, SILDeclRef addressor, SubstitutionMap substitutions,
      ArgumentSource &&optionalSelfValue, bool isSuper,
      bool isDirectAccessorUse,
      PreparedArguments &&optionalSubscripts,
      SILType addressType, bool isOnSelfParameter);

  CleanupHandle emitCoroutineAccessor(SILLocation loc, SILDeclRef accessor,
                                      SubstitutionMap substitutions,
                                      ArgumentSource &&optionalSelfValue,
                                      bool isSuper, bool isDirectAccessorUse,
                                      PreparedArguments &&optionalSubscripts,
                                      SmallVectorImpl<ManagedValue> &yields,
                                      bool isOnSelfParameter);

  RValue emitApplyConversionFunction(SILLocation loc,
                                     Expr *funcExpr,
                                     Type resultType,
                                     RValue &&operand);

  ManagedValue emitManagedRetain(SILLocation loc, SILValue v);
  ManagedValue emitManagedRetain(SILLocation loc, SILValue v,
                                 const TypeLowering &lowering);

  ManagedValue emitManagedLoadCopy(SILLocation loc, SILValue v);
  ManagedValue emitManagedLoadCopy(SILLocation loc, SILValue v,
                                   const TypeLowering &lowering);

  ManagedValue emitManagedStoreBorrow(SILLocation loc, SILValue v,
                                      SILValue addr);
  ManagedValue emitManagedStoreBorrow(SILLocation loc, SILValue v,
                                      SILValue addr,
                                      const TypeLowering &lowering);

  ManagedValue emitManagedLoadBorrow(SILLocation loc, SILValue v);
  ManagedValue emitManagedLoadBorrow(SILLocation loc, SILValue v,
                                     const TypeLowering &lowering);

  ManagedValue emitManagedBeginBorrow(SILLocation loc, SILValue v,
                                      const TypeLowering &lowering);
  ManagedValue emitManagedBeginBorrow(SILLocation loc, SILValue v);

  ManagedValue
  emitManagedBorrowedRValueWithCleanup(SILValue borrowedValue,
                                       const TypeLowering &lowering);
  ManagedValue emitManagedBorrowedRValueWithCleanup(SILValue borrowedValue);

  ManagedValue emitManagedBorrowedRValueWithCleanup(SILValue original,
                                                    SILValue borrowedValue);
  ManagedValue emitManagedBorrowedRValueWithCleanup(
      SILValue original, SILValue borrowedValue, const TypeLowering &lowering);
  ManagedValue emitManagedBorrowedArgumentWithCleanup(SILPhiArgument *arg);
  ManagedValue emitFormalEvaluationManagedBorrowedRValueWithCleanup(
      SILLocation loc, SILValue original, SILValue borrowedValue);
  ManagedValue emitFormalEvaluationManagedBorrowedRValueWithCleanup(
      SILLocation loc, SILValue original, SILValue borrowedValue,
      const TypeLowering &lowering);

  ManagedValue emitFormalEvaluationManagedBeginBorrow(SILLocation loc,
                                                      SILValue v);
  ManagedValue
  emitFormalEvaluationManagedBeginBorrow(SILLocation loc, SILValue v,
                                         const TypeLowering &lowering);

  ManagedValue emitFormalEvaluationManagedStoreBorrow(SILLocation loc,
                                                      SILValue v,
                                                      SILValue addr);

  ManagedValue emitManagedRValueWithCleanup(SILValue v);
  ManagedValue emitManagedRValueWithCleanup(SILValue v,
                                            const TypeLowering &lowering);

  ManagedValue emitManagedBufferWithCleanup(SILValue addr);
  ManagedValue emitManagedBufferWithCleanup(SILValue addr,
                                            const TypeLowering &lowering);

  ManagedValue emitManagedPackWithCleanup(SILValue addr,
                                          CanPackType formalPackType
                                            = CanPackType());

  ManagedValue emitFormalAccessManagedRValueWithCleanup(SILLocation loc,
                                                        SILValue value);
  ManagedValue emitFormalAccessManagedBufferWithCleanup(SILLocation loc,
                                                        SILValue addr);

  void emitSemanticLoadInto(SILLocation loc, SILValue src,
                            const TypeLowering &srcLowering,
                            SILValue dest,
                            const TypeLowering &destLowering,
                            IsTake_t isTake, IsInitialization_t isInit);

  SILValue emitSemanticLoad(SILLocation loc, SILValue src,
                            const TypeLowering &srcLowering,
                            const TypeLowering &rvalueLowering,
                            IsTake_t isTake);

  void emitSemanticStore(SILLocation loc, SILValue value,
                         SILValue dest, const TypeLowering &destTL,
                         IsInitialization_t isInit);
  
  SILValue emitConversionFromSemanticValue(SILLocation loc,
                                           SILValue semanticValue,
                                           SILType storageType);

  SILValue emitUnwrapIntegerResult(SILLocation loc, SILValue value);
  SILValue emitWrapIntegerLiteral(SILLocation loc, SILType ty,
                                  unsigned value);
  /// Load an r-value out of the given address. This does not handle
  /// reabstraction or bridging. If that is needed, use the other emit load
  /// entry point.
  ///
  /// \param rvalueTL - the type lowering for the type-of-rvalue
  ///   of the address
  /// \param isAddrGuaranteed - true if the value in this address
  ///   is guaranteed to be valid for the duration of the current
  ///   evaluation (see SGFContext::AllowGuaranteedPlusZero)
  ManagedValue emitLoad(SILLocation loc, SILValue addr,
                        const TypeLowering &rvalueTL,
                        SGFContext C, IsTake_t isTake,
                        bool isAddrGuaranteed = false);

  /// Load an r-value out of the given address handling re-abstraction and
  /// bridging if required.
  ///
  /// \param rvalueTL - the type lowering for the type-of-rvalue
  ///   of the address
  /// \param isAddrGuaranteed - true if the value in this address
  ///   is guaranteed to be valid for the duration of the current
  ///   evaluation (see SGFContext::AllowGuaranteedPlusZero)
  ManagedValue emitLoad(SILLocation loc, SILValue addr,
                        AbstractionPattern origFormalType,
                        CanType substFormalType,
                        const TypeLowering &rvalueTL,
                        SGFContext C, IsTake_t isTake,
                        bool isAddrGuaranteed = false);

  ManagedValue emitFormalAccessLoad(SILLocation loc, SILValue addr,
                                    const TypeLowering &rvalueTL, SGFContext C,
                                    IsTake_t isTake,
                                    bool isAddrGuaranteed = false);

  void emitAssignToLValue(SILLocation loc, ArgumentSource &&src, LValue &&dest);
  void emitAssignToLValue(SILLocation loc, RValue &&src, LValue &&dest);
  void emitAssignLValueToLValue(SILLocation loc,
                                LValue &&src, LValue &&dest);
  void emitCopyLValueInto(SILLocation loc, LValue &&src,
                          Initialization *dest);

  /// Emit an assignment to the variables in the destination pattern, given
  /// an rvalue source that has the same type as the pattern.
  void emitAssignToPatternVars(
      SILLocation loc, Pattern *destPattern, RValue &&src);

  ManagedValue emitAddressOfLValue(SILLocation loc, LValue &&src,
                                   TSanKind tsanKind = TSanKind::None);
  ManagedValue emitBorrowedLValue(SILLocation loc, LValue &&src,
                                  TSanKind tsanKind = TSanKind::None);
  ManagedValue emitConsumedLValue(SILLocation loc, LValue &&src,
                                  TSanKind tsanKind = TSanKind::None);
  LValue emitOpenExistentialLValue(SILLocation loc,
                                   LValue &&existentialLV,
                                   CanArchetypeType openedArchetype,
                                   CanType formalRValueType,
                                   SGFAccessKind accessKind);

  RValue emitLoadOfLValue(SILLocation loc, LValue &&src, SGFContext C,
                          bool isBaseLValueGuaranteed = false);

  /// Emit a reference to a method from within another method of the type.
  std::tuple<ManagedValue, SILType>
  emitSiblingMethodRef(SILLocation loc,
                       SILValue selfValue,
                       SILDeclRef methodConstant,
                       SubstitutionMap subMap);
  
  SILValue emitMetatypeOfValue(SILLocation loc, Expr *baseExpr);

  void emitReturnExpr(SILLocation loc, Expr *ret);

  void emitYield(SILLocation loc, MutableArrayRef<ArgumentSource> yieldValues,
                 ArrayRef<AbstractionPattern> origTypes,
                 JumpDest unwindDest);
  void emitRawYield(SILLocation loc, ArrayRef<ManagedValue> yieldArgs,
                    JumpDest unwindDest, bool isUniqueYield);

  RValue emitAnyHashableErasure(SILLocation loc,
                                ManagedValue value,
                                Type type,
                                ProtocolConformanceRef conformance,
                                SGFContext C);

  /// Turn a consumable managed value into a +1 managed value.
  ManagedValue getManagedValue(SILLocation loc,
                               ConsumableManagedValue value);

  /// Do the initial work common to all emissions of a pack
  /// expansion expression.  This is a placeholder meant to mark
  /// places that will need to support any sort of future feature
  /// where e.g. certain `each` operands need to be evaluated once
  /// for the entire expansion.
  void prepareToEmitPackExpansionExpr(PackExpansionExpr *E);

  //
  // Helpers for emitting ApplyExpr chains.
  //

  RValue emitApplyExpr(ApplyExpr *e, SGFContext c);

  /// Emit a function application, assuming that the arguments have been
  /// lowered appropriately for the abstraction level but that the
  /// result does need to be turned back into something matching a
  /// formal type.
  RValue emitApply(ResultPlanPtr &&resultPlan, ArgumentScope &&argScope,
                   SILLocation loc, ManagedValue fn, SubstitutionMap subs,
                   ArrayRef<ManagedValue> args,
                   const CalleeTypeInfo &calleeTypeInfo, ApplyOptions options,
                   SGFContext evalContext,
                   llvm::Optional<ActorIsolation> implicitActorHopTarget);

  RValue emitApplyOfDefaultArgGenerator(SILLocation loc,
                                        ConcreteDeclRef defaultArgsOwner,
                                        unsigned destIndex,
                                        CanType resultType,
                                        SGFContext C = SGFContext());

  RValue emitApplyOfStoredPropertyInitializer(
      SILLocation loc,
      VarDecl *anchoringVar,
      SubstitutionMap subs,
      CanType resultType,
      AbstractionPattern origResultType,
      SGFContext C);

  RValue emitApplyOfPropertyWrapperBackingInitializer(
      SILLocation loc,
      VarDecl *var,
      SubstitutionMap subs,
      RValue &&originalValue,
      SILDeclRef::Kind initKind = SILDeclRef::Kind::PropertyWrapperBackingInitializer,
      SGFContext C = SGFContext());

  /// A convenience method for emitApply that just handles monomorphic
  /// applications.
  RValue emitMonomorphicApply(
      SILLocation loc, ManagedValue fn, ArrayRef<ManagedValue> args,
      CanType foreignResultType, CanType nativeResultType, ApplyOptions options,
      llvm::Optional<SILFunctionTypeRepresentation> overrideRep,
      const llvm::Optional<ForeignErrorConvention> &foreignError,
      SGFContext ctx = SGFContext());

  RValue emitApplyOfLibraryIntrinsic(SILLocation loc,
                                     FuncDecl *fn,
                                     SubstitutionMap subMap,
                                     ArrayRef<ManagedValue> args,
                                     SGFContext ctx);

  RValue emitApplyOfLibraryIntrinsic(SILLocation loc, SILDeclRef declRef,
                                     SubstitutionMap subMap,
                                     ArrayRef<ManagedValue> args,
                                     SGFContext ctx);

  /// Emits a call to the `_diagnoseUnavailableCodeReached()` function in the
  /// standard library.
  void emitApplyOfUnavailableCodeReached();

  RValue emitApplyAllocatingInitializer(SILLocation loc, ConcreteDeclRef init,
                                        PreparedArguments &&args, Type overriddenSelfType,
                                        SGFContext ctx);

  CleanupHandle emitBeginApply(SILLocation loc, ManagedValue fn,
                               SubstitutionMap subs, ArrayRef<ManagedValue> args,
                               CanSILFunctionType substFnType,
                               ApplyOptions options,
                               SmallVectorImpl<ManagedValue> &yields);

  SILValue emitApplyWithRethrow(SILLocation loc, SILValue fn,
                                SILType substFnType,
                                SubstitutionMap subs,
                                ArrayRef<SILValue> args);

  std::pair<MultipleValueInstructionResult *, CleanupHandle>
  emitBeginApplyWithRethrow(SILLocation loc, SILValue fn, SILType substFnType,
                            SubstitutionMap subs, ArrayRef<SILValue> args,
                            SmallVectorImpl<SILValue> &yields);
  void emitEndApplyWithRethrow(SILLocation loc,
                               MultipleValueInstructionResult *token);

  /// Emit a literal that applies the various initializers.
  RValue emitLiteral(LiteralExpr *literal, SGFContext C);

  SILBasicBlock *getTryApplyErrorDest(SILLocation loc,
                                      CanSILFunctionType fnTy,
                                      ExecutorBreadcrumb prevExecutor,
                                      SILResultInfo exnResult,
                                      bool isSuppressed);

  /// Emit a dynamic member reference.
  RValue emitDynamicMemberRef(SILLocation loc, SILValue operand,
                              ConcreteDeclRef memberRef, CanType refTy,
                              SGFContext C);

  /// Emit a dynamic subscript getter application.
  RValue emitDynamicSubscriptGetterApply(SILLocation loc, SILValue operand,
                                         ConcreteDeclRef subscriptRef,
                                         PreparedArguments &&indexArgs,
                                         CanType resultTy, SGFContext C);

  /// Open up the given existential expression and emit its
  /// subexpression in a caller-specified manner.
  ///
  /// \param e The expression.
  ///
  /// \param emitSubExpr A function to call to emit the subexpression
  /// (which will be passed in).
  void emitOpenExistentialExprImpl(OpenExistentialExpr *e,
                                  llvm::function_ref<void(Expr *)> emitSubExpr);

  /// Open up the given existential expression and emit its
  /// subexpression in a caller-specified manner.
  ///
  /// \param e The expression.
  ///
  /// \param emitSubExpr A function to call to emit the subexpression
  /// (which will be passed in).
  template<typename R, typename F>
  R emitOpenExistentialExpr(OpenExistentialExpr *e, F emitSubExpr) {
    llvm::Optional<R> result;
    emitOpenExistentialExprImpl(e,
                            [&](Expr *subExpr) {
                              result.emplace(emitSubExpr(subExpr));
                            });
    return std::move(*result);
  }

  /// Open up the given existential expression and emit its
  /// subexpression in a caller-specified manner.
  ///
  /// \param e The expression.
  ///
  /// \param emitSubExpr A function to call to emit the subexpression
  /// (which will be passed in).
  template<typename F>
  void emitOpenExistentialExpr(OpenExistentialExpr *e, F emitSubExpr) {
    emitOpenExistentialExprImpl(e, emitSubExpr);
  }

  /// Mapping from active opaque value expressions to their values.
  llvm::SmallDenseMap<OpaqueValueExpr *, ManagedValue>
    OpaqueValues;

  /// A mapping from opaque value expressions to the open-existential
  /// expression that determines them, used while lowering lvalues.
  llvm::SmallDenseMap<OpaqueValueExpr *, OpenExistentialExpr *>
    OpaqueValueExprs;

  /// RAII object that introduces a temporary binding for an opaque value.
  ///
  /// Each time the opaque value expression is referenced, it will be
  /// retained/released separately. When this RAII object goes out of
  /// scope, the value will be destroyed if requested.
  class OpaqueValueRAII {
    SILGenFunction &Self;
    OpaqueValueExpr *OpaqueValue;

    OpaqueValueRAII(const OpaqueValueRAII &) = delete;
    OpaqueValueRAII &operator=(const OpaqueValueRAII &) = delete;

  public:
    OpaqueValueRAII(SILGenFunction &self, OpaqueValueExpr *opaqueValue,
                    ManagedValue value)
    : Self(self), OpaqueValue(opaqueValue) {
      assert(Self.OpaqueValues.count(OpaqueValue) == 0 &&
             "Opaque value already has a binding");
      Self.OpaqueValues[OpaqueValue] = value;
    }

    ~OpaqueValueRAII();
  };

  /// Emit a conditional checked cast branch. Does not
  /// re-abstract the argument to the success branch. Terminates the
  /// current BB.
  ///
  /// \param loc          The AST location associated with the operation.
  /// \param src          The abstract value to cast.
  /// \param sourceType   The formal source type.
  /// \param targetType   The formal target type.
  /// \param C            Information about the result of the cast.
  /// \param handleTrue   A callback to invoke with the result of the cast
  ///                     in the success path.  The current BB should be
  ///                     terminated.
  /// \param handleFalse  A callback to invoke in the failure path.  The
  ///                     current BB should be terminated.
  void emitCheckedCastBranch(
      SILLocation loc, ConsumableManagedValue src, Type sourceType,
      CanType targetType, SGFContext C,
      llvm::function_ref<void(ManagedValue)> handleTrue,
      llvm::function_ref<void(llvm::Optional<ManagedValue>)> handleFalse,
      ProfileCounter TrueCount = ProfileCounter(),
      ProfileCounter FalseCount = ProfileCounter());

  /// Emit a conditional checked cast branch, starting from an
  /// expression.  Terminates the current BB.
  ///
  /// \param loc          The AST location associated with the operation.
  /// \param src          An expression which will generate the value to cast.
  /// \param targetType   The formal target type.
  /// \param C            Information about the result of the cast.
  /// \param handleTrue   A callback to invoke with the result of the cast
  ///                     in the success path.  The current BB should be
  ///                     terminated.
  /// \param handleFalse  A callback to invoke in the failure path.  The
  ///                     current BB should be terminated.
  void emitCheckedCastBranch(
      SILLocation loc, Expr *src, Type targetType, SGFContext C,
      llvm::function_ref<void(ManagedValue)> handleTrue,
      llvm::function_ref<void(llvm::Optional<ManagedValue>)> handleFalse,
      ProfileCounter TrueCount = ProfileCounter(),
      ProfileCounter FalseCount = ProfileCounter());

  /// Emit the control flow for an optional 'bind' operation, branching to the
  /// active failure destination if the optional value addressed by optionalAddr
  /// is nil, and leaving the insertion point on the success branch.
  ///
  /// NOTE: This operation does consume the managed value.
  ManagedValue emitBindOptional(SILLocation loc,
                                ManagedValue optionalAddrOrValue,
                                unsigned depth);

  /// Emit the control flow for an optional 'bind' operation, branching to the
  /// active failure destination if the optional value addressed by optionalAddr
  /// is nil, and leaving the insertion point on the success branch.
  ///
  /// NOTE: This operation does not consume the managed address.
  void emitBindOptionalAddress(SILLocation loc, ManagedValue optionalAddr,
                               unsigned depth);

  void emitOptionalEvaluation(SILLocation loc, Type optionalType,
                              SmallVectorImpl<ManagedValue> &results,
                              SGFContext C,
                      llvm::function_ref<void(SmallVectorImpl<ManagedValue> &,
                                              SGFContext primaryC)>
                                generateNormalResults);

  //===--------------------------------------------------------------------===//
  // Bridging thunks
  //===--------------------------------------------------------------------===//

  /// Convert a native Swift value to a value that can be passed as an argument
  /// to or returned as the result of a function with the given calling
  /// convention.
  ManagedValue emitNativeToBridgedValue(SILLocation loc, ManagedValue v,
                                        CanType nativeType,
                                        CanType bridgedType,
                                        SILType loweredBridgedType,
                                        SGFContext C = SGFContext());
  
  /// Convert a value received as the result or argument of a function with
  /// the given calling convention to a native Swift value of the given type.
  ManagedValue emitBridgedToNativeValue(SILLocation loc, ManagedValue v,
                                        CanType bridgedType,
                                        CanType nativeType,
                                        SILType loweredNativeType,
                                        SGFContext C = SGFContext(),
                                        bool isCallResult = false);

  /// Convert a bridged error type to the native Swift Error
  /// representation.  The value may be optional.
  ManagedValue emitBridgedToNativeError(SILLocation loc, ManagedValue v);

  /// Convert a value in the native Swift Error representation to
  /// a bridged error type representation.
  ManagedValue emitNativeToBridgedError(SILLocation loc, ManagedValue v,
                                        CanType nativeType,
                                        CanType bridgedType);
  
  SILValue emitBridgeErrorForForeignError(SILLocation loc,
                                          SILValue nativeError,
                                          SILType bridgedResultType,
                                          SILValue foreignErrorSlot,
                                    const ForeignErrorConvention &foreignError);

  SILValue
  emitBridgeReturnValueForForeignError(SILLocation loc,
                                       SILValue result,
                                       CanType formalNativeType,
                                       CanType formalBridgedType,
                                       SILType bridgedType,
                                       SILValue foreignErrorSlot,
                                 const ForeignErrorConvention &foreignError);

  SILValue
  emitForeignErrorBlock(SILLocation loc, SILBasicBlock *errorBB,
                        llvm::Optional<ManagedValue> errorSlot,
                        llvm::Optional<ForeignAsyncConvention> foreignAsync);

  SILValue
  emitForeignErrorCheck(SILLocation loc,
                        SmallVectorImpl<ManagedValue> &directResults,
                        ManagedValue errorSlot, bool suppressErrorCheck,
                        const ForeignErrorConvention &foreignError,
                        llvm::Optional<ForeignAsyncConvention> foreignAsync);

  //===--------------------------------------------------------------------===//
  // Re-abstraction thunks
  //===--------------------------------------------------------------------===//

  /// Convert a value with the abstraction patterns of the original type
  /// to a value with the abstraction patterns of the substituted type.
  ManagedValue emitOrigToSubstValue(SILLocation loc, ManagedValue input,
                                    AbstractionPattern origType,
                                    CanType substType,
                                    SGFContext ctx = SGFContext());
  ManagedValue emitOrigToSubstValue(SILLocation loc, ManagedValue input,
                                    AbstractionPattern origType,
                                    CanType substType,
                                    SILType loweredResultTy,
                                    SGFContext ctx = SGFContext());
  RValue emitOrigToSubstValue(SILLocation loc, RValue &&input,
                              AbstractionPattern origType,
                              CanType substType,
                              SGFContext ctx = SGFContext());
  RValue emitOrigToSubstValue(SILLocation loc, RValue &&input,
                              AbstractionPattern origType,
                              CanType substType,
                              SILType loweredResultTy,
                              SGFContext ctx = SGFContext());

  /// Convert a value with the abstraction patterns of the substituted
  /// type to a value with the abstraction patterns of the original type.
  ManagedValue emitSubstToOrigValue(SILLocation loc, ManagedValue input,
                                    AbstractionPattern origType,
                                    CanType substType,
                                    SGFContext ctx = SGFContext());
  RValue emitSubstToOrigValue(SILLocation loc, RValue &&input,
                              AbstractionPattern origType,
                              CanType substType,
                              SGFContext ctx = SGFContext());
  ManagedValue emitSubstToOrigValue(SILLocation loc, ManagedValue input,
                                    AbstractionPattern origType,
                                    CanType substType,
                                    SILType loweredResultTy,
                                    SGFContext ctx = SGFContext());
  RValue emitSubstToOrigValue(SILLocation loc, RValue &&input,
                              AbstractionPattern origType,
                              CanType substType,
                              SILType loweredResultTy,
                              SGFContext ctx = SGFContext());

  /// Transform the AST-level types in the function signature without an
  /// abstraction or representation change.
  ManagedValue emitTransformedValue(SILLocation loc, ManagedValue input,
                                    CanType inputType,
                                    CanType outputType,
                                    SGFContext ctx = SGFContext());

  /// Most general form of the above.
  ManagedValue emitTransformedValue(SILLocation loc, ManagedValue input,
                                    AbstractionPattern inputOrigType,
                                    CanType inputSubstType,
                                    AbstractionPattern outputOrigType,
                                    CanType outputSubstType,
                                    SILType loweredResultTy,
                                    SGFContext ctx = SGFContext());
  RValue emitTransformedValue(SILLocation loc, RValue &&input,
                              AbstractionPattern inputOrigType,
                              CanType inputSubstType,
                              AbstractionPattern outputOrigType,
                              CanType outputSubstType,
                              SILType loweredResultTy,
                              SGFContext ctx = SGFContext());

  /// Used for emitting SILArguments of bare functions, such as thunks.
  void collectThunkParams(
      SILLocation loc, SmallVectorImpl<ManagedValue> &params,
      SmallVectorImpl<SILArgument *> *indirectResultParams = nullptr);

  /// Build the type of a function transformation thunk.
  CanSILFunctionType buildThunkType(CanSILFunctionType &sourceType,
                                    CanSILFunctionType &expectedType,
                                    CanType &inputSubstType,
                                    CanType &outputSubstType,
                                    GenericEnvironment *&genericEnv,
                                    SubstitutionMap &interfaceSubs,
                                    CanType &dynamicSelfType,
                                    bool withoutActuallyEscaping=false);

  //===--------------------------------------------------------------------===//
  // NoEscaping to Escaping closure thunk
  //===--------------------------------------------------------------------===//
  ManagedValue
  createWithoutActuallyEscapingClosure(SILLocation loc,
                                       ManagedValue noEscapingFunctionValue,
                                       SILType escapingFnTy);

  //===--------------------------------------------------------------------===//
  // Differentiation thunks
  //===--------------------------------------------------------------------===//

  /// Get or create a thunk for reabstracting and self-reordering
  /// differentials/pullbacks returned by user-defined JVP/VJP functions, and
  /// apply it to the given differential/pullback.
  ///
  /// If `reorderSelf` is true, reorder self so that it appears as:
  /// - The last parameter, for differentials.
  /// - The last result, for pullbacks.
  ManagedValue getThunkedAutoDiffLinearMap(ManagedValue linearMap,
                                           AutoDiffLinearMapKind linearMapKind,
                                           CanSILFunctionType fromType,
                                           CanSILFunctionType toType,
                                           bool reorderSelf);

  //===--------------------------------------------------------------------===//
  // Back Deployment thunks
  //===--------------------------------------------------------------------===//

  /// Invokes an original function if it is available at runtime. Otherwise,
  /// invokes a fallback copy of the function emitted into the client.
  void emitBackDeploymentThunk(SILDeclRef thunk);

  //===---------------------------------------------------------------------===//
  // Distributed Actors
  //===---------------------------------------------------------------------===//

  /// Initializes the implicit stored properties of a distributed actor that correspond to
  /// its transport and identity.
  void emitDistributedActorImplicitPropertyInits(
      ConstructorDecl *ctor, ManagedValue selfArg);

  /// Initializes just the implicit identity property of a distributed actor.
  /// \param selfVal a value corresponding to the actor's self
  /// \param actorSystemVal a value corresponding to the actorSystem, to be used
  /// to invoke its \p assignIdentity method.
  void emitDistActorIdentityInit(ConstructorDecl *ctor,
                                 SILLocation loc,
                                 SILValue selfVal,
                                 SILValue actorSystemVal);

  /// Given a function representing a distributed actor factory, emits the
  /// corresponding SIL function for it.
  void emitDistributedActorFactory(FuncDecl *fd); // TODO(distributed): this is the "resolve"

  void emitDistributedIfRemoteBranch(SILLocation Loc,
                                     ManagedValue selfValue, Type selfTy,
                                     SILBasicBlock *isRemoteBB,
                                     SILBasicBlock *isLocalBB);

  /// Notify transport that actor has initialized successfully,
  /// and is ready to receive messages.
  void emitDistributedActorReady(
      SILLocation loc, ConstructorDecl *ctor, ManagedValue actorSelf);
  
  /// For a distributed actor, emits code to invoke the system's
  /// resignID function.
  ///
  /// Specifically, this code emits SIL that performs the call
  ///
  /// \verbatim
  ///   self.actorSystem.resignID(self.id)
  /// \endverbatim
  ///
  /// using the current builder's state as the injection point.
  ///
  /// \param actorDecl the declaration corresponding to the actor
  /// \param actorSelf the SIL value representing the distributed actor instance
  void emitDistributedActorSystemResignIDCall(SILLocation loc,
                              ClassDecl *actorDecl, ManagedValue actorSelf);
  
  /// Emit code that tests whether the distributed actor is local, and if so,
  /// resigns the distributed actor's identity.
  /// \param continueBB the target block where execution will continue after
  ///                   the conditional call, whether actor is local or remote.
  void emitConditionalResignIdentityCall(SILLocation loc,
                                         ClassDecl *actorDecl,
                                         ManagedValue actorSelf,
                                         SILBasicBlock *continueBB,
                                         SILBasicBlock *finishBB);

  void emitDistributedActorClassMemberDestruction(
      SILLocation cleanupLoc, ManagedValue selfValue, ClassDecl *cd,
      SILBasicBlock *normalMemberDestroyBB,
      SILBasicBlock *remoteMemberDestroyBB,
      SILBasicBlock *finishBB);

  //===--------------------------------------------------------------------===//
  // Declarations
  //===--------------------------------------------------------------------===//
  
  void visitDecl(Decl *D) {
    llvm_unreachable("Not yet implemented");
  }

  // Emitted as part of its storage.
  void visitAccessorDecl(AccessorDecl *D) {}

  void visitFuncDecl(FuncDecl *D);
  /// \param generateDebugInfo  Pattern bindings inside of capture list
  /// expressions should not introduce new variables into the debug info.
  void visitPatternBindingDecl(PatternBindingDecl *D,
                               bool generateDebugInfo = true);

  void emitPatternBinding(PatternBindingDecl *D, unsigned entry,
                          bool generateDebugInfo);

  std::unique_ptr<Initialization>
  emitPatternBindingInitialization(Pattern *P, JumpDest failureDest,
                                   bool generateDebugInfo = true);

  void visitNominalTypeDecl(NominalTypeDecl *D) {
    // No lowering support needed.
  }

  void visitTypeAliasDecl(TypeAliasDecl *D) {
    // No lowering support needed.
  }

  void visitGenericTypeParamDecl(GenericTypeParamDecl *D) {
    // No lowering support needed.
  }
  void visitAssociatedTypeDecl(AssociatedTypeDecl *D) {
    // No lowering support needed.
  }

  void visitPoundDiagnosticDecl(PoundDiagnosticDecl *D) {
    // No lowering support needed.
  }

  void visitVarDecl(VarDecl *D);

  void visitMacroExpansionDecl(MacroExpansionDecl *D);

  /// Emit an Initialization for a 'var' or 'let' decl in a pattern.
  std::unique_ptr<Initialization>
  emitInitializationForVarDecl(VarDecl *vd, bool immutable,
                               bool generateDebugInfo = true);

  /// Emit the allocation for a local variable, provides an Initialization
  /// that can be used to initialize it, and registers cleanups in the active
  /// scope.
  /// \param ArgNo optionally describes this function argument's
  /// position for debug info.
  std::unique_ptr<Initialization> emitLocalVariableWithCleanup(
      VarDecl *D, llvm::Optional<MarkUninitializedInst::Kind> kind,
      unsigned ArgNo = 0, bool generateDebugInfo = true);

  /// Emit the allocation for a local temporary, provides an
  /// Initialization that can be used to initialize it, and registers
  /// cleanups in the active scope.
  ///
  /// The initialization is guaranteed to be a single buffer.
  std::unique_ptr<TemporaryInitialization>
  emitTemporary(SILLocation loc, const TypeLowering &tempTL);

  /// Emit the allocation for a local temporary, provides an
  /// Initialization that can be used to initialize it, and registers
  /// cleanups in the current active formal evaluation scope.
  ///
  /// The initialization is guaranteed to be a single buffer.
  std::unique_ptr<TemporaryInitialization>
  emitFormalAccessTemporary(SILLocation loc, const TypeLowering &tempTL);

  /// Provides an Initialization that can be used to initialize an already-
  /// allocated temporary, and registers cleanups in the active scope.
  ///
  /// The initialization is guaranteed to be a single buffer.
  std::unique_ptr<TemporaryInitialization>
  useBufferAsTemporary(SILValue addr, const TypeLowering &tempTL);

  /// Enter a currently-dormant cleanup to destroy the value in the
  /// given address.
  CleanupHandle enterDormantTemporaryCleanup(SILValue temp,
                                             const TypeLowering &tempTL);

  CleanupHandle enterDeallocBoxCleanup(SILValue box);

  /// Enter a currently-dormant cleanup to destroy the value in the
  /// given address.
  CleanupHandle
  enterDormantFormalAccessTemporaryCleanup(SILValue temp, SILLocation loc,
                                           const TypeLowering &tempTL);

  /// Destroy and deallocate an initialized local variable.
  void destroyLocalVariable(SILLocation L, VarDecl *D);

  /// Destroy the class member.
  void destroyClassMember(SILLocation L, ManagedValue selfValue, VarDecl *D);

  /// Enter a cleanup to deallocate a stack variable.
  CleanupHandle enterDeallocStackCleanup(SILValue address);

  /// Enter a cleanup to deallocate a pack.
  CleanupHandle enterDeallocPackCleanup(SILValue address);
  
  /// Enter a cleanup to emit a ReleaseValue/DestroyAddr of the specified value.
  CleanupHandle enterDestroyCleanup(SILValue valueOrAddr);

  /// Enter a cleanup to destroy all of the values in the given pack.
  CleanupHandle enterDestroyPackCleanup(SILValue addr,
                                        CanPackType formalPackType);

  /// Enter a cleanup to destroy the preceding values in a pack-expansion
  /// component of a pack.
  ///
  /// \param limitWithinComponent - if non-null, the number of elements
  ///   to destroy in the pack expansion component; defaults to the
  ///   dynamic length of the expansion component
  CleanupHandle enterPartialDestroyPackCleanup(SILValue addr,
                                               CanPackType formalPackType,
                                               unsigned componentIndex,
                                               SILValue limitWithinComponent);

  /// Enter a cleanup to destroy the following values in a
  /// pack-expansion component of a pack.  Note that this only destroys
  /// the values *in that specific component*, not all the other values
  /// in the pack.
  ///
  /// \param currentIndexWithinComponent - the current index in the
  ///   pack expansion component; any elements in the component that
  ///   *follow* this component will be destroyed. If nil, all the
  ///   elements in the component will be destroyed
  CleanupHandle
  enterPartialDestroyRemainingPackCleanup(SILValue addr,
                                          CanPackType formalPackType,
                                          unsigned componentIndex,
                                          SILValue currentIndexWithinComponent);

  /// Enter a cleanup to destroy all of the components in a pack starting
  /// at a particular component index.
  CleanupHandle
  enterDestroyRemainingPackComponentsCleanup(SILValue addr,
                                             CanPackType formalPackType,
                                             unsigned componentIndex);

  /// Enter a cleanup to destroy the preceding values in a pack-expansion
  /// component of a tuple.
  ///
  /// \param limitWithinComponent - if non-null, the number of elements
  ///   to destroy in the pack expansion component; defaults to the
  ///   dynamic length of the expansion component
  CleanupHandle enterPartialDestroyTupleCleanup(SILValue addr,
                                                CanPackType inducedPackType,
                                                unsigned componentIndex,
                                                SILValue limitWithinComponent);

  /// Enter a cleanup to destroy the following values in a
  /// pack-expansion component of a tuple.  Note that this only destroys
  /// the values *in that specific component*, not all the other values
  /// in the tuple.
  ///
  /// \param currentIndexWithinComponent - the current index in the
  ///   pack expansion component; any elements in the component that
  ///   *follow* this component will be destroyed. If nil, all the
  ///   elements in the component will be destroyed
  CleanupHandle
  enterPartialDestroyRemainingTupleCleanup(SILValue addr,
                                           CanPackType inducedPackType,
                                           unsigned componentIndex,
                                           SILValue currentIndexWithinComponent);

  /// Enter a cleanup to destroy all of the components in a tuple starting
  /// at a particular component index.
  CleanupHandle
  enterDestroyRemainingTupleElementsCleanup(SILValue addr,
                                            CanPackType inducedPackType,
                                            unsigned componentIndex);

  /// Copy the elements of a pack, which must consist of a single pack expansion,
  /// into a tuple value having the same pack expansion and its sole element type.
  void copyPackElementsToTuple(SILLocation loc, SILValue tupleAddr, SILValue pack,
                               CanPackType formalPackType);

  /// Initialize a pack with the addresses of the elements of a tuple, which must
  /// consist of a single pack expansion.
  void projectTupleElementsToPack(SILLocation loc, SILValue tupleAddr, SILValue pack,
                                  CanPackType formalPackType);

  /// Return an owned managed value for \p value that is cleaned up using an end_lifetime instruction.
  ///
  /// The end_lifetime cleanup is not placed into the ManagedValue itself and
  /// thus can not be forwarded. This means that the ManagedValue is treated
  /// as a +0 value. This means that the owned value will be copied by SILGen
  /// if it is ever needed as a +1 value (meaning any time that the value
  /// escapes).
  ///
  /// DISCUSSION: end_lifetime ends the lifetime of an owned value in OSSA
  /// without resulting in a destroy being emitted. This cleanup should only
  /// be used for owned values that do not need to be destroyed if they do not
  /// escape the current call frame but need to be copied if they escape.
  ManagedValue emitManagedRValueWithEndLifetimeCleanup(SILValue value);

  /// Enter a cleanup to emit a DeinitExistentialAddr or DeinitExistentialBox
  /// of the specified value.
  CleanupHandle enterDeinitExistentialCleanup(CleanupState state,
                                              SILValue addr,
                                              CanType concreteFormalType,
                                              ExistentialRepresentation repr);

  /// Enter a cleanup to cancel the given task.
  CleanupHandle enterCancelAsyncTaskCleanup(SILValue task);

  // Enter a cleanup to cancel and destroy an AsyncLet as it leaves the scope.
  CleanupHandle enterAsyncLetCleanup(SILValue alet, SILValue resultBuf);

  /// Evaluate an Expr as an lvalue.
  LValue emitLValue(Expr *E, SGFAccessKind accessKind,
                    LValueOptions options = LValueOptions());

  RValue emitRValueForNonMemberVarDecl(SILLocation loc,
                                       ConcreteDeclRef declRef,
                                       CanType formalRValueType,
                                       AccessSemantics semantics,
                                       SGFContext C);

  /// Emit an lvalue that directly refers to the given instance variable
  /// (without going through getters or setters).
  LValue emitPropertyLValue(SILLocation loc, ManagedValue base,
                            CanType baseFormalType, VarDecl *var,
                            LValueOptions options,
                            SGFAccessKind accessKind,
                            AccessSemantics semantics);

  struct PointerAccessInfo {
    CanType PointerType;
    PointerTypeKind PointerKind;
    SGFAccessKind AccessKind;
  };

  PointerAccessInfo getPointerAccessInfo(Type pointerType);
  ManagedValue emitLValueToPointer(SILLocation loc, LValue &&lvalue,
                                   PointerAccessInfo accessInfo);

  struct ArrayAccessInfo {
    Type PointerType;
    Type ArrayType;
    SGFAccessKind AccessKind;
  };
  ArrayAccessInfo getArrayAccessInfo(Type pointerType, Type arrayType);
  std::pair<ManagedValue,ManagedValue>
  emitArrayToPointer(SILLocation loc, LValue &&lvalue,
                     ArrayAccessInfo accessInfo);

  std::pair<ManagedValue,ManagedValue>
  emitArrayToPointer(SILLocation loc, ManagedValue arrayValue,
                     ArrayAccessInfo accessInfo);

  std::pair<ManagedValue,ManagedValue>
  emitStringToPointer(SILLocation loc, ManagedValue stringValue,
                      Type pointerType);

  class ForceTryEmission {
    SILGenFunction &SGF;
    ForceTryExpr *Loc;
    JumpDest OldThrowDest;

  public:
    ForceTryEmission(SILGenFunction &SGF, ForceTryExpr *loc);

    ForceTryEmission(const ForceTryEmission &) = delete;
    ForceTryEmission &operator=(const ForceTryEmission &) = delete;

    void finish();

    ~ForceTryEmission() {
      if (Loc) finish();
    }
  };

  /// Return forwarding substitutions for the archetypes in the current
  /// function.
  SubstitutionMap getForwardingSubstitutionMap();

  /// Get the _Pointer protocol used for pointer argument operations.
  ProtocolDecl *getPointerProtocol();

  /// Returns the SILDeclRef to use for references to the given accessor.
  SILDeclRef getAccessorDeclRef(AccessorDecl *accessor) {
    return SGM.getAccessorDeclRef(accessor, F.getResilienceExpansion());
  }

  /// Given a lowered pack expansion type, produce a generic environment
  /// sufficient for doing value operations on it and map the type into
  /// the environment.
  std::pair<GenericEnvironment*, SILType>
  createOpenedElementValueEnvironment(SILType packExpansionTy);

  GenericEnvironment *
  createOpenedElementValueEnvironment(ArrayRef<SILType> packExpansionTys,
                                      ArrayRef<SILType*> eltTys);

  /// Emit a dynamic loop over a single pack-expansion component of a pack.
  ///
  /// \param formalPackType - a pack type with the right shape for the
  ///   overall pack being iterated over
  /// \param componentIndex - the index of the pack expansion component
  ///   within the formal pack type
  /// \param startingAfterIndexWithinComponent - the index prior to the
  ///   first index within the component to dynamically visit; if null,
  ///   visitation will start at 0
  /// \param limitWithinComponent - the number of elements in a prefix of
  ///   the expansion component to dynamically visit; if null, all elements
  ///   will be visited
  /// \param openedElementEnv - a set of opened element archetypes to bind
  ///   within the loop; can be null to bind no elements
  /// \param reverse - if true, iterate the elements in reverse order,
  ///   starting at index limitWithinComponent - 1
  /// \param emitBody - a function that will be called to emit the body of
  ///   the loop. It's okay if this has paths that exit the body of the loop,
  ///   but it should leave the insertion point set at the end.
  ///
  ///   The first parameter is the current index within the expansion
  ///   component, a value of type Builtin.Word.  The second parameter is
  ///   that index as a pack indexing instruction that indexes into packs
  ///   with the shape of the pack expasion.  The third parameter is the
  ///   current pack index within the overall pack, a pack indexing instruction
  ///   that indexes into packs with the shape of formalPackType.
  ///
  ///   This function will be called within a cleanups scope and with
  ///   InnermostPackExpansion set up properly for the context.
  void emitDynamicPackLoop(SILLocation loc,
                           CanPackType formalPackType,
                           unsigned componentIndex,
                           SILValue startingAfterIndexWithinComponent,
                           SILValue limitWithinComponent,
                           GenericEnvironment *openedElementEnv,
                           bool reverse,
                        llvm::function_ref<void(SILValue indexWithinComponent,
                                                SILValue packExpansionIndex,
                                                SILValue packIndex)> emitBody);

  /// A convenience version of dynamic pack loop that visits an entire
  /// pack expansion component in forward order.
  void emitDynamicPackLoop(SILLocation loc,
                           CanPackType formalPackType,
                           unsigned componentIndex,
                           GenericEnvironment *openedElementEnv,
                        llvm::function_ref<void(SILValue indexWithinComponent,
                                                SILValue packExpansionIndex,
                                                SILValue packIndex)> emitBody);

  /// Emit a transform on each element of a pack-expansion component
  /// of a pack, write the result into a pack-expansion component of
  /// another pack.
  ///
  /// \param inputPackAddr - the address of the input pack; the cleanup
  ///   on this pack should be a cleanup for just the pack component,
  ///   not for the entire pack
  ManagedValue emitPackTransform(SILLocation loc,
                                 ManagedValue inputPackAddr,
                                 CanPackType inputFormalPackType,
                                 unsigned inputComponentIndex,
                                 SILValue outputPackAddr,
                                 CanPackType outputFormalPackType,
                                 unsigned outputComponentIndex,
                                 bool isSimpleProjection,
                                 bool outputIsPlusOne,
              llvm::function_ref<ManagedValue(ManagedValue input,
                                              SILType outputTy,
                                              SGFContext context)> emitBody);

  /// Emit a loop which destroys a prefix of a pack expansion component
  /// of a pack value.
  ///
  /// \param packAddr - the address of the overall pack value
  /// \param formalPackType - a pack type with the same shape as the
  ///   overall pack value
  /// \param componentIndex - the index of the pack expansion component
  ///   within the formal pack type
  /// \param limitWithinComponent - the number of elements in a prefix of
  ///   the expansion component to destroy; if null, all elements in the
  ///   component will be destroyed
  void emitPartialDestroyPack(SILLocation loc,
                              SILValue packAddr,
                              CanPackType formalPackType,
                              unsigned componentIndex,
                              SILValue limitWithinComponent);

  /// Emit a loop which destroys all the elements of a pack value.
  ///
  /// \param packAddr - the address of the overall pack value
  /// \param formalPackType - a pack type with the same shape as the
  ///   overall pack value
  void emitDestroyPack(SILLocation loc,
                       SILValue packAddr,
                       CanPackType formalPackType,
                       unsigned firstComponentIndex = 0);

  /// Emit instructions to destroy a suffix of a tuple value.
  ///
  /// \param tupleAddr - the address of the overall tuple value
  /// \param inducedPackType - a pack type with the same shape as the
  ///   element types of the overall tuple value; can be null if the
  ///   tuple type doesn't contain pack expansions
  /// \param componentIndex - the index of the first component to
  ///   destroy in the tuple
  void emitDestroyRemainingTupleElements(SILLocation loc,
                                         SILValue tupleAddr,
                                         CanPackType inducedPackType,
                                         unsigned componentIndex);

  /// Emit a loop which destroys a prefix of a pack expansion component
  /// of a tuple value.
  ///
  /// \param tupleAddr - the address of the overall tuple value
  /// \param inducedPackType - a pack type with the same shape as the
  ///   element types of the overall tuple value
  /// \param componentIndex - the index of the pack expansion component
  ///   within the tuple
  /// \param limitWithinComponent - the number of elements in a prefix of
  ///   the expansion component to destroy; if null, all elements in the
  ///   component will be destroyed
  void emitPartialDestroyTuple(SILLocation loc,
                               SILValue tupleAddr,
                               CanPackType inducedPackType,
                               unsigned componentIndex,
                               SILValue limitWithinComponent);

  /// Emit a loop which destroys a suffix of a pack expansion component
  /// of a tuple value.
  ///
  /// \param tupleAddr - the address of the overall tuple value
  /// \param inducedPackType - a pack type with the same shape as the
  ///   element types of the overall tuple value
  /// \param componentIndex - the index of the pack expansion component
  ///   within the tuple
  /// \param currentIndexWithinComponent - the current index in the
  ///   pack expansion component; all elements *following* this index will
  ///   be destroyed
  void emitPartialDestroyRemainingTuple(SILLocation loc,
                                        SILValue tupleAddr,
                                        CanPackType inducedPackType,
                                        unsigned componentIndex,
                                        SILValue currentIndexWithinComponent);

  /// Emit a loop which destroys a suffix of a pack expansion component
  /// of a pack value.
  ///
  /// \param packAddr - the address of the overall pack value
  /// \param formalPackType - a pack type with the same shape as the
  ///   component types of the overall pack value
  /// \param componentIndex - the index of the pack expansion component
  ///   within the pack
  /// \param currentIndexWithinComponent - the current index in the
  ///   pack expansion component; all elements *following* this index will
  ///   be destroyed
  void emitPartialDestroyRemainingPack(SILLocation loc,
                                       SILValue packAddr,
                                       CanPackType formalPackType,
                                       unsigned componentIndex,
                                       SILValue currentIndexWithinComponent);

  /// If context is init accessor, find a mapping between the given type
  /// property and argument declaration synthesized for it.
  ParamDecl *isMappedToInitAccessorArgument(VarDecl *property);
};


/// A utility class for saving and restoring the insertion point.
class SILGenSavedInsertionPoint {
  SILGenFunction &SGF;
  SILBasicBlock *SavedIP;
  FunctionSection SavedSection;
public:
  SILGenSavedInsertionPoint(
      SILGenFunction &SGF, SILBasicBlock *newIP,
      llvm::Optional<FunctionSection> optSection = llvm::None)
      : SGF(SGF), SavedIP(SGF.B.getInsertionBB()),
        SavedSection(SGF.CurFunctionSection) {
    FunctionSection section = (optSection ? *optSection : SavedSection);
    assert((section != FunctionSection::Postmatter ||
            SGF.StartOfPostmatter != SGF.F.end()) &&
           "trying to move to postmatter without a registered start "
           "of postmatter?");

    SGF.B.setInsertionPoint(newIP);
    SGF.CurFunctionSection = section;
  }

  SILGenSavedInsertionPoint(const SILGenSavedInsertionPoint &) = delete;
  SILGenSavedInsertionPoint &
  operator=(const SILGenSavedInsertionPoint &) = delete;

  ~SILGenSavedInsertionPoint() {
    if (SavedIP) {
      SGF.B.setInsertionPoint(SavedIP);
    } else {
      SGF.B.clearInsertionPoint();
    }
    SGF.CurFunctionSection = SavedSection;
  }
};

} // end namespace Lowering
} // end namespace swift

#endif
