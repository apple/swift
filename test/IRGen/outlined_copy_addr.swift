// RUN: %target-swift-frontend -disable-type-layout -emit-ir -module-name outcopyaddr -primary-file %s | %FileCheck %s

public protocol BaseProt {
}

public protocol ChildProt: BaseProt {
}

public struct BaseStruct<T: BaseProt> {
    public typealias Element = T
    public var elem1: Element
    public var elem2: Element
}

public struct StructWithBaseStruct<T: BaseProt> {
    public typealias Element = T
    var elem1: Element
    var elem2: BaseStruct<Element>
}

// CHECK-LABEL: define hidden swiftcc void @"$s11outcopyaddr010StructWithbc4BaseB0V4elemAA0bcdB0VyxGvg"(ptr noalias nocapture sret({{.*}}) %0, ptr %"StructWithStructWithBaseStruct<T>", ptr noalias nocapture swiftself %1)
// CHECK: call ptr @"$s11outcopyaddr014StructWithBaseB0VyxGAA9ChildProtRzlWOc"
public struct StructWithStructWithBaseStruct<T: ChildProt> {
    public typealias Element = T
    let elem: StructWithBaseStruct<Element>
}

protocol P { }

class OtherPrivate<T> { }

struct OtherInternal<T> {
  var myPrivate: OtherPrivate<T>? = nil
}

struct MyPrivate<T: P> {
  var otherHelper: OtherInternal<T>? = nil

  // CHECK-LABEL: define hidden swiftcc {{i32|i64}} @"$s11outcopyaddr9MyPrivateVyACyxGxcfC"(ptr noalias nocapture %0, ptr %T, ptr %T.P) {{.*}} {
  // CHECK: call ptr @"$s11outcopyaddr9MyPrivateVyxGAA1PRzlWOh"(ptr {{%.*}})
  // CHECK: ret
  init(_: T) { }
}

extension P {
  func foo(data: Any) {
    _ = MyPrivate(data as! Self)
  }
}

enum GenericError<T: BaseProt> {
  case payload(T)
}

func testIt<P: BaseProt>(_ f: GenericError<P>?) {
}

func dontCrash<P: BaseProt>(_ f: GenericError<P>) {
  testIt(f)
}

protocol Baz : class {
}
extension Baz {
  static func crash(setup: ((Self) -> ())?){
  }
}
class Foobar {
  public static func dontCrash() -> Baz? {
     let cls : Baz.Type = Foobar1.self
     // This used to crash because we tried to outline the optional consume with
     // an opened payload existential type.
     cls.crash(setup: { (arg:  Baz) -> () in  })
     return nil
  }
}
class Foobar1 : Foobar, Baz { }
