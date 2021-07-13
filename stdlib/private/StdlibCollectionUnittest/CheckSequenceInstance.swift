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


import StdlibUnittest

// Generate two overloads: one for Array (which will get
// picked up when the caller passes a literal), and another that
// accepts any appropriate Collection type.

public func checkIterator<
  Expected: Collection, I : IteratorProtocol
>(
  _ expected: Expected,
  _ iterator: I,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Expected.Element, I.Element) -> Bool
) {
  // Copying a `IteratorProtocol` is allowed.
  var mutableGen = iterator
  var actual: [I.Element] = []
  while let e = mutableGen.next() {
    actual.append(e)
  }
  expectEqualSequence(
    expected, actual, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)

  // Having returned `.None` once, a `IteratorProtocol` should not generate
  // more elements.
  for _ in 0..<10 {
    expectNil(mutableGen.next(), message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  }
}

public func checkIterator<
  Expected: Collection, I : IteratorProtocol
>(
  _ expected: Expected,
  _ iterator: I,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all
) where I.Element == Expected.Element, Expected.Element : Equatable {
  checkIterator(
    expected, iterator, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), showFrame: false,
    resiliencyChecks: resiliencyChecks
  ) { $0 == $1 }
}

public func checkSequence<
  Expected: Collection, S : Sequence
>(
  _ expected: Expected,
  _ sequence: S,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Expected.Element, S.Element) -> Bool
) {
  let expectedCount: Int = expected.count
  checkIterator(
    expected, sequence.makeIterator(), message(),
  stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
    resiliencyChecks: resiliencyChecks,
    sameValue: sameValue)

  expectGE(
    expectedCount, sequence.underestimatedCount, message(),
      stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
}

public func checkSequence<
  Expected: Collection, S : Sequence
>(
  _ expected: Expected,
  _ sequence: S,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all
) where
  S.Element == Expected.Element,
  S.Element : Equatable {

  checkSequence(
    expected, sequence, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), showFrame: false,
    resiliencyChecks: resiliencyChecks
  ) { $0 == $1 }
}

public func checkIterator<
  Element, I : IteratorProtocol
>(
  _ expected: Array<Element>,
  _ iterator: I,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Element, Element) -> Bool
) where I.Element == Element {
  // Copying a `IteratorProtocol` is allowed.
  var mutableGen = iterator
  var actual: [I.Element] = []
  while let e = mutableGen.next() {
    actual.append(e)
  }
  expectEqualSequence(
    expected, actual, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), sameValue: sameValue)

  // Having returned `.None` once, a `IteratorProtocol` should not generate
  // more elements.
  for _ in 0..<10 {
    expectNil(mutableGen.next(), message(),
  stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
  }
}

public func checkIterator<
  Element, I : IteratorProtocol
>(
  _ expected: Array<Element>,
  _ iterator: I,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all
) where I.Element == Element, Element : Equatable {
  checkIterator(
    expected, iterator, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), showFrame: false,
    resiliencyChecks: resiliencyChecks
  ) { $0 == $1 }
}

public func checkSequence<
  Element, S : Sequence
>(
  _ expected: Array<Element>,
  _ sequence: S,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all,
  sameValue: (Element, Element) -> Bool
) where S.Element == Element {
  let expectedCount: Int = expected.count
  checkIterator(
    expected, sequence.makeIterator(), message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line),
    resiliencyChecks: resiliencyChecks,
    sameValue: sameValue)

  expectGE(
    expectedCount, sequence.underestimatedCount, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line))
}

public func checkSequence<
  Element, S : Sequence
>(
  _ expected: Array<Element>,
  _ sequence: S,
  _ message: @autoclosure () -> String = "",
  stackTrace: SourceLocStack = SourceLocStack(),
  showFrame: Bool = true,
  file: String = #file, line: UInt = #line,
  resiliencyChecks: CollectionMisuseResiliencyChecks = .all
) where
  S.Element == Element,
  S.Element : Equatable {

  checkSequence(
    expected, sequence, message(),
    stackTrace: stackTrace.pushIf(showFrame, file: file, line: line), showFrame: false,
    resiliencyChecks: resiliencyChecks
  ) { $0 == $1 }
}

