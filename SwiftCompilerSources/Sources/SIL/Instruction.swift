//===--- Instruction.swift - Defines the Instruction classes --------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import Basic
import SILBridging

//===----------------------------------------------------------------------===//
//                       Instruction base classes
//===----------------------------------------------------------------------===//

@_semantics("arc.immortal")
public class Instruction : CustomStringConvertible, Hashable {
  final public var next: Instruction? {
    bridged.getNext().instruction
  }

  final public var previous: Instruction? {
    bridged.getPrevious().instruction
  }

  final public var parentBlock: BasicBlock {
    bridged.getParent().block
  }

  final public var parentFunction: Function { parentBlock.parentFunction }

  final public var description: String {
    let stdString = bridged.getDebugDescription()
    return String(_cxxString: stdString)
  }

  final public var isDeleted: Bool {
    return bridged.isDeleted()
  }

  final public var operands: OperandArray {
    let operands = bridged.getOperands()
    return OperandArray(base: operands.base, count: operands.count)
  }

  fileprivate var resultCount: Int { 0 }
  fileprivate func getResult(index: Int) -> Value { fatalError() }

  public struct Results : RandomAccessCollection {
    fileprivate let inst: Instruction
    fileprivate let numResults: Int

    public var startIndex: Int { 0 }
    public var endIndex: Int { numResults }
    public subscript(_ index: Int) -> Value { inst.getResult(index: index) }
  }

  final public var results: Results {
    Results(inst: self, numResults: resultCount)
  }

  final public var location: Location {
    return Location(bridged: bridged.getLocation())
  }

  public var mayTrap: Bool { false }

  final public var mayHaveSideEffects: Bool {
    return mayTrap || mayWriteToMemory
  }

  final public var memoryEffects: SideEffects.Memory {
    switch bridged.getMemBehavior() {
    case .None:
      return SideEffects.Memory()
    case .MayRead:
      return SideEffects.Memory(read: true)
    case .MayWrite:
      return SideEffects.Memory(write: true)
    case .MayReadWrite, .MayHaveSideEffects:
      return SideEffects.Memory(read: true, write: true)
    default:
      fatalError("invalid memory behavior")
    }
  }

  final public var mayReadFromMemory: Bool { memoryEffects.read }
  final public var mayWriteToMemory: Bool { memoryEffects.write }
  final public var mayReadOrWriteMemory: Bool { memoryEffects.read || memoryEffects.write }

  public final var mayRelease: Bool {
    return bridged.mayRelease()
  }

  public final var hasUnspecifiedSideEffects: Bool {
    return bridged.mayHaveSideEffects()
  }

  public final var mayAccessPointer: Bool {
    return bridged.mayAccessPointer()
  }

  public final var mayLoadWeakOrUnowned: Bool {
    return bridged.mayLoadWeakOrUnowned()
  }

  public final var maySynchronizeNotConsideringSideEffects: Bool {
    return bridged.maySynchronizeNotConsideringSideEffects()
  }

  public final var mayBeDeinitBarrierNotConsideringSideEffects: Bool {
    return bridged.mayBeDeinitBarrierNotConsideringSideEffects()
  }

  public func visitReferencedFunctions(_ cl: (Function) -> ()) {
  }

  public static func ==(lhs: Instruction, rhs: Instruction) -> Bool {
    lhs === rhs
  }

  public func hash(into hasher: inout Hasher) {
    hasher.combine(ObjectIdentifier(self))
  }

  public var bridged: BridgedInstruction {
    BridgedInstruction(SwiftObject(self))
  }
}

extension BridgedInstruction {
  public var instruction: Instruction { obj.getAs(Instruction.self) }
  public func getAs<T: Instruction>(_ instType: T.Type) -> T { obj.getAs(T.self) }
  public var optional: OptionalBridgedInstruction {
    OptionalBridgedInstruction(self.obj)
  }
}

extension OptionalBridgedInstruction {
  public var instruction: Instruction? { obj.getAs(Instruction.self) }

  public static var none: OptionalBridgedInstruction {
    OptionalBridgedInstruction()
  }
}

extension Optional where Wrapped == Instruction {
  public var bridged: OptionalBridgedInstruction {
    OptionalBridgedInstruction(self?.bridged.obj)
  }
}

public class SingleValueInstruction : Instruction, Value {
  final public var definingInstruction: Instruction? { self }

  fileprivate final override var resultCount: Int { 1 }
  fileprivate final override func getResult(index: Int) -> Value { self }

  public static func ==(lhs: SingleValueInstruction, rhs: SingleValueInstruction) -> Bool {
    lhs === rhs
  }
}

public final class MultipleValueInstructionResult : Value, Hashable {
  public var parentInstruction: MultipleValueInstruction {
    bridged.getParent().getAs(MultipleValueInstruction.self)
  }

  public var definingInstruction: Instruction? { parentInstruction }

  public var parentBlock: BasicBlock { parentInstruction.parentBlock }

  public var index: Int { bridged.getIndex() }

  var bridged: BridgedMultiValueResult {
    BridgedMultiValueResult(obj: SwiftObject(self))
  }

  public static func ==(lhs: MultipleValueInstructionResult, rhs: MultipleValueInstructionResult) -> Bool {
    lhs === rhs
  }

  public func hash(into hasher: inout Hasher) {
    hasher.combine(ObjectIdentifier(self))
  }
}

extension BridgedMultiValueResult {
  var result: MultipleValueInstructionResult {
    obj.getAs(MultipleValueInstructionResult.self)
  }
}

public class MultipleValueInstruction : Instruction {
  fileprivate final override var resultCount: Int {
    bridged.MultipleValueInstruction_getNumResults()
  }
  fileprivate final override func getResult(index: Int) -> Value {
    bridged.MultipleValueInstruction_getResult(index).result
  }
}

/// Instructions, which have a single operand.
public protocol UnaryInstruction : Instruction {
  var operands: OperandArray { get }
  var operand: Operand { get }
}

extension UnaryInstruction {
  public var operand: Operand { operands[0] }
}

//===----------------------------------------------------------------------===//
//                             no-value instructions
//===----------------------------------------------------------------------===//

/// Used for all non-value instructions which are not implemented here, yet.
/// See registerBridgedClass() in SILBridgingUtils.cpp.
final public class UnimplementedInstruction : Instruction {
}

public protocol StoringInstruction : AnyObject {
  var operands: OperandArray { get }
}

extension StoringInstruction {
  public var sourceOperand: Operand { return operands[0] }
  public var destinationOperand: Operand { return operands[1] }
  public var source: Value { return sourceOperand.value }
  public var destination: Value { return destinationOperand.value }
}

final public class StoreInst : Instruction, StoringInstruction {
  // must match with enum class StoreOwnershipQualifier
  public enum StoreOwnership: Int {
    case unqualified = 0, initialize = 1, assign = 2, trivial = 3

    public init(for type: Type, in function: Function, initialize: Bool) {
      if function.hasOwnership {
        if type.isTrivial(in: function) {
          self = .trivial
        } else {
          self = initialize ? .initialize : .assign
        }
      } else {
        self = .unqualified
      }
    }
  }
  public var storeOwnership: StoreOwnership {
    StoreOwnership(rawValue: bridged.StoreInst_getStoreOwnership())!
  }
}

final public class StoreWeakInst : Instruction, StoringInstruction { }
final public class StoreUnownedInst : Instruction, StoringInstruction { }

final public class CopyAddrInst : Instruction {
  public var sourceOperand: Operand { return operands[0] }
  public var destinationOperand: Operand { return operands[1] }
  public var source: Value { return sourceOperand.value }
  public var destination: Value { return destinationOperand.value }
  
  public var isTakeOfSrc: Bool {
    bridged.CopyAddrInst_isTakeOfSrc()
  }
  public var isInitializationOfDest: Bool {
    bridged.CopyAddrInst_isInitializationOfDest()
  }
}

final public class EndAccessInst : Instruction, UnaryInstruction {
  public var beginAccess: BeginAccessInst {
    return operand.value as! BeginAccessInst
  }
}

final public class EndBorrowInst : Instruction, UnaryInstruction {}

final public class MarkUninitializedInst : SingleValueInstruction, UnaryInstruction {
}

final public class CondFailInst : Instruction, UnaryInstruction {
  public var condition: Value { operand.value }
  public override var mayTrap: Bool { true }

  public var message: String { bridged.CondFailInst_getMessage().string }
}

final public class FixLifetimeInst : Instruction, UnaryInstruction {}

final public class DebugValueInst : Instruction, UnaryInstruction {}

final public class DebugStepInst : Instruction {}

final public class UnconditionalCheckedCastAddrInst : Instruction {
  public override var mayTrap: Bool { true }
}

final public class EndApplyInst : Instruction, UnaryInstruction {}
final public class AbortApplyInst : Instruction, UnaryInstruction {}

final public class SetDeallocatingInst : Instruction, UnaryInstruction {}

public class RefCountingInst : Instruction, UnaryInstruction {
  public var isAtomic: Bool { bridged.RefCountingInst_getIsAtomic() }
}

final public class StrongRetainInst : RefCountingInst {
  public var instance: Value { operand.value }
}

final public class UnownedRetainInst : RefCountingInst {
  public var instance: Value { operand.value }
}

final public class RetainValueInst : RefCountingInst {
  public var value: Value { return operand.value }
}

final public class StrongReleaseInst : RefCountingInst {
  public var instance: Value { operand.value }
}

final public class UnownedReleaseInst : RefCountingInst {
  public var instance: Value { operand.value }
}

final public class ReleaseValueInst : RefCountingInst {
  public var value: Value { return operand.value }
}

final public class DestroyValueInst : Instruction, UnaryInstruction {
  public var destroyedValue: Value { operand.value }
}

final public class DestroyAddrInst : Instruction, UnaryInstruction {
  public var destroyedAddress: Value { operand.value }
}

final public class InjectEnumAddrInst : Instruction, UnaryInstruction, EnumInstruction {
  public var `enum`: Value { operand.value }
  public var caseIndex: Int { bridged.InjectEnumAddrInst_caseIndex() }
}

final public class UnimplementedRefCountingInst : RefCountingInst {}

//===----------------------------------------------------------------------===//
//                      no-value deallocation instructions
//===----------------------------------------------------------------------===//

public protocol Deallocation : Instruction {
  var allocatedValue: Value { get }
}

extension Deallocation {
  public var allocatedValue: Value { operands[0].value }
}


final public class DeallocStackInst : Instruction, UnaryInstruction, Deallocation {
  public var allocstack: AllocStackInst {
    return operand.value as! AllocStackInst
  }
}

final public class DeallocPackInst : Instruction, UnaryInstruction, Deallocation {}

final public class DeallocStackRefInst : Instruction, UnaryInstruction, Deallocation {
  public var allocRef: AllocRefInstBase { operand.value as! AllocRefInstBase }
}

final public class DeallocRefInst : Instruction, UnaryInstruction, Deallocation {}

final public class DeallocPartialRefInst : Instruction, Deallocation {}

final public class DeallocBoxInst : Instruction, UnaryInstruction, Deallocation {}

final public class DeallocExistentialBoxInst : Instruction, UnaryInstruction, Deallocation {}

//===----------------------------------------------------------------------===//
//                           single-value instructions
//===----------------------------------------------------------------------===//

/// Used for all SingleValueInstructions which are not implemented here, yet.
/// See registerBridgedClass() in SILBridgingUtils.cpp.
final public class UnimplementedSingleValueInst : SingleValueInstruction {
}

final public class LoadInst : SingleValueInstruction, UnaryInstruction {
  public var address: Value { operand.value }

  // must match with enum class LoadOwnershipQualifier
  public enum LoadOwnership: Int {
    case unqualified = 0, take = 1, copy = 2, trivial = 3
  }
  public var loadOwnership: LoadOwnership {
    LoadOwnership(rawValue: bridged.LoadInst_getLoadOwnership())!
  }
}

final public class LoadWeakInst : SingleValueInstruction, UnaryInstruction {}
final public class LoadUnownedInst : SingleValueInstruction, UnaryInstruction {}
final public class LoadBorrowInst : SingleValueInstruction, UnaryInstruction {}

final public class BuiltinInst : SingleValueInstruction {
  public typealias ID = swift.BuiltinValueKind

  public var id: ID {
    return bridged.BuiltinInst_getID()
  }

  public var intrinsicID: BridgedInstruction.IntrinsicID {
    return bridged.BuiltinInst_getIntrinsicID()
  }

  public var substitutionMap: SubstitutionMap {
    SubstitutionMap(bridged.BuiltinInst_getSubstitutionMap())
  }
}

final public class UpcastInst : SingleValueInstruction, UnaryInstruction {
  public var fromInstance: Value { operand.value }
}

final public
class UncheckedRefCastInst : SingleValueInstruction, UnaryInstruction {
  public var fromInstance: Value { operand.value }
}

final public class UncheckedAddrCastInst : SingleValueInstruction, UnaryInstruction {
  public var fromAddress: Value { operand.value }
}

final public class UncheckedTrivialBitCastInst : SingleValueInstruction, UnaryInstruction {
  public var fromValue: Value { operand.value }
}

final public
class RawPointerToRefInst : SingleValueInstruction, UnaryInstruction {
  public var pointer: Value { operand.value }
}

final public
class AddressToPointerInst : SingleValueInstruction, UnaryInstruction {
  public var address: Value { operand.value }

  public var needsStackProtection: Bool {
    bridged.AddressToPointerInst_needsStackProtection()
  }
}

final public
class PointerToAddressInst : SingleValueInstruction, UnaryInstruction {
  public var pointer: Value { operand.value }
  public var isStrict: Bool { bridged.PointerToAddressInst_isStrict() }
}

final public
class IndexAddrInst : SingleValueInstruction {
  public var base: Value { operands[0].value }
  public var index: Value { operands[1].value }
  
  public var needsStackProtection: Bool {
    bridged.IndexAddrInst_needsStackProtection()
  }
}

final public
class InitExistentialRefInst : SingleValueInstruction, UnaryInstruction {
  public var instance: Value { operand.value }
}

final public
class OpenExistentialRefInst : SingleValueInstruction, UnaryInstruction {
  public var existential: Value { operand.value }
}

final public
class InitExistentialValueInst : SingleValueInstruction, UnaryInstruction {}

final public
class OpenExistentialValueInst : SingleValueInstruction, UnaryInstruction {}

final public
class InitExistentialAddrInst : SingleValueInstruction, UnaryInstruction {}

final public
class OpenExistentialAddrInst : SingleValueInstruction, UnaryInstruction {}

final public
class OpenExistentialBoxInst : SingleValueInstruction, UnaryInstruction {}

final public
class OpenExistentialBoxValueInst : SingleValueInstruction, UnaryInstruction {}

final public
class InitExistentialMetatypeInst : SingleValueInstruction, UnaryInstruction {
  public var metatype: Value { operand.value }
}

final public
class OpenExistentialMetatypeInst : SingleValueInstruction, UnaryInstruction {}

final public class MetatypeInst : SingleValueInstruction {}

final public
class ValueMetatypeInst : SingleValueInstruction, UnaryInstruction {}

final public
class ExistentialMetatypeInst : SingleValueInstruction, UnaryInstruction {}

public class GlobalAccessInst : SingleValueInstruction {
  final public var global: GlobalVariable {
    bridged.GlobalAccessInst_getGlobal().globalVar
  }
}

public class FunctionRefBaseInst : SingleValueInstruction {
  public var referencedFunction: Function {
    bridged.FunctionRefBaseInst_getReferencedFunction().function
  }

  public override func visitReferencedFunctions(_ cl: (Function) -> ()) {
    cl(referencedFunction)
  }
}

final public class FunctionRefInst : FunctionRefBaseInst {
}

final public class DynamicFunctionRefInst : FunctionRefBaseInst {
}

final public class PreviousDynamicFunctionRefInst : FunctionRefBaseInst {
}

final public class GlobalAddrInst : GlobalAccessInst {}

final public class GlobalValueInst : GlobalAccessInst {
  public var isBare: Bool { bridged.GlobalValueInst_isBare() }
}

final public class AllocGlobalInst : Instruction {
  public var global: GlobalVariable {
    bridged.AllocGlobalInst_getGlobal().globalVar
  }
}

final public class IntegerLiteralInst : SingleValueInstruction {
  public var value: llvm.APInt { bridged.IntegerLiteralInst_getValue() }
}

final public class FloatLiteralInst : SingleValueInstruction {
  public var value: llvm.APFloat { bridged.FloatLiteralInst_getValue() }
}

final public class StringLiteralInst : SingleValueInstruction {
  public enum Encoding {
    case Bytes
    case UTF8
    /// UTF-8 encoding of an Objective-C selector.
    case ObjCSelector
  }

  public var value: StringRef { StringRef(bridged: bridged.StringLiteralInst_getValue()) }

  public var encoding: Encoding {
    switch bridged.StringLiteralInst_getEncoding() {
    case 0: return .Bytes
    case 1: return .UTF8
    case 2: return .ObjCSelector
    default: fatalError("invalid encoding in StringLiteralInst")
    }
  }
}

final public class TupleInst : SingleValueInstruction {
}

final public class TupleExtractInst : SingleValueInstruction, UnaryInstruction {
  public var `tuple`: Value { operand.value }
  public var fieldIndex: Int { bridged.TupleExtractInst_fieldIndex() }
}

final public
class TupleElementAddrInst : SingleValueInstruction, UnaryInstruction {
  public var `tuple`: Value { operand.value }
  public var fieldIndex: Int { bridged.TupleElementAddrInst_fieldIndex() }
}

final public class StructInst : SingleValueInstruction {
}

final public class StructExtractInst : SingleValueInstruction, UnaryInstruction {
  public var `struct`: Value { operand.value }
  public var fieldIndex: Int { bridged.StructExtractInst_fieldIndex() }
}

final public
class StructElementAddrInst : SingleValueInstruction, UnaryInstruction {
  public var `struct`: Value { operand.value }
  public var fieldIndex: Int { bridged.StructElementAddrInst_fieldIndex() }
}

public protocol EnumInstruction : AnyObject {
  var caseIndex: Int { get }
}

final public class EnumInst : SingleValueInstruction, EnumInstruction {
  public var caseIndex: Int { bridged.EnumInst_caseIndex() }

  public var operand: Operand? { operands.first }
  public var payload: Value? { operand?.value }
}

final public class UncheckedEnumDataInst : SingleValueInstruction, UnaryInstruction, EnumInstruction {
  public var `enum`: Value { operand.value }
  public var caseIndex: Int { bridged.UncheckedEnumDataInst_caseIndex() }
}

final public class InitEnumDataAddrInst : SingleValueInstruction, UnaryInstruction, EnumInstruction {
  public var `enum`: Value { operand.value }
  public var caseIndex: Int { bridged.InitEnumDataAddrInst_caseIndex() }
}

final public class UncheckedTakeEnumDataAddrInst : SingleValueInstruction, UnaryInstruction, EnumInstruction {
  public var `enum`: Value { operand.value }
  public var caseIndex: Int { bridged.UncheckedTakeEnumDataAddrInst_caseIndex() }
}

final public class RefElementAddrInst : SingleValueInstruction, UnaryInstruction {
  public var instance: Value { operand.value }
  public var fieldIndex: Int { bridged.RefElementAddrInst_fieldIndex() }

  public var fieldIsLet: Bool { bridged.RefElementAddrInst_fieldIsLet() }
}

final public class RefTailAddrInst : SingleValueInstruction, UnaryInstruction {
  public var instance: Value { operand.value }
}

final public class KeyPathInst : SingleValueInstruction {
  public override func visitReferencedFunctions(_ cl: (Function) -> ()) {
    var results = BridgedInstruction.KeyPathFunctionResults()
    for componentIdx in 0..<bridged.KeyPathInst_getNumComponents() {
      bridged.KeyPathInst_getReferencedFunctions(componentIdx, &results)
      let numFuncs = results.numFunctions
      withUnsafePointer(to: &results) {
        $0.withMemoryRebound(to: BridgedFunction.self, capacity: numFuncs) {
          let functions = UnsafeBufferPointer(start: $0, count: numFuncs)
          for idx in 0..<numFuncs {
            cl(functions[idx].function)
          }
        }
      }
    }
  }
}

final public
class UnconditionalCheckedCastInst : SingleValueInstruction, UnaryInstruction {
  public override var mayTrap: Bool { true }
}

final public
class ConvertFunctionInst : SingleValueInstruction, UnaryInstruction {
  public var fromFunction: Value { operand.value }
}

final public
class ThinToThickFunctionInst : SingleValueInstruction, UnaryInstruction {}

final public
class ObjCExistentialMetatypeToObjectInst : SingleValueInstruction,
                                            UnaryInstruction {}

final public
class ObjCMetatypeToObjectInst : SingleValueInstruction, UnaryInstruction {}

final public
class ValueToBridgeObjectInst : SingleValueInstruction, UnaryInstruction {
  public var value: Value { return operand.value }
}

final public
class MarkDependenceInst : SingleValueInstruction {
  public var value: Value { return operands[0].value }
  public var base: Value { return operands[1].value }
}

final public class RefToBridgeObjectInst : SingleValueInstruction,
                                           UnaryInstruction {}

final public class BridgeObjectToRefInst : SingleValueInstruction,
                                           UnaryInstruction {}

final public class BridgeObjectToWordInst : SingleValueInstruction,
                                           UnaryInstruction {}

public typealias AccessKind = swift.SILAccessKind


// TODO: add support for begin_unpaired_access
final public class BeginAccessInst : SingleValueInstruction, UnaryInstruction {
  public var accessKind: AccessKind { bridged.BeginAccessInst_getAccessKind() }

  public var isStatic: Bool { bridged.BeginAccessInst_isStatic() }

  public var address: Value { operand.value }
}

// An instruction that is always paired with a scope ending instruction
// such as `begin_access` (ending with `end_access`) and `alloc_stack`
// (ending with `dealloc_stack`).
public protocol ScopedInstruction {
  // The type of the ending instructions (while `IteratorProtocol` would be
  // ideal, for performance reasons we allow the user to specify any type as return)
  associatedtype EndInstructions

  // The instructions that end the scope of the instruction denoted
  // by `self`.
  var endInstructions: EndInstructions { get }
}

extension BeginAccessInst : ScopedInstruction {
  public typealias EndInstructions = LazyMapSequence<LazyFilterSequence<LazyMapSequence<UseList, EndAccessInst?>>, EndAccessInst>

  public var endInstructions: EndInstructions {
    uses.lazy.compactMap({ $0.instruction as? EndAccessInst })
  }
}

final public class BeginBorrowInst : SingleValueInstruction, UnaryInstruction {
  public var borrowedValue: Value { operand.value }
}

final public class ProjectBoxInst : SingleValueInstruction, UnaryInstruction {
  public var box: Value { operand.value }
  public var fieldIndex: Int { bridged.ProjectBoxInst_fieldIndex() }
}

final public class CopyValueInst : SingleValueInstruction, UnaryInstruction {
  public var fromValue: Value { operand.value }
}

final public class MoveValueInst : SingleValueInstruction, UnaryInstruction {
  public var fromValue: Value { operand.value }
}

final public class DropDeinitInst : SingleValueInstruction, UnaryInstruction {
  public var fromValue: Value { operand.value }
}

final public class StrongCopyUnownedValueInst : SingleValueInstruction, UnaryInstruction {}

final public class StrongCopyUnmanagedValueInst : SingleValueInstruction, UnaryInstruction  {}

final public class EndCOWMutationInst : SingleValueInstruction, UnaryInstruction {
  public var instance: Value { operand.value }
  public var doKeepUnique: Bool { bridged.EndCOWMutationInst_doKeepUnique() }
}

final public
class ClassifyBridgeObjectInst : SingleValueInstruction, UnaryInstruction {}

final public class PartialApplyInst : SingleValueInstruction, ApplySite {
  public var numArguments: Int { bridged.PartialApplyInst_numArguments() }
  public var isOnStack: Bool { bridged.PartialApplyInst_isOnStack() }

  public func calleeArgIndex(callerArgIndex: Int) -> Int {
    bridged.PartialApply_getCalleeArgIndexOfFirstAppliedArg() + callerArgIndex
  }

  public func callerArgIndex(calleeArgIndex: Int) -> Int? {
    let firstIdx = bridged.PartialApply_getCalleeArgIndexOfFirstAppliedArg()
    if calleeArgIndex >= firstIdx {
      let callerIdx = calleeArgIndex - firstIdx
      if callerIdx < numArguments {
        return callerIdx
      }
    }
    return nil
  }
}

final public class ApplyInst : SingleValueInstruction, FullApplySite {
  public var numArguments: Int { bridged.ApplyInst_numArguments() }

  public var singleDirectResult: Value? { self }

  public var isNonThrowing: Bool { bridged.ApplyInst_getNonThrowing() }
  public var isNonAsync: Bool { bridged.ApplyInst_getNonAsync() }

  public typealias SpecializationInfo = UnsafePointer<swift.GenericSpecializationInformation>?

  public var specializationInfo: SpecializationInfo { bridged.ApplyInst_getSpecializationInfo() }
}

final public class ClassMethodInst : SingleValueInstruction, UnaryInstruction {}

final public class SuperMethodInst : SingleValueInstruction, UnaryInstruction {}

final public class ObjCMethodInst : SingleValueInstruction, UnaryInstruction {}

final public class ObjCSuperMethodInst : SingleValueInstruction, UnaryInstruction {}

final public class WitnessMethodInst : SingleValueInstruction {}

final public class IsUniqueInst : SingleValueInstruction, UnaryInstruction {}

final public class IsEscapingClosureInst : SingleValueInstruction, UnaryInstruction {}

final public
class MarkMustCheckInst : SingleValueInstruction, UnaryInstruction {}

final public class ObjectInst : SingleValueInstruction {
  public var baseOperands: OperandArray {
    operands[0..<bridged.ObjectInst_getNumBaseElements()]
  }

  public var tailOperands: OperandArray {
    let ops = operands
    return ops[bridged.ObjectInst_getNumBaseElements()..<ops.endIndex]
  }
}

//===----------------------------------------------------------------------===//
//                      single-value allocation instructions
//===----------------------------------------------------------------------===//

public protocol Allocation : SingleValueInstruction { }

final public class AllocStackInst : SingleValueInstruction, Allocation {
  public var hasDynamicLifetime: Bool { bridged.AllocStackInst_hasDynamicLifetime() }
}

public class AllocRefInstBase : SingleValueInstruction, Allocation {
  final public var isObjC: Bool { bridged.AllocRefInstBase_isObjc() }

  final public var canAllocOnStack: Bool {
    bridged.AllocRefInstBase_canAllocOnStack()
  }

  final public var tailAllocatedCounts: OperandArray {
    let numTailTypes = bridged.AllocRefInstBase_getNumTailTypes()
    return operands[0..<numTailTypes]
  }

  final public var tailAllocatedTypes: TypeArray {
    TypeArray(bridged: bridged.AllocRefInstBase_getTailAllocatedTypes())
  }
}

final public class AllocRefInst : AllocRefInstBase {
  public var isBare: Bool { bridged.AllocRefInst_isBare() }
}

final public class AllocRefDynamicInst : AllocRefInstBase {
  public var isDynamicTypeDeinitAndSizeKnownEquivalentToBaseType: Bool {
    bridged.AllocRefDynamicInst_isDynamicTypeDeinitAndSizeKnownEquivalentToBaseType()
  }
}

final public class AllocBoxInst : SingleValueInstruction, Allocation {
}

final public class AllocExistentialBoxInst : SingleValueInstruction, Allocation {
}

//===----------------------------------------------------------------------===//
//                            multi-value instructions
//===----------------------------------------------------------------------===//

final public class BeginCOWMutationInst : MultipleValueInstruction,
                                          UnaryInstruction {
  public var instance: Value { operand.value }
  public var uniquenessResult: Value { return getResult(index: 0) }
  public var instanceResult: Value { return getResult(index: 1) }
}

final public class DestructureStructInst : MultipleValueInstruction, UnaryInstruction {
  public var `struct`: Value { operand.value }
}

final public class DestructureTupleInst : MultipleValueInstruction, UnaryInstruction {
  public var `tuple`: Value { operand.value }
}

final public class BeginApplyInst : MultipleValueInstruction, FullApplySite {
  public var numArguments: Int { bridged.BeginApplyInst_numArguments() }

  public var singleDirectResult: Value? { nil }
}

//===----------------------------------------------------------------------===//
//                            terminator instructions
//===----------------------------------------------------------------------===//

public class TermInst : Instruction {
  final public var successors: SuccessorArray {
    let succArray = bridged.TermInst_getSuccessors()
    return SuccessorArray(base: succArray.base, count: succArray.count)
  }
  
  public var isFunctionExiting: Bool { false }
}

final public class UnreachableInst : TermInst {
}

final public class ReturnInst : TermInst, UnaryInstruction {
  public var returnedValue: Value { operand.value }
  public override var isFunctionExiting: Bool { true }
}

final public class ThrowInst : TermInst, UnaryInstruction {
  public var thrownValue: Value { operand.value }
  public override var isFunctionExiting: Bool { true }
}

final public class YieldInst : TermInst {
}

final public class UnwindInst : TermInst {
  public override var isFunctionExiting: Bool { true }
}

final public class TryApplyInst : TermInst, FullApplySite {
  public var numArguments: Int { bridged.TryApplyInst_numArguments() }

  public var normalBlock: BasicBlock { successors[0] }
  public var errorBlock: BasicBlock { successors[1] }

  public var singleDirectResult: Value? { normalBlock.arguments[0] }
}

final public class BranchInst : TermInst {
  public var targetBlock: BasicBlock { bridged.BranchInst_getTargetBlock().block }

  /// Returns the target block argument for the cond_br `operand`.
  public func getArgument(for operand: Operand) -> Argument {
    return targetBlock.arguments[operand.index]
  }
}

final public class CondBranchInst : TermInst {
  public var trueBlock: BasicBlock { successors[0] }
  public var falseBlock: BasicBlock { successors[1] }

  public var condition: Value { operands[0].value }

  public var trueOperands: OperandArray { operands[1..<(bridged.CondBranchInst_getNumTrueArgs() &+ 1)] }
  public var falseOperands: OperandArray {
    let ops = operands
    return ops[(bridged.CondBranchInst_getNumTrueArgs() &+ 1)..<ops.count]
  }

  /// Returns the true or false block argument for the cond_br `operand`.
  ///
  /// Return nil if `operand` is the condition itself.
  public func getArgument(for operand: Operand) -> Argument? {
    let opIdx = operand.index
    if opIdx == 0 {
      return nil
    }
    let argIdx = opIdx - 1
    let numTrueArgs = bridged.CondBranchInst_getNumTrueArgs()
    if (0..<numTrueArgs).contains(argIdx) {
      return trueBlock.arguments[argIdx]
    } else {
      return falseBlock.arguments[argIdx - numTrueArgs]
    }
  }
}

final public class SwitchValueInst : TermInst {
}

final public class SwitchEnumInst : TermInst {

  public var enumOp: Value { operands[0].value }

  public struct CaseIndexArray : RandomAccessCollection {
    fileprivate let switchEnum: SwitchEnumInst

    public var startIndex: Int { return 0 }
    public var endIndex: Int { switchEnum.bridged.SwitchEnumInst_getNumCases() }

    public subscript(_ index: Int) -> Int {
      switchEnum.bridged.SwitchEnumInst_getCaseIndex(index)
    }
  }

  var caseIndices: CaseIndexArray { CaseIndexArray(switchEnum: self) }

  var cases: Zip2Sequence<CaseIndexArray, SuccessorArray> {
    zip(caseIndices, successors)
  }

  // This does not handle the special case where the default covers exactly
  // the "missing" case.
  public func getUniqueSuccessor(forCaseIndex: Int) -> BasicBlock? {
    cases.first(where: { $0.0 == forCaseIndex })?.1
  }

  // This does not handle the special case where the default covers exactly
  // the "missing" case.
  public func getUniqueCase(forSuccessor: BasicBlock) -> Int? {
    cases.first(where: { $0.1 == forSuccessor })?.0
  }
}

final public class SwitchEnumAddrInst : TermInst {
}

final public class DynamicMethodBranchInst : TermInst {
}

final public class AwaitAsyncContinuationInst : TermInst, UnaryInstruction {
}

final public class CheckedCastBranchInst : TermInst, UnaryInstruction {
  public var source: Value { operand.value }
  public var successBlock: BasicBlock { bridged.CheckedCastBranch_getSuccessBlock().block }
  public var failureBlock: BasicBlock { bridged.CheckedCastBranch_getFailureBlock().block }
}

final public class CheckedCastAddrBranchInst : TermInst, UnaryInstruction {
}
