//===--- MandatoryPerformanceOptimizations.swift --------------------------===//
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

/// Performs mandatory optimizations for performance-annotated functions.
///
/// Optimizations include:
/// * de-virtualization
/// * mandatory inlining
/// * generic specialization
/// * mandatory memory optimizations
/// * dead alloc elimination
/// * instruction simplification
///
/// The pass starts with performance-annotated functions and transitively handles
/// called functions.
///
let mandatoryPerformanceOptimizations = ModulePass(name: "mandatory-performance-optimizations") {
  (moduleContext: ModulePassContext) in

  var worklist = FunctionWorklist()
  worklist.addAllPerformanceAnnotatedFunctions(of: moduleContext)

  optimizeFunctionsTopDown(using: &worklist, moduleContext)
}

private func optimizeFunctionsTopDown(using worklist: inout FunctionWorklist,
                                      _ moduleContext: ModulePassContext) {
  while let f = worklist.pop() {
    moduleContext.transform(function: f) { context in
      if !context.loadFunction(function: f, loadCalleesRecursively: true) {
        return
      }
      optimize(function: f, context)
      worklist.add(calleesOf: f)
    }
  }
}

private func optimize(function: Function, _ context: FunctionPassContext) {
  runSimplification(on: function, context, preserveDebugInfo: true) { instruction, simplifyCtxt in
    if let i = instruction as? OnoneSimplifyable {
      i.simplify(simplifyCtxt)
      if instruction.isDeleted {
        return
      }
    }
    switch instruction {
    case let apply as FullApplySite:
      inlineAndDevirtualize(apply: apply, context, simplifyCtxt)
    case let mt as MetatypeInst:
      if mt.isTriviallyDeadIgnoringDebugUses {
        simplifyCtxt.erase(instructionIncludingDebugUses: mt)
      }
    default:
      break
    }
  }

  _ = context.specializeApplies(in: function, isMandatory: true)

  // If this is a just specialized function, try to optimize copy_addr, etc.
  if context.optimizeMemoryAccesses(in: function) {
    _ = context.eliminateDeadAllocations(in: function)
  }
}

private func inlineAndDevirtualize(apply: FullApplySite, _ context: FunctionPassContext, _ simplifyCtxt: SimplifyContext) {

  if simplifyCtxt.tryDevirtualize(apply: apply, isMandatory: true) != nil {
    return
  }

  guard let callee = apply.referencedFunction else {
    return
  }

  if !context.loadFunction(function: callee, loadCalleesRecursively: true) {
    // We don't have the funcion body of the callee.
    return
  }

  if shouldInline(apply: apply, callee: callee) {
    simplifyCtxt.inlineFunction(apply: apply, mandatoryInline: true)

    // In OSSA `partial_apply [on_stack]`s are represented as owned values rather than stack locations.
    // It is possible for their destroys to violate stack discipline.
    // When inlining into non-OSSA, those destroys are lowered to dealloc_stacks.
    // This can result in invalid stack nesting.
    if callee.hasOwnership && !apply.parentFunction.hasOwnership  {
      simplifyCtxt.notifyInvalidatedStackNesting()
    }
  }
}

private func shouldInline(apply: FullApplySite, callee: Function) -> Bool {
  if callee.isTransparent {
    return true
  }
  if apply is BeginApplyInst {
    // Avoid co-routines because they might allocate (their context).
    return true
  }
  if apply.parentFunction.isGlobalInitOnceFunction && callee.inlineStrategy == .always {
    // Some arithmetic operations, like integer conversions, are not transparent but `inline(__always)`.
    // Force inlining them in global initializers so that it's possible to statically initialize the global.
    return true
  }
  return false
}

fileprivate struct FunctionWorklist {
  private(set) var functions = Array<Function>()
  private var pushedFunctions = Set<Function>()
  private var currentIndex = 0

  mutating func pop() -> Function? {
    if currentIndex < functions.count {
      let f = functions[currentIndex]
      currentIndex += 1
      return f
    }
    return nil
  }

  mutating func addAllPerformanceAnnotatedFunctions(of moduleContext: ModulePassContext) {
    for f in moduleContext.functions where f.performanceConstraints != .none {
      pushIfNotVisited(f)
    }
  }

  mutating func add(calleesOf function: Function) {
    for inst in function.instructions {
      switch inst {
      case let apply as ApplySite:
        if let callee = apply.referencedFunction {
          pushIfNotVisited(callee)
        }
      case let bi as BuiltinInst:
        switch bi.id {
        case .Once, .OnceWithContext:
          if let fri = bi.operands[1].value as? FunctionRefInst {
            pushIfNotVisited(fri.referencedFunction)
          }
          break;
        default:
          break
        }
      default:
        break
      }
    }
  }

  mutating func pushIfNotVisited(_ element: Function) {
    if pushedFunctions.insert(element).inserted {
      functions.append(element)
    }
  }
}
