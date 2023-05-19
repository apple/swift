// RUN: %target-swift-frontend -parse-as-library -primary-file %s -O -module-name=test -emit-sil | %FileCheck %s

import SwiftShims

@_optimize(none) public func make_test(_ x: Int) -> Int {
  return x
}

struct Foo {
  let x : Int
  let y : Int
  func both() -> Int { x + y }
}

// CHECK-NOT: sil_global private [let] {{.*}}unused1{{.*}}
private let unused1 = 0
// CHECK-NOT: sil_global private {{.*}}unused2{{.*}}
private var unused2 = 42
// CHECK: sil_global private @${{.*}}used1{{.*}} : $Int
private var used1 = 0
// CHECK: sil_global private @${{.*}}used2{{.*}} : $Int
private var used2 = 0

// non-constant / non-trivial values
// CHECK-NOT: sil_global private {{.*}}unused7{{.*}}
private let unused7 = make_test(42)
// CHECK-NOT: sil_global private {{.*}}unused8{{.*}}
private let unused8 = Foo(x: 1, y: 1)
// CHECK-NOT: sil_global private {{.*}}unused9{{.*}}
private let unused9 = Foo(x: 1, y: 1).both()

// CHECK: sil_global [let] @${{.*}}unused3{{.*}} : $Int
public let unused3 = 0
// CHECK: sil_global @${{.*}}unused4{{.*}} : $Int
public var unused4 = 0

// These should only be optimized with -wmo.
// CHECK: sil_global hidden [let] @${{.*}}unused5{{.*}} : $Int
// CHECK-WMO-NOT: sil_global hidden [let] @${{.*}}unused5{{.*}} : $Int
let unused5 = 0
// CHECK: sil_global hidden @${{.*}}unused6{{.*}} : $Int
// CHECK-WMO-NOT: sil_global hidden @${{.*}}unused6{{.*}} : $Int
var unused6 = 0

// Edge case: static and static with computed valued
// See Baz - line 71
// CHECK: sil_global [let] {{.*}}darwin{{.*}} : $Baz

// CHECK-LABEL: sil [Onone] @${{.*}}test{{.*}}
@_optimize(none) public func test(x: Int) -> Int {
  return used1 + used2 + x
}

// CHECK-LABEL: sil @${{.*}}storageVar{{.*}}
@inlinable
internal var storageVar: _SwiftEmptyArrayStorage {
  // CHECK: return %2 : $_SwiftEmptyArrayStorage
  return _swiftEmptyArrayStorage
}

public struct Bar {
  let storage: _SwiftEmptyArrayStorage
  
  init () {
    storage = storageVar
  }
}

public struct Baz {
  public init() { }

  public static let darwin = Baz()
  public static var currentPlatform: Baz { return .darwin }
}
