//===--- Type.swift - Value type ------------------------------------------===//
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

public struct Type : CustomStringConvertible, NoReflectionChildren {
  public let bridged: BridgedType

  public var isAddress: Bool { bridged.isAddress() }
  public var isObject: Bool { !isAddress }

  public var addressType: Type { bridged.getAddressType().type }
  public var objectType: Type { bridged.getObjectType().type }

  public func isTrivial(in function: Function) -> Bool {
    return bridged.isTrivial(function.bridged)
  }

  /// Returns true if the type is a trivial type and is and does not contain a Builtin.RawPointer.
  public func isTrivialNonPointer(in function: Function) -> Bool {
    return !bridged.isNonTrivialOrContainsRawPointer(function.bridged)
  }

  /// True if this type is a value type (struct/enum) that requires deinitialization beyond
  /// destruction of its members.
  public var isValueTypeWithDeinit: Bool { bridged.isValueTypeWithDeinit() }

  public func isLoadable(in function: Function) -> Bool {
    return bridged.isLoadable(function.bridged)
  }

  public func isReferenceCounted(in function: Function) -> Bool {
    return bridged.isReferenceCounted(function.bridged)
  }

  public var isUnownedStorageType: Bool {
    return bridged.isUnownedStorageType()
  }

  public var hasArchetype: Bool { bridged.hasArchetype() }

  public var isNominal: Bool { bridged.isNominalOrBoundGenericNominal() }
  public var isClass: Bool { bridged.isClassOrBoundGenericClass() }
  public var isStruct: Bool { bridged.isStructOrBoundGenericStruct() }
  public var isTuple: Bool { bridged.isTuple() }
  public var isEnum: Bool { bridged.isEnumOrBoundGenericEnum() }
  public var isFunction: Bool { bridged.isFunction() }
  public var isMetatype: Bool { bridged.isMetatype() }
  public var isNoEscapeFunction: Bool { bridged.isNoEscapeFunction() }
  public var containsNoEscapeFunction: Bool { bridged.containsNoEscapeFunction() }
  public var isThickFunction: Bool { bridged.isThickFunction() }
  public var isAsyncFunction: Bool { bridged.isAsyncFunction() }

  public var canBeClass: BridgedType.TraitResult { bridged.canBeClass() }

  public var isMoveOnly: Bool { bridged.isMoveOnly() }

  public func isEscapable(in function: Function) -> Bool {
    bridged.isEscapable(function.bridged)
  }
  public func mayEscape(in function: Function) -> Bool {
    !isNoEscapeFunction && isEscapable(in: function)
  }

  /// Can only be used if the type is in fact a nominal type (`isNominal` is true).
  public var nominal: NominalTypeDecl {
    NominalTypeDecl(_bridged: bridged.getNominalOrBoundGenericNominal())
  }

  public var isOrContainsObjectiveCClass: Bool { bridged.isOrContainsObjectiveCClass() }

  public var isBuiltinInteger: Bool { bridged.isBuiltinInteger() }
  public var isBuiltinFloat: Bool { bridged.isBuiltinFloat() }
  public var isBuiltinVector: Bool { bridged.isBuiltinVector() }
  public var builtinVectorElementType: Type { bridged.getBuiltinVectorElementType().type }

  public func isBuiltinInteger(withFixedWidth width: Int) -> Bool {
    bridged.isBuiltinFixedWidthInteger(width)
  }

  public func isExactSuperclass(of type: Type) -> Bool {
    bridged.isExactSuperclassOf(type.bridged)
  }

  public func isEmpty(in function: Function) -> Bool {
    bridged.isEmpty(function.bridged)
  }

  public var tupleElements: TupleElementArray { TupleElementArray(type: self) }

  /// Can only be used if the type is in fact a nominal type (`isNominal` is true).
  /// Returns nil if the nominal is a resilient type because in this case the complete list
  /// of fields is not known.
  public func getNominalFields(in function: Function) -> NominalFieldsArray? {
    if nominal.isResilient(in: function) {
      return nil
    }
    return NominalFieldsArray(type: self, function: function)
  }

  /// Can only be used if the type is in fact an enum type.
  /// Returns nil if the enum is a resilient type because in this case the complete list
  /// of cases is not known.
  public func getEnumCases(in function: Function) -> EnumCases? {
    if nominal.isResilient(in: function) {
      return nil
    }
    return EnumCases(enumType: self, function: function)
  }

  public typealias MetatypeRepresentation = BridgedType.MetatypeRepresentation

  public func instanceTypeOfMetatype(in function: Function) -> Type {
    bridged.getInstanceTypeOfMetatype(function.bridged).type
  }

  public var isDynamicSelfMetatype: Bool {
    bridged.isDynamicSelfMetatype()
  }

  public func representationOfMetatype(in function: Function) -> MetatypeRepresentation {
    bridged.getRepresentationOfMetatype(function.bridged)
  }

  public var isCalleeConsumedFunction: Bool { bridged.isCalleeConsumedFunction() }

  public var isMarkedAsImmortal: Bool { bridged.isMarkedAsImmortal() }

  public func getIndexOfEnumCase(withName name: String) -> Int? {
    let idx = name._withBridgedStringRef {
      bridged.getCaseIdxOfEnumType($0)
    }
    return idx >= 0 ? idx : nil
  }

  public func getFunctionType(withNoEscape: Bool) -> Type {
    bridged.getFunctionTypeWithNoEscape(withNoEscape).type
  }

  public var description: String {
    String(taking: bridged.getDebugDescription())
  }
}

extension Value {
  public var isEscapable: Bool {
    type.objectType.isEscapable(in: parentFunction)
  }

  public var mayEscape: Bool {
    type.objectType.mayEscape(in: parentFunction)
  }
}

extension Type: Equatable {
  public static func ==(lhs: Type, rhs: Type) -> Bool { 
    lhs.bridged.opaqueValue == rhs.bridged.opaqueValue
  }
}

public struct TypeArray : RandomAccessCollection, CustomReflectable {
  private let bridged: BridgedSILTypeArray

  public var startIndex: Int { return 0 }
  public var endIndex: Int { return bridged.getCount() }

  public init(bridged: BridgedSILTypeArray) {
    self.bridged = bridged
  }

  public subscript(_ index: Int) -> Type {
    bridged.getAt(index).type
  }

  public var customMirror: Mirror {
    let c: [Mirror.Child] = map { (label: nil, value: $0) }
    return Mirror(self, children: c)
  }
}

public struct OptionalTypeArray : RandomAccessCollection, CustomReflectable {
  private let bridged: BridgedTypeArray

  public var startIndex: Int { return 0 }
  public var endIndex: Int { return bridged.getCount() }

  public init(bridged: BridgedTypeArray) {
    self.bridged = bridged
  }

  public subscript(_ index: Int) -> Type? {
    bridged.getAt(index).typeOrNil
  }

  public var customMirror: Mirror {
    let c: [Mirror.Child] = map { (label: nil, value: $0 ?? "<invalid>") }
    return Mirror(self, children: c)
  }
}

public struct NominalFieldsArray : RandomAccessCollection, FormattedLikeArray {
  fileprivate let type: Type
  fileprivate let function: Function

  public var startIndex: Int { return 0 }
  public var endIndex: Int { Int(type.bridged.getNumNominalFields()) }

  public subscript(_ index: Int) -> Type {
    type.bridged.getFieldType(index, function.bridged).type
  }

  public func getIndexOfField(withName name: String) -> Int? {
    let idx = name._withBridgedStringRef {
      type.bridged.getFieldIdxOfNominalType($0)
    }
    return idx >= 0 ? idx : nil
  }

  public func getNameOfField(withIndex idx: Int) -> StringRef {
    StringRef(bridged: type.bridged.getFieldName(idx))
  }
}

public struct EnumCase {
  public let payload: Type?
  public let index: Int
}

public struct EnumCases : CollectionLikeSequence, IteratorProtocol {
  fileprivate let enumType: Type
  fileprivate let function: Function
  private var caseIterator: BridgedType.EnumElementIterator
  private var caseIndex = 0

  fileprivate init(enumType: Type, function: Function) {
    self.enumType = enumType
    self.function = function
    self.caseIterator = enumType.bridged.getFirstEnumCaseIterator()
  }

  public mutating func next() -> EnumCase? {
    if !enumType.bridged.isEndCaseIterator(caseIterator) {
      defer {
        caseIterator = caseIterator.getNext()
        caseIndex += 1
      }
      return EnumCase(payload: enumType.bridged.getEnumCasePayload(caseIterator, function.bridged).typeOrNil,
                      index: caseIndex)
    }
    return nil
  }
}

public struct TupleElementArray : RandomAccessCollection, FormattedLikeArray {
  fileprivate let type: Type

  public var startIndex: Int { return 0 }
  public var endIndex: Int { Int(type.bridged.getNumTupleElements()) }

  public subscript(_ index: Int) -> Type {
    type.bridged.getTupleElementType(index).type
  }

  public subscript(nameOf index: Int) -> String {
    String(type.bridged.getTupleElementName(index))
  }
}

extension BridgedType {
  public var type: Type { Type(bridged: self) }
  var typeOrNil: Type? { isNull() ? nil : type }
}

// TODO: use an AST type for this once we have it
public struct NominalTypeDecl : Equatable, Hashable {
  public let bridged: BridgedNominalTypeDecl

  public init(_bridged: BridgedNominalTypeDecl) {
    self.bridged = _bridged
  }

  public var name: StringRef { StringRef(bridged: bridged.getName()) }

  public static func ==(lhs: NominalTypeDecl, rhs: NominalTypeDecl) -> Bool {
    lhs.bridged.raw == rhs.bridged.raw
  }

  public func hash(into hasher: inout Hasher) {
    hasher.combine(bridged.raw)
  }

  public func isResilient(in function: Function) -> Bool {
    function.bridged.isResilientNominalDecl(bridged)
  }

  public var isStructWithUnreferenceableStorage: Bool {
    bridged.isStructWithUnreferenceableStorage()
  }

  public var isGlobalActor: Bool {
    return bridged.isGlobalActor()
  }

  public var hasValueDeinit: Bool {
    return bridged.hasValueDeinit()
  }
}
