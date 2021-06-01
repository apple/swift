//===--- Utils.swift - some SIL utilities ---------------------------------===//
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

import SILBridging

//===----------------------------------------------------------------------===//
//                              Lists
//===----------------------------------------------------------------------===//

public protocol ListNode : AnyObject {
  associatedtype Element
  var next: Element? { get }
  var previous: Element? { get }
}

public struct List<NodeType: ListNode> :
      Sequence, IteratorProtocol, CustomReflectable
      where NodeType.Element == NodeType {
  private var currentNode: NodeType?
  
  public init(startAt: NodeType?) { currentNode = startAt }

  public mutating func next() -> NodeType? {
    if let node = currentNode {
      currentNode = node.next
      return node
    }
    return nil
  }

  public var customMirror: Mirror {
    let c: [Mirror.Child] = map { (label: nil, value: $0) }
    return Mirror(self, children: c)
  }
}

public struct ReverseList<NodeType: ListNode> :
      Sequence, IteratorProtocol, CustomReflectable
      where NodeType.Element == NodeType {
  private var currentNode: NodeType?
  
  public init(startAt: NodeType?) { currentNode = startAt }

  public mutating func next() -> NodeType? {
    if let node = currentNode {
      currentNode = node.previous
      return node
    }
    return nil
  }

  public var customMirror: Mirror {
    let c: [Mirror.Child] = map { (label: nil, value: $0) }
    return Mirror(self, children: c)
  }
}

//===----------------------------------------------------------------------===//
//                            Bridging Utilities
//===----------------------------------------------------------------------===//

extension BridgedStringRef {
  public var string: String {
    let buffer = UnsafeBufferPointer<UInt8>(start: data, count: Int(length))
    return String(decoding: buffer, as: UTF8.self)
  }
  
  func takeString() -> String {
    let str = string
    freeBridgedStringRef(self)
    return str
  }
}

extension String {
  public func withBridgedStringRef<T>(_ c: (BridgedStringRef) -> T) -> T {
    var str = self
    return str.withUTF8 { buffer in
      return c(BridgedStringRef(data: buffer.baseAddress, length: buffer.count))
    }
  }
}

extension Array where Element == Value {
  public func withBridgedValues<T>(_ c: (BridgedValueArray) -> T) -> T {
    return self.withUnsafeBytes { valPtr in
      assert(valPtr.count == self.count * 16)
      return c(BridgedValueArray(data: valPtr.baseAddress, count: self.count))
    }
  }
}

public typealias SwiftObject = UnsafeMutablePointer<BridgedSwiftObject>

extension UnsafeMutablePointer where Pointee == BridgedSwiftObject {
  init<T: AnyObject>(_ object: T) {
    let ptr = Unmanaged.passUnretained(object).toOpaque()
    self = ptr.bindMemory(to: BridgedSwiftObject.self, capacity: 1)
  }
  
  func getAs<T: AnyObject>(_ objectType: T.Type) -> T {
    return Unmanaged<T>.fromOpaque(self).takeUnretainedValue()
  }
}

extension Optional where Wrapped == UnsafeMutablePointer<BridgedSwiftObject> {
  func getAs<T: AnyObject>(_ objectType: T.Type) -> T? {
    if let pointer = self {
      return Unmanaged<T>.fromOpaque(pointer).takeUnretainedValue()
    }
    return nil
  }
}

