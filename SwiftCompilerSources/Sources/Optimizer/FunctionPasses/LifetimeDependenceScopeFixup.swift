//===--- LifetimeDependenceScopeFixup.swift ----------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===---------------------------------------------------------------------===//

// For an apply that returns a lifetime dependent value, 
// LifetimeDependenceInsertion inserts mark_dependence [unresolved] on parent
// value's access scope. LifetimeDependenceScopeFixup then extends the access
// scope to cover all uses of the dependent value.

// This pass must run after LifetimeDependenceInsertion and before
// LifetimeDependenceDiagnostics.

import SIL

private let verbose = false

private func log(prefix: Bool = true, _ message: @autoclosure () -> String) {
  if verbose {
    print((prefix ? "### " : "") + message())
  }
}

let lifetimeDependenceScopeFixupPass = FunctionPass(
  name: "lifetime-dependence-scope-fixup")
{ (function: Function, context: FunctionPassContext) in
#if os(Windows)
  if !context.options.hasFeature(.NonescapableTypes) {
    return
  }
#endif
  log(prefix: false, "\n--- Scope fixup for lifetime dependence in \(function.name)")

  let localReachabilityCache = LocalVariableReachabilityCache()

  for instruction in function.instructions {
    guard let markDep = instruction as? MarkDependenceInst else {
      continue
    }
    guard let lifetimeDep = LifetimeDependence(markDep, context) else {
      continue
    }
    if let arg = extendAccessScopes(dependence: lifetimeDep, localReachabilityCache, context) {
      markDep.baseOperand.set(to: arg, context)
    }
  }
}

/// Extend all access scopes that enclose `dependence`. If dependence is on an access scope in the caller, then return
/// the function argument that represents the dependence scope.
private func extendAccessScopes(dependence: LifetimeDependence,
                                _ localReachabilityCache: LocalVariableReachabilityCache,
                                _ context: FunctionPassContext) -> FunctionArgument? {
  log("Scope fixup for lifetime dependent instructions: \(dependence)")

  guard case .access(let beginAccess) = dependence.scope else {
    return nil
  }
  let function = beginAccess.parentFunction

  // Get the range accessBase lifetime. The accessRange cannot exceed this without producing invalid SIL.
  guard var ownershipRange = AddressOwnershipLiveRange.compute(for: beginAccess.address, at: beginAccess,
                                                               localReachabilityCache, context) else {
    return nil
  }
  defer { ownershipRange.deinitialize() }

  var accessRange = InstructionRange(begin: beginAccess, context)
  defer {accessRange.deinitialize()}

  var walker = LifetimeDependenceScopeFixupWalker(function, localReachabilityCache, context) {
    // Do not extend the accessRange past the ownershipRange.
    let dependentInst = $0.instruction
    if ownershipRange.coversUse(dependentInst) {
      accessRange.insert(dependentInst)
    }
    return .continueWalk
  }
  defer {walker.deinitialize()}

  _ = walker.walkDown(root: dependence.dependentValue)

  log("Scope fixup for dependent uses:\n\(accessRange)")

  // Lifetime dependenent uses may not be dominated by the access. The dependent value may be used by a phi or stored
  // into a memory location. The access may be conditional relative to such uses. If any use was not dominated, then
  // `accessRange` will include the function entry.
  let firstInst = function.entryBlock.instructions.first!
  if firstInst != beginAccess, accessRange.contains(firstInst) {
    return nil
  }
  if let arg = extendAccessScope(beginAccess: beginAccess, range: &accessRange, context) {
    // If the dependent value is returned, then return the FunctionArgument that it depends on.
    assert(walker.dependsOnCaller)
    return arg
  }
  return nil
}

/// Extend this access scope to cover the dependent uses. Recursively extend outer accesses to maintain nesting.
///
/// Note that we cannot simply rewrite the `mark_dependence` to depend on an outer access scope. For 'read' access, this
/// could let us avoid extending the inner scope, but that would not accomplish anything useful because inner 'read's
/// can always be extended up to the extent of their outer 'read' (ignoring the special case when the dependence is on a
/// caller scope, which is handled separately). A nested 'read' access can never interfere with another access in the
/// same outer 'read', because it is impossible to nest a 'modify' access within a 'read'. For 'modify' accesses,
/// however, the inner scope must be extended for correctness. A 'modify' access can interfere with other 'modify'
/// accesss in the same scope. We rely on exclusivity diagnostics to report these interferences. For example:
///
///     sil @foo : $(@inout C) -> () {
///       bb0(%0 : $*C):
///         %a1 = begin_access [modify] %0
///         %d = apply @getDependent(%a1)
///         mark_dependence [unresolved] %d on %a1
///         end_access %a1
///         %a2 = begin_access [modify] %0
///         ...
///         end_access %a2
///         apply @useDependent(%d) // exclusivity violation
///         return
///     }
///
/// The call to `@useDependent` is an exclusivity violation because it uses a value that depends on a 'modify'
/// access. This scope fixup pass must extend '%a1' to cover the `@useDependent` but must not extend the base of the
/// `mark_dependence` to the outer access `%0`. This ensures that exclusivity diagnostics correctly reports the
/// violation, and that subsequent optimizations do not shrink the inner access `%a1`.
private func extendAccessScope(beginAccess: BeginAccessInst, range: inout InstructionRange,
                               _ context: FunctionPassContext) -> FunctionArgument? {
  var endAcceses = [Instruction]()
  // Collect the original end_access instructions and extend the range to to cover them. The resulting access scope must
  // cover the original scope because it may protect other memory operations.
  var requiresExtension = false
  for end in beginAccess.endInstructions {
    endAcceses.append(end)
    if range.contains(end) {
      // If any end_access is inside the new range, then all end_accesses must be rewritten.
      requiresExtension = true
    } else {
      range.insert(end)
    }
  }
  if !requiresExtension {
    return nil
  }
  assert(!range.ends.isEmpty)

  // Create new end_access at the end of extended uses
  var dependsOnCaller = false
  for end in range.ends {
    let location = end.location.autoGenerated
    if end is ReturnInst {
      dependsOnCaller = true
      let endAccess = Builder(before: end, location: location, context).createEndAccess(beginAccess: beginAccess)
      range.insert(endAccess)
      continue
    }
    Builder.insert(after: end, location: location, context) {
      let endAccess = $0.createEndAccess(beginAccess: beginAccess)
      // This scope should be nested in any outer scopes.
      range.insert(endAccess)
    }
  }
  // Delete original end_access instructions
  for endAccess in endAcceses {
    context.erase(instruction: endAccess)
  }
  // TODO: Add SIL support for lifetime dependence and write unit test for nested access scopes
  switch beginAccess.address.enclosingAccessScope {
  case let .scope(enclosingBeginAccess):
    return extendAccessScope(beginAccess: enclosingBeginAccess, range: &range, context)
  case let .base(accessBase):
    if case let .argument(arg) = accessBase, dependsOnCaller {
      return arg
    }
    return nil
  }
}

private struct LifetimeDependenceScopeFixupWalker : LifetimeDependenceDefUseWalker {
  let function: Function
  let context: Context
  let visitor: (Operand) -> WalkResult
  let localReachabilityCache: LocalVariableReachabilityCache
  var visitedValues: ValueSet

  /// Set to true if the dependence is returned from the current function.
  var dependsOnCaller = false

  init(_ function: Function, _ localReachabilityCache: LocalVariableReachabilityCache, _ context: Context,
       visitor: @escaping (Operand) -> WalkResult) {
    self.function = function
    self.context = context
    self.visitor = visitor
    self.localReachabilityCache = localReachabilityCache
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
    if let operand {
      return visitor(operand)
    }
    return .continueWalk
  }

  mutating func leafUse(of operand: Operand) -> WalkResult {
    return visitor(operand)
  }

  mutating func escapingDependence(on operand: Operand) -> WalkResult {
    log(">>> Escaping dependence: \(operand)")
    _ = visitor(operand)
    // Make a best-effort attempt to extend the access scope regardless of escapes. It is possible that some mandatory
    // pass between scope fixup and diagnostics will make it possible for the LifetimeDependenceDefUseWalker to analyze
    // this use.
    return .continueWalk
  }

  mutating func returnedDependence(result operand: Operand) -> WalkResult {
    dependsOnCaller = true
    return visitor(operand)
  }

  mutating func returnedDependence(address: FunctionArgument,
                                   using operand: Operand) -> WalkResult {
    dependsOnCaller = true
    return visitor(operand)
  }

  mutating func yieldedDependence(result: Operand) -> WalkResult {
    return .continueWalk
  }
}

