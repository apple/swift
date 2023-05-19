//===--- NamedReturnValueOptimization.swift --------------------------------==//
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

/// Removes a `copy_addr` to an indirect out argument by replacing the source of the copy
/// (which must be an `alloc_stack`) with the out argument itself.
///
/// The following SIL pattern will be optimized:
///
///   sil @foo : $@convention(thin) <T> () -> @out T {
///   bb0(%0 : $*T):
///     %2 = alloc_stack $T
///     ...
///     copy_addr %some_value to [init] %2       // or any other writes to %2
///     ...
///   bbN:
///     copy_addr [take] %2 to [init] %0 : $*T   // the only use of %0
///     ... // no writes
///     return
///
/// to:
///
///   sil @foo : $@convention(thin) <T> (@out T) -> () {
///   bb0(%0 : $*T):
///     %2 = alloc_stack $T                      // is dead now
///     ...
///     copy_addr %some_value to [init] %0
///     ...
///   bbN:
///     ...
///     return
///
/// This optimization can be done because we know that:
/// * The out argument dominates all uses of the copy_addr's source (because it's a function argument).
/// * It's not aliased (by definition). We can't allow aliases to be accessed between the initialization and the return.
///
/// This pass shouldn't run before serialization. It might prevent predictable memory optimizations
/// in a caller after inlining, because the memory location (the out argument = an alloc_stack in the caller)
/// might be written multiple times after this optimization.
///
let namedReturnValueOptimization = FunctionPass(name: "named-return-value-optimization") {
  (function: Function, context: FunctionPassContext) in

  for outArg in function.arguments[0..<function.numIndirectResultArguments] {
    if let copyToArg = findCopyForNRVO(for: outArg) {
      performNRVO(with: copyToArg, context)
    }
  }
}

/// Returns a copy_addr which copies from an alloc_stack to the `outArg` at the end of the function.
///
private func findCopyForNRVO(for outArg: FunctionArgument) -> CopyAddrInst? {
  guard let singleArgUse = outArg.uses.singleNonDebugUse,
        let copyToArg = singleArgUse.instruction as? CopyAddrInst else {
    return nil
  }

  assert(singleArgUse == copyToArg.destinationOperand,
         "single use of out-argument cannot be the source of a copy")

  // Don't perform NRVO unless the copy is a [take]. This is the easiest way
  // to determine that the local variable has ownership of its value and ensures
  // that removing a copy is a reference count neutral operation. For example,
  // this copy can't be trivially eliminated without adding a retain.
  //   sil @f : $@convention(thin) (@guaranteed T) -> @out T
  //   bb0(%in : $*T, %out : $T):
  //     %local = alloc_stack $T
  //     store %in to %local : $*T
  //     copy_addr %local to [init] %out : $*T
  if !copyToArg.isTakeOfSrc {
    return nil
  }

  guard let sourceStackAlloc = copyToArg.source as? AllocStackInst else {
    return nil
  }

  // NRVO for alloc_stack [dynamic_lifetime] will invalidate OSSA invariants.
  if sourceStackAlloc.hasDynamicLifetime && copyToArg.parentFunction.hasOwnership {
    return nil
  }

  if !(copyToArg.parentBlock.terminator is ReturnInst) {
    return nil
  }

  // This check is overly conservative, because we only need to check if the source
  // of the copy is not written to. But the copy to the out argument is usually the last
  // instruction of the function, so it doesn't matter.
  if isAnyInstructionWritingToMemory(after: copyToArg) {
    return nil
  }

  return copyToArg
}

private func performNRVO(with copy: CopyAddrInst, _ context: FunctionPassContext) {
  copy.source.uses.replaceAllExceptDealloc(with: copy.destination, context)
  assert(copy.source == copy.destination)
  context.erase(instruction: copy)
}

private func isAnyInstructionWritingToMemory(after: Instruction) -> Bool {
  var followingInst = after.next
  while let fi = followingInst {
    if fi.mayWriteToMemory && !(fi is DeallocStackInst) {
      return true
    }
    followingInst = fi.next
  }
  return false
}

private extension UseList {
  func replaceAllExceptDealloc(with replacement: Value, _ context: some MutatingContext) {
    for use in self where !(use.instruction is Deallocation)  {
      use.set(to: replacement, context)
    }
  }
}
