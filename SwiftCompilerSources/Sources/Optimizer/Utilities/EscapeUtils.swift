//===--- EscapeUtils.swift ------------------------------------------------===//
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
//
// This file provides utilities for transitively visiting all uses of a value.
// The most common use case is to check if a value "escapes" to some destination
// (e.g. an instruction) or if it "escapes" the current function at all.
//
// The APIs on `Value` and/or `ProjectedValue` are
//   * `isEscaping(using:)`
//   * `isEscapingWhenWalkingDown(using:)`
//   * `visit(using:)`
//   * `visitByWalkingDown(using:)`
//
// where a `EscapeVisitor` can be passed to the `using` argument to configure
// the visit.
//
// The term "escaping" means that the "bit pattern" of the value is visible
// at the destination. For example, in stack promotion we check if a reference to
// an allocated object can escape it's function, i.e. if the bit pattern of the
// reference can be visible outside it's function.
// But it's also possible to check the "escapeness" of trivial values, e.g. an
// `Int`. An `Int` escapes if its bit pattern is visible at the destination.
// Though, by default trivial values are ignored. This can be configured with
// `EscapeVisitor.followTrivialTypes`.
//
// By default, there is no distinction between addresses and value-type values.
// Even if the value in question has an address type, it's considered escaping
// if the stored value is escaping.
// This can be configured with `EscapeVisitor.followLoads`.
//
// The visit algorithm works by starting a walk at the value and alternately
// walking in two directions:
// * Starting at root definitions, like allocations: walks down from defs to uses
//     ("Where does the value go to?")
// * Starting at stores, walks up from uses to defs
//     ("Were does the value come from?")
//
// The value "escapes" if the walk reaches a point where the further flow of the value
// cannot be tracked anymore.
// Example:
// \code
//   %1 = alloc_ref $X    // 1. initial value: walk down to the `store`
//   %2 = alloc_stack $X  // 3. walk down to %3
//   store %1 to %2       // 2. walk up to `%2`
//   %3 = load %2         // 4. continue walking down to the `return`
//   return %3            // 5. The value is escaping!
// \endcode
//
// The traversal stops at points where the current path doesn't match the original projection.
// For example, let's assume this function is called on a projected value with path `s0.c1`.
// \code
//    %value : $Struct<X>                         // current path == s0.c1, the initial value
//    %ref = struct_extract %value, #field0       // current path == c1
//    %addr = ref_element_addr %ref, #field2      // mismatch: `c1` != `c2` -> ignored
// \endcode
//
//===----------------------------------------------------------------------===//

import SIL

extension ProjectedValue {

  /// Returns true if the projected value escapes.
  ///
  /// The provided `visitor` can be used to override the handling a certain defs and uses during
  /// the walk. See `EscapeVisitor` for details.
  ///
  func isEscaping(using visitor: some EscapeVisitor = DefaultVisitor(),
                  complexityBudget: Int = Int.max,
                  _ context: some Context) -> Bool {
    var walker = EscapeWalker(visitor: visitor, complexityBudget: complexityBudget, context)
    return walker.walkUp(addressOrValue: value, path: path.escapePath) == .abortWalk
  }

  /// Returns true if the function argument escapes, but ignoring any potential escapes in the caller.
  ///
  /// This function is similar to `ProjectedValue.isEscaping()`, but it ignores any potential
  /// escapes which might have happened before the argument's function is called.
  /// Technically, this means that the walk starts downwards instead of upwards.
  ///
  func isEscapingWhenWalkingDown(using visitor: some EscapeVisitor = DefaultVisitor(),
                                 _ context: some Context) -> Bool {
    var walker = EscapeWalker(visitor: visitor, context)
    return walker.walkDown(addressOrValue: value, path: path.escapePath) == .abortWalk
  }

  /// Returns the result of the visitor if the projected value does not escape.
  ///
  /// This function is similar to `isEscaping() -> Bool`, but instead of returning a Bool,
  /// it returns the `result` of the `visitor`, if the projected value does not escape.
  /// Returns nil, if the projected value escapes.
  ///
  func visit<V: EscapeVisitorWithResult>(using visitor: V, _ context: some Context) -> V.Result? {
    var walker = EscapeWalker(visitor: visitor, context)
    if walker.walkUp(addressOrValue: value, path: path.escapePath) == .abortWalk {
      return nil
    }
    return walker.visitor.result
  }

  /// Returns the result of the visitor if the projected value does not escape - ignoring
  /// any potential escapes in the caller.
  ///
  /// This function is similar to `isEscapingIgnoringCallerEscapes() -> Bool`, but instead
  /// of returning a Bool, it returns the `result` of the `visitor`.
  ///
  func visitByWalkingDown<V: EscapeVisitorWithResult>(using visitor: V,
                                                      _ context: some Context) -> V.Result? {
    var walker = EscapeWalker(visitor: visitor, context)
    if walker.walkDown(addressOrValue: value, path: path.escapePath) == .abortWalk {
      return nil
    }
    return walker.visitor.result
  }
}

extension Value {
  /// The un-projected version of `ProjectedValue.isEscaping()`.
  func isEscaping(using visitor: some EscapeVisitor = DefaultVisitor(),
                  _ context: some Context) -> Bool {
    return self.at(SmallProjectionPath()).isEscaping(using: visitor, context)
  }

  func isEscapingWhenWalkingDown(using visitor: some EscapeVisitor = DefaultVisitor(),
                                 _ context: some Context) -> Bool {
    return self.at(SmallProjectionPath()).isEscapingWhenWalkingDown(using: visitor, context)
  }

  /// The un-projected version of `ProjectedValue.visit()`.
  func visit<V: EscapeVisitorWithResult>(using visitor: V, _ context: some Context) -> V.Result? {
    return self.at(SmallProjectionPath()).visit(using: visitor, context)
  }
}

/// This protocol is used to customize `ProjectedValue.isEscaping` (and similar functions)
/// by implementing `visitUse` and `visitDef` which are called for all uses and definitions
/// encountered during a walk.
protocol EscapeVisitor {
  typealias UseResult = EscapeUtilityTypes.UseVisitResult
  typealias DefResult = EscapeUtilityTypes.DefVisitResult
  typealias EscapePath = EscapeUtilityTypes.EscapePath
  
  /// Called during the DefUse walk for each use
  mutating func visitUse(operand: Operand, path: EscapePath) -> UseResult
  
  /// Called during the UseDef walk for each definition
  mutating func visitDef(def: Value, path: EscapePath) -> DefResult

  /// If true, the traversals follow values with trivial types.
  var followTrivialTypes: Bool { get }

  /// If true, the traveral follows loaded values.
  var followLoads: Bool { get }
}

extension EscapeVisitor {
  mutating func visitUse(operand: Operand, path: EscapePath) -> UseResult {
    return .continueWalk
  }

  mutating func visitDef(def: Value, path: EscapePath) -> DefResult {
    return .continueWalkUp
  }

  var followTrivialTypes: Bool { false }

  var followLoads: Bool { true }
}

/// A visitor which returns a `result`.
protocol EscapeVisitorWithResult : EscapeVisitor {
  associatedtype Result
  var result: Result { get }
}

private struct DefaultVisitor : EscapeVisitor {}

struct EscapeUtilityTypes {

  /// The EscapePath is updated and maintained during the up-walk and down-walk.
  ///
  /// It's passed to the EscapeVisitor's `visitUse` and `visitDef`.
  struct EscapePath: SmallProjectionWalkingPath {
    /// During the walk, a projection path indicates where the initial value is
    /// contained in an aggregate.
    /// Example for a walk-down:
    /// \code
    ///   %1 = alloc_ref                   // 1. initial value, path = empty
    ///   %2 = struct $S (%1)              // 2. path = s0
    ///   %3 = tuple (%other, %1)          // 3. path = t1.s0
    ///   %4 = tuple_extract %3, 1         // 4. path = s0
    ///   %5 = struct_extract %4, #field   // 5. path = empty
    /// \endcode
    ///
    let projectionPath: SmallProjectionPath

    /// This flag indicates if stored values should be included in the walk.
    /// If the initial value is stored to some memory allocation, we usually don't
    /// care if other values are stored to that location as well. Example:
    /// \code
    ///   %1 = alloc_ref $X    // 1. initial value, walk down to the `store`
    ///   %2 = alloc_stack $X  // 3. walk down to the second `store`
    ///   store %1 to %2       // 2. walk up to %2
    ///   store %other to %2   // 4. ignore (followStores == false): %other doesn't impact the "escapeness" of %1
    /// \endcode
    ///
    /// But once the the up-walk sees a load, it has to follow stores from that point on.
    /// Example:
    /// \code
    /// bb0(%function_arg):            // 7. escaping! %1 escapes through %function_arg
    ///   %1 = alloc_ref $X            // 1. initial value, walk down to the second `store`
    ///   %addr = alloc_stack %X       // 5. walk down to the first `store`
    ///   store %function_arg to %addr // 6. walk up to %function_arg (followStores == true)
    ///   %2 = load %addr              // 4. walk up to %addr, followStores = true
    ///   %3 = ref_element_addr %2, #f // 3. walk up to %2
    ///   store %1 to %3               // 2. walk up to %3
    /// \endcode
    ///
    let followStores: Bool

    /// Not nil, if the exact type of the current value is know.
    ///
    /// This is used for destructor analysis.
    /// Example:
    /// \code
    ///   %1 = alloc_ref $Derived          // 1. initial value, knownType = $Derived
    ///   %2 = upcast %1 to $Base          // 2. knownType = $Derived
    ///   destroy_value %2 : $Base         // 3. We know that the destructor of $Derived is called here
    /// \endcode
    let knownType: Type?

    func with(projectionPath: SmallProjectionPath) -> Self {
      return Self(projectionPath: projectionPath, followStores: self.followStores, knownType: self.knownType)
    }

    func with(followStores: Bool) -> Self {
      return Self(projectionPath: self.projectionPath, followStores: followStores, knownType: self.knownType)
    }
    
    func with(knownType: Type?) -> Self {
      return Self(projectionPath: self.projectionPath, followStores: self.followStores, knownType: knownType)
    }
    
    func merge(with other: EscapePath) -> EscapePath {
      let mergedPath = self.projectionPath.merge(with: other.projectionPath)
      let mergedFollowStores = self.followStores || other.followStores
      let mergedKnownType: Type?
      if let ty = self.knownType {
        if let otherTy = other.knownType, ty != otherTy {
          mergedKnownType = nil
        } else {
          mergedKnownType = ty
        }
      } else {
        mergedKnownType = other.knownType
      }
      return EscapePath(projectionPath: mergedPath, followStores: mergedFollowStores, knownType: mergedKnownType)
    }
  }
  
  enum DefVisitResult {
    case ignore
    case continueWalkUp
    case walkDown
    case abort
  }

  enum UseVisitResult {
    case ignore
    case continueWalk
    case abort
  }
}

/// EscapeWalker is both a DefUse walker and UseDef walker. It implements both, the up-, and down-walk.
fileprivate struct EscapeWalker<V: EscapeVisitor> : ValueDefUseWalker,
                                                    AddressDefUseWalker,
                                                    ValueUseDefWalker,
                                                    AddressUseDefWalker {
  typealias Path = EscapeUtilityTypes.EscapePath
  
  init(visitor: V, complexityBudget: Int = Int.max, _ context: some Context) {
    self.calleeAnalysis = context.calleeAnalysis
    self.visitor = visitor
    self.complexityBudget = complexityBudget
  }

  //===--------------------------------------------------------------------===//
  //                                   Walking down
  //===--------------------------------------------------------------------===//
  
  mutating func walkDown(addressOrValue: Value, path: Path) -> WalkResult {
    if addressOrValue.type.isAddress {
      return walkDownUses(ofAddress: addressOrValue, path: path)
    } else {
      return walkDownUses(ofValue: addressOrValue, path: path)
    }
  }
  
  mutating func cachedWalkDown(addressOrValue: Value, path: Path) -> WalkResult {
    if let path = walkDownCache.needWalk(for: addressOrValue, path: path) {
      return walkDown(addressOrValue: addressOrValue, path: path)
    } else {
      return .continueWalk
    }
  }
  
  mutating func walkDown(value: Operand, path: Path) -> WalkResult {
    if complexityBudgetExceeded(value.value) {
      return .abortWalk
    }
    if hasRelevantType(value.value, at: path.projectionPath) {
      switch visitor.visitUse(operand: value, path: path) {
      case .continueWalk:
        return walkDownDefault(value: value, path: path)
      case .ignore:
        return .continueWalk
      case .abort:
        return .abortWalk
      }
    }
    return .continueWalk
  }
  
  /// ``ValueDefUseWalker`` conformance: called when the value def-use walk can't continue,
  /// i.e. when the result of the use is not a value.
  mutating func leafUse(value operand: Operand, path: Path) -> WalkResult {
    let instruction = operand.instruction
    switch instruction {
    case let rta as RefTailAddrInst:
      if let path = pop(.tailElements, from: path, yielding: rta) {
        return walkDownUses(ofAddress: rta, path: path.with(knownType: nil))
      }
    case let rea as RefElementAddrInst:
      if let path = pop(.classField, index: rea.fieldIndex, from: path, yielding: rea) {
        return walkDownUses(ofAddress: rea, path: path.with(knownType: nil))
      }
    case let pb as ProjectBoxInst:
      if let path = pop(.classField, index: pb.fieldIndex, from: path, yielding: pb) {
        return walkDownUses(ofAddress: pb, path: path.with(knownType: nil))
      }
    case is StoreInst, is StoreWeakInst, is StoreUnownedInst:
      let store = instruction as! StoringInstruction
      assert(operand == store.sourceOperand )
      return walkUp(address: store.destination, path: path)
    case is DestroyValueInst, is ReleaseValueInst, is StrongReleaseInst:
      if handleDestroy(of: operand.value, path: path) == .abortWalk {
        return .abortWalk
      }
    case is ReturnInst:
      return isEscaping
    case is ApplyInst, is TryApplyInst, is BeginApplyInst:
      return walkDownCallee(argOp: operand, apply: instruction as! FullApplySite, path: path)
    case let pai as PartialApplyInst:
      // Check whether the partially applied argument can escape in the body.
      if walkDownCallee(argOp: operand, apply: pai, path: path.with(knownType: nil)) == .abortWalk {
        return .abortWalk
      }
      
      // Additionally we need to follow the partial_apply value for two reasons:
      // 1. the closure (with the captured values) itself can escape
      //    and the use "transitively" escapes
      // 2. something can escape in a destructor when the context is destroyed
      return walkDownUses(ofValue: pai, path: path.with(knownType: nil))
    case let pta as PointerToAddressInst:
      return walkDownUses(ofAddress: pta, path: path.with(knownType: nil))
    case let cv as ConvertFunctionInst:
      return walkDownUses(ofValue: cv, path: path.with(knownType: nil))
    case let bi as BuiltinInst:
      switch bi.id {
      case .DestroyArray:
        // If it's not the array base pointer operand -> bail. Though, that shouldn't happen
        // because the other operands (metatype, count) shouldn't be visited anyway.
        if operand.index != 1 { return isEscaping }
        
        // Class references, which are directly located in the array elements cannot escape,
        // because those are passed as `self` to their deinits - and `self` cannot escape in a deinit.
        if !path.projectionPath.mayHaveClassProjection {
          return .continueWalk
        }
        return isEscaping
      default:
        return isEscaping
      }
    case is StrongRetainInst, is RetainValueInst, is DebugValueInst, is ValueMetatypeInst,
      is InitExistentialMetatypeInst, is OpenExistentialMetatypeInst,
      is ExistentialMetatypeInst, is DeallocRefInst, is SetDeallocatingInst, is FixLifetimeInst,
      is ClassifyBridgeObjectInst, is BridgeObjectToWordInst, is EndBorrowInst,
      is StrongRetainInst, is RetainValueInst,
      is ClassMethodInst, is SuperMethodInst, is ObjCMethodInst,
      is ObjCSuperMethodInst, is WitnessMethodInst, is DeallocStackRefInst:
      return .continueWalk
    case is DeallocStackInst:
      // dealloc_stack %f : $@noescape @callee_guaranteed () -> ()
      // type is a value
      assert(operand.value.definingInstruction is PartialApplyInst)
      return .continueWalk
    default:
      return isEscaping
    }
    return .continueWalk
  }
  
  mutating func walkDown(address: Operand, path: Path) -> WalkResult {
    if complexityBudgetExceeded(address.value) {
      return .abortWalk
    }
    if hasRelevantType(address.value, at: path.projectionPath) {
      switch visitor.visitUse(operand: address, path: path) {
      case .continueWalk:
        return walkDownDefault(address: address, path: path)
      case .ignore:
        return .continueWalk
      case .abort:
        return .abortWalk
      }
    }
    return .continueWalk
  }
  
  /// ``AddressDefUseWalker`` conformance: called when the address def-use walk can't continue,
  /// i.e. when the result of the use is not an address.
  mutating func leafUse(address operand: Operand, path: Path) -> WalkResult {
    let instruction = operand.instruction
    switch instruction {
    case is StoreInst, is StoreWeakInst, is StoreUnownedInst:
      let store = instruction as! StoringInstruction
      assert(operand == store.destinationOperand)
      if let si = store as? StoreInst, si.destinationOwnership == .assign {
        if handleDestroy(of: operand.value, path: path.with(knownType: nil)) == .abortWalk {
          return .abortWalk
        }
      }
      if path.followStores {
        return walkUp(value: store.source, path: path)
      }
    case let copyAddr as CopyAddrInst:
      if !followLoads(at: path.projectionPath) {
        return .continueWalk
      }
      if operand == copyAddr.sourceOperand {
        return walkUp(address: copyAddr.destination, path: path)
      } else {
        if !copyAddr.isInitializationOfDest {
          if handleDestroy(of: operand.value, path: path.with(knownType: nil)) == .abortWalk {
            return .abortWalk
          }
        }
        
        if path.followStores {
          assert(operand == copyAddr.destinationOperand)
          return walkUp(value: copyAddr.source, path: path)
        }
      }
    case is DestroyAddrInst:
      if handleDestroy(of: operand.value, path: path) == .abortWalk {
        return .abortWalk
      }
    case is ReturnInst:
      return isEscaping
    case is ApplyInst, is TryApplyInst, is BeginApplyInst:
      return walkDownCallee(argOp: operand, apply: instruction as! FullApplySite, path: path)
    case let pai as PartialApplyInst:
      if walkDownCallee(argOp: operand, apply: pai, path: path.with(knownType: nil)) == .abortWalk {
        return .abortWalk
      }

      // We need to follow the partial_apply value for two reasons:
      // 1. the closure (with the captured values) itself can escape
      // 2. something can escape in a destructor when the context is destroyed
      return walkDownUses(ofValue: pai, path: path.with(knownType: nil))
    case is LoadInst, is LoadWeakInst, is LoadUnownedInst:
      if !followLoads(at: path.projectionPath) {
        return .continueWalk
      }
      let svi = instruction as! SingleValueInstruction
      
      // Even when analyzing addresses, a loaded trivial value can be ignored.
      if svi.hasTrivialNonPointerType { return .continueWalk }
      return walkDownUses(ofValue: svi, path: path.with(knownType: nil))
    case let atp as AddressToPointerInst:
      return walkDownUses(ofValue: atp, path: path.with(knownType: nil))
    case let ia as IndexAddrInst:
      assert(operand.index == 0)
      return walkDownUses(ofAddress: ia, path: path.with(knownType: nil))
    case is DeallocStackInst, is InjectEnumAddrInst, is FixLifetimeInst, is EndBorrowInst, is EndAccessInst,
         is DebugValueInst:
      return .continueWalk
    default:
      return isEscaping
    }
    return .continueWalk
  }
  
  /// Check whether the value escapes through the deinitializer
  private func handleDestroy(of value: Value, path: Path) -> WalkResult {

    // Even if this is a destroy_value of a struct/tuple/enum, the called destructor(s) only take a
    // single class reference as parameter.
    let p = path.projectionPath.popAllValueFields()

    if p.isEmpty {
      // The object to destroy (= the argument of the destructor) cannot escape itself.
      return .continueWalk
    }
    if !visitor.followLoads && p.matches(pattern: SmallProjectionPath(.anyValueFields).push(.anyClassField)) {
      // Any address of a class property of the object to destroy cannot escape the destructor.
      // (Whereas a value stored in such a property could escape.)
      return .continueWalk
    }

    if path.followStores {
      return isEscaping
    }
    if let exactTy = path.knownType {
      guard let destructor = calleeAnalysis.getDestructor(ofExactType: exactTy) else {
        return isEscaping
      }
      if destructor.effects.escapeEffects.canEscape(argumentIndex: 0, path: pathForArgumentEscapeChecking(p)) {
        return isEscaping
      }
    } else {
      // We don't know the exact type, so get all possible called destructure from
      // the callee analysis.
      guard let destructors = calleeAnalysis.getDestructors(of: value.type) else {
        return isEscaping
      }
      for destructor in destructors {
        if destructor.effects.escapeEffects.canEscape(argumentIndex: 0, path: pathForArgumentEscapeChecking(p)) {
          return isEscaping
        }
      }
    }
    return .continueWalk
  }
  
  /// Handle an apply (full or partial) during the walk-down.
  private mutating
  func walkDownCallee(argOp: Operand, apply: ApplySite, path: Path) -> WalkResult {
    guard let argIdx = apply.argumentIndex(of: argOp) else {
      // The callee or a type dependent operand of the apply does not let escape anything.
      return .continueWalk
    }

    // Indirect arguments cannot escape the function, but loaded values from such can.
    if !followLoads(at: path.projectionPath) &&
       // Except for begin_apply: it can yield an address value.
       !apply.isBeginApplyWithIndirectResults {
      return .continueWalk
    }

    if argOp.value.type.isNoEscapeFunction {
      // Per definition a `partial_apply [on_stack]` cannot escape the callee.
      // Potential escapes of its captured values are already handled when visiting the `partial_apply`.
      return .continueWalk
    }

    // Argument effects do not consider any potential stores to the argument (or it's content).
    // Therefore, if we need to track stores, the argument effects do not correctly describe what we need.
    // For example, argument 0 in the following function is marked as not-escaping, although there
    // is a store to the argument:
    //
    //   sil [escapes !%0.**] @callee(@inout X, @owned X) -> () {
    //   bb0(%0 : $*X, %1 : $X):
    //     store %1 to %0 : $*X
    //   }
    if path.followStores {
      return isEscaping
    }

    guard let callees = calleeAnalysis.getCallees(callee: apply.callee) else {
      // The callees are not know, e.g. if the callee is a closure, class method, etc.
      return isEscaping
    }

    let calleeArgIdx = apply.calleeArgIndex(callerArgIndex: argIdx)

    for callee in callees {
      let effects = callee.effects
      if !effects.escapeEffects.canEscape(argumentIndex: calleeArgIdx,
                                          path: pathForArgumentEscapeChecking(path.projectionPath)) {
        continue
      }
      if walkDownArgument(calleeArgIdx: calleeArgIdx, argPath: path,
                          apply: apply, effects: effects) == .abortWalk {
        return .abortWalk
      }
    }
    return .continueWalk
  }
  
  /// Handle `.escaping` effects for an apply argument during the walk-down.
  private mutating
  func walkDownArgument(calleeArgIdx: Int, argPath: Path,
                        apply: ApplySite, effects: FunctionEffects) -> WalkResult {
    var matched = false
    for effect in effects.escapeEffects.arguments {
      switch effect.kind {
      case .escapingToArgument(let toArgIdx, let toPath):
        // Note: exclusive argument -> argument effects cannot appear, so we don't need to handle them here.
        if effect.matches(calleeArgIdx, argPath.projectionPath) {
          guard let callerToIdx = apply.callerArgIndex(calleeArgIndex: toArgIdx) else {
            return isEscaping
          }

          // Continue at the destination of an arg-to-arg escape.
          let arg = apply.arguments[callerToIdx]
          
          let p = Path(projectionPath: toPath, followStores: false, knownType: nil)
          if walkUp(addressOrValue: arg, path: p) == .abortWalk {
            return .abortWalk
          }
          matched = true
        }
      case .escapingToReturn(let toPath, let exclusive):
        if effect.matches(calleeArgIdx, argPath.projectionPath) {
          guard let fas = apply as? FullApplySite, let result = fas.singleDirectResult else {
            return isEscaping
          }

          let p = Path(projectionPath: toPath, followStores: false, knownType: exclusive ? argPath.knownType : nil)
          
          if walkDownUses(ofValue: result, path: p) == .abortWalk {
            return .abortWalk
          }
          matched = true
        }
      case .notEscaping:
        break
      }
    }
    if !matched { return isEscaping }
    return .continueWalk
  }
  
  //===--------------------------------------------------------------------===//
  //                                   Walking up
  //===--------------------------------------------------------------------===//
  
  mutating func walkUp(addressOrValue: Value, path: Path) -> WalkResult {
    if addressOrValue.type.isAddress {
      return walkUp(address: addressOrValue, path: path)
    } else {
      return walkUp(value: addressOrValue, path: path)
    }
  }
  
  mutating func walkUp(value: Value, path: Path) -> WalkResult {
    if complexityBudgetExceeded(value) {
      return .abortWalk
    }
    if hasRelevantType(value, at: path.projectionPath) {
      switch visitor.visitDef(def: value, path: path) {
      case .continueWalkUp:
        return walkUpDefault(value: value, path: path)
      case .walkDown:
        return cachedWalkDown(addressOrValue: value, path: path.with(knownType: nil))
      case .ignore:
        return .continueWalk
      case .abort:
        return .abortWalk
      }
    }
    return .continueWalk
  }
  
  /// ``ValueUseDefWalker`` conformance: called when the value use-def walk can't continue,
  /// i.e. when the operand (if any) of the instruction of a definition is not a value.
  mutating func rootDef(value def: Value, path: Path) -> WalkResult {
    switch def {
    case is AllocRefInst, is AllocRefDynamicInst:
      return cachedWalkDown(addressOrValue: def, path: path.with(knownType: def.type))
    case is AllocBoxInst:
      return cachedWalkDown(addressOrValue: def, path: path.with(knownType: nil))
    case let arg as BlockArgument:
      let block = arg.parentBlock
      switch block.singlePredecessor!.terminator {
      case let ta as TryApplyInst:
        if block != ta.normalBlock { return isEscaping }
        return walkUpApplyResult(apply: ta, path: path.with(knownType: nil))
      default:
        return isEscaping
      }
    case let ap as ApplyInst:
      return walkUpApplyResult(apply: ap, path: path.with(knownType: nil))
    case is LoadInst, is LoadWeakInst, is LoadUnownedInst:
      if !followLoads(at: path.projectionPath) {
        // When walking up we shouldn't end up at a load where followLoads is false,
        // because going from a (non-followLoads) address to a load always involves a class indirection.
        // There is one exception: loading a raw pointer, e.g.
        //   %l = load %a : $Builtin.RawPointer
        //   %a = pointer_to_address %l           // the up-walk starts at %a
        return isEscaping
      }
      return walkUp(address: (def as! UnaryInstruction).operand.value,
                    path: path.with(followStores: true).with(knownType: nil))
    case let atp as AddressToPointerInst:
      return walkUp(address: atp.address, path: path.with(knownType: nil))
    default:
      return isEscaping
    }
  }
  
  mutating func walkUp(address: Value, path: Path) -> WalkResult {
    if complexityBudgetExceeded(address) {
      return .abortWalk
    }
    if hasRelevantType(address, at: path.projectionPath) {
      switch visitor.visitDef(def: address, path: path) {
      case .continueWalkUp:
        return walkUpDefault(address: address, path: path)
      case .walkDown:
        return cachedWalkDown(addressOrValue: address, path: path)
      case .ignore:
        return .continueWalk
      case .abort:
        return .abortWalk
      }
    }
    return .continueWalk
  }
  
  /// ``AddressUseDefWalker`` conformance: called when the address use-def walk can't continue,
  /// i.e. when the operand (if any) of the instruction of a definition is not an address.
  mutating func rootDef(address def: Value, path: Path) -> WalkResult {
    switch def {
    case is AllocStackInst:
      return cachedWalkDown(addressOrValue: def, path: path.with(knownType: nil))
    case let arg as FunctionArgument:
      if !followLoads(at: path.projectionPath) && arg.convention.isExclusiveIndirect && !path.followStores {
        return cachedWalkDown(addressOrValue: def, path: path.with(knownType: nil))
      } else {
        return isEscaping
      }
    case is PointerToAddressInst, is IndexAddrInst:
      return walkUp(value: (def as! SingleValueInstruction).operands[0].value, path: path.with(knownType: nil))
    case let rta as RefTailAddrInst:
      return walkUp(value: rta.instance, path: path.push(.tailElements, index: 0).with(knownType: nil))
    case let rea as RefElementAddrInst:
      return walkUp(value: rea.instance, path: path.push(.classField, index: rea.fieldIndex).with(knownType: nil))
    case let pb as ProjectBoxInst:
      return walkUp(value: pb.box, path: path.push(.classField, index: pb.fieldIndex).with(knownType: nil))
    default:
      return isEscaping
    }
  }
  
  /// Walks up from the return to the source argument if there is an "exclusive"
  /// escaping effect on an argument.
  private mutating
  func walkUpApplyResult(apply: FullApplySite,
                         path: Path) -> WalkResult {
    guard let callees = calleeAnalysis.getCallees(callee: apply.callee) else {
      return .abortWalk
    }

    for callee in callees {
      var matched = false
      for effect in callee.effects.escapeEffects.arguments {
        switch effect.kind {
        case .escapingToReturn(let toPath, let exclusive):
          if exclusive && path.projectionPath.matches(pattern: toPath) {
            guard let callerArgIdx = apply.callerArgIndex(calleeArgIndex: effect.argumentIndex) else {
              return .abortWalk
            }
            let arg = apply.arguments[callerArgIdx]
            
            let p = Path(projectionPath: effect.pathPattern, followStores: path.followStores, knownType: nil)
            if walkUp(addressOrValue: arg, path: p) == .abortWalk {
              return .abortWalk
            }
            matched = true
          }
        case .notEscaping, .escapingToArgument:
          break
        }
      }
      if !matched {
        return isEscaping
      }
    }
    return .continueWalk
  }
  
  //===--------------------------------------------------------------------===//
  //                             private state
  //===--------------------------------------------------------------------===//

  var visitor: V

  // The caches are not only useful for performance, but are need to avoid infinite
  // recursions of walkUp-walkDown cycles.
  var walkDownCache = WalkerCache<Path>()
  var walkUpCache = WalkerCache<Path>()

  // Only this number of up/and down walks are done until the walk aborts.
  // Used to avoid quadratic complexity in some scenarios.
  var complexityBudget: Int

  private let calleeAnalysis: CalleeAnalysis
  
  //===--------------------------------------------------------------------===//
  //                          private utility functions
  //===--------------------------------------------------------------------===//

  /// Tries to pop the given projection from path, if the projected `value` has a relevant type.
  private func pop(_ kind: Path.FieldKind, index: Int? = nil, from path: Path, yielding value: Value) -> Path? {
    if let newPath = path.popIfMatches(kind, index: index),
       hasRelevantType(value, at: newPath.projectionPath) {
      return newPath
    }
    return nil
  }

  private func hasRelevantType(_ value: Value, at path: SmallProjectionPath) -> Bool {
    if visitor.followTrivialTypes &&
       // When part of a class field only need to follow non-trivial types
       !path.hasClassProjection {
      return true
    }
    if !value.hasTrivialNonPointerType {
      return true
    }
    return false
  }

  private func followLoads(at path: SmallProjectionPath) -> Bool {
    return visitor.followLoads ||
           // When part of a class field we have to follow loads.
           path.mayHaveClassProjection
  }

  private func pathForArgumentEscapeChecking(_ path: SmallProjectionPath) -> SmallProjectionPath {
    if visitor.followLoads {
      return path
    }
    return path.popLastClassAndValuesFromTail()
  }

  private mutating func complexityBudgetExceeded(_ v: Value) -> Bool {
    if complexityBudget <= 0 {
      return true
    }
    complexityBudget = complexityBudget &- 1
    return false
  }

  // Set a breakpoint here to debug when a value is escaping.
  private var isEscaping: WalkResult { .abortWalk }
}

private extension SmallProjectionPath {
  var escapePath: EscapeUtilityTypes.EscapePath {
    EscapeUtilityTypes.EscapePath(projectionPath: self, followStores: false, knownType: nil)
  }
}

private extension ApplySite {
  var isBeginApplyWithIndirectResults: Bool {
    guard let ba = self as? BeginApplyInst else {
      return false
    }
    // Note that the token result is always a non-address type.
    return ba.results.contains { $0.type.isAddress }
  }
}
