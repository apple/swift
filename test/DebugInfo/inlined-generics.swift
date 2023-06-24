// RUN: %target-swift-frontend -Xllvm -sil-inline-generics=true %s -O -g -o - -emit-ir | %FileCheck %s
public protocol P {
  associatedtype DT1
  func getDT() -> DT1
}
 
@inline(__always)
func foo1<T:P>(_ t: T, _ dt: T.DT1) -> T.DT1 {
  var dttmp: T.DT1 = dt
  return dttmp
}

// CHECK: define {{.*}}@"$s4main4foo2yyxAA1PRzlF"
public func foo2<S:P>(_ s: S) {
  // CHECK: call void @llvm.dbg.value(metadata ptr %S,
  // CHECK-SAME:                     metadata ![[META:[0-9]+]]
  foo1(s, s.getDT())
  // T should get substituted with S
  // CHECK: ![[META]] = !DILocalVariable(name: "$\CF\84_0_0"
}


// More complex example -- we inline an alloc_stack that references a VarDecl
// with generic type.

public protocol Q : class {
  associatedtype T : Equatable

  var old: T? { get set }
  var new: T? { get }
}

@inline(__always)
public func update<S : Q>(_ s: S) -> (S.T?, S.T?)? {
  let oldValue = s.old
  let newValue = s.new

  if oldValue != newValue {
    s.old = newValue
    return (oldValue, newValue)
  } else {
    return nil
  }
}

public class C : Q {
  public typealias T = String

  public var old: String? = nil
  public var new: T? = nil
}

public func updateC(_ c: C) {
  update(c)
}
