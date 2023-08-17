//===--- StackProtection.swift --------------------------------------------===//
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

private let verbose = false

private func log(_ message: @autoclosure () -> String) {
  if verbose {
    print("### \(message())")
  }
}

/// Decides which functions need stack protection.
///
/// Sets the `needStackProtection` flags on all function which contain stack-allocated
/// values for which an buffer overflow could occur.
///
/// Within safe swift code there shouldn't be any buffer overflows. But if the address
/// of a stack variable is converted to an unsafe pointer, it's not in the control of
/// the compiler anymore.
/// This means, if an `alloc_stack` ends up at an `address_to_pointer [stack_protection]`,
/// the `alloc_stack`'s function is marked for stack protection.
/// Another case is `index_addr [stack_protection]` for non-tail allocated memory. This
/// pattern appears if pointer arithmetic is done with unsafe pointers in swift code.
///
/// If the origin of an unsafe pointer can only be tracked to a function argument, the
/// pass tries to find the root stack allocation for such an argument by doing an
/// inter-procedural analysis. If this is not possible and the `enableMoveInoutStackProtection`
/// option is set, the fallback is to move the argument into a temporary `alloc_stack`
/// and do the unsafe pointer operations on the temporary.
let stackProtection = ModulePass(name: "stack-protection") {
  (context: ModulePassContext) in

  if !context.options.enableStackProtection {
    return
  }

  var optimization = StackProtectionOptimization(enableMoveInout: context.options.enableMoveInoutStackProtection)
  optimization.processModule(context)
}

/// The stack-protection optimization on function-level.
///
/// In contrast to the `stack-protection` pass, this pass doesn't do any inter-procedural
/// analysis. It runs at Onone.
let functionStackProtection = FunctionPass(name: "function-stack-protection") {
  (function: Function, context: FunctionPassContext) in

  if !context.options.enableStackProtection {
    return
  }

  var optimization = StackProtectionOptimization(enableMoveInout: context.options.enableMoveInoutStackProtection)
  optimization.process(function: function, context)
}

/// The optimization algorithm.
private struct StackProtectionOptimization {

  private let enableMoveInout: Bool

  // The following members are nil/not used if this utility is used on function-level.

  private var moduleContext: ModulePassContext?
  private var functionUses = FunctionUses()
  private var functionUsesComputed = false

  // Functions (other than the currently processed one) which need stack protection,
  // are added to this array in `findOriginsInCallers`.
  private var needStackProtection: [Function] = []

  init(enableMoveInout: Bool) {
    self.enableMoveInout = enableMoveInout
  }

  /// The main entry point if running on module-level.
  mutating func processModule(_ moduleContext: ModulePassContext) {
    self.moduleContext = moduleContext

    // Collect all functions which need stack protection and insert required moves.
    for function in moduleContext.functions {

      moduleContext.transform(function: function) { context in
        process(function: function, context)
      }

      // We cannot modify other functions than the currently processed function in `process(function:)`.
      // Therefore, if `findOriginsInCallers` finds any callers, which need stack protection,
      // add the `needStackProtection` flags here.
      for function in needStackProtection {
        moduleContext.transform(function: function) { context in
          function.setNeedsStackProtection(context)
        }
      }
      needStackProtection.removeAll(keepingCapacity: true)
    }
  }
  
  /// The main entry point if running on function-level.
  mutating func process(function: Function, _ context: FunctionPassContext) {
    var mustFixStackNesting = false
    for inst in function.instructions {
      process(instruction: inst, in: function, mustFixStackNesting: &mustFixStackNesting, context)
    }
    if mustFixStackNesting {
      function.fixStackNesting(context)
    }
  }

  /// Checks if `instruction` may be an unsafe instruction which may cause a buffer overflow.
  ///
  /// If this is the case, either
  /// - set the function's `needStackProtection` flag if the relevant allocation is in the
  ///   same function, or
  /// - if the address is passed as an argument: try to find the origin in its callers and
  ///   add the relevant callers to `self.needStackProtection`, or
  /// - if the origin is unknown, move the value into a temporary and set the function's
  ///   `needStackProtection` flag.
  private mutating func process(instruction: Instruction, in function: Function,
                                mustFixStackNesting: inout Bool, _ context: FunctionPassContext) {

    // `withUnsafeTemporaryAllocation(of:capacity:_:)` is compiled to a `builtin "stackAlloc"`.
    if let bi = instruction as? BuiltinInst, bi.id == .StackAlloc {
      function.setNeedsStackProtection(context)
      return
    }

    // For example:
    //    %accessBase = alloc_stack $S
    //    %scope = begin_access [modify] %accessBase
    //    %instruction = address_to_pointer [stack_protection] %scope
    //
    guard let (accessBase, scope) = instruction.accessBaseToProtect else {
      return
    }

    switch accessBase.isStackAllocated {
    case .no:
      // For example:
      //     %baseAddr = global_addr @global
      break

    case .yes:
      // For example:
      //     %baseAddr = alloc_stack $T
      log("local: \(function.name) -- \(instruction)")

      function.setNeedsStackProtection(context)

    case .decidedInCaller(let arg):
      // For example:
      //   bb0(%baseAddr: $*T):

      var worklist = ArgumentWorklist(context)
      defer { worklist.deinitialize() }
      worklist.push(arg)

      if findOriginsInCallers(&worklist) == NeedInsertMoves.yes {
        // We don't know the origin of the function argument. Therefore we need to do the
        // conservative default which is to move the value to a temporary stack location.
        if let beginAccess = scope {
          // If there is an access, we need to move the destination of the `begin_access`.
          // We should never change the source address of a `begin_access` to a temporary.
          moveToTemporary(scope: beginAccess, mustFixStackNesting: &mustFixStackNesting, context)
        } else {
          moveToTemporary(argument: arg, context)
        }
      }

    case .objectIfStackPromoted(let obj):
      // For example:
      //     %0 = alloc_ref [stack] $Class
      //     %baseAddr = ref_element_addr %0 : $Class, #Class.field

      var worklist = ArgumentWorklist(context)
      defer { worklist.deinitialize() }

      // If the object is passed as an argument to its function, add those arguments
      // to the worklist.
      let (_, foundStackAlloc) = worklist.push(rootsOf: obj)
      if foundStackAlloc {
        // The object is created by an `alloc_ref [stack]`.
        log("objectIfStackPromoted: \(function.name) -- \(instruction)")

        function.setNeedsStackProtection(context)
      }
      // In case the (potentially) stack allocated object is passed via an argument,
      // process the worklist as we do for indirect arguments (see above).
      // For example:
      //   bb0(%0: $Class):
      //     %baseAddr = ref_element_addr %0 : $Class, #Class.field
      if findOriginsInCallers(&worklist) == NeedInsertMoves.yes,
         let beginAccess = scope {
        // We don't know the origin of the object. Therefore we need to do the
        // conservative default which is to move the value to a temporary stack location.
        moveToTemporary(scope: beginAccess, mustFixStackNesting: &mustFixStackNesting, context)
      }

    case .unknown:
      // TODO: better handling of unknown access bases
      break
    }
  }

  /// Return value of `findOriginsInCallers()`.
  enum NeedInsertMoves {
    // Not all call sites could be identified, and if moves are enabled (`enableMoveInout`)
    // the original argument should be moved to a temporary.
    case yes

    // Either all call sites could be identified, which means that stack protection is done
    // in the callers, or moves are not enabled (`enableMoveInout` is false).
    case no
  }

  /// Find all origins of function arguments in `worklist`.
  /// All functions, which allocate such an origin are added to `self.needStackProtection`.
  /// Returns true if all origins could be found and false, if there are unknown origins.
  private mutating func findOriginsInCallers(_ worklist: inout ArgumentWorklist) -> NeedInsertMoves {
  
    guard let moduleContext = moduleContext else {
      // Don't do any inter-procedural analysis when used on function-level.
      return enableMoveInout ? .yes : .no
    }
  
    // Put the resulting functions into a temporary array, because we only add them to
    // `self.needStackProtection` if we don't return false.
    var newFunctions = Stack<Function>(moduleContext)
    defer { newFunctions.deinitialize() }

    if !functionUsesComputed {
      functionUses.collect(context: moduleContext)
      functionUsesComputed = true
    }

    while let arg = worklist.pop() {
      let f = arg.parentFunction
      let uses = functionUses.getUses(of: f)
      if uses.hasUnknownUses && enableMoveInout {
        return NeedInsertMoves.yes
      }
      
      for useInst in uses {
        guard let fri = useInst as? FunctionRefInst else {
          if enableMoveInout {
            return NeedInsertMoves.yes
          }
          continue
        }
      
        for functionRefUse in fri.uses {
          guard let apply = functionRefUse.instruction as? ApplySite,
                let callerArgIdx = apply.callerArgIndex(calleeArgIndex: arg.index) else {
            if enableMoveInout {
              return NeedInsertMoves.yes
            }
            continue
          }
          let callerArg = apply.arguments[callerArgIdx]
          if callerArg.type.isAddress {
            // It's an indirect argument.
            switch callerArg.accessBase.isStackAllocated {
            case .yes:
              if !callerArg.parentFunction.needsStackProtection {
                log("alloc_stack in caller: \(callerArg.parentFunction.name) -- \(callerArg)")
                newFunctions.push(callerArg.parentFunction)
              }
            case .no:
              break
            case .decidedInCaller(let callerFuncArg):
              if !callerFuncArg.convention.isInout {
                break
              }
              // The argumente is itself passed as an argument to its function.
              // Continue with looking into the callers.
              worklist.push(callerFuncArg)
            case .objectIfStackPromoted(let obj):
              // If the object is passed as an argument to its function,
              // add those arguments to the worklist.
              let (foundUnknownRoots, foundStackAlloc) = worklist.push(rootsOf: obj)
              if foundUnknownRoots && enableMoveInout {
                return NeedInsertMoves.yes
              }
              if foundStackAlloc && !obj.parentFunction.needsStackProtection {
                // The object is created by an `alloc_ref [stack]`.
                log("object in caller: \(obj.parentFunction.name) -- \(obj)")
                newFunctions.push(obj.parentFunction)
              }
            case .unknown:
              if enableMoveInout {
                return NeedInsertMoves.yes
              }
            }
          } else {
            // The argument is an object. If the object is itself passed as an argument
            // to its function, add those arguments to the worklist.
            let (foundUnknownRoots, foundStackAlloc) = worklist.push(rootsOf: callerArg)
            if foundUnknownRoots && enableMoveInout {
              return NeedInsertMoves.yes
            }
            if foundStackAlloc && !callerArg.parentFunction.needsStackProtection {
              // The object is created by an `alloc_ref [stack]`.
              log("object arg in caller: \(callerArg.parentFunction.name) -- \(callerArg)")
              newFunctions.push(callerArg.parentFunction)
            }
          }
        }
      }
    }
    needStackProtection.append(contentsOf: newFunctions)
    return NeedInsertMoves.no
  }

  /// Moves the value of an indirect argument to a temporary stack location, if possible.
  private func moveToTemporary(argument: FunctionArgument, _ context: FunctionPassContext) {
    if !argument.convention.isInout {
      // We cannot move from a read-only argument.
      // Also, read-only arguments shouldn't be subject to buffer overflows (because
      // no one should ever write to such an argument).
      return
    }

    let function = argument.parentFunction
    let entryBlock = function.entryBlock
    let loc = entryBlock.instructions.first!.location.autoGenerated
    let builder = Builder(atBeginOf: entryBlock, location: loc, context)
    let temporary = builder.createAllocStack(argument.type)
    argument.uses.replaceAll(with: temporary, context)

    builder.createCopyAddr(from: argument, to: temporary, takeSource: true, initializeDest: true)

    for block in function.blocks {
      let terminator = block.terminator
      if terminator.isFunctionExiting {
        let exitBuilder = Builder(before: terminator, location: terminator.location.autoGenerated, context)
        exitBuilder.createCopyAddr(from: temporary, to: argument, takeSource: true, initializeDest: true)
        exitBuilder.createDeallocStack(temporary)
      }
    }
    log("move addr protection in \(function.name): \(argument)")
    
    function.setNeedsStackProtection(context)
  }

  /// Moves the value of a `beginAccess` to a temporary stack location, if possible.
  private func moveToTemporary(scope beginAccess: BeginAccessInst, mustFixStackNesting: inout Bool,
                               _ context: FunctionPassContext) {
    if beginAccess.accessKind != .Modify {
      // We can only move from a `modify` access.
      // Also, read-only accesses shouldn't be subject to buffer overflows (because
      // no one should ever write to such a storage).
      return
    }
  
    let builder = Builder(after: beginAccess, location: beginAccess.location.autoGenerated, context)
    let temporary = builder.createAllocStack(beginAccess.type)

    for use in beginAccess.uses where !(use.instruction is EndAccessInst) {
      use.instruction.setOperand(at: use.index, to: temporary, context)
    }

    for endAccess in beginAccess.endInstructions {
      let endBuilder = Builder(before: endAccess, location: endAccess.location.autoGenerated, context)
      endBuilder.createCopyAddr(from: temporary, to: beginAccess, takeSource: true, initializeDest: true)
      endBuilder.createDeallocStack(temporary)
    }

    builder.createCopyAddr(from: beginAccess, to: temporary, takeSource: true, initializeDest: true)
    log("move object protection in \(beginAccess.parentFunction.name): \(beginAccess)")

    beginAccess.parentFunction.setNeedsStackProtection(context)

    // Access scopes are not necessarily properly nested, which can result in
    // not properly nested stack allocations.
    mustFixStackNesting = true
  }
}

/// Worklist for inter-procedural analysis of function arguments.
private struct ArgumentWorklist : ValueUseDefWalker {
  var walkUpCache = WalkerCache<SmallProjectionPath>()

  // Used in `push(rootsOf:)`
  private var foundStackAlloc = false
  private var foundUnknownRoots = false

  // Contains arguments which are already handled and don't need to be put into the worklist again.
  // Note that this cannot be a `ValueSet`, because argument can be from different functions.
  private var handled = Set<FunctionArgument>()

  // The actual worklist.
  private var list: Stack<FunctionArgument>

  init(_ context: FunctionPassContext) {
    self.list = Stack(context)
  }

  mutating func deinitialize() {
    list.deinitialize()
  }

  mutating func push(_ arg: FunctionArgument) {
    if handled.insert(arg).0 {
      list.push(arg)
    }
  }

  /// Pushes all roots of `object`, which are function arguments, to the worklist.
  /// If the returned `foundUnknownRoots` is true, it means that not all roots of `object` could
  /// be tracked to a function argument.
  /// If the returned `foundStackAlloc` than at least one found root is an `alloc_ref [stack]`.
  mutating func push(rootsOf object: Value) -> (foundUnknownRoots: Bool, foundStackAlloc: Bool) {
    foundStackAlloc = false
    foundUnknownRoots = false
    _ =  walkUp(value: object, path: SmallProjectionPath(.anything))
    return (foundUnknownRoots, foundStackAlloc)
  }

  mutating func pop() -> FunctionArgument? {
    return list.pop()
  }

  // Internal walker function.
  mutating func rootDef(value: Value, path: Path) -> WalkResult {
    switch value {
      case let ar as AllocRefInstBase:
        if ar.canAllocOnStack {
          foundStackAlloc = true
        }
      case let arg as FunctionArgument:
        push(arg)
    default:
        foundUnknownRoots = true
    }
    return .continueWalk
  }
}

private extension AccessBase {
  enum IsStackAllocatedResult {
    case yes
    case no
    case decidedInCaller(FunctionArgument)
    case objectIfStackPromoted(Value)
    case unknown
  }

  var isStackAllocated: IsStackAllocatedResult {
    switch self {
      case .stack:
        return .yes
      case .box, .global:
        return .no
      case .class(let rea):
        return .objectIfStackPromoted(rea.instance)
      case .tail(let rta):
        return .objectIfStackPromoted(rta.instance)
      case .argument(let arg):
        return .decidedInCaller(arg)
      case .yield, .pointer:
        return .unknown
      case .unidentified:
        // In the rare case of an unidentified access, just ignore it.
        // This should not happen in regular SIL, anyway.
        return .no
    }
  }
}

private extension Instruction {
  /// If the instruction needs stack protection, return the relevant access base and scope.
  var accessBaseToProtect: (AccessBase, scope: BeginAccessInst?)? {
    let baseAddr: Value
    switch self {
      case let atp as AddressToPointerInst:
        if !atp.needsStackProtection {
          return nil
        }
        var hasNoStores = NoStores()
        if hasNoStores.walkDownUses(ofValue: atp, path: SmallProjectionPath()) == .continueWalk {
          return nil
        }

        // The result of an `address_to_pointer` may be used in any unsafe way, e.g.
        // passed to a C function.
        baseAddr = atp.address
      case let ia as IndexAddrInst:
        if !ia.needsStackProtection {
          return nil
        }
        var hasNoStores = NoStores()
        if hasNoStores.walkDownUses(ofAddress: ia, path: SmallProjectionPath()) == .continueWalk {
          return nil
        }

        // `index_addr` is unsafe if not used for tail-allocated elements (e.g. in Array).
        baseAddr = ia.base
      default:
        return nil
    }
    let (accessPath, scope) = baseAddr.accessPathWithScope

    if case .tail = accessPath.base, self is IndexAddrInst {
      // `index_addr` for tail-allocated elements is the usual case (most likely coming from
      // Array code).
      return nil
    }
    return (accessPath.base, scope)
  }
}

/// Checks if there are no stores to an address or raw pointer.
private struct NoStores : ValueDefUseWalker, AddressDefUseWalker {
  var walkDownCache = WalkerCache<SmallProjectionPath>()

  mutating func leafUse(value: Operand, path: SmallProjectionPath) -> WalkResult {
    switch value.instruction {
    case let ptai as PointerToAddressInst:
      return walkDownUses(ofAddress: ptai, path: path)
    case let bi as BuiltinInst:
      switch bi.intrinsicID {
      case .memcpy, .memmove:
        return value.index != 0 ? .continueWalk : .abortWalk
      default:
        return .abortWalk
      }
    default:
      return .abortWalk
    }
  }

  mutating func leafUse(address: Operand, path: SmallProjectionPath) -> WalkResult {
    switch address.instruction {
    case is LoadInst:
      return .continueWalk
    case let cai as CopyAddrInst:
      return address == cai.sourceOperand ? .continueWalk : .abortWalk
    default:
      return .abortWalk
    }
  }
}

private extension Function {
  func setNeedsStackProtection(_ context: FunctionPassContext) {
    if !needsStackProtection {
      set(needStackProtection: true, context)
    }
  }
}
