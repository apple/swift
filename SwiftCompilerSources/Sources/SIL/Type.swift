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
  public let bridged: swift.SILType
  
  public var isAddress: Bool { bridged.isAddress() }
  public var isObject: Bool { !isAddress }

  public var addressType: Type { bridged.getAddressType().type }
  public var objectType: Type { bridged.getObjectType().type }

  public func isTrivial(in function: Function) -> Bool {
    return bridged.isTrivial(function.bridged.getFunction())
  }

  /// Returns true if the type is a trivial type and is and does not contain a Builtin.RawPointer.
  public func isTrivialNonPointer(in function: Function) -> Bool {
    return !bridged.isNonTrivialOrContainsRawPointer(function.bridged.getFunction())
  }

  public func isReferenceCounted(in function: Function) -> Bool {
    return bridged.isReferenceCounted(function.bridged.getFunction())
  }

  public var hasArchetype: Bool { bridged.hasArchetype() }

  public var isNominal: Bool { bridged.getNominalOrBoundGenericNominal() != nil }
  public var isClass: Bool { bridged.getClassOrBoundGenericClass() != nil }
  public var isStruct: Bool { bridged.getStructOrBoundGenericStruct() != nil }
  public var isTuple: Bool { bridged.isTuple() }
  public var isEnum: Bool { bridged.getEnumOrBoundGenericEnum() != nil }
  public var isFunction: Bool { bridged.isFunction() }
  public var isMetatype: Bool { bridged.isMetatype() }
  public var isNoEscapeFunction: Bool { bridged.isNoEscapeFunction() }

  public var canBeClass: swift.TypeTraitResult { bridged.canBeClass() }

  /// Can only be used if the type is in fact a nominal type (`isNominal` is true).
  public var nominal: NominalTypeDecl {
    NominalTypeDecl(bridged: BridgedNominalTypeDecl(decl: bridged.getNominalOrBoundGenericNominal()))
  }

  public var isOrContainsObjectiveCClass: Bool { bridged.isOrContainsObjectiveCClass() }

  public var isBuiltinInteger: Bool { bridged.isBuiltinInteger() }
  public var isBuiltinFloat: Bool { bridged.isBuiltinFloat() }
  public var isBuiltinVector: Bool { bridged.isBuiltinVector() }
  public var builtinVectorElementType: Type { bridged.getBuiltinVectorElementType().type }

  public func isBuiltinInteger(withFixedWidth width: Int) -> Bool {
    bridged.isBuiltinFixedWidthInteger(UInt32(width))
  }

  public func isExactSuperclass(of type: Type) -> Bool {
    bridged.isExactSuperclassOf(type.bridged)
  }

  public var tupleElements: TupleElementArray { TupleElementArray(type: self) }

  public func getNominalFields(in function: Function) -> NominalFieldsArray {
    NominalFieldsArray(type: self, function: function)
  }

  public func instanceTypeOfMetatype(in function: Function) -> Type {
    bridged.getInstanceTypeOfMetatype(function.bridged.getFunction()).type
  }

  public var isCalleeConsumedFunction: Bool { bridged.isCalleeConsumedFunction() }

  public var isMarkedAsImmortal: Bool { bridged.isMarkedAsImmortal() }

  public func getIndexOfEnumCase(withName name: String) -> Int? {
    let idx = name._withStringRef {
      bridged.getCaseIdxOfEnumType($0)
    }
    return idx >= 0 ? idx : nil
  }

  public var description: String {
    String(_cxxString: bridged.getDebugDescription())
  }
}

extension Type: Equatable {
  public static func ==(lhs: Type, rhs: Type) -> Bool { 
    lhs.bridged == rhs.bridged
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
    type.bridged.getFieldType(index, function.bridged.getFunction()).type
  }

  public func getIndexOfField(withName name: String) -> Int? {
    let idx = name._withStringRef {
      type.bridged.getFieldIdxOfNominalType($0)
    }
    return idx >= 0 ? idx : nil
  }

  public func getNameOfField(withIndex idx: Int) -> StringRef {
    StringRef(bridged: type.bridged.getFieldName(idx))
  }
}

public struct TupleElementArray : RandomAccessCollection, FormattedLikeArray {
  fileprivate let type: Type

  public var startIndex: Int { return 0 }
  public var endIndex: Int { Int(type.bridged.getNumTupleElements()) }

  public subscript(_ index: Int) -> Type {
    type.bridged.getTupleElementType(index).type
  }
}

extension swift.SILType {
  var type: Type { Type(bridged: self) }
  var typeOrNil: Type? { isNull() ? nil : type }
}

// TODO: use an AST type for this once we have it
public struct NominalTypeDecl : Equatable {
  let bridged: BridgedNominalTypeDecl

  public var name: StringRef { StringRef(bridged: bridged.getName()) }

  public static func ==(lhs: NominalTypeDecl, rhs: NominalTypeDecl) -> Bool {
    lhs.bridged.decl == rhs.bridged.decl
  }
}
