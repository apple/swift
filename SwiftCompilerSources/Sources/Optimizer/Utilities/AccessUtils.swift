//===--- AccessUtils.swift - Utilities for analyzing memory accesses ------===//
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
// This file provides a set of utilities for analyzing memory accesses.
// It defines the following concepts
// - `AccessBase`: represents the base address of a memory access.
// - `AccessPath`: a pair of an `AccessBase` and `SmallProjectionPath` with the
//   the path describing the specific address (in terms of projections) of the
//   access.
// - Access storage path (which is of type `ProjectedValue`): identifies the
//   reference (or a value which contains a reference) an address originates from.
//
// The snippet below shows the relationship between the access concepts.
// ```
// %ref = struct_extract %value, #f1                                        access storage path
// %base = ref_element_addr %ref, #f2         AccessBase       AccessPath            |
// %scope = begin_access %base                AccessScope          |                 |
// %t = tuple_element_addr %scope, 0                               |                 |
// %s = struct_element_addr %t, #f3                                v                 v
// %l = load %s                               the access
// ```
//===----------------------------------------------------------------------===//

import SIL

/// AccessBase describes the base address of a memory access (e.g. of a `load` or `store``).
/// The "base address" is defined as the address which is obtained from the access address by
/// looking through all address projections.
/// This means that the base address is either the same as the access address or:
/// the access address is a chain of address projections of the base address.
/// The following snippets show examples of memory accesses and their respective bases.
///
/// ```
/// %base1 = ref_element_addr %ref, #Obj.field // A `class` base
/// %base2 = alloc_stack $S                    // A `stack` base
/// %base3 = global_addr @gaddr                // A `global` base
/// %addr1 = struct_element_addr %base1
/// %access1 = store %v1 to [trivial] %addr1   // accessed address is offset from base
/// %access2 = store %v2 to [trivial] %base2   // accessed address is base itself
/// ```
///
/// The base address is never inside an access scope.
enum AccessBase : CustomStringConvertible, Hashable {

  /// The address of a boxed variable, i.e. a field of an `alloc_box`.
  case box(ProjectBoxInst)
  
  /// The address of a stack-allocated value, i.e. an `alloc_stack`
  case stack(AllocStackInst)
  
  /// The address of a global variable.
  case global(GlobalVariable)
  
  /// The address of a stored property of a class instance.
  case `class`(RefElementAddrInst)
  
  /// The base address of the tail allocated elements of a class instance.
  case tail(RefTailAddrInst)

  /// An indirect function argument, like `@inout`.
  case argument(FunctionArgument)
  
  /// An indirect result of a `begin_apply`.
  case yield(MultipleValueInstructionResult)

  /// An address which is derived from a `Builtin.RawPointer`.
  case pointer(PointerToAddressInst)

  /// The access base is some SIL pattern which does not fit into any other case.
  /// This should be a very rare situation.
  case unidentified

  init(baseAddress: Value) {
    switch baseAddress {
    case let rea as RefElementAddrInst   : self = .class(rea)
    case let rta as RefTailAddrInst      : self = .tail(rta)
    case let pbi as ProjectBoxInst       : self = .box(pbi)
    case let asi as AllocStackInst       : self = .stack(asi)
    case let arg as FunctionArgument     : self = .argument(arg)
    case let ga as GlobalAddrInst        : self = .global(ga.global)
    case let mvr as MultipleValueInstructionResult:
      if mvr.parentInstruction is BeginApplyInst && baseAddress.type.isAddress {
        self = .yield(mvr)
      } else {
        self = .unidentified
      }
    default:
      self = .unidentified
    }
  }

  var description: String {
    switch self {
      case .unidentified:      return "?"
      case .box(let pbi):      return "box - \(pbi)"
      case .stack(let asi):    return "stack - \(asi)"
      case .global(let gl):    return "global - @\(gl.name)"
      case .class(let rea):    return "class  - \(rea)"
      case .tail(let rta):     return "tail - \(rta.instance)"
      case .argument(let arg): return "argument - \(arg)"
      case .yield(let result): return "yield - \(result)"
      case .pointer(let p):    return "pointer - \(p)"
    }
  }

  /// True, if this is an access to a class instance.
  var isObjectAccess: Bool {
    switch self {
      case .class, .tail:
        return true
      case .box, .stack, .global, .argument, .yield, .pointer, .unidentified:
        return false
    }
  }

  /// The reference value if this is an access to a referenced objecct (class, box, tail).
  var reference: Value? {
    switch self {
      case .box(let pbi):      return pbi.box
      case .class(let rea):    return rea.instance
      case .tail(let rta):     return rta.instance
      case .stack, .global, .argument, .yield, .pointer, .unidentified:
        return nil
    }
  }

  /// True if this access base may be derived from a reference that is only valid within a locally
  /// scoped OSSA lifetime. For example:
  ///
  ///   %reference = begin_borrow %1
  ///   %base = ref_tail_addr %reference     <- %base must not be used outside the borrow scope
  ///   end_borrow %reference
  ///
  /// This is not true for scoped storage such as alloc_stack and @in arguments.
  ///
  var hasLocalOwnershipLifetime: Bool {
    if let reference = reference {
      // Conservatively assume that everything which is a ref-counted object is within an ownership scope.
      // TODO: we could e.g. exclude guaranteed function arguments.
      return reference.ownership != .none
    }
    return false
  }

  /// True, if the baseAddress is of an immutable property or global variable
  var isLet: Bool {
    switch self {
      case .class(let rea):    return rea.fieldIsLet
      case .global(let g):     return g.isLet
      case .box, .stack, .tail, .argument, .yield, .pointer, .unidentified:
        return false
    }
  }

  /// True, if the address is immediately produced by an allocation in its function.
  var isLocal: Bool {
    switch self {
      case .box(let pbi):      return pbi.box is AllocBoxInst
      case .class(let rea):    return rea.instance is AllocRefInstBase
      case .tail(let rta):     return rta.instance is AllocRefInstBase
      case .stack:             return true
      case .global, .argument, .yield, .pointer, .unidentified:
        return false
    }
  }

  /// True, if the kind of storage of the access is known (e.g. a class property, or global variable).
  var hasKnownStorageKind: Bool {
    switch self {
      case .box, .class, .tail, .stack, .global:
        return true
      case .argument, .yield, .pointer, .unidentified:
        return false
    }
  }

  /// Returns true if it's guaranteed that this access has the same base address as the `other` access.
  ///
  /// `isEqual` abstracts away the projection instructions that are included as part of the AccessBase:
  /// multiple `project_box` and `ref_element_addr` instructions are equivalent bases as long as they
  /// refer to the same variable or class property.
  func isEqual(to other: AccessBase) -> Bool {
    switch (self, other) {
    case (.box(let pb1), .box(let pb2)):
      return pb1.box.referenceRoot == pb2.box.referenceRoot
    case (.class(let rea1), .class(let rea2)):
      return rea1.fieldIndex == rea2.fieldIndex &&
             rea1.instance.referenceRoot == rea2.instance.referenceRoot
    case (.tail(let rta1), .tail(let rta2)):
      return rta1.instance.referenceRoot == rta2.instance.referenceRoot &&
             rta1.type == rta2.type
    case (.stack(let as1), .stack(let as2)):
      return as1 == as2
    case (.global(let gl1), .global(let gl2)):
      return gl1 == gl2
    case (.argument(let arg1), .argument(let arg2)):
      return arg1 == arg2
    case (.yield(let baResult1), .yield(let baResult2)):
      return baResult1 == baResult2
    case (.pointer(let p1), .pointer(let p2)):
      return p1 == p2
    default:
      return false
    }
  }

  /// Returns `true` if the two access bases do not alias.
  func isDistinct(from other: AccessBase) -> Bool {
  
    func isDifferentAllocation(_ lhs: Value, _ rhs: Value) -> Bool {
      switch (lhs, rhs) {
      case (is Allocation, is Allocation):
        return lhs != rhs
      case (is Allocation, is FunctionArgument),
           (is FunctionArgument, is Allocation):
        // A local allocation cannot alias with something passed to the function.
        return true
      default:
        return false
      }
    }

    func argIsDistinct(_ arg: FunctionArgument, from other: AccessBase) -> Bool {
      if arg.convention.isExclusiveIndirect {
        // Exclusive indirect arguments cannot alias with an address for which we know that it
        // is not derived from that argument (which might be the case for `pointer` and `yield`).
        return other.hasKnownStorageKind
      }
      // Non-exclusive argument still cannot alias with anything allocated locally in the function.
      return other.isLocal
    }

    switch (self, other) {
    
    // First handle all pairs of the same kind (except `yield` and `pointer`).
    case (.box(let pb), .box(let otherPb)):
      return pb.fieldIndex != otherPb.fieldIndex ||
             isDifferentAllocation(pb.box, otherPb.box)
    case (.stack(let asi), .stack(let otherAsi)):
      return asi != otherAsi
    case (.global(let global), .global(let otherGlobal)):
      return global != otherGlobal
    case (.class(let rea), .class(let otherRea)):
      return rea.fieldIndex != otherRea.fieldIndex ||
             isDifferentAllocation(rea.instance, otherRea.instance)
    case (.tail(let rta), .tail(let otherRta)):
      return isDifferentAllocation(rta.instance, otherRta.instance)
    case (.argument(let arg), .argument(let otherArg)):
      return (arg.convention.isExclusiveIndirect || otherArg.convention.isExclusiveIndirect) && arg != otherArg
      
    // Handle arguments vs non-arguments
    case (.argument(let arg), _):
      return argIsDistinct(arg, from: other)
    case (_, .argument(let otherArg)):
      return argIsDistinct(otherArg, from: self)

    default:
      // As we already handled pairs of the same kind, here we handle pairs with different kinds.
      // Different storage kinds cannot alias, regardless where the storage comes from.
      // E.g. a class property address cannot alias with a global variable address.
      return hasKnownStorageKind && other.hasKnownStorageKind
    }
  }
}

/// An `AccessPath` is a pair of a `base: AccessBase` and a `projectionPath: Path`
/// which denotes the offset of the access from the base in terms of projections.
struct AccessPath : CustomStringConvertible {
  let base: AccessBase

  /// address projections only
  let projectionPath: SmallProjectionPath

  static func unidentified() -> AccessPath {
    return AccessPath(base: .unidentified, projectionPath: SmallProjectionPath())
  }

  var description: String {
    "\(projectionPath): \(base)"
  }

  func isDistinct(from other: AccessPath) -> Bool {
    if base.isDistinct(from: other.base) {
      // We can already derived from the bases that there is no alias.
      // No need to look at the projection paths.
      return true
    }
    if base == other.base ||
       (base.hasKnownStorageKind && other.base.hasKnownStorageKind) {
      if !projectionPath.mayOverlap(with: other.projectionPath) {
        return true
      }
    }
    return false
  }

  /// Returns true if this access addresses the same memory location as `other` or if `other`
  /// is a sub-field of this access.

  /// Note that this access _contains_ `other` if `other` has a _larger_ projection path than this acccess.
  /// For example:
  ///   `%value.s0` contains `%value.s0.s1`
  func isEqualOrContains(_ other: AccessPath) -> Bool {
    return getProjection(to: other) != nil
  }

  var materializableProjectionPath: SmallProjectionPath? {
    if projectionPath.isMaterializable {
      return projectionPath
    }
    return nil
  }

  /// Returns the projection path to `other` if this access path is equal or contains `other`.
  ///
  /// For example,
  ///   `%value.s0`.getProjection(to: `%value.s0.s1`)
  /// yields
  ///   `s1`
  func getProjection(to other: AccessPath) -> SmallProjectionPath? {
    if !base.isEqual(to: other.base) {
      return nil
    }
    return projectionPath.subtract(from: other.projectionPath)
  }

  /// Like `getProjection`, but also requires that the resulting projection path is materializable.
  func getMaterializableProjection(to other: AccessPath) -> SmallProjectionPath? {
    if let projectionPath = getProjection(to: other),
       projectionPath.isMaterializable {
      return projectionPath
    }
    return nil
  }
}

private extension SmallProjectionPath {
  /// Returns true if the path only contains projections which can be materialized as
  /// SIL struct or tuple projection instructions - for values or addresses.
  var isMaterializable: Bool {
    let (kind, _, subPath) = pop()
    switch kind {
    case .root:
      return true
    case .structField, .tupleField:
      return subPath.isMaterializable
    default:
      return false
    }
  }
}

private func canBeOperandOfIndexAddr(_ value: Value) -> Bool {
  switch value {
  case is IndexAddrInst, is RefTailAddrInst, is PointerToAddressInst:
    return true
  default:
    return false
  }
}

/// Tries to identify from which address the pointer operand originates from.
/// This is useful to identify patterns like
/// ```
/// %orig_addr = global_addr @...
/// %ptr = address_to_pointer %orig_addr
/// %addr = pointer_to_address %ptr
/// ```
extension PointerToAddressInst {
  var originatingAddress: Value? {

    struct Walker : ValueUseDefWalker {
      let addrType: Type
      var result: Value?
      var walkUpCache = WalkerCache<Path>()

      mutating func rootDef(value: Value, path: SmallProjectionPath) -> WalkResult {
        if let atp = value as? AddressToPointerInst {
          if let res = result, atp.address != res {
            return .abortWalk
          }

          if addrType != atp.address.type { return .abortWalk }
          if !path.isEmpty { return .abortWalk }

          self.result = atp.address
          return .continueWalk
        }
        return .abortWalk
      }

      mutating func walkUp(value: Value, path: SmallProjectionPath) -> WalkResult {
        switch value {
        case is BlockArgument, is MarkDependenceInst, is CopyValueInst,
             is StructExtractInst, is TupleExtractInst, is StructInst, is TupleInst,
             is FunctionArgument, is AddressToPointerInst:
          return walkUpDefault(value: value, path: path)
        default:
          return .abortWalk
        }
      }
    }

    var walker = Walker(addrType: type)
    if walker.walkUp(value: pointer, path: SmallProjectionPath()) == .abortWalk {
      return nil
    }
    return walker.result
  }
}

/// The `EnclosingScope` of an access is the innermost `begin_access`
/// instruction that checks for exclusivity of the access.
/// If there is no `begin_access` instruction found, then the scope is
/// the base itself.
///
/// The access scopes for the snippet below are:
///   (l1, .base(%addr)), (l2, .scope(%a2)), (l3, .scope(%a3))
///
/// ````
/// %addr = ... : $*Int64
/// %l1 = load %addr : $*Int64
/// %a1 = begin_access [read] [dynamic] %addr : $*Int64
/// %a2 = begin_access [read] [dynamic] %addr : $*Int64
/// %l2   = load %a2 : $*Int64
/// end_access %a2 : $*Int64
/// end_access %a1 : $*Int64
/// %a3 = begin_access [read] [dynamic] [no_nested_conflict] %addr : $*Int64
/// %l3 = load %a3 : $*Int64
/// end_access %a3 : $*Int64
/// ```
enum EnclosingScope {
  case scope(BeginAccessInst)
  case base(AccessBase)
}

private struct AccessPathWalker : AddressUseDefWalker {
  var result = AccessPath.unidentified()
  var foundBeginAccess: BeginAccessInst?

  mutating func walk(startAt address: Value, initialPath: SmallProjectionPath = SmallProjectionPath()) {
    if walkUp(address: address, path: Path(projectionPath: initialPath)) == .abortWalk {
      assert(result.base == .unidentified,
             "shouldn't have set an access base in an aborted walk")
    }
  }

  struct Path : SmallProjectionWalkingPath {
    let projectionPath: SmallProjectionPath

    // Tracks whether an `index_addr` instruction was crossed.
    // It should be (FIXME: check if it's enforced) that operands
    // of `index_addr` must be `tail_addr` or other `index_addr` results.
    let indexAddr: Bool

    init(projectionPath: SmallProjectionPath = SmallProjectionPath(), indexAddr: Bool = false) {
      self.projectionPath = projectionPath
      self.indexAddr = indexAddr
    }

    func with(projectionPath: SmallProjectionPath) -> Self {
      return Self(projectionPath: projectionPath, indexAddr: indexAddr)
    }

    func with(indexAddr: Bool) -> Self {
      return Self(projectionPath: projectionPath, indexAddr: indexAddr)
    }

    func merge(with other: Self) -> Self {
      return Self(
        projectionPath: projectionPath.merge(with: other.projectionPath),
        indexAddr: indexAddr || other.indexAddr
      )
    }
  }

  mutating func rootDef(address: Value, path: Path) -> WalkResult {
    assert(result.base == .unidentified, "rootDef should only called once")
    // Try identifying the address a pointer originates from
    if let p2ai = address as? PointerToAddressInst {
      if let originatingAddr = p2ai.originatingAddress {
        return walkUp(address: originatingAddr, path: path)
      } else {
        self.result = AccessPath(base: .pointer(p2ai), projectionPath: path.projectionPath)
        return .continueWalk
      }
    }

    let base = AccessBase(baseAddress: address)
    self.result = AccessPath(base: base, projectionPath: path.projectionPath)
    return .continueWalk
  }

  mutating func walkUp(address: Value, path: Path) -> WalkResult {
    if address is IndexAddrInst {
      // Track that we crossed an `index_addr` during the walk-up
      return walkUpDefault(address: address, path: path.with(indexAddr: true))
    } else if path.indexAddr && !canBeOperandOfIndexAddr(address) {
      // An `index_addr` instruction cannot be derived from an address
      // projection. Bail out
      return .abortWalk
    } else if let ba = address as? BeginAccessInst, foundBeginAccess == nil {
      foundBeginAccess = ba
    }
    return walkUpDefault(address: address, path: path.with(indexAddr: false))
  }
}

extension Value {
  // Convenient properties to avoid instantiating an explicit AccessPathWalker.
  //
  // Although an AccessPathWalker is created for each call of these properties,
  // it's very unlikely that this will end up in memory allocations.
  // Only in the rare case of `pointer_to_address` -> `address_to_pointer` pairs, which
  // go through phi-arguments, the AccessPathWalker will allocate memnory in its cache.

  /// Computes the access base of this address value.
  var accessBase: AccessBase { accessPath.base }

  /// Computes the access path of this address value.
  var accessPath: AccessPath {
    var walker = AccessPathWalker()
    walker.walk(startAt: self)
    return walker.result
  }

  func getAccessPath(fromInitialPath: SmallProjectionPath) -> AccessPath {
    var walker = AccessPathWalker()
    walker.walk(startAt: self, initialPath: fromInitialPath)
    return walker.result
  }

  /// Computes the access path of this address value and also returns the scope.
  var accessPathWithScope: (AccessPath, scope: BeginAccessInst?) {
    var walker = AccessPathWalker()
    walker.walk(startAt: self)
    return (walker.result, walker.foundBeginAccess)
  }

  /// Computes the enclosing access scope of this address value.
  var enclosingAccessScope: EnclosingScope {
    var walker = AccessPathWalker()
    walker.walk(startAt: self)
    if let ba = walker.foundBeginAccess {
      return .scope(ba)
    }
    return .base(walker.result.base)
  }

  /// The root definition of a reference, obtained by skipping casts, etc.
  var referenceRoot: Value {
    var value: Value = self
    while true {
      switch value {
      case is BeginBorrowInst, is CopyValueInst, is MoveValueInst,
           is UpcastInst, is UncheckedRefCastInst, is EndCOWMutationInst:
        value = (value as! Instruction).operands[0].value
      case let mvr as MultipleValueInstructionResult:
        guard  let bcm = mvr.parentInstruction as? BeginCOWMutationInst else {
          return value
        }
        value = bcm.instance
      default:
        return value
      }
    }
  }
}

/// A ValueUseDef walker that that visits access storage paths of an address.
///
/// An access storage path is the reference (or a value which contains a reference)
/// an address originates from.
/// In the following example the `storage` is `contains_ref` with `path` `"s0.c0.s0"`
/// ```
///   %ref = struct_extract %contains_ref : $S, #S.l
///   %base = ref_element_addr %ref : $List, #List.x
///   %addr = struct_element_addr %base : $X, #X.e
///   store %v to [trivial] %addr : $*Int
/// ```
extension ValueUseDefWalker where Path == SmallProjectionPath {
  /// The main entry point.
  /// Given an `accessPath` where the access base is a reference (class, tail, box), call
  /// the `visit` function for all storage roots with a the corresponding path.
  /// Returns true on success.
  /// Returns false if not all storage roots could be identified or if `accessPath` has not a "reference" base.
  mutating func visitAccessStorageRoots(of accessPath: AccessPath) -> Bool {
    walkUpCache.clear()
    let path = accessPath.projectionPath
    switch accessPath.base {
      case .box(let pbi):
        return walkUp(value: pbi.box, path: path.push(.classField, index: pbi.fieldIndex)) != .abortWalk
      case .class(let rea):
        return walkUp(value: rea.instance, path: path.push(.classField, index: rea.fieldIndex)) != .abortWalk
      case .tail(let rta):
        return walkUp(value: rta.instance, path: path.push(.tailElements, index: 0)) != .abortWalk
      case .stack, .global, .argument, .yield, .pointer, .unidentified:
        return false
    }
  }
}
