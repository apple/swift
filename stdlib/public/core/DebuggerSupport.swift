//===--- DebuggerSupport.swift --------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SwiftShims

#if SWIFT_ENABLE_REFLECTION

@frozen // namespace
public enum _DebuggerSupport {
  private enum CollectionStatus {
    case notACollection
    case collectionOfElements
    case collectionOfPairs
    case element
    case pair
    case elementOfPair
  
    internal var isCollection: Bool {
      return self != .notACollection
    }
  
    internal func getChildStatus(child: Mirror) -> CollectionStatus {
      let disposition = child.displayStyle
    
      if disposition == .collection { return .collectionOfElements }
      if disposition == .dictionary { return .collectionOfPairs }
      if disposition == .set { return .collectionOfElements }
    
      if self == .collectionOfElements { return .element }
      if self == .collectionOfPairs { return .pair }
      if self == .pair { return .elementOfPair }
    
      return .notACollection
    }
  }

  private static func isClass(_ value: Any) -> Bool {
    return type(of: value) is AnyClass
  }
  
  private static func checkValue<T>(
    _ value: Any,
    ifClass: (AnyObject) -> T,
    otherwise: () -> T
  ) -> T {
    if isClass(value) {
      return ifClass(_unsafeDowncastToAnyObject(fromAny: value))
    }
    return otherwise()
  }

  private static func asObjectIdentifier(_ value: Any) -> ObjectIdentifier? {
    return checkValue(value,
      ifClass: { return ObjectIdentifier($0) },
      otherwise: { return nil })
  }

  private static func asObjectAddress(_ value: Any) -> String {
    let address = checkValue(value,
      ifClass: { return unsafeBitCast($0, to: Int.self) },
      otherwise: { return 0 })
    return String(address, radix: 16, uppercase: false)
  }

  private static func asStringRepresentation(
    value: Any?,
    mirror: Mirror,
    count: Int
  ) -> String? {
    switch mirror.displayStyle {
    case .optional? where count > 0:
        return "\(mirror.subjectType)"
    case .optional?:
      return value.map(String.init(reflecting:))
    case .collection?, .dictionary?, .set?, .tuple?:
      return count == 1 ? "1 element" : "\(count) elements"
    case .`struct`?, .`enum`?, nil:
      switch value {
      case let x as CustomDebugStringConvertible:
        return x.debugDescription
      case let x as CustomStringConvertible:
        return x.description
      case _ where count > 0:
        return "\(mirror.subjectType)"
      default:
        return value.map(String.init(reflecting:))
      }
    case .`class`?:
      switch value {
      case let x as CustomDebugStringConvertible:
        return x.debugDescription
      case let x as CustomStringConvertible:
        return x.description
      case let x?:
        // for a Class with no custom summary, mimic the Foundation default
        return "<\(type(of: x)): 0x\(asObjectAddress(x))>"
      default:
        // but if I can't provide a value, just use the type anyway
        return "\(mirror.subjectType)"
      }
    }
  }

  private static func ivarCount(mirror: Mirror) -> Int {
    let ivars = mirror.superclassMirror.map(ivarCount) ?? 0
    return ivars + mirror._children.count
  }

  private static func shouldExpand(
    mirror: Mirror,
    collectionStatus: CollectionStatus,
    isRoot: Bool
  ) -> Bool {
    if isRoot || collectionStatus.isCollection { return true }
    if !mirror._children.isEmpty { return true }
    if mirror.displayStyle == .`class` { return true }
    if let sc = mirror.superclassMirror { return ivarCount(mirror: sc) > 0 }
    return true
  }

  private static func printForDebuggerImpl<StreamType: TextOutputStream>(
    value: Any?,
    mirror: Mirror,
    name: String?,
    indent: Int,
    maxDepth: Int,
    isRoot: Bool,
    parentCollectionStatus: CollectionStatus,
    refsAlreadySeen: inout Set<ObjectIdentifier>,
    maxItemCounter: inout Int,
    target: inout StreamType
  ) {    
    guard maxItemCounter > 0 else { return }

    guard shouldExpand(mirror: mirror,
                       collectionStatus: parentCollectionStatus,
                       isRoot: isRoot) 
    else { return }

    maxItemCounter -= 1
  
    print(String(repeating: " ", count: indent), terminator: "", to: &target)

    // do not expand classes with no custom Mirror
    // yes, a type can lie and say it's a class when it's not since we only
    // check the displayStyle - but then the type would have a custom Mirror
    // anyway, so there's that...
    let willExpand = mirror.displayStyle != .`class` || value is CustomReflectable?

    let count = mirror._children.count
    let bullet = isRoot && (count == 0 || !willExpand) ? ""
      : count == 0    ? "- "
      : maxDepth <= 0 ? "▹ " : "▿ "
    print(bullet, terminator: "", to: &target)
  
    let collectionStatus = parentCollectionStatus.getChildStatus(child: mirror)
  
    if let name = name {
      print("\(name) : ", terminator: "", to: &target)
    }

    if let str = asStringRepresentation(value: value, mirror: mirror, count: count) {
      print(str, terminator: "", to: &target)
    }
  
    if (maxDepth <= 0) || !willExpand {
      print("", to: &target)
      return
    }

    if let valueIdentifier = value.flatMap(asObjectIdentifier) {
      if refsAlreadySeen.contains(valueIdentifier) {
        print(" { ... }", to: &target)
        return
      } else {
        refsAlreadySeen.insert(valueIdentifier)
      }
    }

    print("", to: &target)
  
    var printedElements = 0
  
    if let sc = mirror.superclassMirror {
      printForDebuggerImpl(
        value: nil,
        mirror: sc,
        name: "super",
        indent: indent + 2,
        maxDepth: maxDepth - 1,
        isRoot: false,
        parentCollectionStatus: .notACollection,
        refsAlreadySeen: &refsAlreadySeen,
        maxItemCounter: &maxItemCounter,
        target: &target)
    }
  
    for (optionalName,child) in mirror._children {
      let childName = optionalName ?? "\(printedElements)"
      if maxItemCounter <= 0 {
        print(String(repeating: " ", count: indent+4), terminator: "", to: &target)
        let remainder = count - printedElements
        print("(\(remainder)", terminator: "", to: &target)
        if printedElements > 0 {
          print(" more", terminator: "", to: &target)
        }
        print(remainder == 1 ? " child)" : " children)", to: &target)
        return
      }
    
      printForDebuggerImpl(
        value: child,
        mirror: Mirror(reflecting: child),
        name: childName,
        indent: indent + 2,
        maxDepth: maxDepth - 1,
        isRoot: false,
        parentCollectionStatus: collectionStatus,
        refsAlreadySeen: &refsAlreadySeen,
        maxItemCounter: &maxItemCounter,
        target: &target)
      printedElements += 1
    }
  }

  public static func stringForPrintObject(_ value: Any) -> String {
    var maxItemCounter = Int.max
    var refs = Set<ObjectIdentifier>()
    var target = ""

    printForDebuggerImpl(
      value: value,
      mirror: Mirror(reflecting: value),
      name: nil,
      indent: 0,
      maxDepth: maxItemCounter,
      isRoot: true,
      parentCollectionStatus: .notACollection,
      refsAlreadySeen: &refs,
      maxItemCounter: &maxItemCounter,
      target: &target)

    return target
  }
}

public func _stringForPrintObject(_ value: Any) -> String {
  return _DebuggerSupport.stringForPrintObject(value)
}

#endif // SWIFT_ENABLE_REFLECTION

public func _debuggerTestingCheckExpect(_: String, _: String) { }

// Utilities to get refcount(s) of class objects.
@_silgen_name("swift_retainCount")
public func _getRetainCount(_ Value: AnyObject) -> UInt
@_silgen_name("swift_unownedRetainCount")
public func _getUnownedRetainCount(_ Value: AnyObject) -> UInt
@_silgen_name("swift_weakRetainCount")
public func _getWeakRetainCount(_ Value: AnyObject) -> UInt
