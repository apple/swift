// RUN: %target-swift-frontend -typecheck -verify -primary-file %s

// rdar://problem/62957095
// https://github.com/apple/swift/issues/55190

struct S1 {
  var x : Int = 0
}
var s1 = S1()
s1[\.x] = 10 // expected-error {{missing argument label 'keyPath:' in subscript}} {{4-4=keyPath: }}

struct S2 {
  var x : Int = 0
  subscript(_ v: Int) -> Int { 0 }
}
var s2 = S2()
s2[\.x] = 10  // expected-error {{missing argument label 'keyPath:' in subscript}} {{4-4=keyPath: }}

struct S3 {
  var x : Int = 0
  subscript(v v: KeyPath<S3, Int>) -> Int { get { 0 } set(newValue) {} }
}
var s3 = S3()
s3[\.x] = 10 // expected-error {{missing argument label 'v:' in subscript}} {{4-4=v: }}

struct S4 {
  var x : Int = 0
  subscript(v: KeyPath<S4, String>) -> Int { get { 0 } set(newValue) {} }
}
var s4 = S4()
s4[\.x] = 10 // expected-error {{cannot convert value of type 'KeyPath<S4, Int>' to expected argument type 'KeyPath<S4, String>'}}
// expected-note@-1 {{arguments to generic parameter 'Value' ('Int' and 'String') are expected to be equal}}
