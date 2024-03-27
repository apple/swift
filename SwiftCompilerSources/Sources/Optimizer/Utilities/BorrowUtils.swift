//===--- BorrowUtils.swift - Utilities for borrow scopes ------------------===//
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
// Utilities that model Ownership SSA (OSSA) borrow scopes.
//
// A BorrowingInstruction borrows one or more operands over a new
// borrow scope, up to its scope-ending uses. This is typically
// checked during a def-use walk.
//
//   %val = some owned value
//   %store = store_borrow %val to %addr // borrowing instruction
//   ...                                 // borrow scope
//   end_borrow %store                   // scope-ending use
//
// A BeginBorrowValue introduces a guaranteed OSSA lifetime. It
// begins a new borrow scope that ends at its scope-ending uses. A
// begin-borrow value may be defined by a borrowing instruction:
//
//   %begin = begin_borrow %val          // %begin borrows %val
//   ...                                 // borrow scope
//   end_borrow %begin                   // scope-ending use
//
// Other kinds of BeginBorrowValues, however, like block arguments and
// `load_borrow`, are not borrowing instructions. BeginBorrowValues
// are typically checked during a use-def walk. Here, walking up from
// `%forward` finds `%begin` as the introducer of its guaranteed
// lifetime:
//
//   %begin = load_borrow %addr          // BeginBorrowValue
//   %forward = struct (%begin)          // forwards a guaranteed value
//   ...
//   end_borrow %begin                   // scope-ending use
//  
// Every guaranteed OSSA value has a set of borrow introducers, each
// of which dominates the value and introduces a borrow scope that
// encloses all forwarded uses of the guaranteed value.
//
//   %1 = begin_borrow %0                // borrow introducer for %2
//   %2 = begin_borrow %1                // borrow introducer for %3
//   %3 = struct (%1, %2)                // forwards two guaranteed values
//   ... all forwarded uses of %3
//   end_borrow %1                       // scope-ending use
//   end_borrow %2                       // scope-ending use
//
// Inner borrow scopes may be nested in outer borrow scopes:
//
//   %1 = begin_borrow %0                // borrow introducer for %2
//   %2 = begin_borrow %1                // borrow introducer for %3
//   %3 = struct (%2)
//   ... all forwarded uses of %3
//   end_borrow %2                       // scope-ending use of %2
//   end_borrow %1                       // scope-ending use of %1
//
// Walking up the nested OSSA lifetimes requires iteratively querying
// "enclosing values" until either a guaranteed function argument or
// owned value is reached. Like a borrow introducer, an enclosing
// value dominates all values that it encloses.
//
//                                Borrow Introducer    Enclosing Value
//                                ~~~~~~~~~~~~~~~~~    ~~~~~~~~~~~~~~~
//   %0 = some owned value        invalid              none
//   %1 = begin_borrow %0         %1                   %0
//   %2 = begin_borrow %1         %2                   %1
//   %3 = struct (%2)             %2                   %2
//
// The borrow introducer of a guaranteed phi is not directly
// determined by a use-def walk because an introducer must dominate
// all uses in its scope:
//
//                                Borrow Introducer    Enclosing Value
//                                ~~~~~~~~~~~~~~~~~    ~~~~~~~~~~~~~~~
//                                
//     cond_br ..., bb1, bb2      
//   bb1:                         
//     %2 = begin_borrow %0       %2                   %0
//     %3 = struct (%2)           %2                   %2
//     br bb3(%2, %3)             
//   bb2:                         
//     %6 = begin_borrow %0       %6                   %0
//     %7 = struct (%6)           %6                   %6
//     br bb3(%6, %7)             
//   bb3(%reborrow: @reborrow,    %reborrow            %0
//       %phi: @guaranteed):      %phi                 %reborrow
//  
// `%reborrow` is an outer-adjacent phi to `%phi` because it encloses
// `%phi`. `%phi` is an inner-adjacent phi to `%reborrow` because its
// uses keep `%reborrow` alive. An outer-adjacent phi is either an
// owned value or a reborrow. An inner-adjacent phi is either a
// reborrow or a guaranteed forwarding phi. Here is an example of an
// owned outer-adjacent phi with an inner-adjacent reborrow:
// 
//                                Borrow Introducer    Enclosing Value
//                                ~~~~~~~~~~~~~~~~~    ~~~~~~~~~~~~~~~
//                                
//     cond_br ..., bb1, bb2      
//   bb1:                         
//     %1 = owned value           
//     %2 = begin_borrow %1       %2                   %1
//     br bb3(%1, %2)             
//   bb2:                         
//     %5 = owned value           
//     %6 = begin_borrow %5       %6                   %5
//     br bb3(%5, %6)             
//   bb3(%phi: @owned,            invalid              none
//       %reborrow: @reborrow):   %reborrow            %phi
//  
// In OSSA, each owned value defines a separate lifetime. It is
// consumed on all paths by a direct use. Owned lifetimes can,
// however, be nested within a borrow scope. In this case, finding the
// scope-ending uses requires traversing owned forwarding
// instructions:
//
//   %1 = partial_apply %f(%0) // borrowing instruction borrows %0 and produces
//                             // an owned closure value.
//   %2 = struct (%1)          // end owned lifetime %1, begin owned lifetime %2
//   destroy_value %2          // end owned lifetime %2, scope-ending use of %1
//
//
// TODO: These utilities should be integrated with OSSA SIL verification and
// guaranteed to be compelete (produce known results for all legal SIL
// patterns).
// ===----------------------------------------------------------------------===//

import SIL

/// A scoped instruction that borrows one or more operands.
///
/// If this instruction produces a borrowed value, then
/// BeginBorrowValue(resultOf: self) != nil.
///
/// This does not include instructions like `apply` and `try_apply` that
/// instantaneously borrow a value from the caller.
///
/// This does not include `load_borrow` because it borrows a memory
/// location, not the value of its operand.
///
/// Note: This must handle all instructions with a .borrow operand ownership.
///
/// Note: mark_dependence is a BorrowingInstruction because it creates
/// a borrow scope for its base operand. Its result, however, is not a
/// BeginBorrowValue. It is instead a ForwardingInstruction relative
/// to its value operand.
///
/// TODO: replace BorrowIntroducingInstruction
///
/// TODO: Add non-escaping MarkDependence.
enum BorrowingInstruction : CustomStringConvertible, Hashable {
  case beginBorrow(BeginBorrowInst)
  case storeBorrow(StoreBorrowInst)
  case beginApply(BeginApplyInst)
  case partialApply(PartialApplyInst)
  case markDependence(MarkDependenceInst)
  case startAsyncLet(BuiltinInst)

  init?(_ inst: Instruction) {
    switch inst {
    case let bbi as BeginBorrowInst:
      self = .beginBorrow(bbi)
    case let sbi as StoreBorrowInst:
      self = .storeBorrow(sbi)
    case let bai as BeginApplyInst:
      self = .beginApply(bai)
    case let pai as PartialApplyInst where !pai.mayEscape:
      self = .partialApply(pai)
    case let mdi as MarkDependenceInst:
      self = .markDependence(mdi)
    case let bi as BuiltinInst
           where bi.id == .StartAsyncLetWithLocalBuffer:
      self = .startAsyncLet(bi)
    default:
      return nil
    }
  }
  
  var instruction: Instruction {
    switch self {
    case .beginBorrow(let bbi):
      return bbi
    case .storeBorrow(let sbi):
      return sbi
    case .beginApply(let bai):
      return bai
    case .partialApply(let pai):
      return pai
    case .markDependence(let mdi):
      return mdi
    case .startAsyncLet(let bi):
      return bi
    }
  }

  /// Visit the operands that end the local borrow scope.
  ///
  /// Note: When this instruction's result is BeginBorrowValue the
  /// scopeEndingOperand may include reborrows. To find all uses that
  /// contribute to liveness, the caller needs to determine whether an
  /// incoming value dominates or is consumed by an outer adjacent
  /// phi. See InteriorLiveness.
  ///
  /// FIXME: To generate conservatively correct liveness, this should return
  /// .abortWalk if this is a mark_dependence and the scope-ending use is not
  /// the last in the function (e.g. a store rather than a destroy or return).
  /// The client needs to use LifetimeDependenceDefUseWalker to do better.
  ///
  /// TODO: to hande reborrow-extended uses, migrate ExtendedLiveness
  /// to SwiftCompilerSources.
  ///
  /// TODO: Handle .partialApply and .markDependence forwarded uses
  /// that are phi operands. Currently, partial_apply [on_stack]
  /// and mark_dependence [nonescaping] cannot be cloned, so walking
  /// through the phi safely returns dominated scope-ending operands.
  /// Instead, this could report the phi as a scope-ending use, and
  /// the client could decide whether to walk through them or to
  /// construct reborrow-extended liveness.
  ///
  /// TODO: For instructions that are not a BeginBorrowValue, verify
  /// that scope ending instructions exist on all paths. These
  /// instructions should be complete after SILGen and never cloned to
  /// produce phis.
  func visitScopeEndingOperands(_ context: Context,
                                visitor: @escaping (Operand) -> WalkResult)
  -> WalkResult {
    switch self {
    case .beginBorrow, .storeBorrow:
      let svi = instruction as! SingleValueInstruction
      return svi.uses.filterUsers(ofType: EndBorrowInst.self).walk {
        visitor($0)
      }
    case .beginApply(let bai):
      return bai.token.uses.walk { return visitor($0) }
    case .partialApply, .markDependence:
      let svi = instruction as! SingleValueInstruction
      assert(svi.ownership == .owned)
      return visitForwardedUses(introducer: svi, context) {
        switch $0 {
        case let .operand(operand):
          if operand.endsLifetime {
            return visitor(operand)
          }
          return .continueWalk
        case let .deadValue(_, operand):
          if let operand = operand {
            assert(!operand.endsLifetime,
                   "a dead forwarding instruction cannot end a lifetime")
          }
          return .continueWalk
        }
      }
    case .startAsyncLet(let builtin):
      return builtin.uses.walk {
        if let builtinUser = $0.instruction as? BuiltinInst,
          builtinUser.id == .EndAsyncLetLifetime {
          return visitor($0)
        }
        return .continueWalk
      }
    }
  }

  var description: String { instruction.description }
}

/// A value that introduces a borrow scope:
/// begin_borrow, load_borrow, reborrow, guaranteed function argument.
///
/// If the value introduces a local scope, then that scope is
/// terminated by scope ending operands. Function arguments do not
/// introduce a local scope because the caller owns the scope.
///
/// If the value is a begin_apply result, then it may be the token or
/// one of the yielded values. In any case, the scope ending operands
/// are on the end_apply or abort_apply intructions that use the
/// token.
///
/// Note: equivalent to C++ BorrowedValue, but also handles begin_apply.
enum BeginBorrowValue {
  case beginBorrow(BeginBorrowInst)
  case loadBorrow(LoadBorrowInst)
  case beginApply(Value)
  case functionArgument(FunctionArgument)
  case reborrow(Phi)

  init?(_ value: Value) {
    switch value {
    case let bbi as BeginBorrowInst: self = .beginBorrow(bbi)
    case let lbi as LoadBorrowInst: self = .loadBorrow(lbi)
    case let arg as FunctionArgument where arg.ownership == .guaranteed:
      self = .functionArgument(arg)
    case let arg as Argument where arg.isReborrow:
      self = .reborrow(Phi(arg)!)
    default:
      if value.definingInstruction is BeginApplyInst {
        self = .beginApply(value)
        break
      }
      return nil
    }
  }
  
  var value: Value {
    switch self {
    case .beginBorrow(let bbi): return bbi
    case .loadBorrow(let lbi): return lbi
    case .beginApply(let v): return v
    case .functionArgument(let arg): return arg
    case .reborrow(let phi): return phi.value
    }
  }

  init?(using operand: Operand) {
    switch operand.instruction {
    case is BeginBorrowInst, is LoadBorrowInst:
      let inst = operand.instruction as! SingleValueInstruction
      self = BeginBorrowValue(inst)!
    case is BranchInst:
      guard let phi = Phi(using: operand) else {
        return nil
      }
      guard phi.isReborrow else {
        return nil
      }
      self = .reborrow(phi)
    default:
      return nil
    }
  }

  init?(resultOf borrowInstruction: BorrowingInstruction) {
    switch borrowInstruction {
    case let .beginBorrow(beginBorrow):
      self = BeginBorrowValue(beginBorrow)!
    case let .beginApply(beginApply):
      self = BeginBorrowValue(beginApply.token)!
    case .storeBorrow, .partialApply, .markDependence, .startAsyncLet:
      return nil
    }
  }

  var hasLocalScope: Bool {
    switch self {
    case .beginBorrow, .loadBorrow, .beginApply, .reborrow:
      return true
    case .functionArgument:
      return false
    }
  }

  // Return the value borrowed by begin_borrow or address borrowed by
  // load_borrow.
  //
  // Return nil for begin_apply and reborrow, which need special handling.
  var baseOperand: Operand? {
    switch self {
    case let .beginBorrow(beginBorrow):
    return beginBorrow.operand
    case let .loadBorrow(loadBorrow):
      return loadBorrow.operand
    case .beginApply, .functionArgument, .reborrow:
      return nil
    }
  }

  /// The EndBorrows, reborrows (phis), and consumes (of closures)
  /// that end the local borrow scope. Empty if hasLocalScope is false.
  var scopeEndingOperands: LazyFilterSequence<UseList> {
    switch self {
    case let .beginApply(value):
      return (value.definingInstruction
                as! BeginApplyInst).token.uses.endingLifetime
    case let .reborrow(phi):
      return phi.value.lookThroughBorrowedFromUser.uses.endingLifetime
    default:
      return value.uses.endingLifetime
    }
  }
}

/// Compute the live range for the borrow scopes of a guaranteed value. This returns a separate instruction range for
/// each of the value's borrow introducers.
///
/// TODO: This should return a single multiply-defined instruction range.
func computeBorrowLiveRange(for value: Value, _ context: FunctionPassContext)
  -> SingleInlineArray<(BeginBorrowValue, InstructionRange)> {
  assert(value.ownership == .guaranteed)

  var ranges = SingleInlineArray<(BeginBorrowValue, InstructionRange)>()
  // If introducers is empty, then the dependence is on a trivial value, so
  // there is no ownership range.
  for beginBorrow in value.getBorrowIntroducers(context) {
    /// FIXME: Remove calls to computeKnownLiveness() as soon as lifetime completion runs immediately after
    /// SILGen. Instead, this should compute linear liveness for borrowed value by switching over BeginBorrowValue, just
    /// like LifetimeDependenc.Scope.computeRange().
    ranges.push((beginBorrow, computeKnownLiveness(for: beginBorrow.value, context)))
  }
  return ranges
}

extension Value {
  var lookThroughBorrowedFrom: Value {
    if let bfi = self as? BorrowedFromInst {
      return bfi.borrowedValue.lookThroughBorrowedFrom
    }
    return self
  }
}

struct BorrowIntroducers<Ctxt: Context> : CollectionLikeSequence {
  let initialValue: Value
  let context: Ctxt

  func makeIterator() -> EnclosingValueIterator {
    EnclosingValueIterator(forBorrowIntroducers: initialValue, context)
  }
}

struct EnclosingValues<Ctxt: Context> : CollectionLikeSequence {
  let initialValue: Value
  let context: Ctxt

  func makeIterator() -> EnclosingValueIterator {
    EnclosingValueIterator(forEnclosingValues: initialValue, context)
  }
}

// This iterator must be a class because we need a deinit.
// It shouldn't be a performance problem because the optimizer should always be able to stack promote the iterator.
// TODO: Make it a struct once this is possible with non-copyable types.
final class EnclosingValueIterator : IteratorProtocol {
  var worklist: ValueWorklist

  init(forBorrowIntroducers value: Value, _ context: some Context) {
    self.worklist = ValueWorklist(context)
    self.worklist.pushIfNotVisited(value)
  }

  init(forEnclosingValues value: Value, _ context: some Context) {
    self.worklist = ValueWorklist(context)
    if value is Undef || value.ownership != .guaranteed {
      return
    }
    if let beginBorrow = BeginBorrowValue(value.lookThroughBorrowedFrom) {
      switch beginBorrow {
      case let .beginBorrow(bbi):
        // Gather the outer enclosing borrow scope.
        worklist.pushIfNotVisited(bbi.borrowedValue)
      case .loadBorrow, .beginApply, .functionArgument:
        // There is no enclosing value on this path.
        break
      case .reborrow(let phi):
        worklist.pushIfNotVisited(contentsOf: phi.borrowedFrom!.enclosingValues)
      }
    } else {
      // Handle forwarded guaranteed values.
      worklist.pushIfNotVisited(value)
    }
  }

  deinit {
    worklist.deinitialize()
  }

  func next() -> Value? {
    while let value = worklist.pop() {
      switch value.ownership {
      case .none, .unowned:
        break

      case .owned:
        return value

      case .guaranteed:
        if BeginBorrowValue(value) != nil {
          return value
        } else if let bfi = value as? BorrowedFromInst {
          if bfi.borrowedPhi.isReborrow {
            worklist.pushIfNotVisited(bfi.borrowedValue)
          } else {
            worklist.pushIfNotVisited(contentsOf: bfi.enclosingValues)
          }
        } else if let forwardingInst = value.forwardingInstruction {
          // Recurse through guaranteed forwarding non-phi instructions.
          let ops = forwardingInst.forwardedOperands
          worklist.pushIfNotVisited(contentsOf: ops.lazy.map { $0.value })
        } else {
          fatalError("cannot get borrow introducers for unknown guaranteed value")
        }
      }
    }
    return nil
  }
}


extension Value {
  /// Get the borrow introducers for this value. This gives you a set of
  /// OSSA lifetimes that directly include this value. If this value is owned,
  /// or introduces a borrow scope, then this value is the single introducer for itself.
  ///
  /// If this value is an address or any trivial type, then it has no introducers.
  ///
  /// Example:                                       // introducers:
  ///                                                // ~~~~~~~~~~~~
  ///   bb0(%0 : @owned $Class,                      // %0
  ///       %1 : @guaranteed $Class):                // %1
  ///     %borrow0 = begin_borrow %0                 // %borrow0
  ///     %pair = struct $Pair(%borrow0, %1)         // %borrow0, %1
  ///     %first = struct_extract %pair              // %borrow0, %1
  ///     %field = ref_element_addr %first           // (none)
  ///     %load = load_borrow %field : $*C           // %load
  ///
  func getBorrowIntroducers<Ctxt: Context>(_ context: Ctxt) -> LazyMapSequence<BorrowIntroducers<Ctxt>, BeginBorrowValue> {
    BorrowIntroducers(initialValue: self, context: context).lazy.map { BeginBorrowValue($0)! }
  }

  /// Get "enclosing values" whose OSSA lifetime immediately encloses a guaranteed value.
  ///
  /// The guaranteed value being enclosed effectively keeps these enclosing values alive.
  /// This lets you walk up the levels of nested OSSA lifetimes to determine all the
  /// lifetimes that are kept alive by a given SILValue. In particular, it discovers "outer-adjacent phis":
  /// phis that are kept alive by uses of another phi in the same block.
  ///
  /// If this value is a forwarded guaranteed value, then this finds the
  /// introducers of the current borrow scope, which is never an empty set.
  ///
  /// If this value introduces a borrow scope, then this finds the introducers of the outer
  /// enclosing borrow scope that contains this inner scope.
  ///
  /// If this value is a `begin_borrow`, then this function returns its operand.
  ///
  /// If this value is an owned value, a function argument, or a load_borrow, then this is an empty set.
  ///
  /// If this value is a reborrow, then this either returns a dominating enclosing value or an outer adjacent phi.
  ///
  /// Example:                                       // enclosing value:
  ///                                                // ~~~~~~~~~~~~
  ///   bb0(%0 : @owned $Class,                      // (none)
  ///       %1 : @guaranteed $Class):                // (none)
  ///     %borrow0 = begin_borrow %0                 // %0
  ///     %pair = struct $Pair(%borrow0, %1)         // %borrow0, %1
  ///     %first = struct_extract %pair              // %borrow0, %1
  ///     %field = ref_element_addr %first           // (none)
  ///     %load = load_borrow %field : $*C           // %load
  ///
  /// Example:                                       // enclosing value:
  ///                                                // ~~~~~~~~~~~~
  ///     %outerBorrow = begin_borrow %0             // %0
  ///     %innerBorrow = begin_borrow %outerBorrow   // %outerBorrow
  ///     br bb1(%outerBorrow, %innerBorrow)
  ///   bb1(%outerReborrow : @reborrow,              // %0
  ///       %innerReborrow : @reborrow)              // %outerReborrow
  ///
  func getEnclosingValues<Ctxt: Context>(_ context: Ctxt) -> EnclosingValues<Ctxt> {
    EnclosingValues(initialValue: self, context: context)
  }
}

extension Phi {
  /// The inner adjacent phis of this outer "enclosing" phi.
  /// These keep the enclosing (outer adjacent) phi alive.
  var innerAdjacentPhis: LazyMapSequence<LazyFilterSequence<LazyMapSequence<UseList, Phi?>>, Phi> {
    value.uses.lazy.compactMap { use in
      if let bfi = use.instruction as? BorrowedFromInst,
         use.index != 0
      {
         return Phi(bfi.borrowedValue)
      }
      return nil
    }
  }
}

/// Gathers enclosing values by visiting predecessor blocks.
/// Only used for updating borrowed-from instructions and for verification.
func gatherEnclosingValuesFromPredecessors(
  for phi: Phi,
  in enclosingValues: inout Stack<Value>,
  _ context: some Context
) {
  var alreadyAdded = ValueSet(context)
  defer { alreadyAdded.deinitialize() }

  for predecessor in phi.predecessors {
    let incomingOperand = phi.incomingOperand(inPredecessor: predecessor)

    for predEV in incomingOperand.value.getEnclosingValues(context) {
      let ev = predecessor.mapToPhiInSuccessor(incomingEnclosingValue: predEV)
      if alreadyAdded.insert(ev) {
        enclosingValues.push(ev)
      }
    }
  }
}

extension BasicBlock {
  func mapToPhiInSuccessor(incomingEnclosingValue: Value) -> Value {
    let branch = terminator as! BranchInst
    if let incomingEV = branch.operands.first(where: { $0.value.lookThroughBorrowedFrom == incomingEnclosingValue }) {
      return branch.getArgument(for: incomingEV)
    }
    // No candidates phi are outer-adjacent phis. The incoming
    // `predDef` must dominate the current guaranteed phi.
    return incomingEnclosingValue
  }
}

let borrowIntroducersTest = FunctionTest("borrow_introducers") {
  function, arguments, context in
  let value = arguments.takeValue()
  print(function)
  print("Borrow introducers for: \(value)")
  for bi in value.lookThroughBorrowedFromUser.getBorrowIntroducers(context) {
    print(bi)
  }
}

let enclosingValuesTest = FunctionTest("enclosing_values") {
  function, arguments, context in
  let value = arguments.takeValue()
  print(function)
  print("Enclosing values for: \(value)")
  var enclosing = Stack<Value>(context)
  defer {
    enclosing.deinitialize()
  }
  for ev in value.lookThroughBorrowedFromUser.getEnclosingValues(context) {
    print(ev)
  }
}

extension Value {
  var lookThroughBorrowedFromUser: Value {
    for use in uses {
      if let bfi = use.forwardingBorrowedFromUser {
        return bfi
      }
    }
    return self
  }
}

