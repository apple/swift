//===--- WalkUtils.swift - Utilities for use-def def-use walks ------------===//
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
// This file provides utilities for SSA def-use and use-def walking.
// There are four walker protocols:
// * for both directions: down (= def-use) and up (= use-def)
// * for values and addresses
// ```
//   protocol ValueDefUseWalker
//   protocol AddressDefUseWalker
//   protocol ValueUseDefWalker
//   protocol AddressUseDefWalker
// ```
//
// To use a walker, just conform to one (or multiple) of those protocols.
// There are several ways to configure the walker by providing implementations of
// their protocol functions. For details see the protocol definitions.
// The value-walkers also require to provide a "cache" property - see `WalkerCache`.
//
// The walkers maintain a "path" during the walk, which in it's simplest form can
// just be a SmallProjectionPath. For details see `WalkingPath`.
//===----------------------------------------------------------------------===//

import SIL

/// Result returned by the walker functions
enum WalkResult {
  /// Continue the walk
  case continueWalk
  /// Stop the walks of all uses, a sufficient condition has been found
  case abortWalk
}

/// The path which is updated throughout a walk.
///
/// Usually this is just a SmallProjectionPath, but clients can implement their own path, e.g.
/// to maintain additional data throughout the walk.
protocol WalkingPath : Equatable {
  typealias FieldKind = SmallProjectionPath.FieldKind

  /// Returns the merged path of this path and `with`.
  func merge(with: Self) -> Self
  
  /// Pops the first path component if it is exactly of kind `kind` - not considering wildcards.
  ///
  /// Returns the index of the component and the new path or - if not matching - returns nil.
  /// Called for destructure instructions during down-walking and for aggregate instructions during up-walking.
  func pop(kind: FieldKind) -> (index: Int, path: Self)?

  /// Pops the first path component if it matches `kind` and (optionally) `index`.
  ///
  /// Called for projection instructions during down-walking and for aggregate instructions during up-walking.
  func popIfMatches(_ kind: FieldKind, index: Int?) -> Self?
  
  /// Pushes a new first component to the path and returns the new path.
  ///
  /// Called for aggregate instructions during down-walking and for projection instructions during up-walking.
  func push(_ kind: FieldKind, index: Int) -> Self
}

extension SmallProjectionPath : WalkingPath { }

/// A `WalkingPath` where `push` and `pop` instructions
/// are forwarded to an underlying `projectionPath`.
protocol SmallProjectionWalkingPath : WalkingPath {
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
  var projectionPath: SmallProjectionPath { get }
  func with(projectionPath: SmallProjectionPath) -> Self
}

extension SmallProjectionWalkingPath {
  func pop(kind: FieldKind) -> (index: Int, path: Self)? {
    if let (idx, p) = projectionPath.pop(kind: kind) {
      return (idx, with(projectionPath: p))
    }
    return nil
  }

  func popIfMatches(_ kind: FieldKind, index: Int?) -> Self? {
    if let p = projectionPath.popIfMatches(kind, index: index) {
      return with(projectionPath: p)
    }
    return nil
  }
  func push(_ kind: FieldKind, index: Int) -> Self {
    return with(projectionPath: projectionPath.push(kind, index: index))
  }
}

/// A walking path which matches everything.
///
/// Useful for walkers which don't care about the path and unconditionally walk to all defs/uses.
struct UnusedWalkingPath : WalkingPath {
  func merge(with: Self) -> Self { self }
  func pop(kind: FieldKind) -> (index: Int, path: Self)? { nil }
  func popIfMatches(_ kind: FieldKind, index: Int?) -> Self? { self }
  func push(_ kind: FieldKind, index: Int) -> Self { self }
}

/// Caches the state of a walk.
///
/// A client must provide this cache in a `walkUpCache` or `walkDownCache` property.
struct WalkerCache<Path : WalkingPath> {
  mutating func needWalk(for value: Value, path: Path) -> Path? {
  
    // Handle the first inline entry.
    guard let e = inlineEntry0 else {
      inlineEntry0 = (value, path)
      return path
    }
    if e.value == value {
      let newPath = e.path.merge(with: path)
      if newPath != e.path {
        inlineEntry0 = (value, newPath)
        return newPath
      }
      return nil
    }
    
    // Handle the second inline entry.
    guard let e = inlineEntry1 else {
      inlineEntry1 = (value, path)
      return path
    }
    if e.value == value {
      let newPath = e.path.merge(with: path)
      if newPath != e.path {
        inlineEntry1 = (value, newPath)
        return newPath
      }
      return nil
    }
    
    // If there are more than two elements, it goes into the `cache` Dictionary.
    return cache[value.hashable, default: CacheEntry()].needWalk(path: path)
  }

  mutating func clear() {
    inlineEntry0 = nil
    inlineEntry1 = nil
    cache.removeAll(keepingCapacity: true)
  }

  private struct CacheEntry {
    var cachedPath: Path?
    
    mutating func needWalk(path: Path) -> Path? {
      guard let previousPath = cachedPath else {
        self.cachedPath = path
        return path
      }
      let newPath = previousPath.merge(with: path)
      if newPath != previousPath {
        self.cachedPath = newPath
        return newPath
      }
      return nil
    }
  }

  // If there are no more than 2 elements in the cache, we can avoid using the `cache` Dictionary,
  // which avoids memory allocations.
  // Fortunately this is the common case by far (about 97% of all walker invocations).
  private var inlineEntry0: (value: Value, path: Path)?
  private var inlineEntry1: (value: Value, path: Path)?

  // All elements, which don't fit into the inline entries.
  private var cache = Dictionary<HashableValue, CacheEntry>()
}

/// - A `DefUseWalker` finds all uses of a target value.
///
/// - A target value is described by an "initial" value and a projection path.
///   1. If the projection path is empty (`""`) then the target value is the initial value itself.
///   2. If the projection path is non-empty (`"s0.1.e3"`), then the target value is the one
///     reachable from the initial value through the series of projections described by the path.
/// - A path can also contain a pattern such as `"v**"` which means any series of "value"
///   projections (excluding `ref_element_addr` and similar, i.e. `c*`) from any field.
///   In the `v**` case, the target value*s* are many, i.e. all the ones reachable from
///   the initial value through _any of the fields_ through _any number_ of value projections.
///   `c*` means values reachable through a _single_ projection of _any_ of the fields of the class.
///
/// - A walk is started with a call to `walkDownUses(initial, path: path)`.
/// - This function will call `walkDown(operand, path: path)`
///   for every use of `initial` as `operand` in an instruction.
/// - For each use, then the walk can continue with initial value the result if the result of the using
///   instruction might still reach the target value with a new projection path.
///   1. If the use is a construction such as a
///     `%res = struct $S (%f0)` (or `%res = tuple (%unk, %1)`) instruction and the path is `p`
///     then the `%res` result value reaches the target value through the new projection`s0.p` (respectively `1.p`).
///   2.  If the use is a projection such as `%res = struct_extract %s : $S, #S.field0` and the
///     path is `s0.s1` then the target value is reachable from `%res` with path `s1`.
///     If the path doesn't match `unmatchedPath` is called.
///   3. If the use is a "forwarding instruction", such as a cast, the walk continues with the same path.
///   4. If the use is an unhandled instruction then `leafUse` is called to denote that the client has to
///     handle this use.
///
/// There are two types of DefUseWalkers, one for values (`ValueDefUseWalker`) and one for
/// addresses (`AddressDefUseWalker`)


/// A `ValueDefUseWalker` can only handle "value" initial values, which correspond
/// to types that are not addresses, i.e. _do not have_ an asterisk (`*`) in the textual
/// representation of their SIL type (`$T`).
/// These can be values of reference type, or struct/tuple etc.
/// A `ValueDefUseWalker.walkDownDefault` called on a use of a initial "value" which
/// yields an "address" value (such as `ref_element_addr %initial_value`) will call `leafUse`
/// since the walk can't proceed.
///
/// Example call `walkDownUses(%str, path: "s0.s1")`
/// ```
/// %fa    = struct_extract %str  : $S1, #S1.fa   // 1. field 0, walkDownUses(%fa, "s1")
/// %fb    = struct_extract %str  : $S1, #S1.fb   // 5. field 1, unmatchedPath(%str, "s0.s1")
/// %fa.ga = struct_extract %fa   : $S2, #S2.ga   // 2. field 1, walkDownUses(%fa.ga, "")
/// ...    = struct_extract %fa.ga: $S3, #S3.ha   // 3. empty path, unmatchedPath(%fa.ga, "")
/// ...    = <instruction>  %fa.ga:               // 4. unknown instruction, leafUse(%fa.ga, "")
/// ...    = <instruction>  %str:                 // 6. unknown instruction, leafUse(%str, "s0.s1")
/// ```
protocol ValueDefUseWalker {
  associatedtype Path: WalkingPath
  
  /// Called on each use. The implementor can decide to continue the walk by calling
  /// `walkDownDefault(value: value, path: path)` or
  /// do nothing.
  mutating func walkDown(value: Operand, path: Path) -> WalkResult
  
  /// Walks down all results of the multi-value instruction `inst`.
  ///
  /// This is called if the path doesn't filter a specific result, but contains a wildcard which matches all results.
  /// Clients can but don't need to customize this function.
  mutating func walkDownAllResults(of inst: MultipleValueInstruction, path: Path) -> WalkResult

  /// `leafUse` is called from `walkDownDefault` when the walk can't continue for this use since
  /// this is an instruction unknown to the default walker which _might_ be a "transitive use"
  /// of the target value (such as `destroy_value %initial` or a `builtin ... %initial` instruction)
  mutating func leafUse(value: Operand, path: Path) -> WalkResult
  
  /// `unmatchedPath` is called from `walkDownDefault` when this is a use
  /// of the initial value in an instruction recognized by the walker
  /// but for which the requested `path` does not allow the walk to continue.
  mutating func unmatchedPath(value: Operand, path: Path) -> WalkResult

  /// A client must implement this function to cache walking results.
  /// The function returns `nil` if the walk doesn't need to continue because
  /// the `def` was already handled before.
  /// In case the walk needs to be continued, this function returns the path for continuing the walk.
  ///
  /// This method is called for two cases:
  /// 1. To avoid exponential complexity during a walk down with a wildcard path `v**` or `**`
  ///   ```
  ///   (%1, %2, %3, %4) = destructure_tuple %t1
  ///   %t2 = tuple (%1, %2, %3, %4)
  ///   (%5, %6, %7, %8) = destructure_tuple %t2
  ///   %t3 = tuple (%5, %6, %7, %8)
  ///   ```
  /// 2. To handle "phi webs" of `br` instructions which would lead to an infinite
  ///   walk down. In this case the implementor must ensure that eventually
  ///   `shouldRecomputeDown` returns `nil`, i.e. a fixpoint has been reached.
  ///   - If the implementor doesn't need for the walk to cross phi webs,
  ///     it can intercept `BranchInst`/`CondBranchInst` in `walkDown` and
  ///     not call `walkDownDefault` for these cases.
  ///   - Phi webs arise only for "value"s.
  var walkDownCache: WalkerCache<Path> { get set }
}

extension ValueDefUseWalker {
  mutating func walkDown(value operand: Operand, path: Path) -> WalkResult {
    return walkDownDefault(value: operand, path: path)
  }
  
  mutating func unmatchedPath(value: Operand, path: Path) -> WalkResult {
    return .continueWalk
  }
  
  /// Given an operand to an instruction, tries to continue the walk with the uses of
  /// instruction's result if the target value is reachable from it (i.e. matches the `path`) .
  /// If the walk can't continue, it calls `leafUse` or `unmatchedPath`
  mutating func walkDownDefault(value operand: Operand, path: Path) -> WalkResult {
    let instruction = operand.instruction
    switch instruction {
    case let str as StructInst:
      return walkDownUses(ofValue: str,
                          path: path.push(.structField, index: operand.index))
    case let t as TupleInst:
      return walkDownUses(ofValue: t,
                          path: path.push(.tupleField, index: operand.index))
    case let e as EnumInst:
      return walkDownUses(ofValue: e,
                          path: path.push(.enumCase, index: e.caseIndex))
    case let se as StructExtractInst:
      if let path = path.popIfMatches(.structField, index: se.fieldIndex) {
        return walkDownUses(ofValue: se, path: path)
      } else {
        return unmatchedPath(value: operand, path: path)
      }
    case let te as TupleExtractInst:
      if let path = path.popIfMatches(.tupleField, index: te.fieldIndex) {
        return walkDownUses(ofValue: te, path: path)
      } else {
        return unmatchedPath(value: operand, path: path)
      }
    case let ued as UncheckedEnumDataInst:
      if let path = path.popIfMatches(.enumCase, index: ued.caseIndex) {
        return walkDownUses(ofValue: ued, path: path)
      } else {
        return unmatchedPath(value: operand, path: path)
      }
    case let ds as DestructureStructInst:
      if let (index, path) = path.pop(kind: .structField) {
        return walkDownUses(ofValue: ds.results[index], path: path)
      } else if path.popIfMatches(.anyValueFields, index: nil) != nil {
        return walkDownAllResults(of: ds, path: path)
      } else {
        return unmatchedPath(value: operand, path: path)
      }
    case let dt as DestructureTupleInst:
      if let (index, path) = path.pop(kind: .tupleField) {
        return walkDownUses(ofValue: dt.results[index], path: path)
      } else if path.popIfMatches(.anyValueFields, index: nil) != nil {
        return walkDownAllResults(of: dt, path: path)
      } else {
        return unmatchedPath(value: operand, path: path)
      }
    case let ier as InitExistentialRefInst:
      return walkDownUses(ofValue: ier, path: path.push(.existential, index: 0))
    case let oer as OpenExistentialRefInst:
      if let path = path.popIfMatches(.existential, index: 0) {
        return walkDownUses(ofValue: oer, path: path)
      } else {
        return unmatchedPath(value: operand, path: path)
      }
    case is BeginBorrowInst, is CopyValueInst, is MoveValueInst,
      is UpcastInst, is UncheckedRefCastInst, is EndCOWMutationInst,
      is RefToBridgeObjectInst, is BridgeObjectToRefInst, is MarkMustCheckInst:
      return walkDownUses(ofValue: (instruction as! SingleValueInstruction), path: path)
    case let mdi as MarkDependenceInst:
      if operand.index == 0 {
        return walkDownUses(ofValue: mdi, path: path)
      } else {
        return unmatchedPath(value: operand, path: path)
      }
    case let br as BranchInst:
      let val = br.getArgument(for: operand)
      if let path = walkDownCache.needWalk(for: val, path: path) {
        return walkDownUses(ofValue: val, path: path)
      } else {
        return .continueWalk
      }
    case let cbr as CondBranchInst:
      if let val = cbr.getArgument(for: operand) {
        if let path = walkDownCache.needWalk(for: val, path: path) {
          return walkDownUses(ofValue: val, path: path)
        } else {
          return .continueWalk
        }
      } else {
        return leafUse(value: operand, path: path)
      }
    case let se as SwitchEnumInst:
      if let (caseIdx, path) = path.pop(kind: .enumCase),
         let succBlock = se.getUniqueSuccessor(forCaseIndex: caseIdx),
         let payload = succBlock.arguments.first {
        return walkDownUses(ofValue: payload, path: path)
      } else if path.popIfMatches(.anyValueFields, index: nil) != nil {
        for succBlock in se.parentBlock.successors {
          if let payload = succBlock.arguments.first,
             walkDownUses(ofValue: payload, path: path) == .abortWalk {
            return .abortWalk
          }
        }
        return .continueWalk
      } else {
        return unmatchedPath(value: operand, path: path)
      }
    case let bcm as BeginCOWMutationInst:
      return walkDownUses(ofValue: bcm.instanceResult, path: path)
    default:
      return leafUse(value: operand, path: path)
    }
  }
  
  /// Starts the walk
  mutating func walkDownUses(ofValue: Value, path: Path) -> WalkResult {
    for operand in ofValue.uses where !operand.isTypeDependent {
      if walkDown(value: operand, path: path) == .abortWalk {
        return .abortWalk
      }
    }
    return .continueWalk
  }
  
  mutating func walkDownAllResults(of inst: MultipleValueInstruction, path: Path) -> WalkResult {
    for result in inst.results {
      if let path = walkDownCache.needWalk(for: result, path: path) {
        if walkDownUses(ofValue: result, path: path) == .abortWalk {
          return .abortWalk
        }
      }
    }
    return .continueWalk
  }
}

/// An `AddressDefUseWalker` can only handle initial "addresses", which correspond
/// to types that are addresses (`$*T`).
/// An `AddressDefUseWalker.walkDownDefault` called on a use of an initial "address"
/// which results in a "value" (such as `load %initial_addr`) will call `leafUse` since the walk
/// can't proceed.
/// All functions return a boolean flag which, if true, can stop the walk of the other uses
/// and the whole walk.
protocol AddressDefUseWalker {
  associatedtype Path: WalkingPath
  
  /// Called on each use. The implementor can decide to continue the walk by calling
  /// `walkDownDefault(address: address, path: path)` or
  /// do nothing.
  mutating func walkDown(address: Operand, path: Path) -> WalkResult
  
  /// `leafUse` is called from `walkDownDefault` when the walk can't continue for this use since
  /// this is an instruction unknown to the default walker which might be a "transitive use"
  /// of the target value (such as `destroy_addr %initial_addr` or a `builtin ... %initial_addr` instruction).
  mutating func leafUse(address: Operand, path: Path) -> WalkResult
  
  /// `unmatchedPath` is called from `walkDownDefault` when this is a use
  /// of the initial address in an instruction recognized by the walker
  /// but for which the requested `path` does not allow the walk to continue.
  mutating func unmatchedPath(address: Operand, path: Path) -> WalkResult
}

extension AddressDefUseWalker {
  mutating func walkDown(address operand: Operand, path: Path) -> WalkResult {
    return walkDownDefault(address: operand, path: path)
  }
  
  mutating func unmatchedPath(address: Operand, path: Path) -> WalkResult {
    return .continueWalk
  }
  
  mutating func walkDownDefault(address operand: Operand, path: Path) -> WalkResult {
    let instruction = operand.instruction
    switch instruction {
    case let sea as StructElementAddrInst:
      if let path = path.popIfMatches(.structField, index: sea.fieldIndex) {
        return walkDownUses(ofAddress: sea, path: path)
      } else {
        return unmatchedPath(address: operand, path: path)
      }
    case let tea as TupleElementAddrInst:
      if let path = path.popIfMatches(.tupleField, index: tea.fieldIndex) {
        return walkDownUses(ofAddress: tea, path: path)
      } else {
        return unmatchedPath(address: operand, path: path)
      }
    case is InitEnumDataAddrInst, is UncheckedTakeEnumDataAddrInst:
      let ei = instruction as! SingleValueInstruction
      if let path = path.popIfMatches(.enumCase, index: (instruction as! EnumInstruction).caseIndex) {
        return walkDownUses(ofAddress: ei, path: path)
      } else {
        return unmatchedPath(address: operand, path: path)
      }
    case is InitExistentialAddrInst, is OpenExistentialAddrInst:
      if let path = path.popIfMatches(.existential, index: 0) {
        return walkDownUses(ofAddress: instruction as! SingleValueInstruction, path: path)
      } else {
        return unmatchedPath(address: operand, path: path)
      }
    case let ia as IndexAddrInst:
      if let (pathIdx, subPath) = path.pop(kind: .indexedElement) {
        if let idx = ia.constantSmallIndex,
           idx == pathIdx {
          return walkDownUses(ofAddress: ia, path: subPath)
        }
        return walkDownUses(ofAddress: ia, path: subPath.push(.anyIndexedElement, index: 0))
      }
      return walkDownUses(ofAddress: ia, path: path)
    case let mmc as MarkMustCheckInst:
      return walkDownUses(ofAddress: mmc, path: path)
    case let ba as BeginAccessInst:
      // Don't treat `end_access` as leaf-use. Just ignore it.
      return walkDownNonEndAccessUses(of: ba, path: path)
    case let mdi as MarkDependenceInst:
      if operand.index == 0 {
        return walkDownUses(ofAddress: mdi, path: path)
      } else {
        return unmatchedPath(address: operand, path: path)
      }
    default:
      return leafUse(address: operand, path: path)
    }
  }
  
  mutating func walkDownUses(ofAddress: Value, path: Path) -> WalkResult {
    for operand in ofAddress.uses where !operand.isTypeDependent {
      if walkDown(address: operand, path: path) == .abortWalk {
        return .abortWalk
      }
    }
    return .continueWalk
  }

  private mutating func walkDownNonEndAccessUses(of beginAccess: BeginAccessInst, path: Path) -> WalkResult {
    for operand in beginAccess.uses where !operand.isTypeDependent {
      if !(operand.instruction is EndAccessInst),
         walkDown(address: operand, path: path) == .abortWalk {
        return .abortWalk
      }
    }
    return .continueWalk
  }
}

/// - A `UseDefWalker` can be used to find all "generating" definitions of
///   a target value.
/// - A target value is described by an "initial" value and a projection path as in a `DefUseWalker.`
///   1. If the projection path is empty (`""`) then the target value is the initial value itself.
///   2. If the projection path is non-empty (`"s0.1.e3"`), then the target value is the one
///     reachable through the series of projections described by the path, applied to the initial value.
/// - The same notes about wildcard paths in `DefUseWalker` apply here.
///
/// - A walk is started with a call to `walkUp(initial, path: path)`.
///
/// - The implementor of `walkUp` can then track the definition if needed and
///   continue the walk by calling `walkUpDefault`.
///   `walkUpDefault` will do the following:
///   1. If the instruction of the definition is a projection, then it will continue
///     the walk by calling `walkUp` on the operand definition and an adjusted (pushed) path
///     to reflect that a further projection is needed to reach the value of interest from the new initial value.
///   2. If the instruction of the definition is a value construction such as `struct` and
///     the head of the path matches the instruction type then the walk continues
///     with a call to `walkUp` with initial value the operand defintion denoted by the path
///     and the suffix path as path since the target value can now be reached with fewer projections.
///     If the defining instruction of the value does not match the head of the path as in
///     `%t = tuple ...` and `"s0.t1"` then `unmatchedPath(%t, ...)` is called.
///   3. If the instruction is a forwarding instruction, such as a cast, the walk continues with `walkUp`
///     with the operand definition as initial value and same path.
///   4. If the instruction is not handled by this walker or the path is empty, then `rootDef` is called to
///     denote that the walk can't continue and that the definition of the target has been reached.
protocol ValueUseDefWalker {
  associatedtype Path: WalkingPath
  
  /// Starting point of the walk. The implementor can decide to continue the walk by calling
  /// `walkUpDefault(value: value, path: path)` or
  /// do nothing.
  mutating func walkUp(value: Value, path: Path) -> WalkResult

  /// Walks up all operands of `def`. This is called if the path doesn't filter a specific operand,
  /// but contains a wildcard which matches all operands.
  /// Clients can but don't need to customize this function.
  mutating func walkUpAllOperands(of def: Instruction, path: Path) -> WalkResult

  /// `rootDef` is called from `walkUpDefault` when the walk can't continue for this use since
  /// either
  /// * the defining instruction is unknown to the default walker
  /// * the `path` is empty (`""`) and therefore this is the definition of the target value.
  mutating func rootDef(value: Value, path: Path) -> WalkResult
  
  /// `unmatchedPath` is called from `walkUpDefault` when the defining instruction
  /// is unrelated to the `path` the walk should follow.
  mutating func unmatchedPath(value: Value, path: Path) -> WalkResult
  
  /// A client must implement this function to cache walking results.
  /// The function returns nil if the walk doesn't need to continue because
  /// the `def` was already handled before.
  /// In case the walk needs to be continued, this function returns the path
  /// for continuing the walk.
  var walkUpCache: WalkerCache<Path> { get set }
}

extension ValueUseDefWalker {
  mutating func walkUp(value: Value, path: Path) -> WalkResult {
    return walkUpDefault(value: value, path: path)
  }
  
  mutating func unmatchedPath(value: Value, path: Path) -> WalkResult {
    return .continueWalk
  }
  
  mutating func walkUpDefault(value def: Value, path: Path) -> WalkResult {
    switch def {
    case let str as StructInst:
      if let (index, path) = path.pop(kind: .structField) {
        if index >= str.operands.count {
          // This can happen if there is a type mismatch, e.g. two different concrete types of an existential
          // are visited for the same path.
          return unmatchedPath(value: str, path: path)
        }
        return walkUp(value: str.operands[index].value, path: path)
      } else if path.popIfMatches(.anyValueFields, index: nil) != nil {
        return walkUpAllOperands(of: str, path: path)
      } else {
        return unmatchedPath(value: str, path: path)
      }
    case let t as TupleInst:
      if let (index, path) = path.pop(kind: .tupleField) {
        if index >= t.operands.count {
          // This can happen if there is a type mismatch, e.g. two different concrete types of an existential
          // are visited for the same path.
          return unmatchedPath(value: t, path: path)
        }
        return walkUp(value: t.operands[index].value, path: path)
      } else if path.popIfMatches(.anyValueFields, index: nil) != nil {
        return walkUpAllOperands(of: t, path: path)
      } else {
        return unmatchedPath(value: t, path: path)
      }
    case let e as EnumInst:
      if let path = path.popIfMatches(.enumCase, index: e.caseIndex),
         let payload = e.payload {
        return walkUp(value: payload, path: path)
      } else {
        return unmatchedPath(value: e, path: path)
      }
    case let se as StructExtractInst:
      return walkUp(value: se.struct, path: path.push(.structField, index: se.fieldIndex))
    case let te as TupleExtractInst:
      return walkUp(value: te.tuple, path: path.push(.tupleField, index: te.fieldIndex))
    case let ued as UncheckedEnumDataInst:
      return walkUp(value: ued.enum, path: path.push(.enumCase, index: ued.caseIndex))
    case let mvr as MultipleValueInstructionResult:
      let instruction = mvr.parentInstruction
      if let ds = instruction as? DestructureStructInst {
        return walkUp(value: ds.struct, path: path.push(.structField, index: mvr.index))
      } else if let dt = instruction as? DestructureTupleInst {
        return walkUp(value: dt.tuple, path: path.push(.tupleField, index: mvr.index))
      } else if let bcm = instruction as? BeginCOWMutationInst {
        return walkUp(value: bcm.instance, path: path)
      } else {
        return rootDef(value: mvr, path: path)
      }
    case let ier as InitExistentialRefInst:
      if let path = path.popIfMatches(.existential, index: 0) {
        return walkUp(value: ier.instance, path: path)
      } else {
        return unmatchedPath(value: ier, path: path)
      }
    case let oer as OpenExistentialRefInst:
      return walkUp(value: oer.existential, path: path.push(.existential, index: 0))
    case is BeginBorrowInst, is CopyValueInst, is MoveValueInst,
      is UpcastInst, is UncheckedRefCastInst, is EndCOWMutationInst,
      is RefToBridgeObjectInst, is BridgeObjectToRefInst, is MarkMustCheckInst:
      return walkUp(value: (def as! Instruction).operands[0].value, path: path)
    case let arg as BlockArgument:
      if arg.isPhiArgument {
        for incoming in arg.incomingPhiValues {
          // Check the cache to avoid cycles in the walk
          if let path = walkUpCache.needWalk(for: incoming, path: path) {
            if walkUp(value: incoming, path: path) == .abortWalk {
              return .abortWalk
            }
          }
        }
        return .continueWalk
      }
      
      let block = arg.parentBlock
      if let pred = block.singlePredecessor,
         let se = pred.terminator as? SwitchEnumInst,
         let caseIdx = se.getUniqueCase(forSuccessor: block) {
        return walkUp(value: se.enumOp, path: path.push(.enumCase, index: caseIdx))
      }
      
      return rootDef(value: def, path: path)
    default:
      return rootDef(value: def, path: path)
    }
  }
  
  mutating func walkUpAllOperands(of def: Instruction, path: Path) -> WalkResult {
    for operand in def.operands {
      // `shouldRecompute` is called to avoid exponential complexity in
      // programs like
      //
      // (%1, %2) = destructure_struct %0
      // %3 = struct $Struct %1 %2
      // (%4, %5) = destructure_struct %3
      // %6 = struct $Struct %4 %5
      if let path = walkUpCache.needWalk(for: operand.value, path: path) {
        if walkUp(value: operand.value, path: path) == .abortWalk {
          return .abortWalk
        }
      }
    }
    return .continueWalk
  }
}

protocol AddressUseDefWalker {
  associatedtype Path: WalkingPath
  
  /// Starting point of the walk. The implementor can decide to continue the walk by calling
  /// `walkUpDefault(address: address, path: path)` or
  /// do nothing.
  mutating func walkUp(address: Value, path: Path) -> WalkResult
  
  /// `rootDef` is called from `walkUpDefault` when the walk can't continue for this use since
  /// either
  /// * the defining instruction is unknown to the default walker
  /// * the `path` is empty (`""`) and therefore this is the definition of the target value.
  mutating func rootDef(address: Value, path: Path) -> WalkResult
  
  /// `unmatchedPath` is called from `walkUpDefault` when the defining instruction
  /// is unrelated to the `path` the walk should follow.
  mutating func unmatchedPath(address: Value, path: Path) -> WalkResult
}

extension AddressUseDefWalker {
  
  mutating func walkUp(address: Value, path: Path) -> WalkResult {
    return walkUpDefault(address: address, path: path)
  }
  
  mutating func unmatchedPath(address: Value, path: Path) -> WalkResult {
    return .continueWalk
  }
  
  mutating func walkUpDefault(address def: Value, path: Path) -> WalkResult {
    switch def {
    case let sea as StructElementAddrInst:
      return walkUp(address: sea.struct, path: path.push(.structField, index: sea.fieldIndex))
    case let tea as TupleElementAddrInst:
      return walkUp(address: tea.tuple, path: path.push(.tupleField, index: tea.fieldIndex))
    case is InitEnumDataAddrInst, is UncheckedTakeEnumDataAddrInst:
      return walkUp(address: (def as! UnaryInstruction).operand.value,
                    path: path.push(.enumCase, index: (def as! EnumInstruction).caseIndex))
    case is InitExistentialAddrInst, is OpenExistentialAddrInst:
      return walkUp(address: (def as! Instruction).operands[0].value, path: path.push(.existential, index: 0))
    case is BeginAccessInst, is MarkMustCheckInst:
      return walkUp(address: (def as! Instruction).operands[0].value, path: path)
    case let ia as IndexAddrInst:
      if let idx = ia.constantSmallIndex {
        return walkUp(address: ia.base, path: path.push(.indexedElement, index: idx))
      } else {
        return walkUp(address: ia.base, path: path.push(.anyIndexedElement, index: 0))
      }
    case let mdi as MarkDependenceInst:
      return walkUp(address: mdi.operands[0].value, path: path)
    default:
      return rootDef(address: def, path: path)
    }
  }
}

private extension IndexAddrInst {
  var constantSmallIndex: Int? {
    guard let literal = index as? IntegerLiteralInst else {
      return nil
    }
    let index = literal.value
    if index.isIntN(16) {
      return Int(index.getSExtValue())
    }
    return nil
  }
}

