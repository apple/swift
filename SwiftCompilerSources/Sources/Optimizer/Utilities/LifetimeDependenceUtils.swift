//===--- LifetimeDependenceUtils.swift - Utils for lifetime dependence ----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Utilities that specify lifetime dependence:
//
// gatherVariableIntroducers(for:) is a use-def walk that returns the
// values that most closely associated with the variable declarations
// that the given value holds an instance of.
//
// LifetimeDependence.init models the lifetime dependence for a
// FunctionArgument or a MarkDependenceInst, categorizing the kind of
// dependence scope that the lifetime represents.
//
// LifetimeDependence.Scope.computeRange() computes the instruction
// range covered by a dependence scope.
//
// LifetimeDependence.visitDependenceRoots(enclosing:) is a use-def
// walk that walks up the chain of dependent values and visits the
// earliest LifetimeDependencies that whose lifetime can be inherited
// by the given value.
//
// LifetimeDependenceDefUseWalker walks the def-use chain to find all
// values that depend on the given OSSA lifetime.
//
//===----------------------------------------------------------------------===//
//
// TODO_reachingdef: see this tag (in visitStoredUses) to see where
// reaching-def analysis can improve diagnostics. The analysis will
// work as follows:
//
// Given an operand that satisfies `isAddressInitialization`,
// transitively find all uses of the stored value, conservatively.
//
// Given a load/copy_addr transitively find all values stored.
//
// This can be a reaching-def style analysis on an alloc_stack, very
// similar to the mem2reg analysis. Or it can be a cached
// walker. Either way, we only care about mutable local. Temporary
// copies should already be handled by findSingleInitializer. There is
// no need for any complicated value depenence walk. The client can do
// that. Just walk the address projections to find all writes. Partial
// writes are destructive, but only full writes initialize the local.
//
//===----------------------------------------------------------------------===//

import SIL

private let verbose = false

private func log(_ message: @autoclosure () -> String) {
  if verbose {
    print("### \(message())")
  }
}

/// Walk up the value dependence chain to find the best-effort
/// variable declaration. Typically called while diagnosing an error.
///
/// The walk stops at:
/// - an address
/// - a variable declaration (begin_borrow [var_decl], move_value [var_decl])
/// - the root of the dependence chain
///
/// If the introducer is an address, then the client can call
/// Value.enclosingAccess iteratively to find to AccessBase. This
/// walker is useful for finding the innermost access, which may also
/// be relevant for diagnostics.
func gatherVariableIntroducers(for value: Value, _ context: Context)
  -> SingleInlineArray<Value>
{
  var introducers = SingleInlineArray<Value>()
  var useDefVisitor = VariableIntroducerUseDefWalker(context) {
    introducers.push($0)
    return .continueWalk
  }
  defer { useDefVisitor.deinitialize() }
  _ = useDefVisitor.walkUp(valueOrAddress: value)
  return introducers
}

/// A lifetime dependence represents a scope in which some parent
/// value is alive and accessible along with a dependent value. All
/// values derived from the dependent value must be used within this
/// scope. This supports diagnostics on non-escapable types.
///
/// A lifetime dependence is produced by either 'mark_dependence [nonescaping]':
///
///   %dependent = mark_dependence [nonescaping] %value on %parent
///
/// or a non-escapable function argument:
///
///   bb0(%dependent : NonEscapableThing):
///
/// A lifetime dependence identifies its parent value, the kind of
/// scope that the parent value represents, and a dependent value. A
/// self-dependence has the same parent and dependent value:
///
///   %dependent = mark_dependence [nonescaping] %value on %value
///
/// Self-dependence is useful to ensure that derived values, including
/// copies, do not escape the lifetime of the original
/// value. Non-escapable function arguments are implicitly
/// self-dependent, meaning that the argument's value does not escape
/// the function body. Note that we do not insert a 'mark_dependence
/// [nonescaping]' for function arguments because the caller must
/// already represent the argument's dependence on some parent
/// value. That parent value may not be the value directly passed to
/// the argument. After inlining, an additional self-dependence on
/// argument value would be overly strict.
struct LifetimeDependence : CustomStringConvertible {
  enum Scope : CustomStringConvertible {
    /// A guaranteed or inout argument whose scope is provided by the caller
    /// and covers the entire function.
    case caller(Argument)
    /// An access scope.
    case access(BeginAccessInst)
    /// An coroutine.
    case yield(Value)
    /// An owned value whose OSSA lifetime encloses nonescapable values
    case owned(Value)
    /// Singly-initialized addressible storage (likely for an
    /// immutable address-only value). The lifetime extends until the
    /// memory is destroyed. e.g. A value produced by an @in
    /// FunctionArgument or @out apply. @inout has caller scope
    /// instead because its lifetime does not end inside the callee. A
    /// separate analysis diagnoses mutation after the dependence is
    /// formed.
    ///
    /// If `initializingStore` is nil, then the `initialAddress` is
    /// initialized on function entry.
    case initialized(initialAddress: Value, initializingStore: Instruction?)
    // TODO: make .unknown a SIL Verification error
    case unknown(Value)

    var parentValue: Value {
      switch self {
      case let .caller(argument): return argument
      case let .access(beginAccess): return beginAccess
      case let .yield(value): return value
      case let .owned(value): return value
      case let .initialized(initialAddress, _): return initialAddress
      case let .unknown(value): return value
      }
    }

    func checkPrecondition() {
      switch self {
      case let .caller(argument):
        precondition(argument.ownership == .guaranteed,
                     "only guaranteed arguments have a caller scope")
      case .access, .unknown:
        break        
      case let .yield(value):
        precondition(value.definingInstruction is BeginApplyInst)
      case let .owned(value):
        precondition(value.ownership == .owned)
      case let .initialized(initialAddress, initializingStore):
        precondition(initialAddress.type.isAddress, "expected an address")
        precondition(initialAddress is AllocStackInst
                       || initialAddress is FunctionArgument,
                     "expected storage for a a local 'let'")
        precondition(initializingStore is StoringInstruction
                       || initializingStore is SourceDestAddrInstruction
                       || initializingStore is FullApplySite,
                     "expected a store")
      }
    }
    var description: String {
      {
        switch self {
        case .caller: return "Caller: "
        case .access: return "Access: "
        case .yield: return "Yield: "
        case .owned: return "Owned: "
        case .initialized: return "Initialized: "
        case .unknown: return "Unknown: "
        }
      }() + "\(parentValue)"
    }
  }
  let scope: Scope
  let dependentValue: Value
  
  var parentValue: Value { scope.parentValue }

  var function: Function {
    dependentValue.parentFunction
  }

  var description: String {
    return scope.description + "\nDependent: \(dependentValue)"
  }
}

extension LifetimeDependence {
  /// Construct LifetimeDependence from a function argument.
  ///
  /// Returns 'nil' for indirect results.
  init?(_ arg: FunctionArgument, _ context: some Context) {
    if arg.isIndirectResult {
      return nil
    }
    self.scope = Scope(base: arg, context)!
    self.dependentValue = arg
  }

  // Construct a LifetimeDependence from a return value. This only
  // constructs a dependence for ~Escapable results that do not have a
  // lifetime dependence (@_unsafeNonescapableResult).
  //
  // TODO: handle indirect results
  init?(unsafeApplyResult value: Value, _ context: some Context) {
    if value.isEscapable {
      return nil
    }
    if (value.definingInstruction as! FullApplySite).hasResultDependence {
      return nil
    }
    assert(value.ownership == .owned, "apply result must be owned")
    self.scope = Scope(base: value, context)!
    self.dependentValue = value
  }

  var isUnsafeApplyResult: Bool {
    if case let .owned(value) = scope {
      if let apply = value.definingInstruction as? FullApplySite {
        assert(!apply.hasResultDependence)
        return true
      }
    }
    return false
  }

  /// Construct LifetimeDependence from mark_dependence [unresolved]
  ///
  /// For any LifetimeDependence constructed from a mark_dependence,
  /// its `dependentValue` will be the result of the mark_dependence.
  ///
  /// TODO: Add SIL verification that all mark_depedence [unresolved]
  /// have a valid LifetimeDependence.
  init?(_ markDep: MarkDependenceInst, _ context: some Context) {
    switch markDep.dependenceKind {
    case .Unresolved, .NonEscaping:
      guard let scope = Scope(base: markDep.base, context) else {
        return nil
      }
      self.scope = scope
      self.dependentValue = markDep
    case .Escaping:
      return nil
    }
  }

  /// Compute the range of the dependence scope.
  ///
  /// Returns nil if the dependence scope covers the entire function
  /// or if 'dependentValue' is part of the parent's forwarded
  /// lifetime.
  ///
  /// Note: The caller must deinitialize the returned range.
  func computeRange(_ context: Context) -> InstructionRange? {
    if dependentValue.isForwarded(from: parentValue) {
      return nil
    }
    return scope.computeRange(context)
  }

  func resolve(_ context: some Context) {
    if let mdi = dependentValue as? MarkDependenceInst {
      mdi.resolveToNonEscaping()
    }
  }
}

private extension Value {
  func isForwarded(from: Value) -> Bool {
    if self == from {
      return true
    }
    if let forward = self.forwardingInstruction,
       let singleOp = forward.singleForwardedOperand {
      return singleOp.value.isForwarded(from: from)
    }
    return false
  }
}

extension LifetimeDependence.Scope {
  /// Construct a lifetime dependence scope from the base value that
  /// other values depend on. This derives the kind of dependence
  /// scope and its parentValue from `base`.
  ///
  /// `base` represents the OSSA lifetime that the dependent value
  /// must be used within. If `base` is owned, then it directly
  /// defines the parent lifetime. If `base` is guaranteed, then it
  /// must have a single borrow introducer, which defines the parent
  /// lifetime. `base` must not be derived from a guaranteed phi or
  /// forwarded (via struct/tuple) from multiple guaranteed values.
  init?(base: Value, _ context: some Context) {
    if base.type.isAddress {
      guard let scope = Self(address: base, context) else {
        return nil
      }
      self = scope
      return
    }
    switch base.ownership {
    case .owned:
      self = .owned(base)
      return
    case .guaranteed:
      guard let scope = Self(guaranteed: base, context) else {
        return nil
      }
      self = scope
    case .none:
      // lifetime dependence requires a nontrivial value"
      return nil
    case .unowned:
      self = .unknown(base)
    }
  }

  private init?(address: Value, _ context: some Context) {
    switch address.enclosingAccessScope {
    case let .scope(access):
      self = .access(access)
    case let .base(accessBase):
      switch accessBase {
      case let .box(projectBox):
        // Note: the box may be in a borrow scope.
        guard let scope = Self(base: projectBox.operand.value, context) else {
          return nil
        }
        self = scope
      case let .stack(allocStack):
        guard let scope = Self(allocation: allocStack, context) else {
          return nil
        }
        self = scope
      case .global:
        self = .unknown(address)
      case .class, .tail:
        let refElt = address as! UnaryInstruction
        guard let scope = Self(guaranteed: refElt.operand.value, context) else {
          return nil
        }
        self = scope
      case let .argument(arg):
        if arg.convention.isIndirectIn {
          self = .initialized(initialAddress: arg, initializingStore: nil)
        } else if arg.convention.isIndirectOut {
          // TODO: verify that @out values are never reassigned.
          self = .caller(arg)
        } else {
          // Note: we do not expect arg.convention.isInout because
          // mutable variables require an access scope. The .caller
          // scope is assumed to be immutable.
          self = .unknown(address)
        }
      case let .yield(result):
        self = Self(yield: result)
      case .storeBorrow(let sb):
        guard let scope = Self(base: sb.source, context) else {
          return nil
        }
        self = scope
      case .pointer, .unidentified:
        self = .unknown(address)
      }
    }
  }

  private init?(guaranteed base: Value, _ context: some Context) {
    // If introducers is empty, then the dependence is on a trivial value, so
    // there is no dependence scope.
    //
    // TODO: Add a SIL verifier check that a mark_dependence [nonescaping]
    // base is never a guaranteed phi.
    var iter = base.getBorrowIntroducers(context).makeIterator()
    guard let beginBorrow = iter.next() else { return nil }
    assert(iter.next() == nil,
           "guaranteed phis not allowed when diagnosing lifetime dependence")
    switch beginBorrow {
    case .beginBorrow, .loadBorrow:
      let borrowOperand = beginBorrow.baseOperand!
      guard let scope = LifetimeDependence.Scope(base: borrowOperand.value,
                                                 context) else {
        return nil
      }
      self = scope
    case let .beginApply(value):
      self = .yield(value)
    case .functionArgument:
      self = .caller(beginBorrow.value as! Argument)
    case .reborrow:
      fatalError("reborrows are not supported in diagnostics")
    }
  }

  private init(yield result: MultipleValueInstructionResult) {
    // Consider an @in yield an .initialized scope. We must find the destroys.
    let apply = result.parentInstruction as! FullApplySite
    if apply.convention(of: result).isIndirectIn {
      self = .initialized(initialAddress: result, initializingStore: nil)
      return
    }
    self = .yield(result)
  }

  private init?(allocation: AllocStackInst, _ context: Context) {
    if let initializer = allocation.accessBase.findSingleInitializer(context) {
      self = .initialized(initialAddress: initializer.initialAddress,
                          initializingStore: initializer.initializingStore)
    }
    return nil
  }
}

extension LifetimeDependence.Scope {
  /// Compute the range of the dependence scope. 
  ///
  /// Returns nil if the dependence scope covers the entire function.
  ///
  /// Note: The caller must deinitialize the returned range.
  func computeRange(_ context: Context) -> InstructionRange? {
    switch self {
    case .caller:
      return nil
    case let .access(beginAccess):
      var range = InstructionRange(begin: beginAccess, context)
      range.insert(contentsOf: beginAccess.endInstructions)
      return range
    case let .yield(value):
      // This assumes @inout or @in_guaranteed convention.
      let def = value.definingInstruction!
      var range = InstructionRange(begin: def, context)
      _ = BorrowingInstruction(def)!.visitScopeEndingOperands(context) {
        range.insert($0.instruction)
        return .continueWalk
      }
      return range
    case let .owned(value):
      // Note: This could compute forwarded liveness instead of linear
      // liveness. That would be more forgiving for copies. But, then
      // how would we ensure that the borrowed mark_dependence value
      // is within this value's OSSA lifetime?
      return computeLinearLiveness(for: value, context)
    case let .initialized(initialAddress, initializingStore):
      return LifetimeDependence.Scope.computeInitializedRange(
        initialAddress: initialAddress, initializingStore: initializingStore,
        context)
    case let .unknown(value):
      // Return an empty range.
      return InstructionRange(for: value, context)
    }
  }
  
  private static func computeInitializedRange(initialAddress: Value,
                                              initializingStore: Instruction?,
                                              _ context: Context)
    -> InstructionRange {
    assert(initialAddress.type.isAddress)

    var range: InstructionRange
    if let initializingStore {
      range = InstructionRange(begin: initializingStore, context)
    } else {
      range = InstructionRange(for: initialAddress, context)
    }
    for use in initialAddress.uses {
      let inst = use.instruction
      switch inst {
      case is DestroyAddrInst:
        range.insert(inst)
      case let sdai as SourceDestAddrInstruction
             where sdai.sourceOperand == use && sdai.isTakeOfSrc:
        range.insert(inst)
      case let li as LoadInst where li.loadOwnership == .take:
        range.insert(inst)
      default:
        break
      }
    }
    return range
  }
}

extension LifetimeDependence {
  /// TODO: By making UseDefVisitor a NonEscapable type, we should be
  /// able to make \p visitor a non-escaping closure.
  static func visitDependenceRoots(enclosing value: Value,
    _ context: Context,
    _ visitor: @escaping (LifetimeDependence.Scope) -> WalkResult)
  -> WalkResult {
    var useDefVisitor = UseDefVisitor(context, visitor)
    defer { useDefVisitor.deinitialize() }
    return useDefVisitor.walkUp(valueOrAddress: value)
  }
  
  private struct UseDefVisitor : LifetimeDependenceUseDefWalker {
    let context: Context
    // This visited set is only really needed for instructions with
    // multiple results, including phis.
    private var visitedValues: ValueSet
    // Call \p visit rather than calling this directly.
    private let visitorClosure: (LifetimeDependence.Scope) -> WalkResult
    
    init(_ context: Context,
         _ visitor: @escaping (LifetimeDependence.Scope) -> WalkResult) {
      self.context = context
      self.visitedValues = ValueSet(context)
      self.visitorClosure = visitor
    }
    
    mutating func deinitialize() {
      visitedValues.deinitialize()
    }
    
    mutating func needWalk(for value: Value, _ owner: Value?) -> Bool {
      // FIXME: cache the value's owner, and support walking up
      // multiple guaranteed forwards to different owners, then
      // reconverging.
      visitedValues.insert(value)
    }

    // Visit the base value of a lifetime dependence. If the base is an address, the dependence scope is the enclosing
    // access. The walker does not walk past an `mark_dependence [nonescaping]` that produces an address, because that
    // will never occur inside of an access scope. An address type mark_dependence [unresolved]` can only result from an
    // indirect function result when opaque values are not enabled. Address type `mark_dependence [nonescaping]`
    // instruction are also produced for captured arguments but ClosureLifetimeFixup, but those aren't considered to
    // have a LifetimeDependence scope.
    mutating func introducer(_ value: Value, _ owner: Value?) -> WalkResult {
      let base = owner ?? value
      guard let scope = LifetimeDependence.Scope(base: base, context)
      else {
        return .continueWalk
      }
      scope.checkPrecondition()
      return visitorClosure(scope)
    }
  }
}

/// Walk up the lifetime dependence
///
/// This uses LifetimeDependenceUseDefWalker to find the introducers
/// of a dependence chain, which represent the value's "inherited"
/// dependencies. This stops at an address, unless the address refers
/// to a singly-initialized temprorary, in which case it continues to
/// walk up the stored value.
///
/// This overrides LifetimeDependenceUseDefWalker to stop at a value
/// that introduces an immutable variable: move_value [var_decl] or
/// begin_borrow [var_decl], and to stop at an access of a mutable
/// variable: begin_access.
///
/// Start walking:
///     walkUp(valueOrAddress: Value) -> WalkResult
struct VariableIntroducerUseDefWalker : LifetimeDependenceUseDefWalker {
  let context: Context
  // This visited set is only really needed for instructions with
  // multiple results, including phis.
  private var visitedValues: ValueSet

  // Call \p visit rather than calling this directly.
  private let visitorClosure: (Value) -> WalkResult

  init(_ context: Context, _ visitor: @escaping (Value) -> WalkResult) {
    self.context = context
    self.visitedValues = ValueSet(context)
    self.visitorClosure = visitor
  }

  mutating func deinitialize() {
    visitedValues.deinitialize()
  }
 
  mutating func needWalk(for value: Value, _ owner: Value?) -> Bool {
    visitedValues.insert(value)
  }

  mutating func introducer(_ value: Value, _ owner: Value?) -> WalkResult {
    return visitorClosure(value)
  }

  mutating func walkUp(value: Value, _ owner: Value?) -> WalkResult {
    switch value.definingInstruction {
    case let moveInst as MoveValueInst:
      if moveInst.isFromVarDecl {
        return introducer(moveInst, owner)
      }
    case let borrow as BeginBorrowInst:
      if borrow.isFromVarDecl {
        return introducer(borrow, owner)
      }
    default:
      break
    }
    return walkUpDefault(dependent: value, owner: owner)
  }

  mutating func walkUp(address: Value) -> WalkResult {
    if let beginAccess = address.definingInstruction as? BeginAccessInst {
      return introducer(beginAccess, nil)
    }
    return walkUpDefault(address: address)
  }
}

/// Walk up the lifetime dependence chain.
///
/// This finds the introducers of a dependence chain. which represent the value's "inherited" dependencies. This stops
/// at phis, so all introducers dominate their dependencies. This stops at addresses in general, but if the value is
/// loaded from a singly-initialized location, then it continues walking up the value stored by the initializer. This
/// bypasses the copies to temporary memory locations emitted by SILGen.
///
/// In this example, the dependence root is copied, borrowed, and forwarded before being used as the base operand of
/// `mark_dependence`. The dependence "root" is the parent of the outer-most dependence scope.
///
///   %root = apply                  // lifetime dependence root
///   %copy = copy_value %root
///   %parent = begin_borrow %copy   // lifetime dependence parent value
///   %base = struct_extract %parent // lifetime dependence base value
///   %dependent = mark_dependence [nonescaping] %value on %base
///
/// This extends the ForwardingUseDefWalker, which finds the forward-extended lifetime introducers. Certain
/// forward-extended lifetime introducers can inherit a lifetime dependency from their operand: namely copies, moves,
/// and borrows. These introducers are considered part of their operand's dependence scope because non-escapable values
/// can be copied, moved, and borrowed. Nonetheless, all of their uses must remain within original dependence scope.
///
///   # owned lifetime dependence
///   %parent = apply               // begin dependence scope -+
///   ...                                                      |
///   %1 = mark_dependence [nonescaping] %value on %parent     |
///   ...                                                      |
///   %2 = copy_value %1        -+                             |
///   # forwarding instruction   |                             |
///   %3 = struct $S (%2)        | forward-extended lifetime   |
///                              |                             | OSSA Lifetime
///   %4 = move_value %3        -+                             |
///   ...                        | forward-extended lifetime   |
///   %5 = begin_borrow %4       | -+                          |
///   # dependent use of %1      |  | forward-extended lifetime|
///   end_borrow %5              | -+                          |
///   destroy_value %4          -+                             |
///   ...                                                      |
///   destroy_value %parent        // end dependence scope    -+
///
/// All of the dependent uses including `end_borrow %5` and `destroy_value %4` must be before the end of the dependence
/// scope: `destroy_value %parent`. In this case, the dependence parent is an owned value, so the scope is simply the
/// value's OSSA lifetime.
///
/// Minimal requirements:
///   var context: Context
///   introducer(_ value: Value) -> WalkResult
///   needWalk(for value: Value) -> Bool
///
/// Start walking:
///   walkUp(valueOrAddress: Value) -> WalkResult
protocol LifetimeDependenceUseDefWalker : ForwardingUseDefWalker where PathContext == Value? {
  var context: Context { get }

  mutating func introducer(_ value: Value, _ owner: Value?) -> WalkResult

  // Minimally, check a ValueSet. This walker may traverse chains of
  // aggregation and destructuring along with phis.
  mutating func needWalk(for value: Value, _ owner: Value?) -> Bool

  mutating func walkUp(value: Value, _ owner: Value?) -> WalkResult

  mutating func walkUp(address: Value) -> WalkResult
}

// Implement ForwardingUseDefWalker
extension LifetimeDependenceUseDefWalker {
  mutating func walkUp(valueOrAddress: Value) -> WalkResult {
    if valueOrAddress.type.isAddress {
      return walkUp(address: valueOrAddress)
    }
    let owner = valueOrAddress.ownership == .owned ? valueOrAddress : nil
    return walkUp(value: valueOrAddress, owner)
  }

  mutating func walkUp(value: Value, _ owner: Value?) -> WalkResult {
    walkUpDefault(dependent: value, owner: owner)
  }

  mutating func walkUp(address: Value) -> WalkResult {
    walkUpDefault(address: address)
  }

  // Extend ForwardingUseDefWalker to handle copies, moves, and
  // borrows. Also transitively walk up other lifetime dependencies to
  // find the roots.
  //
  // If `value` is an address, this immediately invokes
  // `introducer()`. We only expect to see an address-type
  // `mark_dependence [nonescaping]` when opaque values are disabled.
  //
  // Handles loads as a convenience so the client receives the load's
  // address as an introducer.
  mutating func walkUpDefault(dependent value: Value, owner: Value?)
    -> WalkResult {
    switch value.definingInstruction {
    case let transition as OwnershipTransitionInstruction:
      return walkUp(newLifetime: transition.operand.value)
    case let load as LoadInstruction:
      return walkUp(address: load.address)
    case let markDep as MarkDependenceInst:
      if let dependence = LifetimeDependence(markDep, context) {
        let parent = dependence.parentValue
        if markDep.isForwarded(from: parent) {
          return walkUp(value: dependence.parentValue, owner)
        } else {
          return walkUp(newLifetime: dependence.parentValue)
        }
      }
    default:
      break
    }
    // If the dependence chain has a phi, consider it a root. Dependence roots
    // are currently expected to dominate all dependent values.
    if Phi(value) != nil {
      return introducer(value, owner)
    }
    // ForwardingUseDefWalker will callback to introducer() when it finds no forwarding instruction.
    return walkUpDefault(forwarded: value, owner)
  }

  // Walk up from a load of a singly-initialized address to find the
  // dependence root of the stored value. This ignores mutable
  // variables, which require an access scope. This ignores applies
  // because an lifetime dependence will already be expressed as a
  // mark_dependence.
  mutating func walkUpDefault(address: Value) -> WalkResult {
    if let (_, initializingStore) =
         address.accessBase.findSingleInitializer(context) {
      switch initializingStore {
      case let store as StoringInstruction:
        return walkUp(newLifetime: store.source)
      case let srcDestInst as SourceDestAddrInstruction:
        return walkUp(address: srcDestInst.sourceOperand.value)
      default:
        break
      }
    }
    return introducer(address, nil)
  }

  private mutating func walkUp(newLifetime: Value) -> WalkResult {
    let newOwner = newLifetime.ownership == .owned ? newLifetime : nil
    return walkUp(value: newLifetime, newOwner)
  }
}

/// Walk down dependent values.
///
/// Delegates value def-use walking to the ForwardingUseDefWalker and
/// overrides copy, move, borrow, and mark_dependence.
///
/// Ignores trivial values (~Escapable types are never
/// trivial. Escapable types may only be lifetime-depenent values if
/// they are non-trivial).
///
/// Skips uses within nested borrow scopes.
///
/// TODO: override BeginAccessInst to handle EndAccessInst.
///
/// Minimal requirements:
///   needWalk(for value: Value) -> Bool
///   leafUse(of: Operand) -> WalkResult
///   deadValue(_ value: Value, using operand: Operand?) -> WalkResult
///   escapingDependence(on operand: Operand) -> WalkResult
///   returnedDependence(result: Operand) -> WalkResult
///   returnedDependence(address: FunctionArgument, using: Operand) -> WalkResult
///   yieldedDependence(result: Operand) -> WalkResult
/// Start walking:
///   walkDown(root: Value)
///
/// Note: this may visit values that are not dominated by `root` because of dependent phi operands.
protocol LifetimeDependenceDefUseWalker : ForwardingDefUseWalker,
                                          OwnershipUseVisitor,
                                          AddressUseVisitor {
  var function: Function { get }

  /// Dependence tracking through local variables.
  var localReachabilityCache: LocalVariableReachabilityCache { get }

  mutating func leafUse(of operand: Operand) -> WalkResult

  mutating func escapingDependence(on operand: Operand) -> WalkResult

  mutating func returnedDependence(result: Operand) -> WalkResult

  mutating func returnedDependence(address: FunctionArgument, using: Operand) -> WalkResult

  mutating func yieldedDependence(result: Operand) -> WalkResult
}

extension LifetimeDependenceDefUseWalker {
  // Use a distict context name to avoid rdar://123424566 (Unable to open existential)
  var walkerContext: Context { context }
}

// Start a forward walk.
extension LifetimeDependenceDefUseWalker {
  mutating func walkDown(root: Value) -> WalkResult {
    if root.type.isAddress {
      return walkDownAddressUses(of: root)
    }
    return walkDownUses(of: root, using: nil)
  }
}

// Implement ForwardingDefUseWalker
extension LifetimeDependenceDefUseWalker {
  // Override ForwardingDefUseWalker.
  mutating func walkDownUses(of value: Value, using operand: Operand?)
    -> WalkResult {
    // Only track ~Escapable and @noescape types.
    if value.mayEscape {
      return .continueWalk
    }
    return walkDownUsesDefault(forwarding: value, using: operand)
  }

  // Override ForwardingDefUseWalker.
  mutating func walkDown(operand: Operand) -> WalkResult {
    // Initially delegate all usess to OwnershipUseVisitor.
    // walkDownDefault will be called for uses that forward ownership.
    return classify(operand: operand)
  }

  // Callback from (a) ForwardingDefUseWalker or (b) ownershipLeafUse.
  //
  // (a) OwnershipUseVisitor classified this as a forwardingUse(), but
  // ForwardingDefUseWalker does not recognize it as a forwarding
  // instruction. This includes apply arguments.
  //
  // (b) OwnershipUseVisitor classified this as a ownershipLeafUse(), but
  // it was not a recognized copy, destroy, or instantaneous use.
  mutating func nonForwardingUse(of operand: Operand) -> WalkResult {
    if let apply = operand.instruction as? FullApplySite {
      return visitAppliedUse(of: operand, by: apply)
    }
    if operand.instruction is ReturnInst, !operand.value.isEscapable {
      return returnedDependence(result: operand)
    }
    if operand.instruction is YieldInst, !operand.value.isEscapable {
      return yieldedDependence(result: operand)
    }
    return escapingDependence(on: operand)
  }
}

// Implement OwnershipUseVisitor
extension LifetimeDependenceDefUseWalker {
  // Handle uses that do not propagate the OSSA lifetime. They may still
  // copy the value, which propagates the dependence.
  mutating func ownershipLeafUse(of operand: Operand, isInnerLifetime: Bool)
    -> WalkResult {
    if operand.ownership == .endBorrow {
      // Record the leaf use here because, in some cases, like
      // begin_apply, we have skipped the inner uses.
      return leafUse(of: operand)
    }
    switch operand.instruction {
    case let transition as OwnershipTransitionInstruction:
      return walkDownUses(of: transition.ownershipResult, using: operand)

    case let mdi as MarkDependenceInst where mdi.isUnresolved:
      // Override mark_dependence [unresolved] to handle them just
      // like [nonescaping] even though they are not considered OSSA
      // borrows until after resolution.
      assert(operand == mdi.baseOperand)
      return dependentUse(of: operand, into: mdi)

    case is ExistentialMetatypeInst, is FixLifetimeInst, is WitnessMethodInst,
         is DynamicMethodBranchInst, is ValueMetatypeInst,
         is IsEscapingClosureInst, is ClassMethodInst, is SuperMethodInst,
         is ClassifyBridgeObjectInst, is DebugValueInst,
         is ObjCMethodInst, is ObjCSuperMethodInst, is UnmanagedRetainValueInst,
         is UnmanagedReleaseValueInst, is SelectEnumInst:
      // Catch .instantaneousUse operations that are dependence leaf uses.
      return leafUse(of: operand)

    case is DestroyValueInst, is EndLifetimeInst, is DeallocRefInst,
         is DeallocBoxInst, is DeallocExistentialBoxInst,
         is BeginCOWMutationInst, is EndCOWMutationInst,
         is EndInitLetRefInst, is DeallocPartialRefInst, is BeginDeallocRefInst:
      // Catch .destroyingConsume operations that are dependence leaf
      // uses.
      return leafUse(of: operand)

    case let si as StoringInstruction where si.sourceOperand == operand:
      return visitStoredUses(of: operand, into: si.destinationOperand.value)

    case let tai as TupleAddrConstructorInst:
      return visitStoredUses(of: operand, into: tai.destinationOperand.value)

    case let bi as BuiltinInst where bi.id == .Copy:  
      // This must be a non-address-lowered form of Builtin.Copy that
      // produces an owned value.
      assert(bi.ownership == .owned)
      return walkDownUses(of: bi, using: operand)

    default:
      return nonForwardingUse(of: operand)
    }
  }

  mutating func forwardingUse(of operand: Operand, isInnerLifetime: Bool)
    -> WalkResult {
    // Delegate ownership forwarding operations to the ForwardingDefUseWalker.
    return walkDownDefault(forwarding: operand)
  }

  mutating func interiorPointerUse(of: Operand, into address: Value)
    -> WalkResult {
    return walkDownAddressUses(of: address)
  }

  mutating func pointerEscapingUse(of operand: Operand) -> WalkResult {
    return escapingDependence(on: operand)
  }

  mutating func dependentUse(of operand: Operand, into value: Value)
    -> WalkResult {
    return walkDownUses(of: value, using: operand)
  }

  mutating func borrowingUse(of operand: Operand,
                             by borrowInst: BorrowingInstruction)
    -> WalkResult {
    return visitAllBorrowUses(of: operand, by: borrowInst)
  }

  // TODO: Consider supporting lifetime dependence analysis of
  // guaranteed phis. See InteriorUseWalker.walkDown(guaranteedPhi: Phi)
  mutating func reborrowingUse(of operand: Operand, isInnerLifetime: Bool)
    -> WalkResult {
    return escapingDependence(on: operand)
  }
}

// Implement AddressUseVisitor
extension LifetimeDependenceDefUseWalker {
  /// An address projection produces a single address result and does not
  /// escape its address operand in any other way.
  mutating func projectedAddressUse(of operand: Operand, into value: Value)
    -> WalkResult {
    return walkDownAddressUses(of: value)
  }

  mutating func scopedAddressUse(of operand: Operand) -> WalkResult {
    switch operand.instruction {
    case let ba as BeginAccessInst:
      return walkDownAddressUses(of: ba)
    case let ba as BeginApplyInst:
      // visitAppliedUse only calls scopedAddressUse for ~Escapable
      // arguments that are not propagated to a result. Here, we only
      // care about the scope of the call.
      return ba.token.uses.walk { leafUse(of: $0) }
    case is StoreBorrowInst:
      return leafUse(of: operand)
    case let load as LoadBorrowInst:
      return walkDownUses(of: load, using: operand)
    default:
      fatalError("Unrecognized scoped address use: \(operand.instruction)")
    }
  }

  mutating func scopeEndingAddressUse(of operand: Operand) -> WalkResult {
    return leafUse(of: operand)
  }

  // Includes StoringInstruction.
  mutating func leafAddressUse(of operand: Operand) -> WalkResult {
    return leafUse(of: operand)
  }

  mutating func appliedAddressUse(of operand: Operand,
                                  by apply: FullApplySite) -> WalkResult {
    return visitAppliedUse(of: operand, by: apply)
  }

  mutating func loadedAddressUse(of operand: Operand, into value: Value)
    -> WalkResult {
    // Record the load itself, in case the loaded value is Escapable.
    if leafUse(of: operand) == .abortWalk {
      return .abortWalk
    }
    return walkDownUses(of: value, using: operand)
  }    

  mutating func loadedAddressUse(of operand: Operand, into address: Operand)
    -> WalkResult {
    if leafUse(of: operand) == .abortWalk {
      return .abortWalk
    }
    return visitStoredUses(of: operand, into: address.value)
  }

  mutating func dependentAddressUse(of operand: Operand, into value: Value)
    -> WalkResult {
    walkDownUses(of: value, using: operand)
  }    

  mutating func escapingAddressUse(of operand: Operand) -> WalkResult {
    if let mdi = operand.instruction as? MarkDependenceInst {
      assert(!mdi.isUnresolved && !mdi.isNonEscaping,
             "should be handled as a dependence by AddressUseVisitor")
    }
    if operand.instruction is ReturnInst, !operand.value.isEscapable {
      return returnedDependence(result: operand)
    }
    if operand.instruction is YieldInst, !operand.value.isEscapable {
      return yieldedDependence(result: operand)
    }
    return escapingDependence(on: operand)
  }

  mutating func unknownAddressUse(of operand: Operand) -> WalkResult {
    return .abortWalk
  }

  private mutating func walkDownAddressUses(of address: Value) -> WalkResult {
    address.uses.ignoreTypeDependence.walk {
      return classifyAddress(operand: $0)
    }
  }
}

// Helpers
extension LifetimeDependenceDefUseWalker {
  // Visit uses of borrowing instruction (operandOwnerhip == .borrow).
  private mutating func visitAllBorrowUses(
    of operand: Operand, by borrowInst: BorrowingInstruction) -> WalkResult {
    switch borrowInst {
    case let .beginBorrow(bbi):
      return walkDownUses(of: bbi, using: operand)
    case let .storeBorrow(sbi):
      return walkDownAddressUses(of: sbi)
    case .beginApply:
      // Skip the borrow scope; the type system enforces non-escapable
      // arguments.
      return visitInnerBorrowUses(of: borrowInst)
    case .partialApply, .markDependence:
      fatalError("OwnershipUseVisitor should bypass partial_apply [on_stack] "
                 + "and mark_dependence [nonescaping]")
    case .startAsyncLet:
      // TODO: If async-let takes a non-escaping closure, we can
      // consider it nonescaping and visit the endAsyncLetLifetime
      // uses here.
      return escapingDependence(on: operand)
    }
  }

  // Visit stores to a local variable (alloc_box), temporary storage
  // (alloc_stack). This handles stores of the entire value and stores
  // to a tuple element. Stores to a field within another nominal
  // value are considered lifetime dependence leaf uses; the type
  // system enforces non-escapability on the aggregate value.
  private mutating func visitStoredUses(of operand: Operand,
                                        into address: Value) -> WalkResult {
    assert(address.type.isAddress)

    var allocation: Value?
    switch address.accessBase {
    case let .box(projectBox):
      allocation = projectBox.box.referenceRoot
    case let .stack(allocStack):
      allocation = allocStack
    case let .argument(arg):
      if arg.convention.isIndirectIn || arg.convention.isInout {
        allocation = arg
      } else if arg.convention.isIndirectOut, !arg.isEscapable {
        return returnedDependence(address: arg, using: operand)
      }
      break
    case .global, .class, .tail, .yield, .storeBorrow, .pointer, .unidentified:
      // An address produced by .storeBorrow should never be stored into.
      break
    }
    if let allocation = allocation {
      if !allocation.isEscapable {
        return visitLocalStore(allocation: allocation, storedOperand: operand, storeAddress: address)
      }
    }
    if address.isEscapable {
      return .continueWalk
    }
    return escapingDependence(on: operand)
  }

  private mutating func visitLocalStore(allocation: Value, storedOperand: Operand, storeAddress: Value) -> WalkResult {
    guard let localReachability = localReachabilityCache.reachability(for: allocation, walkerContext) else {
      return escapingDependence(on: storedOperand)
    }
    var accessStack = Stack<LocalVariableAccess>(walkerContext)
    defer { accessStack.deinitialize() }

    // Get the local variable access that encloses this store.
    var storeAccess = storedOperand.instruction
    if case let .scope(beginAccess) = storeAddress.enclosingAccessScope {
      storeAccess = beginAccess
    }
    if !localReachability.gatherAllReachableUses(of: storeAccess, in: &accessStack) {
      return escapingDependence(on: storedOperand)
    }
    for localAccess in accessStack {
      if visitLocalAccess(allocation: allocation, localAccess: localAccess, initialValue: storedOperand) == .abortWalk {
        return .abortWalk
      }
    }
    return .continueWalk
  }

  private mutating func visitLocalAccess(allocation: Value, localAccess: LocalVariableAccess, initialValue: Operand)
    -> WalkResult {
    switch localAccess.kind {
    case .beginAccess:
      return scopedAddressUse(of: localAccess.operand!)
    case .load:
      switch localAccess.instruction! {
      case let load as LoadInst:
        return loadedAddressUse(of: localAccess.operand!, into: load)
      case let load as LoadBorrowInst:
        return loadedAddressUse(of: localAccess.operand!, into: load)
      case let copyAddr as SourceDestAddrInstruction:
        return loadedAddressUse(of: localAccess.operand!, into: copyAddr.destinationOperand)
      default:
        return .abortWalk
      }
    case .store:
      let si = localAccess.operand!.instruction as! StoringInstruction
      assert(si.sourceOperand == initialValue, "the only reachable store should be the current assignment")
    case .apply:
      return visitAppliedUse(of: localAccess.operand!, by: localAccess.instruction as! FullApplySite)
    case .escape:
      log("Local variable: \(allocation)\n    escapes at: \(localAccess.instruction!)")
      return escapingDependence(on: localAccess.operand!)
    case .outgoingArgument:
      let arg = allocation as! FunctionArgument
      assert(arg.type.isAddress, "returned local must be allocated with an indirect argument")
      return returnedDependence(address: arg, using: initialValue)
    case .incomingArgument:
      fatalError("Incoming arguments are never reachable")
    }
    return .continueWalk
  }

  private mutating func visitAppliedUse(of operand: Operand, by apply: FullApplySite) -> WalkResult {
    if let conv = apply.convention(of: operand), conv.isIndirectOut {
      return leafUse(of: operand)
    }
    if apply.isCallee(operand: operand) {
      return leafUse(of: operand)
    }
    if let dep = apply.resultDependence(on: operand),
       dep == .inherit {
      // Operand is nonescapable and passed as a call argument. If the
      // result inherits its lifetime, then consider any nonescapable
      // result value to be a dependent use.
      //
      // If the lifetime dependence is scoped, then we can ignore it
      // because a mark_dependence [nonescaping] represents the
      // dependence.
      if let result = apply.singleDirectResult, !result.isEscapable {
        if dependentUse(of: operand, into: result) == .abortWalk {
          return .abortWalk
        }
      }
      for resultAddr in apply.indirectResultOperands
          where !resultAddr.value.isEscapable {
        if visitStoredUses(of: operand, into: resultAddr.value) == .abortWalk {
          return .abortWalk
        }
      }
    }
    // Regardless of lifetime dependencies, consider the operand to be
    // use for the duration of the call.
    if apply is BeginApplyInst {
      return scopedAddressUse(of: operand)
    }
    return leafUse(of: operand)
  }
}

let variableIntroducerTest = FunctionTest("variable_introducer") {
    function, arguments, context in
  let value = arguments.takeValue()
  print("Variable introducers of: \(value)")
  print(gatherVariableIntroducers(for: value, context))
}

let lifetimeDependenceScopeTest = FunctionTest("lifetime_dependence_scope") {
    function, arguments, context in
  let markDep = arguments.takeValue() as! MarkDependenceInst
  guard let dependence = LifetimeDependence(markDep, context) else {
    print("Trivial Dependence")
    return
  }
  print(dependence)
  guard var range = dependence.scope.computeRange(context) else {
    print("Caller range")
    return
  }
  defer { range.deinitialize() }
  print(range)
}

let lifetimeDependenceRootTest = FunctionTest("lifetime_dependence_root") {
    function, arguments, context in
  let value = arguments.takeValue()
  _ = LifetimeDependence.visitDependenceRoots(enclosing: value, context) {
    scope in
    print("Scope: \(scope)")
    return .continueWalk
  }
}

private struct LifetimeDependenceUsePrinter : LifetimeDependenceDefUseWalker {
  let context: Context
  let function: Function
  let localReachabilityCache = LocalVariableReachabilityCache()
  var visitedValues: ValueSet
  
  init(function: Function, _ context: Context) {
    self.context = context
    self.function = function
    self.visitedValues = ValueSet(context)
  }
  
  mutating func deinitialize() {
    visitedValues.deinitialize()
  }

  mutating func needWalk(for value: Value) -> Bool {
    visitedValues.insert(value)
  }

  mutating func deadValue(_ value: Value, using operand: Operand?)
    -> WalkResult {
    print("Dead value: \(value)")
    return .continueWalk
  }

  mutating func leafUse(of operand: Operand) -> WalkResult {
    print("Leaf use: \(operand)")
    return .continueWalk
  }

  mutating func escapingDependence(on operand: Operand) -> WalkResult {
    print("Escaping use: \(operand)")
    return .continueWalk
  }

  mutating func returnedDependence(result: Operand) -> WalkResult {
    print("Returned use: \(result)")
    return .continueWalk
  }

  mutating func returnedDependence(address: FunctionArgument,
                                   using operand: Operand) -> WalkResult {
    print("Returned use: \(operand) in: \(address)")
    return .continueWalk
  }

  mutating func yieldedDependence(result: Operand) -> WalkResult {
    print("Yielded use: \(result)")
    return .continueWalk
  }
}

let lifetimeDependenceUseTest = FunctionTest("lifetime_dependence_use") {
    function, arguments, context in
  let value = arguments.takeValue()
  print("LifetimeDependence uses of: \(value)")
  var printer = LifetimeDependenceUsePrinter(function: function, context)
  defer { printer.deinitialize() }
  _ = printer.walkDown(root: value)
}


// SIL Unit tests

let argumentConventionsTest = FunctionTest("argument_conventions") {
  function, arguments, context in
  if arguments.hasUntaken {
    let value = arguments.takeValue()
    let applySite = value.definingInstruction as! ApplySite
    print("Conventions for call: \(applySite)")
    print(applySite.calleeArgumentConventions)
  } else {
    print("Conventions for function: \(function.name)")
    print(function.argumentConventions)
  }
  // TODO: print ~Escapable conformance and lifetime dependencies
}
