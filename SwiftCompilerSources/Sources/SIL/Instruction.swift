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
    return String(taking: bridged.getDebugDescription())
  }

  final public var isDeleted: Bool {
    return bridged.isDeleted()
  }

  final public var isInStaticInitializer: Bool { bridged.isInStaticInitializer() }

  final public var operands: OperandArray {
    let operands = bridged.getOperands()
    return OperandArray(base: operands.base, count: operands.count)
  }

  // All operands defined by the operation. Returns the prefix of `operands` that does not include trailing type dependent operands.
  final public var definedOperands: OperandArray {
    let operands = bridged.getOperands()
    let typeOperands = bridged.getTypeDependentOperands()
    return OperandArray(base: operands.base,
      count: operands.count - typeOperands.count)
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

  public final var maySuspend: Bool {
    return bridged.maySuspend()
  }

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

  public final var maySynchronize: Bool {
    return bridged.maySynchronize()
  }

  public final var mayBeDeinitBarrierNotConsideringSideEffects: Bool {
    return bridged.mayBeDeinitBarrierNotConsideringSideEffects()
  }

  public final var isEndOfScopeMarker: Bool {
    switch self {
    case is EndAccessInst, is EndBorrowInst:
      return true;
    default:
      return false;
    }
  }

  /// Incidental uses are marker instructions that do not propagate
  /// their operand.
  public final var isIncidentalUse: Bool {
    switch self {
    case is DebugValueInst, is FixLifetimeInst, is EndLifetimeInst:
      return true
    default:
      return isEndOfScopeMarker
    }
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
  var operand: Operand { get }
}

extension UnaryInstruction {
  public var operand: Operand { operands[0] }
}

//===----------------------------------------------------------------------===//
//                             no-value instructions
//===----------------------------------------------------------------------===//

/// Only one of the operands may have an address type.
public protocol StoringInstruction : Instruction {
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

final public class AssignInst : Instruction, StoringInstruction {
  // must match with enum class swift::AssignOwnershipQualifier
  public enum AssignOwnership: Int {
    case unknown = 0, reassign = 1, reinitialize = 2, initialize = 3
  }

  public var assignOwnership: AssignOwnership {
    AssignOwnership(rawValue: bridged.AssignInst_getAssignOwnership())!
  }
}

final public class AssignByWrapperInst : Instruction, StoringInstruction {}

final public class AssignOrInitInst : Instruction, StoringInstruction {}

/// Instruction that copy or move from a source to destination address.
public protocol SourceDestAddrInstruction : Instruction {
  var sourceOperand: Operand { get }
  var destinationOperand: Operand { get }
  var isTakeOfSrc: Bool { get }
  var isInitializationOfDest: Bool { get }
}

extension SourceDestAddrInstruction {
  public var sourceOperand: Operand { return operands[0] }
  public var destinationOperand: Operand { return operands[1] }
}

final public class CopyAddrInst : Instruction, SourceDestAddrInstruction {
  public var source: Value { return sourceOperand.value }
  public var destination: Value { return destinationOperand.value }
  
  public var isTakeOfSrc: Bool {
    bridged.CopyAddrInst_isTakeOfSrc()
  }
  public var isInitializationOfDest: Bool {
    bridged.CopyAddrInst_isInitializationOfDest()
  }
}

final public class ExplicitCopyAddrInst : Instruction, SourceDestAddrInstruction {
  public var source: Value { return sourceOperand.value }
  public var destination: Value { return destinationOperand.value }

  public var isTakeOfSrc: Bool {
    bridged.ExplicitCopyAddrInst_isTakeOfSrc()
  }
  public var isInitializationOfDest: Bool {
    bridged.ExplicitCopyAddrInst_isInitializationOfDest()
  }
}

final public class MarkUninitializedInst : SingleValueInstruction, UnaryInstruction {

  /// This enum captures what the mark_uninitialized instruction is designating.
  ///
  // Warning: this enum must be in sync with MarkUninitializedInst::Kind
  public enum Kind: Int {
    /// The start of a normal variable live range.
    case variable = 0

    /// "self" in a struct, enum, or root class.
    case rootSelf = 1

    /// The same as "RootSelf", but in a case where it's not really safe to treat 'self' as root
    /// because the original module might add more stored properties.
    ///
    /// This is only used for Swift 4 compatibility. In Swift 5, cross-module initializers are always delegatingSelf.
    case crossModuleRootSelf = 2

    /// "self" in a derived (non-root) class.
    case derivedSelf = 3

    /// "self" in a derived (non-root) class whose stored properties have already been initialized.
    case derivedSelfOnly = 4

    /// "self" on a struct, enum, or class in a delegating constructor (one that calls self.init).
    case delegatingSelf = 5

    /// "self" in a delegating class initializer where memory has already been allocated.
    case delegatingSelfAllocated = 6

    /// An indirectly returned result which has to be checked for initialization.
    case indirectResult = 7
  }

  public var kind: Kind { Kind(rawValue: bridged.MarkUninitializedInst_getKind())! }
}

final public class CondFailInst : Instruction, UnaryInstruction {
  public var condition: Value { operand.value }
  public override var mayTrap: Bool { true }

  public var message: StringRef { StringRef(bridged: bridged.CondFailInst_getMessage()) }
}

final public class IncrementProfilerCounterInst : Instruction {}

final public class MarkFunctionEscapeInst : Instruction {}

final public class HopToExecutorInst : Instruction, UnaryInstruction {}

final public class FixLifetimeInst : Instruction, UnaryInstruction {}

// VarDecl is a struct wrapper around a C++ VarDecl pointer. This insulates the Swift interface from the C++ interface and avoids the need to fake a Swift class. When the AST exposes VarDecl, then this can be replaced whenever it is convenient.
public struct VarDecl {
  var bridged: BridgedVarDecl
  
  public init?(bridged: BridgedNullableVarDecl) {
    guard let decl = bridged.raw else { return nil }
    self.bridged = BridgedVarDecl(raw: decl)
  }

  public var sourceLoc: SourceLoc? {
    return SourceLoc(bridged: bridged.getSourceLocation())
  }

  public var userFacingName: String { String(bridged.getUserFacingName()) }
}

// See C++ VarDeclCarryingInst
public protocol VarDeclInstruction {
  var varDecl: VarDecl? { get }
}

public protocol DebugVariableInstruction : VarDeclInstruction {
  typealias DebugVariable = OptionalBridgedSILDebugVariable

  var debugVariable: DebugVariable { get }
}

/// A meta instruction is an instruction whose location is not interesting as
/// it is impossible to set a breakpoint on it.
/// That could be because the instruction does not generate code (such as
/// `debug_value`), or because the generated code would be in the prologue
/// (`alloc_stack`).
/// When we are moving code onto an unknown instruction (such as the start of a
/// basic block), we want to ignore any meta instruction that might be there.
public protocol MetaInstruction: Instruction {}

final public class DebugValueInst : Instruction, UnaryInstruction, DebugVariableInstruction, MetaInstruction {
  public var varDecl: VarDecl? {
    VarDecl(bridged: bridged.DebugValue_getDecl())
  }

  public var debugVariable: DebugVariable {
    return bridged.DebugValue_getVarInfo()
  }
}

final public class DebugStepInst : Instruction {}

final public class SpecifyTestInst : Instruction {}

final public class UnconditionalCheckedCastAddrInst : Instruction, SourceDestAddrInstruction {
  public var isTakeOfSrc: Bool { true }
  public var isInitializationOfDest: Bool { true }
  public override var mayTrap: Bool { true }
}

final public class BeginDeallocRefInst : SingleValueInstruction, UnaryInstruction {
  public var reference: Value { operands[0].value }
  public var allocation: AllocRefInstBase { operands[1].value as! AllocRefInstBase }
}

final public class EndInitLetRefInst : SingleValueInstruction, UnaryInstruction {}

final public class BindMemoryInst : SingleValueInstruction {}
final public class RebindMemoryInst : SingleValueInstruction {}

public class RefCountingInst : Instruction, UnaryInstruction {
  public var isAtomic: Bool { bridged.RefCountingInst_getIsAtomic() }
}

final public class StrongRetainInst : RefCountingInst {
  public var instance: Value { operand.value }
}

final public class StrongRetainUnownedInst : RefCountingInst {}

final public class UnownedRetainInst : RefCountingInst {
  public var instance: Value { operand.value }
}

final public class RetainValueInst : RefCountingInst {
  public var value: Value { return operand.value }
}

final public class UnmanagedRetainValueInst : RefCountingInst {
  public var value: Value { return operand.value }
}

final public class RetainValueAddrInst : RefCountingInst {
}

final public class ReleaseValueAddrInst : RefCountingInst {
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

final public class UnmanagedReleaseValueInst : RefCountingInst {
  public var value: Value { return operand.value }
}

final public class AutoreleaseValueInst : RefCountingInst {}
final public class UnmanagedAutoreleaseValueInst : RefCountingInst {}

final public class DestroyValueInst : Instruction, UnaryInstruction {
  public var destroyedValue: Value { operand.value }
}

final public class DestroyAddrInst : Instruction, UnaryInstruction {
  public var destroyedAddress: Value { operand.value }
}

final public class EndLifetimeInst : Instruction, UnaryInstruction {}

final public class ExtendLifetimeInst : Instruction, UnaryInstruction {}

final public class InjectEnumAddrInst : Instruction, UnaryInstruction, EnumInstruction {
  public var `enum`: Value { operand.value }
  public var caseIndex: Int { bridged.InjectEnumAddrInst_caseIndex() }
}

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

public protocol LoadInstruction: SingleValueInstruction, UnaryInstruction {}

extension LoadInstruction {
  public var address: Value { operand.value }
}

final public class LoadInst : SingleValueInstruction, LoadInstruction {
  // must match with enum class LoadOwnershipQualifier
  public enum LoadOwnership: Int {
    case unqualified = 0, take = 1, copy = 2, trivial = 3
  }
  public var loadOwnership: LoadOwnership {
    LoadOwnership(rawValue: bridged.LoadInst_getLoadOwnership())!
  }
}

final public class LoadWeakInst : SingleValueInstruction, LoadInstruction {}
final public class LoadUnownedInst : SingleValueInstruction, LoadInstruction {}

final public class BuiltinInst : SingleValueInstruction {
  public typealias ID = BridgedInstruction.BuiltinValueKind

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

final public
class UncheckedRefCastAddrInst : Instruction, SourceDestAddrInstruction {
  public var isTakeOfSrc: Bool { true }
  public var isInitializationOfDest: Bool { true }
}

final public class UncheckedAddrCastInst : SingleValueInstruction, UnaryInstruction {
  public var fromAddress: Value { operand.value }
}

final public class UncheckedTrivialBitCastInst : SingleValueInstruction, UnaryInstruction {
  public var fromValue: Value { operand.value }
}

final public class UncheckedBitwiseCastInst : SingleValueInstruction, UnaryInstruction {}
final public class UncheckedValueCastInst : SingleValueInstruction, UnaryInstruction {}

final public class RefToRawPointerInst : SingleValueInstruction, UnaryInstruction {}
final public class RefToUnmanagedInst : SingleValueInstruction, UnaryInstruction {}
final public class RefToUnownedInst : SingleValueInstruction, UnaryInstruction {}
final public class UnmanagedToRefInst : SingleValueInstruction, UnaryInstruction {}
final public class UnownedToRefInst : SingleValueInstruction, UnaryInstruction {}

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

final public class IndexRawPointerInst : SingleValueInstruction {}

final public
class TailAddrInst : SingleValueInstruction {
  public var base: Value { operands[0].value }
  public var index: Value { operands[1].value }
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
class DeinitExistentialAddrInst : Instruction {}

final public
class DeinitExistentialValueInst : Instruction {}

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

final public class ObjCProtocolInst : SingleValueInstruction {}

public class GlobalAccessInstruction : SingleValueInstruction {
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

final public class GlobalAddrInst : GlobalAccessInstruction, VarDeclInstruction {
  public var varDecl: VarDecl? {
    VarDecl(bridged: bridged.GlobalAddr_getDecl())
  }

  public var dependencyToken: Value? {
    operands.count == 1 ? operands[0].value : nil
  }
}

final public class GlobalValueInst : GlobalAccessInstruction {
  public var isBare: Bool { bridged.GlobalValueInst_isBare() }
}

final public class BaseAddrForOffsetInst : SingleValueInstruction {}

final public class AllocGlobalInst : Instruction {
  public var global: GlobalVariable {
    bridged.AllocGlobalInst_getGlobal().globalVar
  }
}

final public class IntegerLiteralInst : SingleValueInstruction {
  public var value: Int? {
    let optionalInt = bridged.IntegerLiteralInst_getValue()
    if optionalInt.hasValue {
      return optionalInt.value
    }
    return nil
  }
}

final public class FloatLiteralInst : SingleValueInstruction {
}

final public class StringLiteralInst : SingleValueInstruction {
  public enum Encoding {
    case Bytes
    case UTF8
    /// UTF-8 encoding of an Objective-C selector.
    case ObjCSelector
    case UTF8_OSLOG
  }

  public var value: StringRef { StringRef(bridged: bridged.StringLiteralInst_getValue()) }

  public var encoding: Encoding {
    switch bridged.StringLiteralInst_getEncoding() {
    case 0: return .Bytes
    case 1: return .UTF8
    case 2: return .ObjCSelector
    case 3: return .UTF8_OSLOG
    default: fatalError("invalid encoding in StringLiteralInst")
    }
  }
}

final public class HasSymbolInst : SingleValueInstruction {}

final public class TupleInst : SingleValueInstruction {}

final public class TupleExtractInst : SingleValueInstruction, UnaryInstruction  {
  public var `tuple`: Value { operand.value }
  public var fieldIndex: Int { bridged.TupleExtractInst_fieldIndex() }
}

final public
class TupleElementAddrInst : SingleValueInstruction, UnaryInstruction {
  public var `tuple`: Value { operand.value }
  public var fieldIndex: Int { bridged.TupleElementAddrInst_fieldIndex() }
}

final public class TupleAddrConstructorInst : Instruction {
  public var destinationOperand: Operand { operands[0] }
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

final public class SelectEnumInst : SingleValueInstruction {
  public var enumOperand: Operand { operands[0] }
}

final public class RefElementAddrInst : SingleValueInstruction, UnaryInstruction, VarDeclInstruction {
  public var instance: Value { operand.value }
  public var fieldIndex: Int { bridged.RefElementAddrInst_fieldIndex() }

  public var fieldIsLet: Bool { bridged.RefElementAddrInst_fieldIsLet() }

  public var isImmutable: Bool { bridged.RefElementAddrInst_isImmutable() }
  
  public var varDecl: VarDecl? {
    VarDecl(bridged: bridged.RefElementAddr_getDecl())
  }
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
  public var withoutActuallyEscaping: Bool { bridged.ConvertFunctionInst_withoutActuallyEscaping() }
}

final public
class ThinToThickFunctionInst : SingleValueInstruction, UnaryInstruction {
  public var callee: Value { operand.value }

  public var referencedFunction: Function? {
    if let fri = callee as? FunctionRefInst {
      return fri.referencedFunction
    }
    return nil
  }
}

final public class ThickToObjCMetatypeInst : SingleValueInstruction {}
final public class ObjCToThickMetatypeInst : SingleValueInstruction {}

final public class CopyBlockInst : SingleValueInstruction {}
final public class CopyBlockWithoutEscapingInst : SingleValueInstruction {}

final public
class ConvertEscapeToNoEscapeInst : SingleValueInstruction, UnaryInstruction {
  public var fromFunction: Value { operand.value }
}

final public
class ObjCExistentialMetatypeToObjectInst : SingleValueInstruction {}

final public
class ObjCMetatypeToObjectInst : SingleValueInstruction, UnaryInstruction {}

final public
class ValueToBridgeObjectInst : SingleValueInstruction, UnaryInstruction {
  public var value: Value { return operand.value }
}

final public
class GetAsyncContinuationInst : SingleValueInstruction {}

final public
class GetAsyncContinuationAddrInst : SingleValueInstruction, UnaryInstruction {}

final public class ExtractExecutorInst : SingleValueInstruction {}

final public
class MarkDependenceInst : SingleValueInstruction {
  public enum DependenceKind: Int32 {
    case Unresolved = 0
    case Escaping = 1
    case NonEscaping = 2
  }
  public var valueOperand: Operand { operands[0] }
  public var baseOperand: Operand { operands[1] }
  public var value: Value { return valueOperand.value }
  public var base: Value { return baseOperand.value }
  public var dependenceKind: DependenceKind {
    DependenceKind(rawValue: bridged.MarkDependenceInst_dependenceKind().rawValue)!
  }
  public var isNonEscaping: Bool { dependenceKind == .NonEscaping }
  public var isUnresolved: Bool { dependenceKind == .Unresolved }

  public func resolveToNonEscaping() {
    bridged.MarkDependenceInst_resolveToNonEscaping()
  }
}

final public class RefToBridgeObjectInst : SingleValueInstruction {
  public var convertedOperand: Operand { operands[0] }
  public var maskOperand: Operand { operands[1] }
}

final public class BridgeObjectToRefInst : SingleValueInstruction, UnaryInstruction {}

final public class BridgeObjectToWordInst : SingleValueInstruction, UnaryInstruction {}

final public class BorrowedFromInst : SingleValueInstruction, BorrowIntroducingInstruction {
  public var borrowedValue: Value { operands[0].value }
  public var borrowedPhi: Phi { Phi(borrowedValue)! }
  public var enclosingOperands: OperandArray {
    let ops = operands
    return ops[1..<ops.count]
  }
  public var enclosingValues: LazyMapSequence<LazySequence<OperandArray>.Elements, Value> {
    enclosingOperands.values
  }
}

final public class ProjectBoxInst : SingleValueInstruction, UnaryInstruction {
  public var box: Value { operand.value }
  public var fieldIndex: Int { bridged.ProjectBoxInst_fieldIndex() }
}

final public class ProjectExistentialBoxInst : SingleValueInstruction, UnaryInstruction {}

public protocol CopyingInstruction : SingleValueInstruction, UnaryInstruction, OwnershipTransitionInstruction {}

final public class CopyValueInst : SingleValueInstruction, CopyingInstruction {
  public var fromValue: Value { operand.value }
}

final public class ExplicitCopyValueInst : SingleValueInstruction, CopyingInstruction {
  public var fromValue: Value { operand.value }
}

final public class UnownedCopyValueInst : SingleValueInstruction, CopyingInstruction {}
final public class WeakCopyValueInst : SingleValueInstruction, CopyingInstruction {}

final public class UncheckedOwnershipConversionInst : SingleValueInstruction {}

final public class MoveValueInst : SingleValueInstruction, UnaryInstruction {
  public var fromValue: Value { operand.value }

  public var isLexical: Bool { bridged.MoveValue_isLexical() }

  public var isFromVarDecl: Bool { bridged.MoveValue_isFromVarDecl() }
}

final public class DropDeinitInst : SingleValueInstruction, UnaryInstruction {
  public var fromValue: Value { operand.value }
}

final public class StrongCopyUnownedValueInst : SingleValueInstruction, UnaryInstruction {}
final public class StrongCopyUnmanagedValueInst : SingleValueInstruction, UnaryInstruction  {}
final public class StrongCopyWeakValueInst : SingleValueInstruction, UnaryInstruction  {}

final public class EndCOWMutationInst : SingleValueInstruction, UnaryInstruction {
  public var instance: Value { operand.value }
  public var doKeepUnique: Bool { bridged.EndCOWMutationInst_doKeepUnique() }
}

final public
class ClassifyBridgeObjectInst : SingleValueInstruction, UnaryInstruction {}

final public class PartialApplyInst : SingleValueInstruction, ApplySite {
  public var numArguments: Int { bridged.PartialApplyInst_numArguments() }

  /// WARNING: isOnStack incorrectly returns false for all closures prior to ClosureLifetimeFixup, even if they need to
  /// be diagnosed as on-stack closures. Use has mayEscape instead.
  public var isOnStack: Bool { bridged.PartialApplyInst_isOnStack() }

  public var mayEscape: Bool { !isOnStack && !hasNoescapeCapture }

  /// True if this closure captures anything nonescaping.
  public var hasNoescapeCapture: Bool {
    if operandConventions.contains(.indirectInoutAliasable) {
      return true
    }
    return arguments.contains { $0.type.containsNoEscapeFunction }
  }

  public var hasUnknownResultIsolation: Bool { bridged.PartialApplyInst_hasUnknownResultIsolation() }
  public var unappliedArgumentCount: Int { bridged.PartialApply_getCalleeArgIndexOfFirstAppliedArg() }
  public var calleeConvention: ArgumentConvention { type.bridged.getCalleeConvention().convention }
}

final public class ApplyInst : SingleValueInstruction, FullApplySite {
  public var numArguments: Int { bridged.ApplyInst_numArguments() }

  public var singleDirectResult: Value? { self }

  public var isNonThrowing: Bool { bridged.ApplyInst_getNonThrowing() }
  public var isNonAsync: Bool { bridged.ApplyInst_getNonAsync() }

  public typealias SpecializationInfo = BridgedGenericSpecializationInformation

  public var specializationInfo: SpecializationInfo { bridged.ApplyInst_getSpecializationInfo() }
}

final public class FunctionExtractIsolationInst : SingleValueInstruction {}

final public class ClassMethodInst : SingleValueInstruction, UnaryInstruction {}

final public class SuperMethodInst : SingleValueInstruction, UnaryInstruction {}

final public class ObjCMethodInst : SingleValueInstruction, UnaryInstruction {}

final public class ObjCSuperMethodInst : SingleValueInstruction, UnaryInstruction {}

final public class WitnessMethodInst : SingleValueInstruction {}

final public class IsUniqueInst : SingleValueInstruction, UnaryInstruction {}

final public class IsEscapingClosureInst : SingleValueInstruction, UnaryInstruction {}

final public class MarkUnresolvedNonCopyableValueInst: SingleValueInstruction, UnaryInstruction {}

final public class MarkUnresolvedReferenceBindingInst : SingleValueInstruction {}

final public class MarkUnresolvedMoveAddrInst : Instruction, SourceDestAddrInstruction {
  public var isTakeOfSrc: Bool { true }
  public var isInitializationOfDest: Bool { true }
}

final public class CopyableToMoveOnlyWrapperValueInst: SingleValueInstruction, UnaryInstruction {}

final public class MoveOnlyWrapperToCopyableValueInst: SingleValueInstruction, UnaryInstruction {}

final public class MoveOnlyWrapperToCopyableBoxInst: SingleValueInstruction, UnaryInstruction {}

final public class CopyableToMoveOnlyWrapperAddrInst
  : SingleValueInstruction, UnaryInstruction {}

final public class MoveOnlyWrapperToCopyableAddrInst
  : SingleValueInstruction, UnaryInstruction {}

final public class ObjectInst : SingleValueInstruction {
  public var baseOperands: OperandArray {
    operands[0..<bridged.ObjectInst_getNumBaseElements()]
  }

  public var tailOperands: OperandArray {
    let ops = operands
    return ops[bridged.ObjectInst_getNumBaseElements()..<ops.endIndex]
  }
}

final public class VectorInst : SingleValueInstruction {
}

final public class DifferentiableFunctionInst: SingleValueInstruction {}

final public class LinearFunctionInst: SingleValueInstruction {}
final public class DifferentiableFunctionExtractInst: SingleValueInstruction {}
final public class LinearFunctionExtractInst: SingleValueInstruction {}
final public class DifferentiabilityWitnessFunctionInst: SingleValueInstruction {}

final public class ProjectBlockStorageInst: SingleValueInstruction, UnaryInstruction {}

final public class InitBlockStorageHeaderInst: SingleValueInstruction {}

//===----------------------------------------------------------------------===//
//                      single-value allocation instructions
//===----------------------------------------------------------------------===//

public protocol Allocation : SingleValueInstruction { }

final public class AllocStackInst : SingleValueInstruction, Allocation, DebugVariableInstruction, MetaInstruction {
  public var hasDynamicLifetime: Bool { bridged.AllocStackInst_hasDynamicLifetime() }

  public var varDecl: VarDecl? {
    VarDecl(bridged: bridged.AllocStack_getDecl())
  }

  public var debugVariable: DebugVariable {
    return bridged.AllocStack_getVarInfo()
  }
}

final public class AllocVectorInst : SingleValueInstruction, Allocation, UnaryInstruction {
  public var capacity: Value { operand.value }
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

final public class AllocBoxInst : SingleValueInstruction, Allocation, DebugVariableInstruction {

  public var varDecl: VarDecl? {
    VarDecl(bridged: bridged.AllocBox_getDecl())
  }

  public var debugVariable: DebugVariable {
    return bridged.AllocBox_getVarInfo()
  }
}

final public class AllocExistentialBoxInst : SingleValueInstruction, Allocation {
}

//===----------------------------------------------------------------------===//
//                           scoped instructions
//===----------------------------------------------------------------------===//

/// An instruction whose side effects extend across a scope including other instructions. These are always paired with a
/// scope ending instruction such as `begin_access` (ending with `end_access`) and `begin_borrow` (ending with
/// `end_borrow`).
public protocol ScopedInstruction {
  var endOperands: LazyFilterSequence<UseList> { get }
}

extension Instruction {
  /// Return the sequence of use points of any instruction.
  public var endInstructions: EndInstructions {
    if let scopedInst = self as? ScopedInstruction {
      return .scoped(scopedInst.endOperands.map({ $0.instruction }))
    }
    return .single(self)
  }
}

/// Instructions beginning a borrow-scope which must be ended by `end_borrow`.
public protocol BorrowIntroducingInstruction : SingleValueInstruction, ScopedInstruction {}

final public class EndBorrowInst : Instruction, UnaryInstruction {}

extension BorrowIntroducingInstruction {
  public var endOperands: LazyFilterSequence<UseList> {
    return uses.lazy.filter { $0.instruction is EndBorrowInst }
  }
}

final public class BeginBorrowInst : SingleValueInstruction, UnaryInstruction, BorrowIntroducingInstruction {
  public var borrowedValue: Value { operand.value }

  public var isLexical: Bool { bridged.BeginBorrow_isLexical() }

  public var isFromVarDecl: Bool { bridged.BeginBorrow_isFromVarDecl() }

  public var endOperands: LazyFilterSequence<UseList> {
    return uses.endingLifetime
  }
}

final public class LoadBorrowInst : SingleValueInstruction, LoadInstruction, BorrowIntroducingInstruction {}

final public class StoreBorrowInst : SingleValueInstruction, StoringInstruction, BorrowIntroducingInstruction {
  var allocStack: AllocStackInst {
    var dest = destination
    if let mark = dest as? MarkUnresolvedNonCopyableValueInst {
      dest = mark.operand.value
    }
    return dest as! AllocStackInst
  }
}

final public class BeginAccessInst : SingleValueInstruction, UnaryInstruction {
  // The raw values must match SILAccessKind.
  public enum AccessKind: Int {
    case `init` = 0
    case read = 1
    case modify = 2
    case `deinit` = 3
  }
  public var accessKind: AccessKind {
    AccessKind(rawValue: bridged.BeginAccessInst_getAccessKind())!
  }

  public var isStatic: Bool { bridged.BeginAccessInst_isStatic() }

  public var address: Value { operand.value }

  public typealias EndAccessInstructions = LazyMapSequence<LazyFilterSequence<UseList>, EndAccessInst>

  public var endAccessInstructions: EndAccessInstructions {
    endOperands.map { $0.instruction as! EndAccessInst }
  }
}

final public class EndAccessInst : Instruction, UnaryInstruction {
  public var beginAccess: BeginAccessInst {
    return operand.value as! BeginAccessInst
  }
}

extension BeginAccessInst : ScopedInstruction {
  public var endOperands: LazyFilterSequence<UseList> {
    return uses.lazy.filter { $0.instruction is EndAccessInst }
  }
}

// Unpaired accesses do not have a static scope, are generally unsupported by the optimizer, and should be avoided.
final public class BeginUnpairedAccessInst : Instruction {}

final public class EndUnpairedAccessInst : Instruction {}


final public class BeginApplyInst : MultipleValueInstruction, FullApplySite {
  public var numArguments: Int { bridged.BeginApplyInst_numArguments() }

  public var singleDirectResult: Value? { nil }

  public var token: Value { getResult(index: resultCount - 1) }

  public var yieldedValues: Results {
    Results(inst: self, numResults: resultCount - 1)
  }
}

final public class EndApplyInst : Instruction, UnaryInstruction {}
final public class AbortApplyInst : Instruction, UnaryInstruction {}

extension BeginApplyInst : ScopedInstruction {
  public var endOperands: LazyFilterSequence<UseList> {
    return token.uses.lazy.filter { _ in true }
  }
}

/// A sequence representing the use points of an instruction for the purpose of liveness and the general
/// nesting of scopes.
///
/// Abstracts over simple single-use instructions vs. an instruction that is always paired with scope ending
/// instructions that denote the end of the scoped operation.
public enum EndInstructions: CollectionLikeSequence {
  public typealias EndScopedInstructions = LazyMapSequence<LazyFilterSequence<UseList>, Instruction>

  case single(Instruction)
  case scoped(EndScopedInstructions)

  public enum Iterator : IteratorProtocol {
    case single(Instruction?)
    case scoped(EndScopedInstructions.Iterator)

    public mutating func next() -> Instruction? {
      switch self {
      case let .single(inst):
        if let result = inst {
          self = .single(nil)
          return result
        }
        return nil
      case var .scoped(iter):
        let result = iter.next()
        self = .scoped(iter)
        return result
      }
    }
  }

  public func makeIterator() -> Iterator {
    switch self {
    case let .single(inst):
      return .single(inst)
    case let .scoped(endScoped):
      return .scoped(endScoped.makeIterator())
    }
  }
}

//===----------------------------------------------------------------------===//
//                            multi-value instructions
//===----------------------------------------------------------------------===//

final public class BeginCOWMutationInst : MultipleValueInstruction, UnaryInstruction {
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

//===----------------------------------------------------------------------===//
//                           parameter pack instructions
//===----------------------------------------------------------------------===//

final public class AllocPackInst : SingleValueInstruction, Allocation {}
final public class AllocPackMetadataInst : SingleValueInstruction, Allocation {}

final public class DeallocPackInst : Instruction, UnaryInstruction, Deallocation {}
final public class DeallocPackMetadataInst : Instruction, Deallocation {}

final public class OpenPackElementInst : SingleValueInstruction {}
final public class PackLengthInst : SingleValueInstruction {}
final public class DynamicPackIndexInst : SingleValueInstruction {}
final public class PackPackIndexInst : SingleValueInstruction {}
final public class ScalarPackIndexInst : SingleValueInstruction {}

final public class TuplePackExtractInst: SingleValueInstruction {
  public var indexOperand: Operand { operands[0] }
  public var tupleOperand: Operand { operands[1] }
}

final public class TuplePackElementAddrInst: SingleValueInstruction {
  public var indexOperand: Operand { operands[0] }
  public var tupleOperand: Operand { operands[1] }
}

final public class PackElementGetInst: SingleValueInstruction {}

final public class PackElementSetInst: Instruction {}

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

final public class ThrowAddrInst : TermInst {
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

  public var specializationInfo: ApplyInst.SpecializationInfo { bridged.TryApplyInst_getSpecializationInfo() }
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

final public class SelectEnumAddrInst : SingleValueInstruction {
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

final public class CheckedCastAddrBranchInst : TermInst {
  public var source: Value { operands[0].value }
  public var destination: Value { operands[1].value }

  public var successBlock: BasicBlock { bridged.CheckedCastAddrBranch_getSuccessBlock().block }
  public var failureBlock: BasicBlock { bridged.CheckedCastAddrBranch_getFailureBlock().block }

  public enum CastConsumptionKind {
    /// The source value is always taken, regardless of whether the cast
    /// succeeds.  That is, if the cast fails, the source value is
    /// destroyed.
    case TakeAlways

    /// The source value is taken only on a successful cast; otherwise,
    /// it is left in place.
    case TakeOnSuccess

    /// The source value is always left in place, and the destination
    /// value is copied into on success.
    case CopyOnSuccess
  }

  public var consumptionKind: CastConsumptionKind {
    switch bridged.CheckedCastAddrBranch_getConsumptionKind() {
    case .TakeAlways:    return .TakeAlways
    case .TakeOnSuccess: return .TakeOnSuccess
    case .CopyOnSuccess: return .CopyOnSuccess
    default:
      fatalError("invalid cast consumption kind")
    }
  }
}
