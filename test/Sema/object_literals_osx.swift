// RUN: %target-typecheck-verify-swift
// REQUIRES: OS=macosx

struct S: _ExpressibleByColorLiteral {
  init(_colorLiteralRed: Float, green: Float, blue: Float, alpha: Float) {}
}

let y: S = #colorLiteral(red: 1, green: 0, blue: 0, alpha: 1)
let y2 = #colorLiteral(red: 1, green: 0, blue: 0, alpha: 1) // expected-error{{could not infer type of color literal}}
// expected-note@-1{{import AppKit to use 'NSColor' as the default color literal type}}
let y3 = #colorLiteral(red: 1, bleen: 0, grue: 0, alpha: 1)
// expected-error@-1{{could not infer type of color literal}}
// expected-note@-2{{import AppKit to use 'NSColor' as the default color literal type}}

let _: S = #colorLiteral(red: 1, bleen: 0, grue: 0, alpha: 1)
// expected-error@-1 {{incorrect argument labels in call (have 'red:bleen:grue:alpha:', expected 'red:green:blue:alpha:')}} {{34-39=green}} {{44-48=blue}}

struct I: _ExpressibleByImageLiteral {
  init(imageLiteralResourceName: String) {}
}

let z: I = #imageLiteral(resourceName: "hello.png")
let z2 = #imageLiteral(resourceName: "hello.png") // expected-error{{could not infer type of image literal}}
// expected-note@-1{{import AppKit to use 'NSImage' as the default image literal type}}

struct Path: _ExpressibleByFileReferenceLiteral {
  init(fileReferenceLiteralResourceName: String) {} // expected-note {{'init(fileReferenceLiteralResourceName:)' declared here}}
}

let p1: Path = #fileLiteral(resourceName: "what.txt")
let p2 = #fileLiteral(resourceName: "what.txt") // expected-error{{could not infer type of file reference literal}}
// expected-note@-1{{import Foundation to use 'URL' as the default file reference literal type}}

let text = #fileLiteral(resourceName: "TextFile.txt").relativeString! // expected-error{{could not infer type of file reference literal}}
// expected-note@-1{{import Foundation to use 'URL' as the default file reference literal type}}

// rdar://problem/49861813
#fileLiteral()
// expected-error@-1{{could not infer type of file reference literal}}
// expected-note@-2{{import Foundation to use 'URL' as the default file reference literal type}}

let _: Path = #fileLiteral() // expected-error {{missing argument for parameter 'resourceName' in call}}

// rdar://problem/62927467
func test_literal_arguments_are_loaded() {
  var resource = "foo.txt"
  let _: Path = #fileLiteral(resourceName: resource) // Ok

  func test(red: inout Float, green: inout Float) -> S {
    return #colorLiteral(red: red, green: green, blue: 1, alpha: 1) // Ok
  }
}
