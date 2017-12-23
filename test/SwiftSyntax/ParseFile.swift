// RUN: %target-run-simple-swift
// REQUIRES: executable_test
// REQUIRES: OS=macosx
// REQUIRES: objc_interop

import Foundation
import StdlibUnittest
import SwiftSyntax

var ParseFile = TestSuite("ParseFile")

struct Foo {
  public let x: Int
  private(set) var y: [Bool]
}

class Test: NSObject {
  @objc var bar: Int
  func test() {
    print(#selector(function))
    print(#keyPath(bar))
  }
  @objc func function() {
  }
}

ParseFile.test("ParseSingleFile") {
  let currentFile = URL(fileURLWithPath: #file)
  expectDoesNotThrow({
    let currentFileContents = try String(contentsOf: currentFile)
    let parsed = try Syntax.parse(currentFile)
    expectEqual("\(parsed)", currentFileContents)
  })
}

runAllTests()
