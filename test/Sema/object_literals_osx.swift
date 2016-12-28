// RUN: %target-typecheck-verify-swift
// REQUIRES: OS=macosx

struct S: _ExpressibleByColorLiteral {
  init(colorLiteralRed: Float, green: Float, blue: Float, alpha: Float) {}
}

let y: S = #colorLiteral(red: 1, green: 0, blue: 0, alpha: 1)
let y2 = #colorLiteral(red: 1, green: 0, blue: 0, alpha: 1) // expected-error{{could not infer type of color literal}} expected-note{{import AppKit to use 'NSColor' as the default color literal type}}
let y3 = #colorLiteral(red: 1, bleen: 0, grue: 0, alpha: 1) // expected-error{{cannot convert value of type '(red: Int, bleen: Int, grue: Int, alpha: Int)' to expected argument type '(red: Float, green: Float, blue: Float, alpha: Float)'}}

struct I: _ExpressibleByImageLiteral {
  init(imageLiteralResourceName: StaticString) {}
}

let z: I = #imageLiteral(resourceName: "hello.png")
let z2 = #imageLiteral(resourceName: "hello.png") // expected-error{{could not infer type of image literal}} expected-note{{import AppKit to use 'NSImage' as the default image literal type}}

struct Path: _ExpressibleByFileReferenceLiteral {
  init(fileReferenceLiteralResourceName: StaticString) {}
}

let p1: Path = #fileLiteral(resourceName: "what.txt")
let p2 = #fileLiteral(resourceName: "what.txt") // expected-error{{could not infer type of file reference literal}} expected-note{{import Foundation to use 'URL' as the default file reference literal type}}
let string = "what.txt"
let p3: Path = #fileLiteral(resourceName: string) // expected-error{{cannot convert value of type 'String' to expected argument type 'StaticString'}}

let text = #fileLiteral(resourceName: "TextFile.txt").relativeString! // expected-error{{type of expression is ambiguous without more context}}
