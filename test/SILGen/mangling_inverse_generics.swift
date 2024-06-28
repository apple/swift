// RUN: %empty-directory(%t)
// RUN: %target-swift-emit-silgen %s -module-name test \
// RUN:   -enable-experimental-feature NonescapableTypes \
// RUN:   -parse-as-library \
// RUN:   > %t/test.silgen

// RUN: %FileCheck %s < %t/test.silgen
// RUN: %swift-demangle < %t/test.silgen | %FileCheck %s --check-prefix=DEMANGLED


protocol NoncopyableProto: ~Copyable {}

protocol CopyableProto {}

protocol CopyableProto2 {}

struct S: MyProto, Nothing, Empty {}
class ClassDef {}
protocol MyProto: ~Copyable {}
protocol Nothing: ~Copyable, ~Escapable {}
protocol Empty: ~Copyable, ~Escapable {}
protocol Hello<Person>: ~Copyable, ~Escapable {
  associatedtype Person
}

// CHECK: sil_global private @$s4test13global__old___Wz : $Builtin.Word

// DEMANGLED: test.global__old__ : test.MyProto
// CHECK: sil_global hidden [let] @$s4test13global__old__AA7MyProto_pvp : $any MyProto
let global__old__: any MyProto = S()

// CHECK: sil_global private @$s4test13global__new___Wz : $Builtin.Word

// DEMANGLED: test.global__new__ : any test.MyProto<Self: ~Swift.Copyable>
// CHECK: sil_global hidden [let] @$s4test13global__new__AA7MyProto_pRi_s_XPvp : $any MyProto & ~Copyable
let global__new__: any MyProto & ~Copyable = S()

// DEMANGLED: test.global__none__ : any test.Nothing<Self: ~Swift.Copyable, Self: ~Swift.Escapable>
// CHECK: sil_global hidden [let] @$s4test14global__none__AA7Nothing_pRi_s_Ri0_sXPvp : $any Nothing & ~Copyable & ~Escapable
let global__none__: any Nothing & ~Copyable & ~Escapable = S()

// DEMANGLED: test.global__none2__ : any test.Empty & test.Nothing<Self: ~Swift.Copyable, Self: ~Swift.Escapable>
// CHECK: sil_global hidden [let] @$s4test15global__none2__AA5Empty_AA7NothingpRi_s_Ri0_sXPvp : $any Empty & Nothing & ~Copyable & ~Escapable
let global__none2__: any Nothing & ~Copyable & Empty & ~Escapable = S()

// DEMANGLED: test.global__any1__ : Any
// CHECK: sil_global hidden [let] @$s4test14global__any1__ypvp : $any Copyable
let global__any1__: any Copyable = S()

// DEMANGLED: test.global__any2__ : Any
// CHECK: sil_global hidden [let] @$s4test14global__any2__ypvp : $any Escapable
let global__any2__: any Escapable = S()

// DEMANGLED: test.global__any3__ : Any
// CHECK: sil_global hidden [let] @$s4test14global__any3__ypvp : $Any
let global__any3__: Any = S()

// DEMANGLED: test.global__any4__ : Any
// CHECK: sil_global hidden [let] @$s4test14global__any4__ypvp : $any Copyable & Escapable
let global__any4__: any Copyable & Escapable = S()

// DEMANGLED: test.global__inv1__ : any Any<Self: ~Swift.Copyable>
// CHECK: sil_global hidden [let] @$s4test14global__inv1__ypRi_s_XPvp : $any ~Copyable
let global__inv1__: any ~Copyable = S()

// DEMANGLED: test.global__inv2__ : any Any<Self: ~Swift.Escapable>
// CHECK: sil_global hidden [let] @$s4test14global__inv2__ypRi0_s_XPvp : $any ~Escapable
let global__inv2__: any ~Escapable = S()

// DEMANGLED: test.global__inv3__ : any Any<Self: ~Swift.Copyable, Self: ~Swift.Escapable>
// CHECK: sil_global hidden [let] @$s4test14global__inv3__ypRi_s_Ri0_sXPvp : $any ~Copyable & ~Escapable
let global__inv3__: any ~Escapable & ~Copyable = S()

// DEMANGLED: test.global__anyObj1__ : Swift.AnyObject
// CHECK: sil_global hidden [let] @$s4test17global__anyObj1__yXlvp : $Copyable & Escapable & AnyObject
let global__anyObj1__: any AnyObject & Copyable & Escapable = ClassDef()

// DEMANGLED: test.global__anyObj2__ : Swift.AnyObject
// CHECK: sil_global hidden [let] @$s4test17global__anyObj2__yXlvp : $AnyObject
let global__anyObj2__: any AnyObject = ClassDef()

// CHECK: sil hidden [ossa] @$s4test1SVACycfC : $@convention(method) (@thin S.Type) -> S {

// CHECK: sil private [global_init_once_fn] [ossa] @$s4test13global__old___WZ : $@convention(c) (Builtin.RawPointer) -> () {
// CHECK: sil hidden [global_init] [ossa] @$s4test13global__old__AA7MyProto_pvau : $@convention(thin) () -> Builtin.RawPointer {

// CHECK: sil private [global_init_once_fn] [ossa] @$s4test13global__new___WZ : $@convention(c) (Builtin.RawPointer) -> () {
// CHECK: sil hidden [global_init] [ossa] @$s4test13global__new__AA7MyProto_pRi_s_XPvau : $@convention(thin) () -> Builtin.RawPointer {


// DEMANGLED: test.global__computed__.getter : any Any<Self: ~Swift.Copyable>
// CHECK: sil hidden [ossa] @$s4test18global__computed__ypRi_s_XPvg : $@convention(thin) () -> @out any ~Copyable {

// DEMANGLED: test.global__computed__.setter : any Any<Self: ~Swift.Copyable>
// CHECK: sil hidden [ossa] @$s4test18global__computed__ypRi_s_XPvs : $@convention(thin) (@in any ~Copyable) -> () {
var global__computed__: any ~Copyable {
  get { S() }
  set {}
}

// <T: ~Copyable>
public struct A<T: ~Copyable>: ~Copyable {
  // Members in an extension with the same generic signature as the nominal
  // should mangle the same, so make sure we mangle the following as if it were
  // in a constrained extension.

  // DEMANGLED: (extension in test):test.A< where A: ~Swift.Copyable>.foo() -> ()
  // CHECK: sil [ossa] @$s4test1AVAARi_zrlE3fooyyF : $@convention(method) <T where T : ~Copyable> (@guaranteed A<T>) -> () {
  public func foo() {}

  // DEMANGLED: test.A.weird() -> ()
  // CHECK: sil [ossa] @$s4test1AV5weirdyyF : $@convention(method) <T> (A<T>) -> () {
  public func weird() where T: Copyable {}

  // DEMANGLED: variable initialization expression of (extension in test):test.A< where A: ~Swift.Copyable>.property : Swift.Int
  // CHECK: sil [transparent] [ossa] @$s4test1AVAARi_zrlE8propertySivpfi : $@convention(thin) <T where T : ~Copyable> () -> Int {

  // DEMANGLED: (extension in test):test.A< where A: ~Swift.Copyable>.property.getter : Swift.Int
  // CHECK: sil [transparent] [serialized] [ossa] @$s4test1AVAARi_zrlE8propertySivg : $@convention(method) <T where T : ~Copyable> (@guaranteed A<T>) -> Int {

  // DEMANGLED: (extension in test):test.A< where A: ~Swift.Copyable>.property.setter : Swift.Int
  // CHECK: sil [transparent] [serialized] [ossa] @$s4test1AVAARi_zrlE8propertySivs : $@convention(method) <T where T : ~Copyable> (Int, @inout A<T>) -> () {

  // DEMANGLED: (extension in test):test.A< where A: ~Swift.Copyable>.property.modify : Swift.Int
  // CHECK: sil [transparent] [serialized] [ossa] @$s4test1AVAARi_zrlE8propertySivM : $@yield_once @convention(method) <T where T : ~Copyable> (@inout A<T>) -> @yields @inout Int {
  public var property: Int = 0

  // DEMANGLED: (extension in test):test.A< where A: ~Swift.Copyable>.computed.getter : Swift.String
  // CHECK: sil [ossa] @$s4test1AVAARi_zrlE8computedSSvg : $@convention(method) <T where T : ~Copyable> (@guaranteed A<T>) -> @owned String {
  public var computed: String {
    ""
  }

  // DEMANGLED: static (extension in test):test.A< where A: ~Swift.Copyable>.forceInits() -> test.A<A>
  // CHECK: sil hidden [ossa] @$s4test1AVAARi_zrlE10forceInitsACyxGyFZ : $@convention(method) <T where T : ~Copyable> (@thin A<T>.Type) -> @owned A<T> {
  static func forceInits() -> Self {
    _ = Self(property: 100)
    return Self()
  }

  // DEMANGLED: (extension in test):test.A< where A: ~Swift.Copyable>.init(property: Swift.Int) -> test.A<A>
  // CHECK: sil hidden [ossa] @$s4test1AVAARi_zrlE8propertyACyxGSi_tcfC : $@convention(method) <T where T : ~Copyable> (Int, @thin A<T>.Type) -> @owned A<T> {

  // DEMANGLED: (extension in test):test.A< where A: ~Swift.Copyable>.init() -> test.A<A>
  // CHECK: sil hidden [ossa] @$s4test1AVAARi_zrlEACyxGycfC : $@convention(method) <T where T : ~Copyable> (@thin A<T>.Type) -> @owned A<T> {
}

extension A: Copyable where T: Copyable {}

// <T: ~Copyable>
extension A where T: ~Copyable {
  // DEMANGLED: (extension in test):test.A< where A: ~Swift.Copyable>.bar() -> ()
  // CHECK: sil hidden [ossa] @$s4test1AVAARi_zrlE3baryyF : $@convention(method) <T where T : ~Copyable> (@guaranteed A<T>) -> () {
  func bar() {}

  // DEMANGLED: test.A.weird2() -> ()
  // CHECK: sil hidden [ossa] @$s4test1AV6weird2yyF : $@convention(method) <T> (A<T>) -> () {
  func weird2() where T: Copyable {}
}

// <T: ~Copyable & NoncopyableProto>
extension A where T: ~Copyable & NoncopyableProto {
  // DEMANGLED: (extension in test):test.A< where A: test.NoncopyableProto, A: ~Swift.Copyable>.dumb() -> ()
  // CHECK: sil hidden [ossa] @$s4test1AVA2A16NoncopyableProtoRzRi_zrlE4dumbyyF : $@convention(method) <T where T : NoncopyableProto, T : ~Copyable> (@guaranteed A<T>) -> () {
  func dumb() {}
}

// <T: Copyable>
extension A {
  // However, an extension that has all of the positive requirements to inverse
  // requirements must be mangled as if there were no inverse generics.

  // DEMANGLED: test.A.baz() -> ()
  // CHECK: sil hidden [ossa] @$s4test1AV3bazyyF : $@convention(method) <T> (A<T>) -> () {
  func baz() {}

  // DEMANGLED: test.A.computedAgain.getter : Swift.Int
  // CHECK: sil hidden [ossa] @$s4test1AV13computedAgainSivg : $@convention(method) <T> (A<T>) -> Int {
  var computedAgain: Int {
    123
  }
}

// <T: CopyableProto> (implies T: Copyable)
extension A where T: CopyableProto {
  // An extension where T: Copyable, but has extra constraints must be mangle
  // the extra constraints and not any inverses.

  // DEMANGLED: (extension in test):test.A<A where A: test.CopyableProto>.something() -> ()
  // CHECK: sil hidden [ossa] @$s4test1AVA2A13CopyableProtoRzlE9somethingyyF : $@convention(method) <T where T : CopyableProto> (A<T>) -> () {
  func something() {}
}

// <T == Int>
extension A where T == Int {
  // DEMANGLED: (extension in test):test.A<A where A == Swift.Int>.int() -> ()
  // CHECK: sil hidden [ossa] @$s4test1AVAASiRszlE3intyyF : $@convention(method) (A<Int>) -> () {
  func int() {}
}

// <T: ~Copyable & NoncopyableProto>
struct B<T: ~Copyable & NoncopyableProto>: ~Copyable {}

// <T: Copyable & NoncopyableProto>
extension B {
  // Members of this extension should be treated as if they were just in 'B' and
  // not mangle any requirements about 'NoncopyableProto' because that's implied
  // by the context.

  // DEMANGLED: test.B.foo() -> ()
  // CHECK: sil hidden [ossa] @$s4test1BV3fooyyF : $@convention(method) <T where T : NoncopyableProto> (@guaranteed B<T>) -> () {
  func foo() {}
}

// <T: Copyable>
struct C<T> {
  // DEMANGLED: test.C.foo() -> ()
  // CHECK: sil hidden [ossa] @$s4test1CV3fooyyF : $@convention(method) <T> (C<T>) -> () {
  func foo() {}

  // DEMANGLED: test.C.something<A where A1: ~Swift.Copyable>() -> A1
  // CHECK: sil hidden [ossa] @$s4test1CV9somethingqd__yRi_d__lF : $@convention(method) <T><U where U : ~Copyable> (C<T>) -> @out U {
  func something<U: ~Copyable>() -> U {
    fatalError()
  }
}

// <T: Copyable>
extension C {
  // DEMANGLED: test.C.bar() -> ()
  // CHECK: sil hidden [ossa] @$s4test1CV3baryyF : $@convention(method) <T> (C<T>) -> () {
  func bar() {}
}

// <T: CopyableProto> (implies T: Copyable)
extension C where T: CopyableProto {
  // DEMANGLED: (extension in test):test.C<A where A: test.CopyableProto>.baz() -> ()
  // CHECK: sil hidden [ossa] @$s4test1CVA2A13CopyableProtoRzlE3bazyyF : $@convention(method) <T where T : CopyableProto> (C<T>) -> () {
  func baz() {}
}

// <T: CopyableProto> (implies T: Copyable)
struct D<T: CopyableProto> {
  // DEMANGLED: test.D.foo() -> ()
  // CHECK: sil hidden [ossa] @$s4test1DV3fooyyF : $@convention(method) <T where T : CopyableProto> (D<T>) -> () {
  func foo() {}
}

// <T: CopyableProto> (implies T: Copyable)
extension D {
  // DEMANGLED: test.D.bar() -> ()
  // CHECK: sil hidden [ossa] @$s4test1DV3baryyF : $@convention(method) <T where T : CopyableProto> (D<T>) -> () {
  func bar() {}
}

// <T: CopyableProto & CopyableProto2> (implies T: Copyable)
extension D where T: CopyableProto2 {
  // DEMANGLED: (extension in test):test.D< where A: test.CopyableProto2>.baz() -> ()
  // CHECK: sil hidden [ossa] @$s4test1DVA2A14CopyableProto2RzrlE3bazyyF : $@convention(method) <T where T : CopyableProto, T : CopyableProto2> (D<T>) -> () {
  func baz() {}
}

//===----------------------------------------------------------------------===//
// @_preInverseGenerics
//===----------------------------------------------------------------------===//

// Members with @_preInverseGenerics should completely ignore inverse
// requirements.

// <T: ~Copyable>
public struct E<T: ~Copyable>: ~Copyable {
  // DEMANGLED: test.E.foo() -> ()
  // CHECK: sil [ossa] @$s4test1EV3fooyyF : $@convention(method) <T where T : ~Copyable> (@guaranteed E<T>) -> () {
  @_preInverseGenerics
  public func foo() {}

  // Existentials are also affected by the attribute.
  // DEMANGLE: test.E.__existential1__(__owned Any) -> ()
  // CHECK: sil [ossa] @$s4test1EV16__existential1__yyypnF : $@convention(method) <T where T : ~Copyable> (@in any ~Copyable, @guaranteed E<T>) -> () {
  @_preInverseGenerics
  public func __existential1__(_ t: consuming any ~Copyable) {}

  // DEMANGLE: (extension in test):test.E< where A: ~Swift.Copyable>.__existential2__(__owned any Any<Self: ~Swift.Copyable>) -> ()
  // CHECK: sil [ossa] @$s4test1EVAARi_zrlE16__existential2__yyypRi_s_XPnF : $@convention(method) <T where T : ~Copyable> (@in any ~Copyable, @guaranteed E<T>) -> () {
  public func __existential2__(_ t: consuming any ~Copyable) {}

  // DEMANGLED: test.E.something<A>() -> A1
  // CHECK: sil [ossa] @$s4test1EV9somethingqd__ylF : $@convention(method) <T where T : ~Copyable><U where U : ~Copyable> (@guaranteed E<T>) -> @out U {
  @_preInverseGenerics
  public func something<U: ~Copyable>() -> U {
    fatalError()
  }

  // DEMANGLED: (extension in test):test.E< where A: ~Swift.Copyable>.something<A>() -> A1
  // CHECK: sil [ossa] @$s4test1EVAARi_zrlE9somethingqd__ylF : $@convention(method) <T where T : ~Copyable><U> (@guaranteed E<T>) -> @out U {
  public func something<U>() -> U {
    fatalError()
  }

  // DEMANGLED: test.E.something2<A>() -> A1
  // CHECK: sil [ossa] @$s4test1EV10something2qd__ylF : $@convention(method) <T><U> (@guaranteed E<T>) -> @out U {
  public func something2<U>() -> U where T: Copyable {
    fatalError()
  }

  // DEMANGLED: test.E.something2<A where A1: ~Swift.Copyable>() -> A1
  // CHECK: sil [ossa] @$s4test1EV10something2qd__yRi_d__lF : $@convention(method) <T><U where U : ~Copyable> (@guaranteed E<T>) -> @out U {
  public func something2<U: ~Copyable>() -> U where T: Copyable {
    fatalError()
  }

  // DEMANGLED: variable initialization expression of test.E.property : Swift.Int
  // CHECK: sil [transparent] [ossa] @$s4test1EV8propertySivpfi : $@convention(thin) <T where T : ~Copyable> () -> Int {

  // DEMANGLED: test.E.property.getter : Swift.Int
  // CHECK: sil [transparent] [serialized] [ossa] @$s4test1EV8propertySivg : $@convention(method) <T where T : ~Copyable> (@guaranteed E<T>) -> Int {

  // DEMANGLED: test.E.property.setter : Swift.Int
  // CHECK: sil [transparent] [serialized] [ossa] @$s4test1EV8propertySivs : $@convention(method) <T where T : ~Copyable> (Int, @inout E<T>) -> () {

  // DEMANGLED: test.E.property.modify : Swift.Int
  // CHECK: sil [transparent] [serialized] [ossa] @$s4test1EV8propertySivM : $@yield_once @convention(method) <T where T : ~Copyable> (@inout E<T>) -> @yields @inout Int {
  @_preInverseGenerics
  public var property: Int = 12345 + 213214 + 123124

  // NOTE: the implicit initializers still have inverses!

  // DEMANGLED: (extension in test):test.E< where A: ~Swift.Copyable>.init() -> test.E<A>
  // CHECK: sil hidden [ossa] @$s4test1EVAARi_zrlEACyxGycfC : $@convention(method) <T where T : ~Copyable> (@thin E<T>.Type) -> @owned E<T> {

  // DEMANGLED: (extension in test):test.E< where A: ~Swift.Copyable>.init(property: Swift.Int) -> test.E<A>
  // CHECK: sil hidden [ossa] @$s4test1EVAARi_zrlE8propertyACyxGSi_tcfC : $@convention(method) <T where T : ~Copyable> (Int, @thin E<T>.Type) -> @owned E<T> {
}

func forceInit() {
    _ = E<Int>()
    _ = E<Int>(property: 123454555555)
}

// <T: ~Copyable>
extension E where T: ~Copyable {
  // DEMANGLED: test.E.bar() -> ()
  // CHECK: sil hidden [ossa] @$s4test1EV3baryyF : $@convention(method) <T where T : ~Copyable> (@guaranteed E<T>) -> () {
  @_preInverseGenerics
  func bar() {}
}

// <T: ~Copyable & NoncopyableProto>
extension E where T: ~Copyable & NoncopyableProto {
  // DEMANGLED: (extension in test):test.E<A where A: test.NoncopyableProto>.dumb() -> ()
  // CHECK: sil hidden [ossa] @$s4test1EVA2A16NoncopyableProtoRzlE4dumbyyF : $@convention(method) <T where T : NoncopyableProto, T : ~Copyable> (@guaranteed E<T>) -> () {
  @_preInverseGenerics
  func dumb() {}
}


