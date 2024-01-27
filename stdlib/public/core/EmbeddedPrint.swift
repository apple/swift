//===----------------------------------------------------------------------===//
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

// TODO: This is all a stop-gap so that at least some types are printable in
// embedded Swift, in an embedded-programming friendly way (we mainly need
// printing to not need to heap allocate).

@_silgen_name("putchar")
@discardableResult
func putchar(_: CInt) -> CInt

public func print(_ string: StaticString, terminator: StaticString = "\n") {
  var p = string.utf8Start
  while p.pointee != 0 {
    putchar(CInt(p.pointee))
    p += 1
  }
  p = terminator.utf8Start
  while p.pointee != 0 {
    putchar(CInt(p.pointee))
    p += 1
  }
}

public func print(_ string: String, terminator: StaticString = "\n") {
  var string = string
  _ = string.withUTF8 { buf in
    for c in buf {
      putchar(CInt(c))
    }
  }
  var p = terminator.utf8Start
  while p.pointee != 0 {
    putchar(CInt(p.pointee))
    p += 1
  }
}

func printCharacters(_ buf: UnsafeRawBufferPointer) {
  for c in buf {
    putchar(CInt(c))
  }
}

func printCharacters(_ buf: UnsafeBufferPointer<UInt8>) {
  printCharacters(UnsafeRawBufferPointer(buf))
}

extension BinaryInteger {
  func writeToStdout() {
    if self == (0 as Self) {
      putchar(CInt(("0" as Unicode.Scalar).value))
      return
    }

    func _ascii(_ digit: UInt8) -> UInt8 {
      UInt8(("0" as Unicode.Scalar).value) + digit
    }
    let isNegative = Self.isSigned && self < (0 as Self)
    var value = magnitude

    // Avoid withUnsafeTemporaryAllocation which is not typed-throws ready yet
    let byteCount = 64
    let stackBuffer = Builtin.stackAlloc(byteCount._builtinWordValue,
         1._builtinWordValue, 1._builtinWordValue)
    let buffer = UnsafeMutableRawBufferPointer(start: .init(stackBuffer),
        count: byteCount)

    var index = buffer.count - 1
    while value != 0 {
      let (quotient, remainder) =
          value.quotientAndRemainder(dividingBy: Magnitude(10))
      buffer[index] = _ascii(UInt8(truncatingIfNeeded: remainder))
      index -= 1
      value = quotient
    }
    if isNegative {
      buffer[index] = UInt8(("-" as Unicode.Scalar).value)
      index -= 1
    }
    let start = index + 1
    let end = buffer.count - 1
    let count = end - start + 1

    let pointerToPrint = buffer.baseAddress?.advanced(by: start)
        .assumingMemoryBound(to: UInt8.self)
    printCharacters(UnsafeBufferPointer(start: pointerToPrint, count: count))

    Builtin.stackDealloc(stackBuffer)
  }
}

public func print(_ integer: some BinaryInteger, terminator: StaticString = "\n") {
  integer.writeToStdout()
  print("", terminator: terminator)
}

public func print(_ boolean: Bool, terminator: StaticString = "\n") {
  if boolean {
    print("true", terminator: terminator)
  } else {
    print("false", terminator: terminator)
  }
}
