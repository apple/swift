//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//


import SwiftPrivate
import SwiftPrivateThreadExtras
import SwiftPrivateLibcExtras

#if canImport(Darwin)
#if _runtime(_ObjC)
import Foundation
#endif
import Darwin
#elseif canImport(Glibc)
import Glibc
#elseif os(Windows)
import CRT
import WinSDK
#endif

#if _runtime(_ObjC)
import ObjectiveC
#endif

#if SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY
import _Concurrency
#endif

extension String {
  /// Returns the lines in `self`.
  public var _lines : [String] {
    return _split(separator: "\n")
  }

  /// Splits `self` at occurrences of `separator`.
  public func _split(separator: Unicode.Scalar) -> [String] {
    let scalarSlices = unicodeScalars.split { $0 == separator }
    return scalarSlices.map { String(String.UnicodeScalarView($0)) }
  }
}

public struct SourceLoc {
  public let file: String
  public let line: UInt
  public let comment: String?

  public init(_ file: String, _ line: UInt, comment: String? = nil) {
    self.file = file
    self.line = line
    self.comment = comment
  }

  public func withCurrentLoc(
    _ file: String = #file, line: UInt = #line
  ) -> SourceLocStack {
    return SourceLocStack(self).with(SourceLoc(file, line))
  }
}

public struct SourceLocStack {
  let locs: [SourceLoc]

  public init() {
    locs = []
  }

  public init(_ loc: SourceLoc) {
    locs = [loc]
  }

  init(_locs: [SourceLoc]) {
    locs = _locs
  }

  var isEmpty: Bool {
    return locs.isEmpty
  }

  public func with(_ loc: SourceLoc) -> SourceLocStack {
    var locs = self.locs
    locs.append(loc)
    return SourceLocStack(_locs: locs)
  }

  public func pushIf(
    _ showFrame: Bool, file: String, line: UInt
  ) -> SourceLocStack {
    return showFrame ? self.with(SourceLoc(file, line)) : self
  }

  public func withCurrentLoc(
    file: String = #file, line: UInt = #line
  ) -> SourceLocStack {
    return with(SourceLoc(file, line))
  }

  public func print() {
    let top = locs.first!
    Swift.print("check failed at \(top.file), line \(top.line)")
    _printStackTrace(SourceLocStack(_locs: Array(locs.dropFirst())))
  }
}

fileprivate struct AtomicBool {
    
    private var _value: _stdlib_AtomicInt
    
    init(_ b: Bool) { self._value = _stdlib_AtomicInt(b ? 1 : 0) }
    
    func store(_ b: Bool) { _value.store(b ? 1 : 0) }
    
    func load() -> Bool { return _value.load() != 0 }
    
    @discardableResult
    func orAndFetch(_ b: Bool) -> Bool {
        return _value.orAndFetch(b ? 1 : 0) != 0
    }

    func fetchAndClear() -> Bool {
        return _value.fetchAndAnd(0) != 0
    }
}

func _printStackTrace(_ stackTrace: SourceLocStack?) {
  guard let s = stackTrace, !s.locs.isEmpty else { return }
  print("stacktrace:")
  for (i, loc) in s.locs.reversed().enumerated() {
    let comment = (loc.comment != nil) ? " ; \(loc.comment!)" : ""
    print("  #\(i): \(loc.file):\(loc.line)\(comment)")
  }
}

fileprivate var _anyExpectFailed = AtomicBool(false)
fileprivate var _seenExpectCrash = AtomicBool(false)

/// Run `body` and expect a failure to happen.
///
/// The check passes iff `body` triggers one or more failures.
public func expectFailure(
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line, invoking body: () -> Void) {
  let startAnyExpectFailed = _anyExpectFailed.fetchAndClear()
  body()
  let endAnyExpectFailed = _anyExpectFailed.fetchAndClear()
  expectTrue(
    endAnyExpectFailed, "running `body` should produce an expected failure",
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line)
  )
  _anyExpectFailed.orAndFetch(startAnyExpectFailed)
}

/// An opaque function that ignores its argument and returns nothing.
public func noop<T>(_ value: T) {}

/// An opaque function that simply returns its argument.
public func identity<T>(_ value: T) -> T {
  return value
}

public func identity(_ element: OpaqueValue<Int>) -> OpaqueValue<Int> {
  return element
}

public func identityEq(_ element: MinimalEquatableValue) -> MinimalEquatableValue {
  return element
}

public func identityComp(_ element: MinimalComparableValue)
  -> MinimalComparableValue {
  return element
}

public func expectEqual<T : Equatable>(
  _ first: T,
  _ second: T,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) {
  expectEqualTest(first, second, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), showFrame: false) {$0 == $1}
}

public func expectEqual<T : Equatable, U : Equatable>(
  _ first: (T, U),
  _ second: (T, U),
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) {
  expectEqualTest(first.0, second.0, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), showFrame: false) {$0 == $1}
  expectEqualTest(first.1, second.1, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), showFrame: false) {$0 == $1}
}

public func expectEqual<T : Equatable, U : Equatable, V : Equatable>(
  _ first: (T, U, V),
  _ second: (T, U, V),
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) {
  expectEqualTest(first.0, second.0, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), showFrame: false) {$0 == $1}
  expectEqualTest(first.1, second.1, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), showFrame: false) {$0 == $1}
  expectEqualTest(first.2, second.2, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), showFrame: false) {$0 == $1}
}

public func expectEqual<T : Equatable, U : Equatable, V : Equatable, W : Equatable>(
  _ first: (T, U, V, W),
  _ second: (T, U, V, W),
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) {
  expectEqualTest(first.0, second.0, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), showFrame: false) {$0 == $1}
  expectEqualTest(first.1, second.1, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), showFrame: false) {$0 == $1}
  expectEqualTest(first.2, second.2, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), showFrame: false) {$0 == $1}
  expectEqualTest(first.3, second.3, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), showFrame: false) {$0 == $1}
}

public func expectEqual(
  _ first: String,
  _ second: Substring,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) {
  if !(first == second) {
    expectationFailure(
      """
      first:  \(String(reflecting: first)) (of type \(String(reflecting: type(of: first))))
      second: \(String(reflecting: second)) (of type \(String(reflecting: type(of: second))))
      """,
      trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line)
    )
  }
}
public func expectEqual(
  _ first: Substring,
  _ second: String,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) {
  if !(first == second) {
    expectationFailure(
      """
      first:  \(String(reflecting: first)) (of type \(String(reflecting: type(of: first))))
      second: \(String(reflecting: second)) (of type \(String(reflecting: type(of: second))))
      """,
      trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line)
    )
  }
}
public func expectEqual(
  _ first: String,
  _ second: String,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) {
  if !(first == second) {
    expectationFailure(
      """
      first:  \(String(reflecting: first)) (of type \(String(reflecting: type(of: first))))
      second: \(String(reflecting: second)) (of type \(String(reflecting: type(of: second))))
      """,
      trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line)
    )
  }
}

public func expectEqualReference(
  _ first: AnyObject?,
  _ second: AnyObject?,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) {
  expectEqualTest(first, second, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), showFrame: false) {$0 === $1}
}

public func expectationFailure(
  _ reason: String,
  trace message: String,
  stackTrace: SourceLocStack) {
  _anyExpectFailed.store(true)
  stackTrace.print()
  print(reason, terminator: reason == "" ? "" : "\n")
  print(message, terminator: message == "" ? "" : "\n")
}

// Renamed to avoid collision with expectEqual(_, _, TRACE).
// See <rdar://26058520> Generic type constraints incorrectly applied to
// functions with the same name
public func expectEqualTest<T>(
  _ first: T,
  _ second: T,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  sameValue equal: (T, T) -> Bool
) {
  if !equal(first, second) {
    expectationFailure(
      """
      first:  \(String(reflecting: first)) (of type \(String(reflecting: type(of: first))))
      second: \(String(reflecting: second)) (of type \(String(reflecting: type(of: second))))
      """,
      trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line)
    )
  }
}

public func expectNotEqual<T : Equatable>(
  _ first: T,
  _ second: T,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) {
  if first == second {
    expectationFailure(
      "unexpected value: \"\(second)\" (of type \(String(reflecting: type(of: second))))",
      trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line)
    )
  }
}

public func expectOptionalEqual<T>(
  _ first: T,
  _ second: T?,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  sameValue equal: (T, T) -> Bool
) {
  if (second == nil) || !equal(first, second!) {
    expectationFailure(
      """
      first:  \"\(first)\" (of type \(String(reflecting: type(of: first))))
      second: \"\(second.debugDescription)\" (of type \(String(reflecting: type(of: second))))
      """,
      trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  }
}

public func expectEqual(
  _ first: Any.Type, _ second: Any.Type,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) {
  expectEqualTest(
    first, second, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), showFrame: false
  ) { $0 == $1 }
}

public func expectLT<T : Comparable>(_ lhs: T, _ rhs: T,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line) {
  if !(lhs < rhs) {
    expectationFailure("\(lhs) < \(rhs)", trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  } else if !(rhs > lhs) {
    expectationFailure("\(lhs) < \(rhs) (flipped)", trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  }
}

public func expectLE<T : Comparable>(_ lhs: T, _ rhs: T,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line) {
  if !(lhs <= rhs) {
    expectationFailure("\(lhs) <= \(rhs)", trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  } else if !(rhs >= lhs) {
    expectationFailure("\(lhs) <= \(rhs) (flipped)", trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  }
}

public func expectGT<T : Comparable>(_ lhs: T, _ rhs: T,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line) {
  if !(lhs > rhs) {
    expectationFailure("\(lhs) > \(rhs)", trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  } else if !(rhs < lhs) {
    expectationFailure("\(lhs) > \(rhs) (flipped)", trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  }
}

public func expectGE<T : Comparable>(_ lhs: T, _ rhs: T,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line) {
  if !(lhs >= rhs) {
    expectationFailure("\(lhs) >= \(rhs)", trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  } else if !(rhs <= lhs) {
    expectationFailure("\(lhs) >= \(rhs) (flipped)", trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  }
}

extension Range {
  internal func _contains(_ other: Range<Bound>) -> Bool {
    if other.lowerBound < lowerBound { return false }
    if upperBound < other.upperBound { return false }
    return true
  }
}
extension Range {
  internal func _contains(_ other: ClosedRange<Bound>) -> Bool {
    if other.lowerBound < lowerBound { return false }
    if upperBound <= other.upperBound { return false }
    return true
  }
}

public func expectTrapping<Bound>(
  _ point: Bound, in range: Range<Bound>,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) {
  if !range.contains(point) {
    expectationFailure("\(point) in \(range)", trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
    _trappingExpectationFailedCallback()
  }
}

public func expectTrapping<Bound>(
  _ subRange: Range<Bound>, in range: Range<Bound>,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) {
  if !range._contains(subRange) {
    expectationFailure("\(subRange) in \(range)", trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
    _trappingExpectationFailedCallback()
  }
}
public func expectTrapping<Bound>(
  _ point: Bound, in range: ClosedRange<Bound>,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) {
  if !range.contains(point) {
    expectationFailure("\(point) in \(range)", trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
    _trappingExpectationFailedCallback()
  }
}

public func expectTrapping<Bound>(
  _ subRange: ClosedRange<Bound>, in range: Range<Bound>,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) {
  if !range._contains(subRange) {
    expectationFailure("\(subRange) in \(range)", trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
    _trappingExpectationFailedCallback()
  }
}

extension ClosedRange {
  internal func _contains(_ other: ClosedRange<Bound>) -> Bool {
    if other.lowerBound < lowerBound { return false }
    if upperBound < other.upperBound { return false }
    return true
  }
}

public func expectTrapping<Bound>(
  _ subRange: ClosedRange<Bound>, in range: ClosedRange<Bound>,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) {
  if !range._contains(subRange) {
    expectationFailure("\(subRange) in \(range)", trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
    _trappingExpectationFailedCallback()
  }
}

public func expectType<T>(_: T.Type, _ x: inout T) {}
public func expectEqualType<T>(_: T.Type, _: T.Type) {}

public func expectSequenceType<X : Sequence>(_ x: X) -> X {
  return x
}

public func expectCollectionType<X : Collection>(
  _ x: X.Type
) {}
public func expectMutableCollectionType<X : MutableCollection>(
  _ x: X.Type
) {}

/// A slice is a `Collection` that when sliced returns an instance of
/// itself.
public func expectSliceType<X : Collection>(
  _ sliceType: X.Type
) where X.SubSequence == X {}

/// A mutable slice is a `MutableCollection` that when sliced returns an
/// instance of itself.
public func expectMutableSliceType<X : MutableCollection>(
  _ mutableSliceType: X.Type
) where X.SubSequence == X {}

/// Check that all associated types of a `Sequence` are what we expect them
/// to be.
public func expectSequenceAssociatedTypes<X : Sequence>(
  sequenceType: X.Type,
  iteratorType: X.Iterator.Type
) {}

/// Check that all associated types of a `Collection` are what we expect them
/// to be.
public func expectCollectionAssociatedTypes<X : Collection>(
  collectionType: X.Type,
  iteratorType: X.Iterator.Type,
  subSequenceType: X.SubSequence.Type,
  indexType: X.Index.Type,
  indicesType: X.Indices.Type
) {}

/// Check that all associated types of a `BidirectionalCollection` are what we
/// expect them to be.
public func expectBidirectionalCollectionAssociatedTypes<X : BidirectionalCollection>(
  collectionType: X.Type,
  iteratorType: X.Iterator.Type,
  subSequenceType: X.SubSequence.Type,
  indexType: X.Index.Type,
  indicesType: X.Indices.Type
) {}

/// Check that all associated types of a `RandomAccessCollection` are what we
/// expect them to be.
public func expectRandomAccessCollectionAssociatedTypes<X : RandomAccessCollection>(
  collectionType: X.Type,
  iteratorType: X.Iterator.Type,
  subSequenceType: X.SubSequence.Type,
  indexType: X.Index.Type,
  indicesType: X.Indices.Type
) {}

public struct AssertionResult : CustomStringConvertible {
  init(isPass: Bool) {
    self._isPass = isPass
  }

  public func withDescription(_ description: String) -> AssertionResult {
    var result = self
    result.description += description
    return result
  }

  let _isPass: Bool

  public var description: String = ""
}

public func assertionSuccess() -> AssertionResult {
  return AssertionResult(isPass: true)
}

public func assertionFailure() -> AssertionResult {
  return AssertionResult(isPass: false)
}

public func expectUnreachable(
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line) {
  expectationFailure("this code should not be executed", trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
}

public func expectUnreachableCatch(_ error: Error,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line) {
  expectationFailure(
    "error should not be thrown: \"\(error)\"", trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
}

public func expectTrue(_ actual: AssertionResult,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line) {
  if !actual._isPass {
    expectationFailure(
      "expected: passed assertion\n\(actual.description)", trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  }
}

public func expectFalse(_ actual: AssertionResult,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line) {
  if actual._isPass {
    expectationFailure(
      "expected: failed assertion\n\(actual.description)", trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  }
}

public func expectTrue(_ actual: Bool,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line) {
  if !actual {
    expectationFailure("expected: true", trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  }
}

public func expectFalse(_ actual: Bool,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line) {
  if actual {
    expectationFailure("expected: false", trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  }
}

public func expectThrows<ErrorType: Error & Equatable>(
  _ expectedError: ErrorType? = nil, _ test: () throws -> Void,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line) {
  do {
    try test()
  } catch let error as ErrorType {
    if let expectedError = expectedError {
      expectEqual(expectedError, error)
    }
  } catch {
    expectationFailure("unexpected error thrown: \"\(error)\"", trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  }
}

public func expectDoesNotThrow(_ test: () throws -> Void,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line) {
  do {
    try test()
  } catch {
    expectationFailure("unexpected error thrown: \"\(error)\"", trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  }
}

public func expectNil<T>(_ value: T?,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line) {
  if value != nil {
    expectationFailure(
      "expected optional to be nil\nactual: \"\(value!)\"", trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  }
}

@discardableResult
public func expectNotNil<T>(_ value: T?,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line) -> T? {
  if value == nil {
    expectationFailure("expected optional to be non-nil", trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  }
  return value
}

public func expectCrashLater(withMessage message: String = "") {
  print("\(_stdlibUnittestStreamPrefix);expectCrash;\(_anyExpectFailed.load())")

  var stderr = _Stderr()
  print("\(_stdlibUnittestStreamPrefix);expectCrash;\(message)", to: &stderr)

  _seenExpectCrash.store(true)
}

public func expectCrash(withMessage message: String = "", executing: () -> Void) -> Never {
  expectCrashLater(withMessage: message)
  executing()
  expectUnreachable()
  fatalError()
}

func _defaultTestSuiteFailedCallback() {
  abort()
}

var _testSuiteFailedCallback: () -> Void = _defaultTestSuiteFailedCallback

public func _setTestSuiteFailedCallback(_ callback: @escaping () -> Void) {
  _testSuiteFailedCallback = callback
}

func _defaultTrappingExpectationFailedCallback() {
  abort()
}

var _trappingExpectationFailedCallback: () -> Void
  = _defaultTrappingExpectationFailedCallback

public func _setTrappingExpectationFailedCallback(callback: @escaping () -> Void) {
  _trappingExpectationFailedCallback = callback
}

extension ProcessTerminationStatus {
  var isSwiftTrap: Bool {
    switch self {
    case .signal(let signal):
#if os(Windows)
      return CInt(signal) == SIGILL
#elseif os(WASI)
      // No signals support on WASI yet, see https://github.com/WebAssembly/WASI/issues/166.
      return false
#else
      return CInt(signal) == SIGILL || CInt(signal) == SIGTRAP
#endif
    default:
      // This default case is needed for standard library builds where
      // resilience is enabled.
      // FIXME: Add the .exit case when there is a way to suppress when not.
      //   case .exit: return false
      return false
    }
  }
}

func _stdlib_getline() -> String? {
  var result: [UInt8] = []
  while true {
    let c = getchar()
    if c == EOF {
      if result.isEmpty {
        return nil
      }
      return String(decoding: result, as: UTF8.self)
    }
    if c == CInt(Unicode.Scalar("\n").value) {
      return String(decoding: result, as: UTF8.self)
    }
    result.append(UInt8(c))
  }
}

func _printDebuggingAdvice(_ fullTestName: String) {
  print("To debug, run:")
  var invocation = [CommandLine.arguments[0]]
#if os(Windows)
  var buffer: UnsafeMutablePointer<CChar>?
  var length: Int = 0
  if _dupenv_s(&buffer, &length, "SWIFT_INTERPRETER") != 0, let buffer = buffer {
    invocation.insert(String(cString: buffer), at: 0)
    free(buffer)
  }
#else
  let interpreter = getenv("SWIFT_INTERPRETER")
  if interpreter != nil {
    if let interpreterCmd = String(validatingUTF8: interpreter!) {
        invocation.insert(interpreterCmd, at: 0)
    }
  }
#endif
  print("$ \(invocation.joined(separator: " ")) " +
    "--stdlib-unittest-in-process --stdlib-unittest-filter \"\(fullTestName)\"")
}

var _allTestSuites: [TestSuite] = []
var _testSuiteNameToIndex: [String : Int] = [:]

let _stdlibUnittestStreamPrefix = "__STDLIB_UNITTEST__"
let _crashedPrefix = "CRASHED:"

@_silgen_name("installTrapInterceptor")
func _installTrapInterceptor()

#if _runtime(_ObjC)
@objc protocol _StdlibUnittestNSException {
  @objc optional var name: AnyObject { get }
}
#endif

// Avoid serializing references to objc_setUncaughtExceptionHandler in SIL.
@inline(never)
func _childProcess() {
  _installTrapInterceptor()

#if _runtime(_ObjC)
  objc_setUncaughtExceptionHandler {
    let exception = ($0 as Optional)! as AnyObject
    var stderr = _Stderr()
    let maybeNSException =
        unsafeBitCast(exception, to: _StdlibUnittestNSException.self)
    if let name = maybeNSException.name {
      print("*** [StdlibUnittest] Terminating due to uncaught exception " +
        "\(name): \(exception)",
        to: &stderr)
    } else {
      print("*** [StdlibUnittest] Terminating due to uncaught exception: " +
        "\(exception)",
        to: &stderr)
    }
  }
#endif

  while let line = _stdlib_getline() {
    let parts = line._split(separator: ";")

    if parts[0] == _stdlibUnittestStreamPrefix {
      precondition(parts[1] == "shutdown")
      return
    }

    let testSuiteName = parts[0]
    let testName = parts[1]
    var testParameter: Int?
    if parts.count > 2 {
      testParameter = Int(parts[2])!
    } else {
      testParameter = nil
    }

    let testSuite = _allTestSuites[_testSuiteNameToIndex[testSuiteName]!]
    _anyExpectFailed.store(false)
    testSuite._runTest(name: testName, parameter: testParameter)

    print("\(_stdlibUnittestStreamPrefix);end;\(_anyExpectFailed.load())")

    var stderr = _Stderr()
    print("\(_stdlibUnittestStreamPrefix);end", to: &stderr)

    if testSuite._shouldShutDownChildProcess(forTestNamed: testName) {
      return
    }
  }
}

#if SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY
@available(SwiftStdlib 5.1, *)
@inline(never)
func _childProcessAsync() async {
  _installTrapInterceptor()

#if _runtime(_ObjC)
  objc_setUncaughtExceptionHandler {
    let exception = ($0 as Optional)! as AnyObject
    var stderr = _Stderr()
    let maybeNSException =
        unsafeBitCast(exception, to: _StdlibUnittestNSException.self)
    if let name = maybeNSException.name {
      print("*** [StdlibUnittest] Terminating due to uncaught exception " +
        "\(name): \(exception)",
        to: &stderr)
    } else {
      print("*** [StdlibUnittest] Terminating due to uncaught exception: " +
        "\(exception)",
        to: &stderr)
    }
  }
#endif

  while let line = _stdlib_getline() {
    let parts = line._split(separator: ";")

    if parts[0] == _stdlibUnittestStreamPrefix {
      precondition(parts[1] == "shutdown")
      return
    }

    let testSuiteName = parts[0]
    let testName = parts[1]
    var testParameter: Int?
    if parts.count > 2 {
      testParameter = Int(parts[2])!
    } else {
      testParameter = nil
    }

    let testSuite = _allTestSuites[_testSuiteNameToIndex[testSuiteName]!]
    _anyExpectFailed.store(false)
    await testSuite._runTestAsync(name: testName, parameter: testParameter)

    print("\(_stdlibUnittestStreamPrefix);end;\(_anyExpectFailed.load())")

    var stderr = _Stderr()
    print("\(_stdlibUnittestStreamPrefix);end", to: &stderr)

    if testSuite._shouldShutDownChildProcess(forTestNamed: testName) {
      return
    }
  }
}
#endif

class _ParentProcess {
#if os(Windows)
  internal var _process: HANDLE = INVALID_HANDLE_VALUE
  internal var _childStdin: _FDOutputStream =
      _FDOutputStream(handle: INVALID_HANDLE_VALUE)
  internal var _childStdout: _FDInputStream =
      _FDInputStream(handle: INVALID_HANDLE_VALUE)
  internal var _childStderr: _FDInputStream =
      _FDInputStream(handle: INVALID_HANDLE_VALUE)
#else
  internal var _pid: pid_t?
  internal var _childStdin: _FDOutputStream = _FDOutputStream(fd: -1)
  internal var _childStdout: _FDInputStream = _FDInputStream(fd: -1)
  internal var _childStderr: _FDInputStream = _FDInputStream(fd: -1)
#endif

  internal var _runTestsInProcess: Bool
  internal var _filter: String?
  internal var _args: [String]

  init(runTestsInProcess: Bool, args: [String], filter: String?) {
    self._runTestsInProcess = runTestsInProcess
    self._filter = filter
    self._args = args
  }

  func _spawnChild() {
    let params = ["--stdlib-unittest-run-child"] + _args
#if os(Windows)
    let (hProcess, hStdIn, hStdOut, hStdErr) = spawnChild(params)
    self._process = hProcess
    self._childStdin = _FDOutputStream(handle: hStdIn)
    self._childStdout = _FDInputStream(handle: hStdOut)
    self._childStderr = _FDInputStream(handle: hStdErr)
#else
    let (pid, childStdinFD, childStdoutFD, childStderrFD) = spawnChild(params)
    _pid = pid
    _childStdin = _FDOutputStream(fd: childStdinFD)
    _childStdout = _FDInputStream(fd: childStdoutFD)
    _childStderr = _FDInputStream(fd: childStderrFD)
#endif
  }

  func _waitForChild() -> ProcessTerminationStatus {
#if os(Windows)
    let status = waitProcess(_process)
    _process = INVALID_HANDLE_VALUE

    _childStdin.close()
    _childStdout.close()
    _childStderr.close()
#else
    let status = posixWaitpid(_pid!)
    _pid = nil
    _childStdin.close()
    _childStdout.close()
    _childStderr.close()
    _childStdin = _FDOutputStream(fd: -1)
    _childStdout = _FDInputStream(fd: -1)
    _childStderr = _FDInputStream(fd: -1)
#endif
    return status
  }

  internal func _readFromChild(
    onStdoutLine: @escaping (String) -> (done: Bool, Void),
    onStderrLine: @escaping (String) -> (done: Bool, Void)
  ) {
#if os(Windows)
    let (_, stdoutThread) = _stdlib_thread_create_block({
      while !self._childStdout.isEOF {
        self._childStdout.read()
        while var line = self._childStdout.getline() {
          if let cr = line.firstIndex(of: "\r") {
            line.remove(at: cr)
          }
          var done: Bool
          (done: done, ()) = onStdoutLine(line)
          if done { return }
        }
      }
    }, ())

    let (_, stderrThread) = _stdlib_thread_create_block({
      while !self._childStderr.isEOF {
        self._childStderr.read()
        while var line = self._childStderr.getline() {
          if let cr = line.firstIndex(of: "\r") {
            line.remove(at: cr)
          }
          var done: Bool
          (done: done, ()) = onStderrLine(line)
          if done { return }
        }
      }
    }, ())

    let (_, _) = _stdlib_thread_join(stdoutThread!, Void.self)
    let (_, _) = _stdlib_thread_join(stderrThread!, Void.self)
#else
    var readfds = _stdlib_fd_set()
    var writefds = _stdlib_fd_set()
    var errorfds = _stdlib_fd_set()
    var done = false
    while !((_childStdout.isEOF && _childStderr.isEOF) || done) {
      readfds.zero()
      errorfds.zero()
      if !_childStdout.isEOF {
        readfds.set(_childStdout.fd)
        errorfds.set(_childStdout.fd)
      }
      if !_childStderr.isEOF {
        readfds.set(_childStderr.fd)
        errorfds.set(_childStderr.fd)
      }
      var ret: CInt
      repeat {
        ret = _stdlib_select(&readfds, &writefds, &errorfds, nil)
      } while ret == -1  &&  errno == EINTR
      if ret <= 0 {
        fatalError("select() returned an error")
      }
      if readfds.isset(_childStdout.fd) || errorfds.isset(_childStdout.fd) {
        _childStdout.read()
        while let line = _childStdout.getline() {
          (done: done, ()) = onStdoutLine(line)
        }
        continue
      }
      if readfds.isset(_childStderr.fd) || errorfds.isset(_childStderr.fd) {
        _childStderr.read()
        while let line = _childStderr.getline() {
          (done: done, ()) = onStderrLine(line)
        }
        continue
      }
    }
#endif
  }

  /// Returns the values of the corresponding variables in the child process.
  internal func _runTestInChild(
    _ testSuite: TestSuite,
    _ testName: String,
    parameter: Int?
  ) -> (anyExpectFailed: Bool, seenExpectCrash: Bool,
        status: ProcessTerminationStatus?,
        crashStdout: [Substring], crashStderr: [Substring]) {
#if os(Windows)
    if _process == INVALID_HANDLE_VALUE {
      _spawnChild()
    }
#else
    if _pid == nil {
      _spawnChild()
    }
#endif

    print("\(testSuite.name);\(testName)", terminator: "", to: &_childStdin)
    if let parameter = parameter {
      print(";", terminator: "", to: &_childStdin)
      print(parameter, terminator: "", to: &_childStdin)
    }
    print("", to: &_childStdin)

    let currentTest = testSuite._testByName(testName)
    if let stdinText = currentTest.stdinText {
      print(stdinText, terminator: "", to: &_childStdin)
    }
    if currentTest.stdinEndsWithEOF {
      _childStdin.close()
    }

    var stdoutSeenCrashDelimiter = false
    var stderrSeenCrashDelimiter = false
    var expectingPreCrashMessage = ""
    var stdoutEnd = false
    var stderrEnd = false
    var capturedCrashStdout: [Substring] = []
    var capturedCrashStderr: [Substring] = []
    var anyExpectFailedInChild = false

    func processLine(_ line: String, isStdout: Bool) -> (done: Bool, Void) {
      var line = line[...]
      if let index = findSubstring(line, _stdlibUnittestStreamPrefix) {
        let controlMessage =
            line[index..<line.endIndex].split(separator: ";",
                              omittingEmptySubsequences: false)
        switch controlMessage[1] {
        case "expectCrash":
          fallthrough
        case "expectCrash\r":
          if isStdout {
            stdoutSeenCrashDelimiter = true
            anyExpectFailedInChild = controlMessage[2] == "true"
          } else {
            stderrSeenCrashDelimiter = true
            expectingPreCrashMessage = String(controlMessage[2])
          }
        case "end":
          fallthrough
        case "end\r":
          if isStdout {
            stdoutEnd = true
            anyExpectFailedInChild = controlMessage[2] == "true"
          } else {
            stderrEnd = true
          }
        default:
          fatalError("unexpected message: \(controlMessage[1])")
        }
        line = line[line.startIndex..<index]
        if line.isEmpty {
#if os(Windows)
          return (done: isStdout ? stdoutEnd : stderrEnd, ())
#else
          return (done: stdoutEnd && stderrEnd, ())
#endif
        }
      }
      if !expectingPreCrashMessage.isEmpty
          && findSubstring(line, expectingPreCrashMessage) != nil {
        line = "OK: saw expected pre-crash message in \"\(line)\""[...]
        expectingPreCrashMessage = ""
      }
      if isStdout {
        if stdoutSeenCrashDelimiter {
          capturedCrashStdout.append(line)
        }
      } else {
        if stderrSeenCrashDelimiter {
          capturedCrashStderr.append(line)
          if findSubstring(line, _crashedPrefix) != nil {
            if !expectingPreCrashMessage.isEmpty {
              line = """
                      FAIL: saw expected "\(line.lowercased())", but without \
                      message "\(expectingPreCrashMessage)" before it
                      """[...]
              anyExpectFailedInChild = true
            }
            else {
              line = "OK: saw expected \"\(line.lowercased())\""[...]
            }
          }
        }
      }
      if isStdout {
        print("stdout>>> \(line)")
      } else {
        print("stderr>>> \(line)")
      }
#if os(Windows)
      return (done: isStdout ? stdoutEnd : stderrEnd, ())
#else
      return (done: stdoutEnd && stderrEnd, ())
#endif
    }

    _readFromChild(
      onStdoutLine: { processLine($0, isStdout: true) },
      onStderrLine: { processLine($0, isStdout: false) })

    // Check if the child has sent us "end" markers for the current test.
    if stdoutEnd && stderrEnd {
      var status: ProcessTerminationStatus?
      if testSuite._shouldShutDownChildProcess(forTestNamed: testName) {
        status = _waitForChild()
        switch status! {
        case .exit(0):
          status = nil
        default:
          ()
        }
      }
      return (
        anyExpectFailedInChild,
        stdoutSeenCrashDelimiter || stderrSeenCrashDelimiter,
        status, capturedCrashStdout, capturedCrashStderr)
    }

    // We reached EOF on stdout and stderr and we did not see "end" markers, so
    // it looks like child crashed (of course it could have closed the file
    // descriptors, but we assume it did not, since it prevent further
    // communication with the parent).
    let status = _waitForChild()
    return (
      anyExpectFailedInChild,
      stdoutSeenCrashDelimiter || stderrSeenCrashDelimiter,
      status, capturedCrashStdout, capturedCrashStderr)
  }

  internal func _shutdownChild() -> (failed: Bool, Void) {
#if os(Windows)
    if _process == INVALID_HANDLE_VALUE {
      // The child process is not running.  Report that it didn't fail during
      // shutdown.
      return (failed: false, ())
    }
#else
    if _pid == nil {
      // The child process is not running.  Report that it didn't fail during
      // shutdown.
      return (failed: false, ())
    }
#endif
    // If the child process expects an EOF, its stdin fd has already been closed and
    // it will shut itself down automatically.
    if !_childStdin.isClosed {
      print("\(_stdlibUnittestStreamPrefix);shutdown", to: &_childStdin)
    }

    var childCrashed = false

    func processLine(_ line: String, isStdout: Bool) -> (done: Bool, Void) {
      if isStdout {
        print("stdout>>> \(line)")
      } else {
        if findSubstring(line, _crashedPrefix) != nil {
          childCrashed = true
        }
        print("stderr>>> \(line)")
      }
      return (done: false, ())
    }

    _readFromChild(
      onStdoutLine: { processLine($0, isStdout: true) },
      onStderrLine: { processLine($0, isStdout: false) })

    let status = _waitForChild()
    switch status {
    case .exit(0):
      return (failed: childCrashed, ())
    default:
      print("Abnormal child process termination: \(status).")
      return (failed: true, ())
    }
  }

  internal enum _TestStatus {
    case skip([TestRunPredicate])
    case pass
    case fail
    case uxPass
    case xFail
  }

  internal func runOneTest(
    fullTestName: String,
    testSuite: TestSuite,
    test t: TestSuite._Test,
    testParameter: Int?
  ) -> _TestStatus {
    let activeSkips = t.getActiveSkipPredicates()
    if !activeSkips.isEmpty {
      return .skip(activeSkips)
    }

    let activeXFails = t.getActiveXFailPredicates()
    let expectXFail = !activeXFails.isEmpty
    let activeXFailsText = expectXFail ? " (XFAIL: \(activeXFails))" : ""
    print("[ RUN      ] \(fullTestName)\(activeXFailsText)")

    var expectCrash = false
    var childTerminationStatus: ProcessTerminationStatus?
    var crashStdout: [Substring] = []
    var crashStderr: [Substring] = []
    if _runTestsInProcess {
      if t.stdinText != nil {
        print("The test \(fullTestName) requires stdin input and can't be run in-process, marking as failed")
        _anyExpectFailed.store(true)
      } else if t.requiresOwnProcess {
        print("The test \(fullTestName) requires running in a child process and can't be run in-process, marking as failed.")
        _anyExpectFailed.store(true)
      } else {
        _anyExpectFailed.store(false)
        testSuite._runTest(name: t.name, parameter: testParameter)
      }
    } else {
      var anyExpectFailed = false
      (anyExpectFailed, expectCrash, childTerminationStatus, crashStdout,
       crashStderr) =
        _runTestInChild(testSuite, t.name, parameter: testParameter)
      _anyExpectFailed.store(anyExpectFailed)
    }

    // Determine if the test passed, not taking XFAILs into account.
    var testPassed = false
    switch (childTerminationStatus, expectCrash) {
    case (.none, false):
      testPassed = !_anyExpectFailed.load()

    case (.none, true):
      testPassed = false
      print("expecting a crash, but the test did not crash")

    case (.some, false):
      testPassed = false
      print("the test crashed unexpectedly")

    case (.some, true):
      testPassed = !_anyExpectFailed.load()
    }
    if testPassed && t.crashOutputMatches.count > 0 {
      // If we still think that the test passed, check if the crash
      // output matches our expectations.
      let crashOutput = crashStdout + crashStderr
      for expectedSubstring in t.crashOutputMatches {
        var found = false
        for s in crashOutput {
          if findSubstring(s, expectedSubstring) != nil {
            found = true
            break
          }
        }
        if !found {
          print("did not find expected string after crash: \(expectedSubstring.debugDescription)")
          testPassed = false
        }
      }
    }

    // Apply XFAILs.
    switch (testPassed, expectXFail) {
    case (true, false):
      return .pass

    case (true, true):
      return .uxPass

    case (false, false):
      return .fail

    case (false, true):
      return .xFail
    }
  }

#if SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY
  @available(SwiftStdlib 5.1, *)
  internal func runOneTestAsync(
    fullTestName: String,
    testSuite: TestSuite,
    test t: TestSuite._Test,
    testParameter: Int?
  ) async -> _TestStatus {
    let activeSkips = t.getActiveSkipPredicates()
    if !activeSkips.isEmpty {
      return .skip(activeSkips)
    }

    let activeXFails = t.getActiveXFailPredicates()
    let expectXFail = !activeXFails.isEmpty
    let activeXFailsText = expectXFail ? " (XFAIL: \(activeXFails))" : ""
    print("[ RUN      ] \(fullTestName)\(activeXFailsText)")

    var expectCrash = false
    var childTerminationStatus: ProcessTerminationStatus?
    var crashStdout: [Substring] = []
    var crashStderr: [Substring] = []
    if _runTestsInProcess {
      if t.stdinText != nil {
        print("The test \(fullTestName) requires stdin input and can't be run in-process, marking as failed")
        _anyExpectFailed.store(true)
      } else if t.requiresOwnProcess {
        print("The test \(fullTestName) requires running in a child process and can't be run in-process, marking as failed.")
        _anyExpectFailed.store(true)
      } else {
        _anyExpectFailed.store(false)
        await testSuite._runTestAsync(name: t.name, parameter: testParameter)
      }
    } else {
      var anyExpectFailed = false
      (anyExpectFailed, expectCrash, childTerminationStatus, crashStdout,
       crashStderr) =
        _runTestInChild(testSuite, t.name, parameter: testParameter)
      _anyExpectFailed.store(anyExpectFailed)
    }

    // Determine if the test passed, not taking XFAILs into account.
    var testPassed = false
    switch (childTerminationStatus, expectCrash) {
    case (.none, false):
      testPassed = !_anyExpectFailed.load()

    case (.none, true):
      testPassed = false
      print("expecting a crash, but the test did not crash")

    case (.some, false):
      testPassed = false
      print("the test crashed unexpectedly")

    case (.some, true):
      testPassed = !_anyExpectFailed.load()
    }
    if testPassed && t.crashOutputMatches.count > 0 {
      // If we still think that the test passed, check if the crash
      // output matches our expectations.
      let crashOutput = crashStdout + crashStderr
      for expectedSubstring in t.crashOutputMatches {
        var found = false
        for s in crashOutput {
          if findSubstring(s, expectedSubstring) != nil {
            found = true
            break
          }
        }
        if !found {
          print("did not find expected string after crash: \(expectedSubstring.debugDescription)")
          testPassed = false
        }
      }
    }

    // Apply XFAILs.
    switch (testPassed, expectXFail) {
    case (true, false):
      return .pass

    case (true, true):
      return .uxPass

    case (false, false):
      return .fail

    case (false, true):
      return .xFail
    }
  }
#endif


  func run() {
    if let filter = _filter {
      print("StdlibUnittest: using filter: \(filter)")
    }
    for testSuite in _allTestSuites {
      var uxpassedTests: [String] = []
      var failedTests: [String] = []
      var skippedTests: [String] = []
      for t in testSuite._tests {
        for testParameter in t.parameterValues {
          var testName = t.name
          if let testParameter = testParameter {
            testName += "/"
            testName += String(testParameter)
          }
          let fullTestName = "\(testSuite.name).\(testName)"
          if let filter = _filter,
             findSubstring(fullTestName, filter) == nil {

            continue
          }

          switch runOneTest(
            fullTestName: fullTestName,
            testSuite: testSuite,
            test: t,
            testParameter: testParameter
          ) {
          case .skip(let activeSkips):
            skippedTests.append(testName)
            print("[ SKIP     ] \(fullTestName) (skip: \(activeSkips))")

          case .pass:
            print("[       OK ] \(fullTestName)")

          case .uxPass:
            uxpassedTests.append(testName)
            print("[   UXPASS ] \(fullTestName)")

          case .fail:
            failedTests.append(testName)
            print("[     FAIL ] \(fullTestName)")

          case .xFail:
            print("[    XFAIL ] \(fullTestName)")
          }
        }
      }

      if !uxpassedTests.isEmpty || !failedTests.isEmpty {
        print("\(testSuite.name): Some tests failed, aborting")
        print("UXPASS: \(uxpassedTests)")
        print("FAIL: \(failedTests)")
        print("SKIP: \(skippedTests)")
        if !uxpassedTests.isEmpty {
          _printDebuggingAdvice(uxpassedTests[0])
        }
        if !failedTests.isEmpty {
          _printDebuggingAdvice(failedTests[0])
        }
        _testSuiteFailedCallback()
      } else {
        print("\(testSuite.name): All tests passed")
      }
    }
    let (failed: failedOnShutdown, ()) = _shutdownChild()
    if failedOnShutdown {
      print("The child process failed during shutdown, aborting.")
      _testSuiteFailedCallback()
    }
  }

#if SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY
  @available(SwiftStdlib 5.1, *)
  func runAsync() async {
    if let filter = _filter {
      print("StdlibUnittest: using filter: \(filter)")
    }
    for testSuite in _allTestSuites {
      var uxpassedTests: [String] = []
      var failedTests: [String] = []
      var skippedTests: [String] = []
      for t in testSuite._tests {
        for testParameter in t.parameterValues {
          var testName = t.name
          if let testParameter = testParameter {
            testName += "/"
            testName += String(testParameter)
          }
          let fullTestName = "\(testSuite.name).\(testName)"
          if let filter = _filter,
             findSubstring(fullTestName, filter) == nil {

            continue
          }

          switch await runOneTestAsync(
            fullTestName: fullTestName,
            testSuite: testSuite,
            test: t,
            testParameter: testParameter
          ) {
          case .skip(let activeSkips):
            skippedTests.append(testName)
            print("[ SKIP     ] \(fullTestName) (skip: \(activeSkips))")

          case .pass:
            print("[       OK ] \(fullTestName)")

          case .uxPass:
            uxpassedTests.append(testName)
            print("[   UXPASS ] \(fullTestName)")

          case .fail:
            failedTests.append(testName)
            print("[     FAIL ] \(fullTestName)")

          case .xFail:
            print("[    XFAIL ] \(fullTestName)")
          }
        }
      }

      if !uxpassedTests.isEmpty || !failedTests.isEmpty {
        print("\(testSuite.name): Some tests failed, aborting")
        print("UXPASS: \(uxpassedTests)")
        print("FAIL: \(failedTests)")
        print("SKIP: \(skippedTests)")
        if !uxpassedTests.isEmpty {
          _printDebuggingAdvice(uxpassedTests[0])
        }
        if !failedTests.isEmpty {
          _printDebuggingAdvice(failedTests[0])
        }
        _testSuiteFailedCallback()
      } else {
        print("\(testSuite.name): All tests passed")
      }
    }
    let (failed: failedOnShutdown, ()) = _shutdownChild()
    if failedOnShutdown {
      print("The child process failed during shutdown, aborting.")
      _testSuiteFailedCallback()
    }
  }
#endif

}

// Track repeated calls to runAllTests() and/or runNoTests().
// Complain if a file runs no tests without calling runNoTests().
struct PersistentState {
  static var runAllTestsWasCalled: Bool = false
  static var runNoTestsWasCalled: Bool = false
  static var ranSomething: Bool = false
  static var complaintInstalled = false
  static var hashingKeyOverridden = false

  static func complainIfNothingRuns() {
    if !complaintInstalled {
      complaintInstalled = true
      atexit {
        if !PersistentState.ranSomething {
          print("Ran no tests and runNoTests() was not called. Aborting. ")
          print("Did you forget to call runAllTests()?")
          _testSuiteFailedCallback()
        }
      }
    }
  }
}


// Call runNoTests() if you want to deliberately run no tests.
public func runNoTests() {
  if PersistentState.runAllTestsWasCalled {
    print("runNoTests() called after runAllTests(). Aborting.")
    _testSuiteFailedCallback()
    return
  }
  if PersistentState.runNoTestsWasCalled {
    print("runNoTests() called twice. Aborting.")
    _testSuiteFailedCallback()
    return
  }
  PersistentState.runNoTestsWasCalled = true
  PersistentState.ranSomething = true
}

public func runAllTests() {
  if PersistentState.runNoTestsWasCalled {
    print("runAllTests() called after runNoTests(). Aborting.")
    _testSuiteFailedCallback()
    return
  }
  if PersistentState.runAllTestsWasCalled {
    print("runAllTests() called twice. Aborting.")
    _testSuiteFailedCallback()
    return
  }
  PersistentState.runAllTestsWasCalled = true
  PersistentState.ranSomething = true

#if _runtime(_ObjC)
  autoreleasepool {
    _stdlib_initializeReturnAutoreleased()
  }
#endif

  let _isChildProcess: Bool =
    CommandLine.arguments.contains("--stdlib-unittest-run-child")

  if _isChildProcess {
    _childProcess()
  } else {
    var runTestsInProcess: Bool = false
    var filter: String?
    var args = [String]()
    var i = 0
    i += 1 // Skip the name of the executable.
    while i < CommandLine.arguments.count {
      let arg = CommandLine.arguments[i]
      if arg == "--stdlib-unittest-in-process" {
        runTestsInProcess = true
        i += 1
        continue
      }
      if arg == "--stdlib-unittest-filter" {
        filter = CommandLine.arguments[i + 1]
        i += 2
        continue
      }
      if arg == "--help" {
        let message =
"optional arguments:\n" +
"--stdlib-unittest-in-process\n" +
"                        run tests in-process without intercepting crashes.\n" +
"                        Useful for running under a debugger.\n" +
"--stdlib-unittest-filter FILTER-STRING\n" +
"                        only run tests whose names contain FILTER-STRING as\n" +
"                        a substring."
        print(message)
        return
      }

      // Pass through unparsed arguments to the child process.
      args.append(CommandLine.arguments[i])

      i += 1
    }

    let parent = _ParentProcess(
      runTestsInProcess: runTestsInProcess, args: args, filter: filter)
    parent.run()
  }
}

#if SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY
@available(SwiftStdlib 5.1, *)
public func runAllTestsAsync() async {
  if PersistentState.runNoTestsWasCalled {
    print("runAllTests() called after runNoTests(). Aborting.")
    _testSuiteFailedCallback()
    return
  }
  if PersistentState.runAllTestsWasCalled {
    print("runAllTests() called twice. Aborting.")
    _testSuiteFailedCallback()
    return
  }
  PersistentState.runAllTestsWasCalled = true
  PersistentState.ranSomething = true

#if _runtime(_ObjC)
  autoreleasepool {
    _stdlib_initializeReturnAutoreleased()
  }
#endif

  let _isChildProcess: Bool =
    CommandLine.arguments.contains("--stdlib-unittest-run-child")

  if _isChildProcess {
    await _childProcessAsync()
  } else {
    var runTestsInProcess: Bool = false
    var filter: String?
    var args = [String]()
    var i = 0
    i += 1 // Skip the name of the executable.
    while i < CommandLine.arguments.count {
      let arg = CommandLine.arguments[i]
      if arg == "--stdlib-unittest-in-process" {
        runTestsInProcess = true
        i += 1
        continue
      }
      if arg == "--stdlib-unittest-filter" {
        filter = CommandLine.arguments[i + 1]
        i += 2
        continue
      }
      if arg == "--help" {
        let message =
"optional arguments:\n" +
"--stdlib-unittest-in-process\n" +
"                        run tests in-process without intercepting crashes.\n" +
"                        Useful for running under a debugger.\n" +
"--stdlib-unittest-filter FILTER-STRING\n" +
"                        only run tests whose names contain FILTER-STRING as\n" +
"                        a substring."
        print(message)
        return
      }

      // Pass through unparsed arguments to the child process.
      args.append(CommandLine.arguments[i])

      i += 1
    }

    let parent = _ParentProcess(
      runTestsInProcess: runTestsInProcess, args: args, filter: filter)
    await parent.runAsync()
  }
}
#endif

#if SWIFT_RUNTIME_ENABLE_LEAK_CHECKER

@_silgen_name("_swift_leaks_startTrackingObjects")
func startTrackingObjects(_: UnsafePointer<CChar>)
@_silgen_name("_swift_leaks_stopTrackingObjects")
func stopTrackingObjects(_: UnsafePointer<CChar>) -> Int

#endif

public final class TestSuite {
  public init(_ name: String) {
    self.name = name
    precondition(
      _testNameToIndex[name] == nil,
      "test suite with the same name already exists")
    _allTestSuites.append(self)
    _testSuiteNameToIndex[name] = _allTestSuites.count - 1
    PersistentState.complainIfNothingRuns()
  }

  // This method is prohibited from inlining because inlining the test harness
  // into the test is not interesting from the runtime performance perspective.
  // And it does not really make the test cases more effectively at testing the
  // optimizer from a correctness prospective. On the contrary, it sometimes
  // severely affects the compile time of the test code.
  @inline(never)
  public func test(
    _ name: String,
    file: String = #file, line: UInt = #line,
    _ testFunction: @escaping () -> Void
  ) {
    _TestBuilder(testSuite: self, name: name, loc: SourceLoc(file, line))
    .code(testFunction)
  }

#if SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY
  // This method is prohibited from inlining because inlining the test harness
  // into the test is not interesting from the runtime performance perspective.
  // And it does not really make the test cases more effectively at testing the
  // optimizer from a correctness prospective. On the contrary, it sometimes
  // severely affects the compile time of the test code.
  @inline(never)
  public func test(
    _ name: String,
    file: String = #file, line: UInt = #line,
    _ testFunction: @escaping () async -> Void
  ) {
    _TestBuilder(testSuite: self, name: name, loc: SourceLoc(file, line))
    .code(testFunction)
  }
#endif

  // This method is prohibited from inlining because inlining the test harness
  // into the test is not interesting from the runtime performance perspective.
  // And it does not really make the test cases more effectively at testing the
  // optimizer from a correctness prospective. On the contrary, it sometimes
  // severely affects the compile time of the test code.
  @inline(never)
  public func test(
    _ name: String, file: String = #file, line: UInt = #line
  ) -> _TestBuilder {
    return _TestBuilder(testSuite: self, name: name, loc: SourceLoc(file, line))
  }

  public func setUp(_ code: @escaping () -> Void) {
    precondition(_testSetUpCode == nil, "set-up code already set")
    _testSetUpCode = code
  }

  public func tearDown(_ code: @escaping () -> Void) {
    precondition(_testTearDownCode == nil, "tear-down code already set")
    _testTearDownCode = code
  }

  func _runTest(name testName: String, parameter: Int?) {
    PersistentState.ranSomething = true
    for r in _allResettables {
      r.reset()
    }
    LifetimeTracked.instances = 0
    if let f = _testSetUpCode {
      f()
    }
    let test = _testByName(testName)

#if SWIFT_RUNTIME_ENABLE_LEAK_CHECKER
    startTrackingObjects(name)
#endif

    switch test.code {
    case .single(let code):
      precondition(
        parameter == nil,
        "can't pass parameters to non-parameterized tests")
      code()
    case .parameterized(code: let code, _):
      code(parameter!)
#if SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY
    case .singleAsync(_):
      fatalError("Cannot call async code, use `runAllTestsAsync`")
    case .parameterizedAsync(code: _, _):
      fatalError("Cannot call async code, use `runAllTestsAsync`")
#endif
    }

#if SWIFT_RUNTIME_ENABLE_LEAK_CHECKER
    _ = stopTrackingObjects(name)
#endif

    if let f = _testTearDownCode {
      f()
    }
    expectEqual(
      0, LifetimeTracked.instances, "Found leaked LifetimeTracked instances.",
      file: test.testLoc.file, line: test.testLoc.line)
  }

#if SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY
  @available(SwiftStdlib 5.1, *)
  func _runTestAsync(name testName: String, parameter: Int?) async {
    PersistentState.ranSomething = true
    for r in _allResettables {
      r.reset()
    }
    LifetimeTracked.instances = 0
    if let f = _testSetUpCode {
      f()
    }
    let test = _testByName(testName)

#if SWIFT_RUNTIME_ENABLE_LEAK_CHECKER
    startTrackingObjects(name)
#endif

    switch test.code {
    case .single(let code):
      precondition(
        parameter == nil,
        "can't pass parameters to non-parameterized tests")
      code()
    case .parameterized(code: let code, _):
      code(parameter!)
#if SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY
    case .singleAsync(let code):
      precondition(
        parameter == nil,
        "can't pass parameters to non-parameterized tests")
      await code()
    case .parameterizedAsync(code: let code, _):
      await code(parameter!)
#endif
    }

#if SWIFT_RUNTIME_ENABLE_LEAK_CHECKER
    _ = stopTrackingObjects(name)
#endif

    if let f = _testTearDownCode {
      f()
    }
    expectEqual(
      0, LifetimeTracked.instances, "Found leaked LifetimeTracked instances.",
      file: test.testLoc.file, line: test.testLoc.line)
  }
#endif

  func _testByName(_ testName: String) -> _Test {
    return _tests[_testNameToIndex[testName]!]
  }

  /// Determines if we should shut down the current test process, i.e. if this
  /// test or the next test requires executing in its own process.
  func _shouldShutDownChildProcess(forTestNamed testName: String) -> Bool {
    let index = _testNameToIndex[testName]!
    if index == _tests.count - 1 { return false }
    let currentTest = _tests[index]
    let nextTest = _tests[index + 1]
    if !currentTest.canReuseChildProcessAfterTest { return true }
    return currentTest.requiresOwnProcess || nextTest.requiresOwnProcess
  }

  internal enum _TestCode {
    case single(code: () -> Void)
    case parameterized(code: (Int) -> Void, count: Int)
#if SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY
    case singleAsync(code: () async -> Void)
    case parameterizedAsync(code: (Int) async -> Void, count: Int)
#endif
  }

  internal struct _Test {
    let name: String
    let testLoc: SourceLoc
    let xfail: [TestRunPredicate]
    let skip: [TestRunPredicate]
    let stdinText: String?
    let stdinEndsWithEOF: Bool
    let crashOutputMatches: [String]
    let code: _TestCode
    let requiresOwnProcess: Bool

    /// Whether the test harness should stop reusing the child process after
    /// running this test.
    var canReuseChildProcessAfterTest: Bool {
      return stdinText == nil
    }

    func getActiveXFailPredicates() -> [TestRunPredicate] {
      return xfail.filter { $0.evaluate() }
    }

    func getActiveSkipPredicates() -> [TestRunPredicate] {
      return skip.filter { $0.evaluate() }
    }

    var parameterValues: [Int?] {
      switch code {
      case .single:
        return [nil]
      case .parameterized(code: _, count: let count):
        return Array(0..<count)
#if SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY
      case .singleAsync:
        return [nil]
      case .parameterizedAsync(code: _, count: let count):
        return Array(0..<count)
#endif
      }
    }
  }

  public struct _TestBuilder {
    let _testSuite: TestSuite
    var _name: String
    var _data: _Data = _Data()

    internal final class _Data {
      var _xfail: [TestRunPredicate] = []
      var _skip: [TestRunPredicate] = []
      var _stdinText: String?
      var _stdinEndsWithEOF: Bool = false
      var _crashOutputMatches: [String] = []
      var _testLoc: SourceLoc?
      var _requiresOwnProcess: Bool = false
    }

    init(testSuite: TestSuite, name: String, loc: SourceLoc) {
      _testSuite = testSuite
      _name = name
      _data._testLoc = loc
    }

    public func xfail(_ predicate: TestRunPredicate) -> _TestBuilder {
      _data._xfail.append(predicate)
      return self
    }

    public func skip(_ predicate: TestRunPredicate) -> _TestBuilder {
      _data._skip.append(predicate)
      return self
    }

    public func stdin(_ stdinText: String, eof: Bool = false) -> _TestBuilder {
      _data._stdinText = stdinText
      _data._stdinEndsWithEOF = eof
      return self
    }

    public func crashOutputMatches(_ string: String) -> _TestBuilder {
      _data._crashOutputMatches.append(string)
      return self
    }

    public func requireOwnProcess() -> _TestBuilder {
      _data._requiresOwnProcess = true
      return self
    }

    internal func _build(_ testCode: _TestCode) {
      _testSuite._tests.append(
        _Test(
          name: _name, testLoc: _data._testLoc!, xfail: _data._xfail,
          skip: _data._skip,
          stdinText: _data._stdinText,
          stdinEndsWithEOF: _data._stdinEndsWithEOF,
          crashOutputMatches: _data._crashOutputMatches,
          code: testCode,
          requiresOwnProcess: _data._requiresOwnProcess))
      _testSuite._testNameToIndex[_name] = _testSuite._tests.count - 1
    }

    public func code(_ testFunction: @escaping () -> Void) {
      _build(.single(code: testFunction))
    }

#if SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY
    public func code(_ testFunction: @escaping () async -> Void) {
      _build(.singleAsync(code: testFunction))
    }
#endif

    public func forEach<Data>(
      in parameterSets: [Data],
      testFunction: @escaping (Data) -> Void
    ) {
      _build(.parameterized(
        code: { (i: Int) in testFunction(parameterSets[i]) },
        count: parameterSets.count))
    }

#if SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY
    public func forEach<Data>(
      in parameterSets: [Data],
      testFunction: @escaping (Data) async -> Void
    ) {
      _build(.parameterizedAsync(
        code: { (i: Int) in await testFunction(parameterSets[i]) },
        count: parameterSets.count))
    }
#endif

  }

  var name: String
  var _tests: [_Test] = []

  /// Code that is run before every test.
  var _testSetUpCode: (() -> Void)?

  /// Code that is run after every test.
  var _testTearDownCode: (() -> Void)?

  /// Maps test name to index in `_tests`.
  var _testNameToIndex: [String : Int] = [:]
}

#if canImport(Darwin)
#if _runtime(_ObjC)
func _getSystemVersionPlistProperty(_ propertyName: String) -> String? {
  return NSDictionary(contentsOfFile: "/System/Library/CoreServices/SystemVersion.plist")?[propertyName] as? String
}
#else
func _getSystemVersionPlistProperty(_ propertyName: String) -> String? {
  var count = 0
  sysctlbyname("kern.osproductversion", nil, &count, nil, 0)
  return withUnsafeTemporaryAllocation(of: CChar.self, capacity: count) {
    sysctlbyname("kern.osproductversion", $0.baseAddress, &count, nil, 0)
    return String(cString: $0.baseAddress!)
  }
}
#endif
#endif

public enum OSVersion : CustomStringConvertible {
  case osx(major: Int, minor: Int, bugFix: Int)
  case iOS(major: Int, minor: Int, bugFix: Int)
  case tvOS(major: Int, minor: Int, bugFix: Int)
  case watchOS(major: Int, minor: Int, bugFix: Int)
  case iOSSimulator
  case tvOSSimulator
  case watchOSSimulator
  case linux
  case freeBSD
  case openBSD
  case android
  case ps4
  case windowsCygnus
  case windows
  case haiku
  case wasi

  public var description: String {
    switch self {
    case .osx(let major, let minor, let bugFix):
      return "OS X \(major).\(minor).\(bugFix)"
    case .iOS(let major, let minor, let bugFix):
      return "iOS \(major).\(minor).\(bugFix)"
    case .tvOS(let major, let minor, let bugFix):
      return "TVOS \(major).\(minor).\(bugFix)"
    case .watchOS(let major, let minor, let bugFix):
      return "watchOS \(major).\(minor).\(bugFix)"
    case .iOSSimulator:
      return "iOSSimulator"
    case .tvOSSimulator:
      return "TVOSSimulator"
    case .watchOSSimulator:
      return "watchOSSimulator"
    case .linux:
      return "Linux"
    case .freeBSD:
      return "FreeBSD"
    case .openBSD:
      return "OpenBSD"
    case .ps4:
      return "PS4"
    case .android:
      return "Android"
    case .windowsCygnus:
      return "Cygwin"
    case .windows:
      return "Windows"
    case .haiku:
      return "Haiku"
    case .wasi:
      return "WASI"
    }
  }
}

func _parseDottedVersion(_ s: String) -> [Int] {
  return Array(s._split(separator: ".").lazy.map { Int($0)! })
}

public func _parseDottedVersionTriple(_ s: String) -> (Int, Int, Int) {
  let array = _parseDottedVersion(s)
  if array.count >= 4 {
    fatalError("unexpected version")
  }
  return (
    array.count >= 1 ? array[0] : 0,
    array.count >= 2 ? array[1] : 0,
    array.count >= 3 ? array[2] : 0)
}

func _getOSVersion() -> OSVersion {
#if os(iOS) && targetEnvironment(simulator)
  // On simulator, the plist file that we try to read turns out to be host's
  // plist file, which indicates OS X.
  //
  // FIXME: how to get the simulator version *without* UIKit?
  return .iOSSimulator
#elseif os(tvOS) && targetEnvironment(simulator)
  return .tvOSSimulator
#elseif os(watchOS) && targetEnvironment(simulator)
  return .watchOSSimulator
#elseif os(Linux)
  return .linux
#elseif os(FreeBSD)
  return .freeBSD
#elseif os(OpenBSD)
  return .openBSD
#elseif os(PS4)
  return .ps4
#elseif os(Android)
  return .android
#elseif os(Cygwin)
  return .windowsCygnus
#elseif os(Windows)
  return .windows
#elseif os(Haiku)
  return .haiku
#elseif os(WASI)
  return .wasi
#else
  let productVersion = _getSystemVersionPlistProperty("ProductVersion")!
  let (major, minor, bugFix) = _parseDottedVersionTriple(productVersion)
  #if os(macOS)
  return .osx(major: major, minor: minor, bugFix: bugFix)
  #elseif os(iOS)
  return .iOS(major: major, minor: minor, bugFix: bugFix)
  #elseif os(tvOS)
  return .tvOS(major: major, minor: minor, bugFix: bugFix)
  #elseif os(watchOS)
  return .watchOS(major: major, minor: minor, bugFix: bugFix)
  #else
  fatalError("could not determine OS version")
  #endif
#endif
}

var _runningOSVersion: OSVersion = _getOSVersion()
var _overrideOSVersion: OSVersion?

/// Override the OS version for testing.
public func _setOverrideOSVersion(_ v: OSVersion) {
  _overrideOSVersion = v
}

func _getRunningOSVersion() -> OSVersion {
  // Allow overriding the OS version for testing.
  return _overrideOSVersion ?? _runningOSVersion
}

public enum TestRunPredicate : CustomStringConvertible {
  case custom(() -> Bool, reason: String)

  case always(/*reason:*/ String)
  case never

  case osxAny(/*reason:*/ String)
  case osxMajor(Int, reason: String)
  case osxMinor(Int, Int, reason: String)
  case osxMinorRange(Int, ClosedRange<Int>, reason: String)
  case osxBugFix(Int, Int, Int, reason: String)
  case osxBugFixRange(Int, Int, ClosedRange<Int>, reason: String)

  case iOSAny(/*reason:*/ String)
  case iOSMajor(Int, reason: String)
  case iOSMajorRange(ClosedRange<Int>, reason: String)
  case iOSMinor(Int, Int, reason: String)
  case iOSMinorRange(Int, ClosedRange<Int>, reason: String)
  case iOSBugFix(Int, Int, Int, reason: String)
  case iOSBugFixRange(Int, Int, ClosedRange<Int>, reason: String)

  case iOSSimulatorAny(/*reason:*/ String)

  case tvOSAny(/*reason:*/ String)
  case tvOSMajor(Int, reason: String)
  case tvOSMajorRange(ClosedRange<Int>, reason: String)
  case tvOSMinor(Int, Int, reason: String)
  case tvOSMinorRange(Int, ClosedRange<Int>, reason: String)
  case tvOSBugFix(Int, Int, Int, reason: String)
  case tvOSBugFixRange(Int, Int, ClosedRange<Int>, reason: String)

  case tvOSSimulatorAny(/*reason:*/ String)

  case watchOSAny(/*reason:*/ String)
  case watchOSMajor(Int, reason: String)
  case watchOSMajorRange(ClosedRange<Int>, reason: String)
  case watchOSMinor(Int, Int, reason: String)
  case watchOSMinorRange(Int, ClosedRange<Int>, reason: String)
  case watchOSBugFix(Int, Int, Int, reason: String)
  case watchOSBugFixRange(Int, Int, ClosedRange<Int>, reason: String)

  case watchOSSimulatorAny(/*reason:*/ String)

  case linuxAny(reason: String)

  case freeBSDAny(reason: String)

  case ps4Any(reason: String)

  case androidAny(reason: String)

  case windowsAny(reason: String)

  case windowsCygnusAny(reason: String)

  case haikuAny(reason: String)

  case objCRuntime(/*reason:*/ String)
  case nativeRuntime(/*reason:*/ String)

  public var description: String {
    switch self {
    case .custom(_, let reason):
      return "Custom(reason: \(reason))"

    case .always(let reason):
      return "Always(reason: \(reason))"
    case .never:
      return ""

    case .osxAny(let reason):
      return "osx(*, reason: \(reason))"
    case .osxMajor(let major, let reason):
      return "osx(\(major).*, reason: \(reason))"
    case .osxMinor(let major, let minor, let reason):
      return "osx(\(major).\(minor), reason: \(reason))"
    case .osxMinorRange(let major, let minorRange, let reason):
      return "osx(\(major).[\(minorRange)], reason: \(reason))"
    case .osxBugFix(let major, let minor, let bugFix, let reason):
      return "osx(\(major).\(minor).\(bugFix), reason: \(reason))"
    case .osxBugFixRange(let major, let minor, let bugFixRange, let reason):
      return "osx(\(major).\(minor).[\(bugFixRange)], reason: \(reason))"

    case .iOSAny(let reason):
      return "iOS(*, reason: \(reason))"
    case .iOSMajor(let major, let reason):
      return "iOS(\(major).*, reason: \(reason))"
    case .iOSMajorRange(let range, let reason):
      return "iOS([\(range)], reason: \(reason))"
    case .iOSMinor(let major, let minor, let reason):
      return "iOS(\(major).\(minor), reason: \(reason))"
    case .iOSMinorRange(let major, let minorRange, let reason):
      return "iOS(\(major).[\(minorRange)], reason: \(reason))"
    case .iOSBugFix(let major, let minor, let bugFix, let reason):
      return "iOS(\(major).\(minor).\(bugFix), reason: \(reason))"
    case .iOSBugFixRange(let major, let minor, let bugFixRange, let reason):
      return "iOS(\(major).\(minor).[\(bugFixRange)], reason: \(reason))"

    case .iOSSimulatorAny(let reason):
      return "iOSSimulatorAny(*, reason: \(reason))"

    case .tvOSAny(let reason):
      return "tvOS(*, reason: \(reason))"
    case .tvOSMajor(let major, let reason):
      return "tvOS(\(major).*, reason: \(reason))"
    case .tvOSMajorRange(let range, let reason):
      return "tvOS([\(range)], reason: \(reason))"
    case .tvOSMinor(let major, let minor, let reason):
      return "tvOS(\(major).\(minor), reason: \(reason))"
    case .tvOSMinorRange(let major, let minorRange, let reason):
      return "tvOS(\(major).[\(minorRange)], reason: \(reason))"
    case .tvOSBugFix(let major, let minor, let bugFix, let reason):
      return "tvOS(\(major).\(minor).\(bugFix), reason: \(reason))"
    case .tvOSBugFixRange(let major, let minor, let bugFixRange, let reason):
      return "tvOS(\(major).\(minor).[\(bugFixRange)], reason: \(reason))"

    case .tvOSSimulatorAny(let reason):
      return "tvOSSimulatorAny(*, reason: \(reason))"

    case .watchOSAny(let reason):
      return "watchOS(*, reason: \(reason))"
    case .watchOSMajor(let major, let reason):
      return "watchOS(\(major).*, reason: \(reason))"
    case .watchOSMajorRange(let range, let reason):
      return "watchOS([\(range)], reason: \(reason))"
    case .watchOSMinor(let major, let minor, let reason):
      return "watchOS(\(major).\(minor), reason: \(reason))"
    case .watchOSMinorRange(let major, let minorRange, let reason):
      return "watchOS(\(major).[\(minorRange)], reason: \(reason))"
    case .watchOSBugFix(let major, let minor, let bugFix, let reason):
      return "watchOS(\(major).\(minor).\(bugFix), reason: \(reason))"
    case .watchOSBugFixRange(let major, let minor, let bugFixRange, let reason):
      return "watchOS(\(major).\(minor).[\(bugFixRange)], reason: \(reason))"

    case .watchOSSimulatorAny(let reason):
      return "watchOSSimulatorAny(*, reason: \(reason))"

    case .linuxAny(reason: let reason):
      return "linuxAny(*, reason: \(reason))"

    case .androidAny(reason: let reason):
      return "androidAny(*, reason: \(reason))"

    case .freeBSDAny(reason: let reason):
      return "freeBSDAny(*, reason: \(reason))"

    case .ps4Any(reason: let reason):
      return "ps4Any(*, reason: \(reason))"

    case .windowsAny(reason: let reason):
      return "windowsAny(*, reason: \(reason))"

    case .windowsCygnusAny(reason: let reason):
      return "windowsCygnusAny(*, reason: \(reason))"

    case .haikuAny(reason: let reason):
      return "haikuAny(*, reason: \(reason))"

    case .objCRuntime(let reason):
      return "Objective-C runtime, reason: \(reason))"
    case .nativeRuntime(let reason):
      return "Native runtime (no ObjC), reason: \(reason))"
    }
  }

  public func evaluate() -> Bool {
    switch self {
    case .custom(let predicate, _):
      return predicate()

    case .always:
      return true
    case .never:
      return false

    case .osxAny:
      switch _getRunningOSVersion() {
      case .osx:
        return true
      default:
        return false
      }

    case .osxMajor(let major, _):
      switch _getRunningOSVersion() {
      case .osx(major, _, _):
        return true
      default:
        return false
      }

    case .osxMinor(let major, let minor, _):
      switch _getRunningOSVersion() {
      case .osx(major, minor, _):
        return true
      default:
        return false
      }

    case .osxMinorRange(let major, let minorRange, _):
      switch _getRunningOSVersion() {
      case .osx(major, let runningMinor, _):
        return minorRange.contains(runningMinor)
      default:
        return false
      }

    case .osxBugFix(let major, let minor, let bugFix, _):
      switch _getRunningOSVersion() {
      case .osx(major, minor, bugFix):
        return true
      default:
        return false
      }

    case .osxBugFixRange(let major, let minor, let bugFixRange, _):
      switch _getRunningOSVersion() {
      case .osx(major, minor, let runningBugFix):
        return bugFixRange.contains(runningBugFix)
      default:
        return false
      }

    case .iOSAny:
      switch _getRunningOSVersion() {
      case .iOS:
        return true
      default:
        return false
      }

    case .iOSMajor(let major, _):
      switch _getRunningOSVersion() {
      case .iOS(major, _, _):
        return true
      default:
        return false
      }

    case .iOSMajorRange(let range, _):
      switch _getRunningOSVersion() {
      case .iOS(let major, _, _):
        return range.contains(major)
      default:
        return false
      }

    case .iOSMinor(let major, let minor, _):
      switch _getRunningOSVersion() {
      case .iOS(major, minor, _):
        return true
      default:
        return false
      }

    case .iOSMinorRange(let major, let minorRange, _):
      switch _getRunningOSVersion() {
      case .iOS(major, let runningMinor, _):
        return minorRange.contains(runningMinor)
      default:
        return false
      }

    case .iOSBugFix(let major, let minor, let bugFix, _):
      switch _getRunningOSVersion() {
      case .iOS(major, minor, bugFix):
        return true
      default:
        return false
      }

    case .iOSBugFixRange(let major, let minor, let bugFixRange, _):
      switch _getRunningOSVersion() {
      case .iOS(major, minor, let runningBugFix):
        return bugFixRange.contains(runningBugFix)
      default:
        return false
      }

    case .iOSSimulatorAny:
      switch _getRunningOSVersion() {
      case .iOSSimulator:
        return true
      default:
        return false
      }

    case .tvOSAny:
      switch _getRunningOSVersion() {
      case .tvOS:
        return true
      default:
        return false
      }

    case .tvOSMajor(let major, _):
      switch _getRunningOSVersion() {
      case .tvOS(major, _, _):
        return true
      default:
        return false
      }

    case .tvOSMajorRange(let range, _):
      switch _getRunningOSVersion() {
      case .tvOS(let major, _, _):
        return range.contains(major)
      default:
        return false
      }

    case .tvOSMinor(let major, let minor, _):
      switch _getRunningOSVersion() {
      case .tvOS(major, minor, _):
        return true
      default:
        return false
      }

    case .tvOSMinorRange(let major, let minorRange, _):
      switch _getRunningOSVersion() {
      case .tvOS(major, let runningMinor, _):
        return minorRange.contains(runningMinor)
      default:
        return false
      }

    case .tvOSBugFix(let major, let minor, let bugFix, _):
      switch _getRunningOSVersion() {
      case .tvOS(major, minor, bugFix):
        return true
      default:
        return false
      }

    case .tvOSBugFixRange(let major, let minor, let bugFixRange, _):
      switch _getRunningOSVersion() {
      case .tvOS(major, minor, let runningBugFix):
        return bugFixRange.contains(runningBugFix)
      default:
        return false
      }

    case .tvOSSimulatorAny:
      switch _getRunningOSVersion() {
      case .tvOSSimulator:
        return true
      default:
        return false
      }

    case .watchOSAny:
      switch _getRunningOSVersion() {
      case .watchOS:
        return true
      default:
        return false
      }

    case .watchOSMajor(let major, _):
      switch _getRunningOSVersion() {
      case .watchOS(major, _, _):
        return true
      default:
        return false
      }

    case .watchOSMajorRange(let range, _):
      switch _getRunningOSVersion() {
      case .watchOS(let major, _, _):
        return range.contains(major)
      default:
        return false
      }

    case .watchOSMinor(let major, let minor, _):
      switch _getRunningOSVersion() {
      case .watchOS(major, minor, _):
        return true
      default:
        return false
      }

    case .watchOSMinorRange(let major, let minorRange, _):
      switch _getRunningOSVersion() {
      case .watchOS(major, let runningMinor, _):
        return minorRange.contains(runningMinor)
      default:
        return false
      }

    case .watchOSBugFix(let major, let minor, let bugFix, _):
      switch _getRunningOSVersion() {
      case .watchOS(major, minor, bugFix):
        return true
      default:
        return false
      }

    case .watchOSBugFixRange(let major, let minor, let bugFixRange, _):
      switch _getRunningOSVersion() {
      case .watchOS(major, minor, let runningBugFix):
        return bugFixRange.contains(runningBugFix)
      default:
        return false
      }

    case .watchOSSimulatorAny:
      switch _getRunningOSVersion() {
      case .watchOSSimulator:
        return true
      default:
        return false
      }

    case .linuxAny:
      switch _getRunningOSVersion() {
      case .linux:
        return true
      default:
        return false
      }

    case .androidAny:
      switch _getRunningOSVersion() {
      case .android:
        return true
      default:
        return false
      }

    case .freeBSDAny:
      switch _getRunningOSVersion() {
      case .freeBSD:
        return true
      default:
        return false
      }

    case .ps4Any:
      switch _getRunningOSVersion() {
      case .ps4:
        return true
      default:
        return false
      }

    case .windowsAny:
      switch _getRunningOSVersion() {
      case .windowsCygnus:
        return true
      case .windows:
        return true
      default:
        return false
      }

    case .windowsCygnusAny:
      switch _getRunningOSVersion() {
      case .windowsCygnus:
        return true
      default:
        return false
      }

    case .haikuAny:
      switch _getRunningOSVersion() {
      case .haiku:
        return true
      default:
        return false
      }

    case .objCRuntime:
#if _runtime(_ObjC)
      return true
#else
      return false
#endif

    case .nativeRuntime:
#if _runtime(_ObjC)
      return false
#else
      return true
#endif
    }
  }
}

//
// Semantic tests for protocol conformance
//

/// Test that the elements of `instances` satisfy the semantic
/// requirements of `Equatable`, using `oracle` to generate equality
/// expectations from pairs of positions in `instances`.
///
/// - Note: `oracle` is also checked for conformance to the
///   laws.
public func checkEquatable<Instances : Collection>(
  _ instances: Instances,
  oracle: (Instances.Index, Instances.Index) -> Bool,
  allowBrokenTransitivity: Bool = false,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) where
  Instances.Element : Equatable
{
  let indices = Array(instances.indices)
  _checkEquatableImpl(
    Array(instances),
    oracle: { oracle(indices[$0], indices[$1]) },
    allowBrokenTransitivity: allowBrokenTransitivity,
    message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
}

internal func _checkEquatableImpl<Instance : Equatable>(
  _ instances: [Instance],
  oracle: (Int, Int) -> Bool,
  allowBrokenTransitivity: Bool = false,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) {
  // For each index (which corresponds to an instance being tested) track the
  // set of equal instances.
  var transitivityScoreboard: [Box<Set<Int>>] =
    instances.indices.map { _ in Box(Set()) }

  // TODO: swift-3-indexing-model: add tests for this function.
  for i in instances.indices {
    let x = instances[i]
    expectTrue(oracle(i, i), "bad oracle: broken reflexivity at index \(i)")

    for j in instances.indices {
      let y = instances[j]

      let predictedXY = oracle(i, j)
      expectEqual(
        predictedXY, oracle(j, i),
        "bad oracle: broken symmetry between indices \(i), \(j)",
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

      let isEqualXY = x == y
      expectEqual(
        predictedXY, isEqualXY,
        """
        \((predictedXY
           ? "expected equal, found not equal"
           : "expected not equal, found equal"))
        lhs (at index \(i)): \(String(reflecting: x))
        rhs (at index \(j)): \(String(reflecting: y))
        """,
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

      // Not-equal is an inverse of equal.
      expectNotEqual(
        isEqualXY, x != y,
        """
        lhs (at index \(i)): \(String(reflecting: x))
        rhs (at index \(j)): \(String(reflecting: y))
        """,
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

      if !allowBrokenTransitivity {
        // Check transitivity of the predicate represented by the oracle.
        // If we are adding the instance `j` into an equivalence set, check that
        // it is equal to every other instance in the set.
        if predictedXY && i < j && transitivityScoreboard[i].value.insert(j).inserted {
          if transitivityScoreboard[i].value.count == 1 {
            transitivityScoreboard[i].value.insert(i)
          }
          for k in transitivityScoreboard[i].value {
            expectTrue(
              oracle(j, k),
              "bad oracle: broken transitivity at indices \(i), \(j), \(k)",
              stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
              // No need to check equality between actual values, we will check
              // them with the checks above.
          }
          precondition(transitivityScoreboard[j].value.isEmpty)
          transitivityScoreboard[j] = transitivityScoreboard[i]
        }
      }
    }
  }
}


public func checkEquatable<T : Equatable>(
  _ expectedEqual: Bool, _ lhs: T, _ rhs: T,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) {
  checkEquatable(
    [lhs, rhs],
    oracle: { expectedEqual || $0 == $1 }, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
    showFrame: false)
}

/// Produce an integer hash value for `value` by feeding it to a dedicated
/// `Hasher`. This is always done by calling the `hash(into:)` method.
/// If a non-nil `seed` is given, it is used to perturb the hasher state;
/// this is useful for resolving accidental hash collisions.
internal func hash<H: Hashable>(_ value: H, seed: Int? = nil) -> Int {
  var hasher = Hasher()
  if let seed = seed {
    hasher.combine(seed)
  }
  hasher.combine(value)
  return hasher.finalize()
}

/// Test that the elements of `groups` consist of instances that satisfy the
/// semantic requirements of `Hashable`, with each group defining a distinct
/// equivalence class under `==`.
public func checkHashableGroups<Groups: Collection>(
  _ groups: Groups,
  _ message: @autoclosure () -> String = "",
  allowIncompleteHashing: Bool = false,
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) where Groups.Element: Collection, Groups.Element.Element: Hashable {
  let instances = groups.flatMap { $0 }
  // groupIndices[i] is the index of the element in groups that contains
  // instances[i].
  let groupIndices =
    zip(0..., groups).flatMap { i, group in group.map { _ in i } }
  func equalityOracle(_ lhs: Int, _ rhs: Int) -> Bool {
    return groupIndices[lhs] == groupIndices[rhs]
  }
  checkHashable(
    instances,
    equalityOracle: equalityOracle,
    hashEqualityOracle: equalityOracle,
    allowBrokenTransitivity: false,
    allowIncompleteHashing: allowIncompleteHashing,
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
    showFrame: false)
}

/// Test that the elements of `instances` satisfy the semantic requirements of
/// `Hashable`, using `equalityOracle` to generate equality and hashing
/// expectations from pairs of positions in `instances`.
public func checkHashable<Instances: Collection>(
  _ instances: Instances,
  equalityOracle: (Instances.Index, Instances.Index) -> Bool,
  allowBrokenTransitivity: Bool = false,
  allowIncompleteHashing: Bool = false,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) where Instances.Element: Hashable {
  checkHashable(
    instances,
    equalityOracle: equalityOracle,
    hashEqualityOracle: equalityOracle,
    allowBrokenTransitivity: allowBrokenTransitivity,
    allowIncompleteHashing: allowIncompleteHashing,
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
    showFrame: false)
}

/// Test that the elements of `instances` satisfy the semantic
/// requirements of `Hashable`, using `equalityOracle` to generate
/// equality expectations from pairs of positions in `instances`,
/// and `hashEqualityOracle` to do the same for hashing.
public func checkHashable<Instances: Collection>(
  _ instances: Instances,
  equalityOracle: (Instances.Index, Instances.Index) -> Bool,
  hashEqualityOracle: (Instances.Index, Instances.Index) -> Bool,
  allowBrokenTransitivity: Bool = false,
  allowIncompleteHashing: Bool = false,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) where
  Instances.Element: Hashable {
  checkEquatable(
    instances,
    oracle: equalityOracle,
    allowBrokenTransitivity: allowBrokenTransitivity,
    message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

  for i in instances.indices {
    let x = instances[i]
    for j in instances.indices {
      let y = instances[j]
      let predicted = hashEqualityOracle(i, j)
      expectEqual(
        predicted, hashEqualityOracle(j, i),
        "bad hash oracle: broken symmetry between indices \(i), \(j)",
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
      if x == y {
        expectTrue(
          predicted,
          """
          bad hash oracle: equality must imply hash equality
          lhs (at index \(i)): \(x)
          rhs (at index \(j)): \(y)
          """,
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
      }
      if predicted {
        expectEqual(
          hash(x), hash(y),
          """
          hash(into:) expected to match, found to differ
          lhs (at index \(i)): \(x)
          rhs (at index \(j)): \(y)
          """,
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
        expectEqual(
          x.hashValue, y.hashValue,
          """
          hashValue expected to match, found to differ
          lhs (at index \(i)): \(x)
          rhs (at index \(j)): \(y)
          """,
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
        expectEqual(
          x._rawHashValue(seed: 0), y._rawHashValue(seed: 0),
          """
          _rawHashValue(seed:) expected to match, found to differ
          lhs (at index \(i)): \(x)
          rhs (at index \(j)): \(y)
          """,
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
      } else if !allowIncompleteHashing {
        // Try a few different seeds; at least one of them should discriminate
        // between the hashes. It is extremely unlikely this check will fail
        // all ten attempts, unless the type's hash encoding is not unique,
        // or unless the hash equality oracle is wrong.
        expectTrue(
          (0..<10).contains { hash(x, seed: $0) != hash(y, seed: $0) },
          """
          hash(into:) expected to differ, found to match
          lhs (at index \(i)): \(x)
          rhs (at index \(j)): \(y)
          """,
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
        expectTrue(
          (0..<10).contains { i in
            x._rawHashValue(seed: i) != y._rawHashValue(seed: i)
          },
          """
          _rawHashValue(seed:) expected to differ, found to match
          lhs (at index \(i)): \(x)
          rhs (at index \(j)): \(y)
          """,
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
      }
    }
  }
}

public func checkHashable<T : Hashable>(
  expectedEqual: Bool, _ lhs: T, _ rhs: T,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) {
  checkHashable(
    [lhs, rhs], equalityOracle: { expectedEqual || $0 == $1 }, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
}

public enum ExpectedComparisonResult {
  case lt, eq, gt

  public func isLT() -> Bool {
    return self == .lt
  }

  public func isEQ() -> Bool {
    return self == .eq
  }

  public func isGT() -> Bool {
    return self == .gt
  }

  public func isLE() -> Bool {
    return isLT() || isEQ()
  }

  public func isGE() -> Bool {
    return isGT() || isEQ()
  }

  public func isNE() -> Bool {
    return !isEQ()
  }

  public func flip() -> ExpectedComparisonResult {
    switch self {
    case .lt:
      return .gt
    case .eq:
      return .eq
    case .gt:
      return .lt
    }
  }
}

extension ExpectedComparisonResult : CustomStringConvertible {
  public var description: String {
    switch self {
    case .lt:
      return "<"
    case .eq:
      return "=="
    case .gt:
      return ">"
    }
  }
}

/// Test that the elements of `instances` satisfy the semantic
/// requirements of `Comparable`, using `oracle` to generate comparison
/// expectations from pairs of positions in `instances`.
///
/// - Note: `oracle` is also checked for conformance to the
///   laws.
public func checkComparable<Instances : Collection>(
  _ instances: Instances,
  oracle: (Instances.Index, Instances.Index) -> ExpectedComparisonResult,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) where
  Instances.Element : Comparable {

  // Also checks that equality is consistent with comparison and that
  // the oracle obeys the equality laws
  checkEquatable(instances, oracle: { oracle($0, $1).isEQ() }, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

  for i in instances.indices {
    let x = instances[i]

    expectFalse(
      x < x,
      "found 'x < x'\n" +
      "at index \(i): \(String(reflecting: x))",
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

    expectFalse(
      x > x,
      "found 'x > x'\n" +
      "at index \(i): \(String(reflecting: x))",
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

    expectTrue(x <= x,
      "found 'x <= x' to be false\n" +
      "at index \(i): \(String(reflecting: x))",
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

    expectTrue(x >= x,
      "found 'x >= x' to be false\n" +
      "at index \(i): \(String(reflecting: x))",
        stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

    for j in instances.indices where i != j {
      let y = instances[j]

      let expected = oracle(i, j)

      expectEqual(
        expected.flip(), oracle(j, i),
        "bad oracle: missing antisymmetry: "
        + "(\(String(reflecting: i)), \(String(reflecting: j)))",
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

      expectEqual(expected.isLT(), x < y,
        "x < y\n" +
        "lhs (at index \(i)): \(String(reflecting: x))\n" +
        "rhs (at index \(j)): \(String(reflecting: y))",
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

      expectEqual(expected.isLE(), x <= y,
        "x <= y\n" +
        "lhs (at index \(i)): \(String(reflecting: x))\n" +
        "rhs (at index \(j)): \(String(reflecting: y))",
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

      expectEqual(expected.isGE(), x >= y,
        "x >= y\n" +
        "lhs (at index \(i)): \(String(reflecting: x))\n" +
        "rhs (at index \(j)): \(String(reflecting: y))",
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

      expectEqual(expected.isGT(), x > y,
        "x > y\n" +
        "lhs (at index \(i)): \(String(reflecting: x))\n" +
        "rhs (at index \(j)): \(String(reflecting: y))",
          stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

      for k in instances.indices {
        let expected2 = oracle(j, k)
        if expected == expected2 {
          expectEqual(
            expected, oracle(i, k),
            "bad oracle: missing transitivity "
            + "(\(String(reflecting: i)), \(String(reflecting: j)), "
            + "\(String(reflecting: k)))", stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
        }
      }
    }
  }
}

public func checkComparable<T : Comparable>(
  _ expected: ExpectedComparisonResult, _ lhs: T, _ rhs: T,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) {
  checkComparable(
    [lhs, rhs],
    oracle: { [[ .eq, expected], [ expected.flip(), .eq]][$0][$1] },
    message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
}


/// Test that the elements of `instances` satisfy the semantic
/// requirements of `Strideable`, using `advanceOracle` and
/// 'distanceOracle' to generate expectations about the results of
/// `advanced(by:)` and `distance(to:)` from pairs of positions in
/// `instances` and `strides`.
///
/// - Note: `oracle` is also checked for conformance to the
///   laws.
public func checkStrideable<Instances : Collection, Strides : Collection>(
  _ instances: Instances, strides: Strides,
  distanceOracle:
    (Instances.Index, Instances.Index) -> Strides.Element,
  advanceOracle:
    (Instances.Index, Strides.Index) -> Instances.Element,

  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) where
  Instances.Element : Strideable,
  Instances.Element.Stride == Strides.Element {

  checkComparable(
    instances,
    oracle: {
      let d = distanceOracle($1, $0);
      return d < 0 ? .lt : d == 0 ? .eq : .gt
    },
    message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))

  for i in instances.indices {
    let x = instances[i]
    expectEqual(x, x.advanced(by: 0))

    for j in strides.indices {
      let y = strides[j]
      expectEqual(advanceOracle(i, j), x.advanced(by: y))
    }

    for j in instances.indices {
      let y = instances[j]
      expectEqual(distanceOracle(i, j), x.distance(to: y))
    }
  }
}

public func checkLosslessStringConvertible<Instance>(
  _ instances: [Instance]
) where Instance : LosslessStringConvertible & Equatable {
  expectEqualFunctionsForDomain(instances, { $0 }, { Instance(String($0))! })
}

public func nthIndex<C: Collection>(_ x: C, _ n: Int) -> C.Index {
  return x.index(x.startIndex, offsetBy: n)
}

public func nth<C: Collection>(_ x: C, _ n: Int) -> C.Element {
  return x[nthIndex(x, n)]
}

public func expectEqualSequence<
  Expected: Sequence,
  Actual: Sequence
>(
  _ expected: Expected, _ actual: Actual,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) where
  Expected.Element == Actual.Element,
  Expected.Element : Equatable {

  expectEqualSequence(expected, actual, message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line)) { $0 == $1 }
}

public func expectEqualSequence<
  Expected : Sequence,
  Actual : Sequence,
  T : Equatable,
  U : Equatable
>(
  _ expected: Expected, _ actual: Actual,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) where
  Expected.Element == Actual.Element,
  Expected.Element == (T, U) {

  expectEqualSequence(
    expected, actual, message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line)) {
    (lhs: (T, U), rhs: (T, U)) -> Bool in
    lhs.0 == rhs.0 && lhs.1 == rhs.1
  }
}

public func expectEqualSequence<
  Expected: Sequence,
  Actual: Sequence
>(
  _ expected: Expected, _ actual: Actual,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  sameValue: (Expected.Element, Expected.Element) -> Bool
) where
  Expected.Element == Actual.Element {

  if !expected.elementsEqual(actual, by: sameValue) {
    expectationFailure("expected elements: \"\(expected)\"\n"
      + "actual: \"\(actual)\" (of type \(String(reflecting: type(of: actual))))",
      trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  }
}

public func expectEqualsUnordered<
  Expected : Sequence,
  Actual : Sequence
>(
  _ expected: Expected, _ actual: Actual,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  compare: @escaping (Expected.Element, Expected.Element)
    -> ExpectedComparisonResult
) where
  Expected.Element == Actual.Element {

  let x: [Expected.Element] =
    expected.sorted { compare($0, $1).isLT() }
  let y: [Actual.Element] =
    actual.sorted { compare($0, $1).isLT() }
  expectEqualSequence(
    x, y, message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: { compare($0, $1).isEQ() })
}

public func expectEqualsUnordered<
  Expected : Sequence,
  Actual : Sequence
>(
  _ expected: Expected, _ actual: Actual,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) where
  Expected.Element == Actual.Element,
  Expected.Element : Comparable {

  expectEqualsUnordered(expected, actual, message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line)) {
    $0 < $1 ? .lt : $0 == $1 ? .eq : .gt
  }
}

public func expectEqualsUnordered<T : Comparable>(
  _ expected: [T], _ actual: [T],
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) {
  let x = expected.sorted()
  let y = actual.sorted()
  expectEqualSequence(x, y, message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
}

public func expectEqualsUnordered<T : Strideable>(
  _ expected: Range<T>, _ actual: [T],
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) where T.Stride: SignedInteger {
  if expected.count != actual.count {
    expectationFailure("expected elements: \"\(expected)\"\n"
      + "actual: \"\(actual)\" (of type \(String(reflecting: type(of: actual))))",
      trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  }
  for e in actual {
    if !expected.contains(e) {
      expectationFailure("expected elements: \"\(expected)\"\n"
        + "actual: \"\(actual)\" (of type \(String(reflecting: type(of: actual))))",
        trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
    }
  }
}

/// A nominal type that is equivalent to a tuple of two elements.
///
/// We need a nominal type because we can't add protocol conformances to
/// tuples.
struct Pair<T : Comparable> : Comparable {
  init(_ first: T, _ second: T) {
    self.first = first
    self.second = second
  }

  var first: T
  var second: T

  static func ==(lhs: Pair<T>, rhs: Pair<T>) -> Bool {
    return lhs.first == rhs.first && lhs.second == rhs.second
  }

  static func <(lhs: Pair<T>, rhs: Pair<T>) -> Bool {
    return [lhs.first, lhs.second].lexicographicallyPrecedes(
      [rhs.first, rhs.second])
  }
}

public func expectEqualsUnordered<
  Expected : Sequence,
  Actual : Sequence,
  T : Comparable
>(
  _ expected: Expected, _ actual: Actual,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line
) where
  Actual.Element == (key: T, value: T),
  Expected.Element == (T, T) {

  func comparePairLess(_ lhs: (T, T), rhs: (T, T)) -> Bool {
    return [lhs.0, lhs.1].lexicographicallyPrecedes([rhs.0, rhs.1])
  }

  let x: [(T, T)] =
    expected.sorted(by: comparePairLess)
  let y: [(T, T)] =
    actual.map { ($0.0, $0.1) }
      .sorted(by: comparePairLess)

  func comparePairEquals(_ lhs: (T, T), rhs: (key: T, value: T)) -> Bool {
    return lhs.0 == rhs.0 && lhs.1 == rhs.1
  }

  expectEqualSequence(x, y, message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: comparePairEquals)
}

public func expectEqualFunctionsForDomain<ArgumentType, Result : Equatable>(
    _ arguments: [ArgumentType], _ function1: (ArgumentType) -> Result,
    _ function2: (ArgumentType) -> Result
) {
  for a in arguments {
    let expected = function1(a)
    let actual = function2(a)
    expectEqual(expected, actual, "where the argument is: \(a)")
  }
}

public func expectEqualMethodsForDomain<
  SelfType, ArgumentType, Result : Equatable
>(
  _ selfs: [SelfType], _ arguments: [ArgumentType],
  _ function1: (SelfType) -> (ArgumentType) -> Result,
  _ function2: (SelfType) -> (ArgumentType) -> Result
) {
  for s in selfs {
    for a in arguments {
      let expected = function1(s)(a)
      let actual = function2(s)(a)
      expectEqual(
        expected, actual,
        "where the first argument is: \(s)\nand the second argument is: \(a)"
      )
    }
  }
}

public func expectEqualUnicodeScalars<S: StringProtocol>(
  _ expected: [UInt32], _ actual: S,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line) {
  let actualUnicodeScalars = Array(
    actual.unicodeScalars.lazy.map { $0.value })

  if !expected.elementsEqual(actualUnicodeScalars) {
    expectationFailure(
      "expected elements: \"\(asHex(expected))\"\n"
      + "actual: \"\(asHex(actualUnicodeScalars))\"",
      trace: message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  }
}
