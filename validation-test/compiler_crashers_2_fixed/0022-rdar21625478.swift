// RUN: %target-swift-frontend %s -emit-silgen

import StdlibUnittest

public struct MyRange<Bound : ForwardIndex> {
  var startIndex: Bound
  var endIndex: Bound
}

public protocol ForwardIndex : Equatable {
  associatedtype Distance : SignedNumeric
  func successor() -> Self
}

public protocol MySequence {
  associatedtype Iterator : IteratorProtocol
  associatedtype SubSequence = Void

  func makeIterator() -> Iterator
  var underestimatedCount: Int { get }

  func map<T>(
    _ transform: (Iterator.Element) -> T
  ) -> [T]

  func filter(
    _ isIncluded: (Iterator.Element) -> Bool
  ) -> [Iterator.Element]

  func _customContainsEquatableElement(
    _ element: Iterator.Element
  ) -> Bool?

  func _preprocessingPass<R>(
    _ preprocess: (Self) -> R
  ) -> R?

  func _copyToContiguousArray()
    -> ContiguousArray<Iterator.Element>

  func _copyContents(
    initializing ptr: UnsafeMutablePointer<Iterator.Element>
  ) -> UnsafeMutablePointer<Iterator.Element>
}
extension MySequence {
  var underestimatedCount: Int {
    return 0
  }

  public func map<T>(
    _ transform: (Iterator.Element) -> T
  ) -> [T] {
    return []
  }

  public func filter(
    _ isIncluded: (Iterator.Element) -> Bool
  ) -> [Iterator.Element] {
    return []
  }

  public func _customContainsEquatableElement(
    _ element: Iterator.Element
  ) -> Bool? {
    return nil
  }

  public func _preprocessingPass<R>(
    _ preprocess: (Self) -> R
  ) -> R? {
    return nil
  }

  public func _copyToContiguousArray()
    -> ContiguousArray<Iterator.Element> {
    fatalError()
  }

  public func _copyContents(
    initializing ptr: UnsafeMutablePointer<Iterator.Element>
  ) -> UnsafeMutablePointer<Iterator.Element> {
    fatalError()
  }
}

public protocol MyIndexable : MySequence {
  associatedtype Index : ForwardIndex

  var startIndex: Index { get }
  var endIndex: Index { get }

  associatedtype _Element
  subscript(_: Index) -> _Element { get }
}

public protocol MyCollection : MyIndexable {
  associatedtype Iterator : IteratorProtocol = IndexingIterator<Self>
  associatedtype SubSequence : MySequence

  subscript(_: Index) -> Iterator.Element { get }
  subscript(_: MyRange<Index>) -> SubSequence { get }

  var first: Iterator.Element? { get }
  var isEmpty: Bool { get }
  var count: Index.Distance { get }

  func _customIndexOfEquatableElement(_ element: Iterator.Element) -> Index??
}
extension MyCollection {
  public var isEmpty: Bool {
    return startIndex == endIndex
  }
  public func _preprocessingPass<R>(
    _ preprocess: (Self) -> R
  ) -> R? {
    return preprocess(self)
  }
  public var count: Index.Distance { return 0 }
  public func _customIndexOfEquatableElement(_ element: Iterator.Element) -> Index?? {
    return nil
  }
}

public struct IndexingIterator<I : MyIndexable> : IteratorProtocol {
  public func next() -> I._Element? {
    return nil
  }
}

protocol Resettable : AnyObject {
  func reset()
}

internal var _allResettables: [Resettable] = []

public class TypeIndexed<Value> : Resettable {
  public init(_ value: Value) {
    self.defaultValue = value
    _allResettables.append(self)
  }
  
  public subscript(t: Any.Type) -> Value {
    get {
      return byType[ObjectIdentifier(t)] ?? defaultValue
    }
    set {
      byType[ObjectIdentifier(t)] = newValue
    }
  }

  public func reset() { byType = [:] }

  internal var byType: [ObjectIdentifier:Value] = [:]
  internal var defaultValue: Value
}

//===--- LoggingWrappers.swift --------------------------------------------===//
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

public protocol Wrapper {
  associatedtype Base
  init(_: Base)
  var base: Base {get set}
}

public protocol LoggingType : Wrapper {
  associatedtype Log : AnyObject
}

extension LoggingType {
  public var log: Log.Type {
    return Log.self
  }
  
  public var selfType: Any.Type {
    return type(of: self)
  }
}

public class IteratorLog {
  public static func dispatchTester<G : IteratorProtocol>(
    _ g: G
  ) -> LoggingIterator<LoggingIterator<G>> {
    return LoggingIterator(LoggingIterator(g))
  }
  public static var next = TypeIndexed(0)
}

public struct LoggingIterator<Base: IteratorProtocol>
  : IteratorProtocol, LoggingType {

  public typealias Log = IteratorLog
  
  public init(_ base: Base) {
    self.base = base
  }
  
  public mutating func next() -> Base.Element? {
    Log.next[selfType] += 1
    return base.next()
  }
  
  public var base: Base
}

public class SequenceLog {
  public static func dispatchTester<S: MySequence>(
    _ s: S
  ) -> LoggingSequence<LoggingSequence<S>> {
    return LoggingSequence(LoggingSequence(s))
  }
  public static var iterator = TypeIndexed(0)
  public static var underestimatedCount = TypeIndexed(0)
  public static var map = TypeIndexed(0)
  public static var filter = TypeIndexed(0)
  public static var _customContainsEquatableElement = TypeIndexed(0)
  public static var _preprocessingPass = TypeIndexed(0)
  public static var _copyToContiguousArray = TypeIndexed(0)
  public static var _copyContents = TypeIndexed(0)
}

public protocol LoggingSequenceType : MySequence, LoggingType {
  associatedtype Base : MySequence
  associatedtype Log : AnyObject = SequenceLog
  associatedtype Iterator : IteratorProtocol = LoggingIterator<Base.Iterator>
}

extension LoggingSequenceType {
  public var underestimatedCount: Int {
    SequenceLog.underestimatedCount[selfType] += 1
    return base.underestimatedCount
  }
}

extension LoggingSequenceType
  where Log == SequenceLog, Iterator == LoggingIterator<Base.Iterator> {
  public func makeIterator() -> LoggingIterator<Base.Iterator> {
    Log.iterator[selfType] += 1
    return LoggingIterator(base.makeIterator())
  }

  public func map<T>(
    _ transform: (Base.Iterator.Element) -> T
  ) -> [T] {
    Log.map[selfType] += 1
    return base.map(transform)
  }

  public func filter(
    _ isIncluded: (Base.Iterator.Element) -> Bool
  ) -> [Base.Iterator.Element] {
    Log.filter[selfType] += 1
    return base.filter(isIncluded)
  }
  
  public func _customContainsEquatableElement(
    _ element: Base.Iterator.Element
  ) -> Bool? {
    Log._customContainsEquatableElement[selfType] += 1
    return base._customContainsEquatableElement(element)
  }
  
  /// If `self` is multi-pass (i.e., a `Collection`), invoke
  /// `preprocess` on `self` and return its result.  Otherwise, return
  /// `nil`.
  public func _preprocessingPass<R>(
    _ preprocess: (Self) -> R
  ) -> R? {
    Log._preprocessingPass[selfType] += 1
    return base._preprocessingPass { _ in preprocess(self) }
  }

  /// Create a native array buffer containing the elements of `self`,
  /// in the same order.
  public func _copyToContiguousArray()
    -> ContiguousArray<Base.Iterator.Element> {
    Log._copyToContiguousArray[selfType] += 1
    return base._copyToContiguousArray()
  }

  /// Copy a Sequence into an array.
  public func _copyContents(
    initializing ptr: UnsafeMutablePointer<Base.Iterator.Element>
  ) -> UnsafeMutablePointer<Base.Iterator.Element> {
    Log._copyContents[selfType] += 1
    return base._copyContents(initializing: ptr)
  }
}

public struct LoggingSequence<
  Base_: MySequence
> : LoggingSequenceType, MySequence {
  public typealias Log = SequenceLog
  public typealias Base = Base_

  public init(_ base: Base_) {
    self.base = base
  }
  
  public var base: Base_
}

public class CollectionLog : SequenceLog {
  public class func dispatchTester<C: MyCollection>(
    _ c: C
  ) -> LoggingCollection<LoggingCollection<C>> {
    return LoggingCollection(LoggingCollection(c))
  }
  static var startIndex = TypeIndexed(0)
  static var endIndex = TypeIndexed(0)
  static var subscriptIndex = TypeIndexed(0)
  static var subscriptRange = TypeIndexed(0)
  static var isEmpty = TypeIndexed(0)
  static var count = TypeIndexed(0)
  static var _customIndexOfEquatableElement = TypeIndexed(0)
  static var first = TypeIndexed(0)
}

public protocol LoggingCollectionType : LoggingSequenceType, MyCollection {
  associatedtype Base : MyCollection
  associatedtype Index : ForwardIndex = Base.Index
}

extension LoggingCollectionType
where Index == Base.Index {
  public var startIndex: Base.Index {
    CollectionLog.startIndex[selfType] += 1
    return base.startIndex
  }
  
  public var endIndex: Base.Index {
    CollectionLog.endIndex[selfType] += 1
    return base.endIndex
  }
  public subscript(position: Base.Index) -> Base.Iterator.Element {
    CollectionLog.subscriptIndex[selfType] += 1
    return base[position]
  }
  
  public subscript(_prext_bounds: MyRange<Base.Index>) -> Base.SubSequence {
    CollectionLog.subscriptRange[selfType] += 1
    return base[_prext_bounds]
  }    

  public var isEmpty: Bool {
    CollectionLog.isEmpty[selfType] += 1
    return base.isEmpty
  }

  public var count: Base.Index.Distance {
    CollectionLog.count[selfType] += 1
    return base.count
  }
  
  public func _customIndexOfEquatableElement(_ element: Base.Iterator.Element) -> Base.Index?? {
    CollectionLog._customIndexOfEquatableElement[selfType] += 1
    return base._customIndexOfEquatableElement(element)
  }

  public var first: Base.Iterator.Element? {
    CollectionLog.first[selfType] += 1
    return base.first
  }
}

public struct LoggingCollection<Base_ : MyCollection> : LoggingCollectionType {
  public typealias Iterator = LoggingIterator<Base.Iterator>
  public typealias Log = CollectionLog
  public typealias Base = Base_
  public typealias SubSequence = Base.SubSequence

  public func makeIterator() -> Iterator {
    return Iterator(base.makeIterator())
  }
  public init(_ base: Base_) {
    self.base = base
  }
  
  public var base: Base_
}

public func expectCustomizable<
  T : Wrapper
>(_: T, _ counters: TypeIndexed<Int>,
  stackTrace: SourceLocStack? = nil,
  file: String = #file, line: UInt = #line,
  collectMoreInfo: (()->String)? = nil
)
  where
    T : LoggingType,
    T.Base : Wrapper, T.Base : LoggingType,
    T.Log == T.Base.Log {
  expectNotEqual(
    0, counters[T.self], collectMoreInfo?() ?? "",
    stackTrace: stackTrace ?? SourceLocStack(), file: file, line: line)
  
  expectEqual(
    counters[T.self], counters[T.Base.self], collectMoreInfo?() ?? "",
    stackTrace: stackTrace ?? SourceLocStack(), file: file, line: line)
}

public func expectNotCustomizable<
  T : Wrapper
>(_: T, _ counters: TypeIndexed<Int>,
  stackTrace: SourceLocStack? = nil,
  file: String = #file, line: UInt = #line,
  collectMoreInfo: (()->String)? = nil
)
  where
    T : LoggingType,
    T.Base : Wrapper, T.Base : LoggingType,
    T.Log == T.Base.Log {
  expectNotEqual(
    0, counters[T.self], collectMoreInfo?() ?? "",
    stackTrace: stackTrace ?? SourceLocStack(), file: file, line: line)
  
  expectEqual(
    0, counters[T.Base.self], collectMoreInfo?() ?? "",
    stackTrace: stackTrace ?? SourceLocStack(), file: file, line: line)
}
