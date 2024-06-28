// RUN: %target-run-simple-swift
// REQUIRES: executable_test

import StdlibUnittest


#if canImport(Darwin)
  import Darwin
#elseif canImport(Glibc)
  import Glibc
#elseif os(Windows)
  import MSVCRT
#elseif canImport(Android)
  import Android
#else
#error("Unsupported platform")
#endif

func simple_getline() -> [UInt8]? {
  var result = [UInt8]()
  while true {
    let c = getchar()
    if c == EOF {
      if result.count == 0 {
        return nil
      }
      return result
    }
    result.append(UInt8(c))
    if c == CInt(UnicodeScalar("\n").value) {
      return result
    }
  }
}

var StdinTestSuite = TestSuite("Stdin")

StdinTestSuite.test("Empty")
  .stdin("")
  .code {
}

StdinTestSuite.test("EmptyLine")
  .stdin("\n")
  .code {
  expectEqual([ 0x0a ], simple_getline())
}

StdinTestSuite.test("Whitespace")
  .stdin(" \n")
  .code {
  expectEqual([ 0x20, 0x0a ], simple_getline())
}

StdinTestSuite.test("NonEmptyLine")
  .stdin("abc\n")
  .code {
  expectEqual([ 0x61, 0x62, 0x63, 0x0a ], simple_getline())
}

StdinTestSuite.test("TwoLines")
  .stdin("abc\ndefghi\n")
  .code {
  expectEqual([ 0x61, 0x62, 0x63, 0x0a ], simple_getline())
  expectEqual(
    [ 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x0a ], simple_getline())
}

StdinTestSuite.test("EOF/1")
  .stdin("abc\n", eof: true)
  .code {
  expectEqual([ 0x61, 0x62, 0x63, 0x0a ], simple_getline())
}

StdinTestSuite.test("EOF/2")
  .stdin("abc\n", eof: true)
  .code {
  expectEqual([ 0x61, 0x62, 0x63, 0x0a ], simple_getline())
}

runAllTests()

