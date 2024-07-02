// RUN: %target-swift-frontend -emit-sil %s -O| %FileCheck %s

public enum Foo {
  case bar
  case baz(Int)
}

public protocol Frob {
  func foo(_ x: Int) -> Foo
}

public struct Nicate {
  public var frob: any Frob
  public var isInitializing: Bool
 
  // CHECK-LABEL: sil @$s31closure_lifetime_fixup_copyelim6NicateV4slowyS2iF :
  // CHECK-NOT: copy_addr 
  // CHECK-LABEL: } // end sil function '$s31closure_lifetime_fixup_copyelim6NicateV4slowyS2iF'
  public func slow(_ x: Int) -> Int {
    let foo = frob.foo(x)
    switch foo {
    case .bar:
      return 10
    case .baz(let y):
      if y == 0 && isInitializing {
        return foos[x]
      } else {
        return y
      }
    }
  }
}

private let foos = [1, 2, 3]
