//===--- LifetimeDependenceDiagnostics.swift - Lifetime dependence --------===//
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

import SIL

private let verbose = false

private func log(prefix: Bool = true, _ message: @autoclosure () -> String) {
  if verbose {
    print((prefix ? "### " : "") + message())
  }
}

/// Diagnostic pass.
///
/// Find the roots of all non-escapable values in this function. All
/// non-escapable values either depend on a NonEscapingScope, or they
/// are produced by a LifetimeDependentInstruction that has no
/// dependence on a parent value (@_unsafeNonEscapableResult).
let lifetimeDependenceDiagnosticsPass = FunctionPass(
  name: "lifetime-dependence-diagnostics")
{ (function: Function, context: FunctionPassContext) in
#if os(Windows)
  if !context.options.hasFeature(.NonescapableTypes) {
    return
  }
#endif
  log(prefix: false, "\n--- Diagnosing lifetime dependence in \(function.name)")
  log("\(function)")

  for argument in function.arguments
      where !argument.type.isEscapable(in: function)
  {
    // Indirect results are not checked here. Type checking ensures
    // that they have a lifetime dependence.
    if let lifetimeDep = LifetimeDependence(argument, context) {
      analyze(dependence: lifetimeDep, context)
    }
  }
  for instruction in function.instructions {
    if let markDep = instruction as? MarkDependenceInst, markDep.isUnresolved {
      if let lifetimeDep = LifetimeDependence(markDep, context) {
        analyze(dependence: lifetimeDep, context)
      }
      continue
    }
    if let apply = instruction as? FullApplySite {
      // Handle ~Escapable results that do not have a lifetime
      // dependence (@_unsafeNonescapableResult).
      apply.resultOrYields.forEach {
        if let lifetimeDep = LifetimeDependence(unsafeApplyResult: $0,
                                                context) {
          analyze(dependence: lifetimeDep, context)
        }
      }
      continue
    }
  }
}

/// Analyze a single Lifetime dependence and trigger diagnostics.
///
/// 1. Compute the LifetimeDependence scope.
///
/// 2. Walk down all dependent values checking that they are within range.
private func analyze(dependence: LifetimeDependence,
  _ context: FunctionPassContext) {
  log("Dependence scope:\n\(dependence)")
    
  // Compute this dependence scope.
  var range = dependence.computeRange(context)
  defer { range?.deinitialize() }

  var error = false
  let diagnostics =
    DiagnoseDependence(dependence: dependence, range: range,
                       onError: { error = true }, context: context)

  // Check each lifetime-dependent use via a def-use visitor
  var walker = DiagnoseDependenceWalker(diagnostics, context)
  defer { walker.deinitialize() }
  _ = walker.walkDown(root: dependence.dependentValue)

  if !error {
    dependence.resolve(context)
  }
}

/// Analyze and diagnose a single LifetimeDependence.
private struct DiagnoseDependence {
  let dependence: LifetimeDependence
  let range: InstructionRange?
  let onError: ()->()
  let context: FunctionPassContext

  var function: Function { dependence.function }

  func diagnose(_ position: SourceLoc?, _ id: DiagID,
                _ args: DiagnosticArgument...) {
    context.diagnosticEngine.diagnose(position, id, args)
  }

  /// Check that this use is inside the dependence scope.
  func checkInScope(operand: Operand) -> WalkResult {
    if let range, !range.inclusiveRangeContains(operand.instruction) {
      log("  out-of-range: \(operand.instruction)")
      reportError(operand: operand, diagID: .lifetime_outside_scope_use)
      return .abortWalk
    }
    log("  contains: \(operand.instruction)")
    return .continueWalk
  }

  func reportEscaping(operand: Operand) {
    log("  escaping: \(operand.instruction)")
    reportError(operand: operand, diagID: .lifetime_outside_scope_escape)
  }

  func reportUnknown(operand: Operand) {
#if !os(Windows)
    // TODO: https://github.com/apple/swift/issues/73252
    standardError.write("Unknown use: \(operand)\n\(function)")
#endif
    reportEscaping(operand: operand)
  }

  func checkFunctionResult(operand: Operand) -> WalkResult {

    if function.hasUnsafeNonEscapableResult {
      return .continueWalk
    }
    // FIXME: remove this condition once we have a Builtin.dependence,
    // which developers should use to model the unsafe
    // dependence. Builtin.lifetime_dependence will be lowered to
    // mark_dependence [unresolved], which will be checked
    // independently. Instead, of this function result check, allow
    // isUnsafeApplyResult to be used be mark_dependence [unresolved]
    // without checking its dependents.
    //
    // Allow returning an apply result (@_unsafeNonescapableResult) if
    // the calling function has a dependence. This implicitly makes
    // the unsafe nonescapable result dependent on the calling
    // function's lifetime dependence arguments.
    if dependence.isUnsafeApplyResult, function.hasResultDependence {
      return .continueWalk
    }
    // Check that the argument dependence for this result is the same
    // as the current dependence scope.
    if let arg = dependence.scope.parentValue as? FunctionArgument,
       function.argumentConventions[resultDependsOn: arg.index] != nil {
      // The returned value depends on a lifetime that is inherited or
      // borrowed in the caller. The lifetime of the argument value
      // itself is irrelevant here.
      return .continueWalk
    }
    reportEscaping(operand: operand)
    return .abortWalk
  }

  func reportError(operand: Operand, diagID: DiagID) {
    onError()

    // Identify the escaping variable.
    let escapingVar = LifetimeVariable(dependent: operand.value, context)
    let varName = escapingVar.name
    if let varName {
      diagnose(escapingVar.sourceLoc, .lifetime_variable_outside_scope,
               varName)
    } else {
      diagnose(escapingVar.sourceLoc, .lifetime_value_outside_scope)
    }
    reportScope()
    // Identify the use point.
    let userSourceLoc = operand.instruction.location.sourceLoc
    diagnose(userSourceLoc, diagID)
  }

  // Identify the dependence scope.
  func reportScope() {
    if case let .access(beginAccess) = dependence.scope {
      let parentVar = LifetimeVariable(dependent: beginAccess, context)
      if let sourceLoc = beginAccess.location.sourceLoc ?? parentVar.sourceLoc {
        diagnose(sourceLoc, .lifetime_outside_scope_access,
                 parentVar.name ?? "")
      }
      return
    }
    if let arg = dependence.parentValue as? Argument,
       let varDecl = arg.varDecl,
       let sourceLoc = arg.sourceLoc {
      diagnose(sourceLoc, .lifetime_outside_scope_argument,
               varDecl.userFacingName)
      return
    }
    let parentVar = LifetimeVariable(dependent: dependence.parentValue, context)
    if let parentLoc = parentVar.sourceLoc {
      if let parentName = parentVar.name {
        diagnose(parentLoc, .lifetime_outside_scope_variable, parentName)
      } else {
        diagnose(parentLoc, .lifetime_outside_scope_value)
      }
    }
  }
}

private extension Instruction {
  func findVarDecl() -> VarDecl? {
    if let varDeclInst = self as? VarDeclInstruction {
      return varDeclInst.varDecl
    }
    for result in results {
      for use in result.uses {
        if let debugVal = use.instruction as? DebugValueInst {
          return debugVal.varDecl
        }
      }
    }
    return nil
  }
}

// Identify a best-effort variable declaration based on a defining SIL
// value or any lifetime dependent use of that SIL value.
private struct LifetimeVariable {
  var varDecl: VarDecl?
  var sourceLoc: SourceLoc?
  
  var name: String? {
    return varDecl?.userFacingName
  }

  init(dependent value: Value, _ context: some Context) {
    if value.type.isAddress {
      self = Self(accessBase: value.accessBase, context)
      return
    }
    if let firstIntroducer = getFirstVariableIntroducer(of: value, context) {
      self = Self(introducer: firstIntroducer)
      return
    }
    self.varDecl = nil
    self.sourceLoc = nil
  }

  private func getFirstVariableIntroducer(of value: Value, _ context: some Context) -> Value? {
    var introducer: Value?
    var useDefVisitor = VariableIntroducerUseDefWalker(context) {
      introducer = $0
      return .abortWalk
    }
    defer { useDefVisitor.deinitialize() }
    _ = useDefVisitor.walkUp(valueOrAddress: value)
    return introducer
  }

  private init(introducer: Value) {
    if let arg = introducer as? Argument {
      self.varDecl = arg.varDecl
    } else {
      self.sourceLoc = introducer.definingInstruction?.location.sourceLoc
      self.varDecl = introducer.definingInstruction?.findVarDecl()
    }
    if let varDecl {
      sourceLoc = varDecl.sourceLoc
    }
  }

  // Record the source location of the variable decl if possible. The
  // caller will already have a source location for the formal access,
  // which is more relevant for diagnostics.
  private init(accessBase: AccessBase, _ context: some Context) {
    switch accessBase {
    case .box(let projectBox):
      // Note: referenceRoot looks through `begin_borrow [var_decl]` and `move_value [var_decl]`. But the box should
      // never be produced by one of these, except when it is redundant with the `alloc_box` VarDecl. It does not seem
      // possible for a box to be moved/borrowed directly into another variable's box. Reassignment always loads/stores
      // the value.
      self = Self(introducer: projectBox.box.referenceRoot)
    case .stack(let allocStack):
      self = Self(introducer: allocStack)
    case .global(let globalVar):
      self.varDecl = globalVar.varDecl
      self.sourceLoc = nil
    case .class(let refAddr):
      self.varDecl = refAddr.varDecl
      self.sourceLoc = refAddr.location.sourceLoc
    case .tail(let refTail):
      self = Self(introducer: refTail.instance)
    case .argument(let arg):
      self.varDecl = arg.varDecl
      self.sourceLoc = arg.sourceLoc
    case .yield(let result):
      // TODO: bridge VarDecl for FunctionConvention.Yields
      self.varDecl = nil
      self.sourceLoc = result.parentInstruction.location.sourceLoc
    case .storeBorrow(let sb):
      self = .init(dependent: sb.source, context)
    case .pointer(let ptrToAddr):
      self.varDecl = nil
      self.sourceLoc = ptrToAddr.location.sourceLoc
    case .unidentified:
      self.varDecl = nil
      self.sourceLoc = nil
    }
  }
}

/// Walk down lifetime depenence uses. For each check that all dependent
/// leaf uses are non-escaping and within the dependence scope. The walk
/// starts with add address for .access dependencies. The walk can
/// transition from an address to a value at a load. The walk can
/// transition from a value to an address as follows:
///
///     %dependent_addr = mark_dependence [nonescaping] %base_addr on %value
///
/// TODO: handle stores to singly initialized temporaries like copies using a standard reaching-def analysis.
private struct DiagnoseDependenceWalker {
  let context: Context
  var diagnostics: DiagnoseDependence
  let localReachabilityCache = LocalVariableReachabilityCache()
  var visitedValues: ValueSet

  var function: Function { diagnostics.function }
  
  init(_ diagnostics: DiagnoseDependence, _ context: Context) {
    self.context = context
    self.diagnostics = diagnostics
    self.visitedValues = ValueSet(context)
  }
  
  mutating func deinitialize() {
    visitedValues.deinitialize()
  }
}

extension DiagnoseDependenceWalker : LifetimeDependenceDefUseWalker {
  mutating func needWalk(for value: Value) -> Bool {
    visitedValues.insert(value)
  }

  mutating func leafUse(of operand: Operand) -> WalkResult {
    return diagnostics.checkInScope(operand: operand)
  }

  mutating func deadValue(_ value: Value, using operand: Operand?)
    -> WalkResult {
    // Ignore a dead root value. It never escapes.
    if let operand {
      return diagnostics.checkInScope(operand: operand)
    }
    return .continueWalk
  }

  mutating func escapingDependence(on operand: Operand) -> WalkResult {
    diagnostics.reportEscaping(operand: operand)
    return .abortWalk
  }

  mutating func returnedDependence(result: Operand) -> WalkResult {
    return diagnostics.checkFunctionResult(operand: result)
  }

  mutating func returnedDependence(address: FunctionArgument,
                                   using operand: Operand) -> WalkResult {
    return diagnostics.checkFunctionResult(operand: operand)
  }

  mutating func yieldedDependence(result: Operand) -> WalkResult {
    return diagnostics.checkFunctionResult(operand: result)
  }

  // Override AddressUseVisitor here because LifetimeDependenceDefUseWalker
  // returns .abortWalk, and we want a more useful crash report.
  mutating func unknownAddressUse(of operand: Operand) -> WalkResult {
    diagnostics.reportUnknown(operand: operand)
    return .continueWalk
  }
}
