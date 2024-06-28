// RUN: %target-swift-frontend -sil-verify-all -verify -emit-sil %s

struct AO<T> {
  borrowing func borrow2Borrow() -> Int { p1 }
  borrowing func borrow2Consume() -> Int {  // expected-error{{'self' is borrowed and cannot be consumed}}
    p2 // expected-note{{consumed here}}
  }
  consuming func consume2Borrow() -> Int { p1 }
  consuming func consume2Consume() -> Int { p2 }
  let t: T
  var p1 : Int { borrowing get { 666 } }
  var p2: Int { consuming get { 666 } }
}

// https://github.com/apple/swift/issues/73292
struct Example {
  protocol Proto {
    var count: Int { borrowing get }
  }
  
  func takeProto(_ p: borrowing some Proto) -> Int {
    p.count
  }
}
