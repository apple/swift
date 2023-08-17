//===--- ReleaseDevirtualizer.swift - Devirtualizes release-instructions --===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SIL

/// Devirtualizes release instructions which are known to destruct the object.
///
/// This means, it replaces a sequence of
///    %x = alloc_ref [stack] $X
///      ...
///    strong_release %x
///    dealloc_stack_ref %x
/// with
///    %x = alloc_ref [stack] $X
///      ...
///    set_deallocating %x
///    %d = function_ref @dealloc_of_X
///    %a = apply %d(%x)
///    dealloc_stack_ref %x
///
/// The optimization is only done for stack promoted objects because they are
/// known to have no associated objects (which are not explicitly released
/// in the deinit method).
let releaseDevirtualizerPass = FunctionPass(name: "release-devirtualizer") {
  (function: Function, context: FunctionPassContext) in

  for block in function.blocks {
    // The last `release_value`` or `strong_release`` instruction before the
    // deallocation.
    var lastRelease: RefCountingInst?

    for instruction in block.instructions {
      if let release = lastRelease {
        // We only do the optimization for stack promoted object, because for
        // these we know that they don't have associated objects, which are
        // _not_ released by the deinit method.
        if let deallocStackRef = instruction as? DeallocStackRefInst {
          if !context.continueWithNextSubpassRun(for: release) {
            return
          }
          tryDevirtualizeReleaseOfObject(context, release, deallocStackRef)
          lastRelease = nil
          continue
        }
      }

      switch instruction {
        case is ReleaseValueInst, is StrongReleaseInst:
          lastRelease = instruction as? RefCountingInst
        case is DeallocRefInst, is SetDeallocatingInst:
          lastRelease = nil
        default:
          if instruction.mayRelease {
            lastRelease = nil
          }
      }
    }
  }
}

/// Tries to de-virtualize the final release of a stack-promoted object.
private func tryDevirtualizeReleaseOfObject(
  _ context: FunctionPassContext,
  _ release: RefCountingInst,
  _ deallocStackRef: DeallocStackRefInst
) {
  let allocRefInstruction = deallocStackRef.allocRef

  // Check if the release instruction right before the `dealloc_stack_ref` really releases
  // the allocated object (and not something else).
  var finder = FindAllocationOfRelease(allocation: allocRefInstruction)
  if !finder.allocationIsRoot(of: release.operand.value) {
    return
  }

  let type = allocRefInstruction.type

  guard let dealloc = context.calleeAnalysis.getDestructor(ofExactType: type) else {
    return
  }

  let builder = Builder(before: release, location: release.location, context)

  var object: Value = allocRefInstruction
  if object.type != type {
    object = builder.createUncheckedRefCast(from: object, to: type)
  }

  // Do what a release would do before calling the deallocator: set the object
  // in deallocating state, which means set the RC_DEALLOCATING_FLAG flag.
  builder.createSetDeallocating(operand: object, isAtomic: release.isAtomic)

  // Create the call to the destructor with the allocated object as self
  // argument.
  let functionRef = builder.createFunctionRef(dealloc)

  let substitutionMap = context.getContextSubstitutionMap(for: type)
  builder.createApply(function: functionRef, substitutionMap, arguments: [object])
  context.erase(instruction: release)
}

// Up-walker to find the root of a release instruction.
private struct FindAllocationOfRelease : ValueUseDefWalker {
  private let allocInst: AllocRefInstBase
  private var allocFound = false

  var walkUpCache = WalkerCache<UnusedWalkingPath>()

  init(allocation: AllocRefInstBase) { allocInst = allocation }

  /// The top-level entry point: returns true if the root of `value` is the `allocInst`.
  mutating func allocationIsRoot(of value: Value) -> Bool {
    return walkUp(value: value, path: UnusedWalkingPath()) != .abortWalk &&
           allocFound
  }

  mutating func rootDef(value: Value, path: UnusedWalkingPath) -> WalkResult {
    if value == allocInst {
      allocFound = true
      return .continueWalk
    }
    return .abortWalk
  }

  // This function is called for `struct` and `tuple` instructions in case the `path` doesn't select
  // a specific operand but all operands.
  mutating func walkUpAllOperands(of def: Instruction, path: UnusedWalkingPath) -> WalkResult {
  
    // Instead of walking up _all_ operands (which would be the default behavior), require that only a
    // _single_ operand is not trivial and walk up that operand.
    // We need to check this because the released value must not contain multiple copies of the
    // allocated object. We can only replace a _single_ release with the destructor call and not
    // multiple releases of the same object. E.g.
    //
    //    %x = alloc_ref [stack] $X
    //    strong_retain %x
    //    %t = tuple (%x, %x)
    //    release_value %t       // -> releases %x two times!
    //    dealloc_stack_ref %x
    //
    var nonTrivialOperandFound = false
    for operand in def.operands {
      if !operand.value.hasTrivialType {
        if nonTrivialOperandFound {
          return .abortWalk
        }
        nonTrivialOperandFound = true
        if walkUp(value: operand.value, path: path) == .abortWalk {
          return .abortWalk
        }
      }
    }
    return .continueWalk
  }
}
