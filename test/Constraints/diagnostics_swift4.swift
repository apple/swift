// RUN: %target-typecheck-verify-swift -swift-version 4

// https://github.com/apple/swift/issues/45110
// Call arguments did not match up assertion

func f_45110(_ a: Any) {} // expected-note {{}}
do {
  f_45110()        // expected-error {{missing argument for parameter #1 in call}}
  f_45110(a: 1)    // expected-error {{extraneous argument label 'a:' in call}}
  f_45110(1, 2)    // expected-error {{extra argument in call}}
  f_45110(a: 1, 2) // expected-error {{extra argument in call}}
}

struct S_45110 {
  init(_ arg: Any) {
  }
}

protocol P_45110 {
}

extension S_45110 {
  init<T>(from: [T]) where T: P_45110 {
  }
}

class C_45110: P_45110 {
}

let _ = S_45110(arg: [C_45110()]) // expected-error {{extraneous argument label 'arg:' in call}}

// rdar://problem/31898542 - Swift 4: 'type of expression is ambiguous without a type annotation' errors, without a fixit

enum R31898542<T> {
  case success(T) // expected-note {{'success' declared here}}
  case failure
}

func foo() -> R31898542<()> {
  return .success() // expected-error {{missing argument for parameter #1 in call}} {{19-19=<#()#>}}
}

// rdar://problem/31973368 - Cannot convert value of type '(K, V) -> ()' to expected argument type '((key: _, value: _)) -> Void'

// SE-0110: We reverted to allowing this for the time being, but this
// test is valuable in case we end up disallowing it again in the
// future.

class R<K: Hashable, V> {
  func forEach(_ body: (K, V) -> ()) {
    let dict: [K:V] = [:]
    dict.forEach(body)
  }
}

// Make sure that solver doesn't try to form solutions with available overloads when better generic choices are present.
infix operator +=+ : AdditionPrecedence
func +=+(_ lhs: Int, _ rhs: Int) -> Bool { return lhs == rhs }
func +=+<T: BinaryInteger>(_ lhs: T, _ rhs: Int) -> Bool { return lhs == rhs }
