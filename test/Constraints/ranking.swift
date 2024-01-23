// RUN: %target-swift-emit-silgen %s -verify -swift-version 5 | %FileCheck %s

protocol P {
  var p: P { get set }
  var q: P? { get set }
  func p(_: P)
  func q(_: P)
}

struct S : P {
  var p: P
  var q: P?
  func p(_: P) {}
  func q(_: P) {}
}

class Base : P {
  var p: P
  var q: P?
  func p(_: P) {}
  func q(_: P) {}
  init(_ p: P) { self.p = p }
}

class Derived : Base {
}

func genericOverload<T>(_: T) {}
func genericOverload<T>(_: T?) {}
func genericOptional<T>(_: T?) {}
func genericNoOptional<T>(_: T) {}

// CHECK-LABEL: sil hidden [ossa] @$s7ranking22propertyVersusFunctionyyAA1P_p_xtAaCRzlF
func propertyVersusFunction<T : P>(_ p: P, _ t: T) {
  // CHECK: witness_method $@opened("{{.*}}", any P) Self, #P.p!getter
  let _ = p.p
  // CHECK: witness_method $@opened("{{.*}}", any P) Self, #P.p!getter
  let _: P = p.p
  // CHECK: function_ref @$s7ranking22propertyVersusFunctionyyAA1P_p_xtAaCRzlFyAaC_pcAaC_pcfu_ : $@convention(thin) (@in_guaranteed any P) -> @owned @callee_guaranteed (@in_guaranteed any P) -> ()
  let _: (P) -> () = p.p
  // CHECK: witness_method $@opened("{{.*}}", any P) Self, #P.p!getter
  let _: P? = p.p
  // CHECK: witness_method $@opened("{{.*}}", any P) Self, #P.p!getter
  let _: Any = p.p
  // CHECK: witness_method $@opened("{{.*}}", any P) Self, #P.p!getter
  let _: Any? = p.p

  // CHECK: witness_method $@opened("{{.*}}", any P) Self, #P.p!getter
  // CHECK: function_ref @$s7ranking15genericOverloadyyxlF
  genericOverload(p.p)
  // CHECK: witness_method $@opened("{{.*}}", any P) Self, #P.q!getter
  // CHECK: function_ref @$s7ranking15genericOverloadyyxSglF
  genericOverload(p.q)
  // CHECK: witness_method $@opened("{{.*}}", any P) Self, #P.p!getter
  // CHECK: function_ref @$s7ranking15genericOptionalyyxSglF
  genericOptional(p.p)
  // CHECK: witness_method $@opened("{{.*}}", any P) Self, #P.q!getter
  // CHECK: function_ref @$s7ranking15genericOptionalyyxSglF
  genericOptional(p.q)
  // CHECK: witness_method $@opened("{{.*}}", any P) Self, #P.p!getter
  // CHECK: function_ref @$s7ranking17genericNoOptionalyyxlF
  genericNoOptional(p.p)
  // CHECK: witness_method $@opened("{{.*}}", any P) Self, #P.q!getter
  // CHECK: function_ref @$s7ranking17genericNoOptionalyyxlF
  genericNoOptional(p.q)

  // CHECK: witness_method $T, #P.p!getter
  let _ = t.p
  // CHECK: witness_method $T, #P.p!getter
  let _: P = t.p
  // CHECK: function_ref @$s7ranking22propertyVersusFunctionyyAA1P_p_xtAaCRzlFyAaC_pcxcfu1_ : $@convention(thin) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> @owned @callee_guaranteed (@in_guaranteed any P) -> ()
  let _: (P) -> () = t.p
  // CHECK: witness_method $T, #P.p!getter
  let _: P? = t.p
  // CHECK: witness_method $T, #P.p!getter
  let _: Any = t.p
  // CHECK: witness_method $T, #P.p!getter
  let _: Any? = t.p

  // CHECK: witness_method $T, #P.p!getter
  // CHECK: function_ref @$s7ranking15genericOverloadyyxlF
  genericOverload(t.p)
  // CHECK: witness_method $T, #P.q!getter
  // CHECK: function_ref @$s7ranking15genericOverloadyyxSglF
  genericOverload(t.q)
  // CHECK: witness_method $T, #P.p!getter
  // CHECK: function_ref @$s7ranking15genericOptionalyyxSglF
  genericOptional(t.p)
  // CHECK: witness_method $T, #P.q!getter
  // CHECK: function_ref @$s7ranking15genericOptionalyyxSglF
  genericOptional(t.q)
  // CHECK: witness_method $T, #P.p!getter
  // CHECK: function_ref @$s7ranking17genericNoOptionalyyxlF
  genericNoOptional(t.p)
  // CHECK: witness_method $T, #P.q!getter
  // CHECK: function_ref @$s7ranking17genericNoOptionalyyxlF
  genericNoOptional(t.q)
}

extension P {
  func propertyVersusFunction() {
    // CHECK: witness_method $Self, #P.p!getter
    let _ = self.p
    // CHECK: witness_method $Self, #P.p!getter
    let _: P = self.p
    // CHECK: function_ref @$s7ranking1PPAAE22propertyVersusFunctionyyFyAaB_pcxcfu_ : $@convention(thin) <τ_0_0 where τ_0_0 : P> (@in_guaranteed τ_0_0) -> @owned @callee_guaranteed (@in_guaranteed any P) -> ()
    let _: (P) -> () = self.p
    // CHECK: witness_method $Self, #P.p!getter
    let _: P? = self.p
    // CHECK: witness_method $Self, #P.p!getter
    let _: Any = self.p
    // CHECK: witness_method $Self, #P.p!getter
    let _: Any? = self.p

    // CHECK: witness_method $Self, #P.p!getter
    // CHECK: function_ref @$s7ranking15genericOverloadyyxlF
    genericOverload(self.p)
    // CHECK: witness_method $Self, #P.q!getter
    // CHECK: function_ref @$s7ranking15genericOverloadyyxSglF
    genericOverload(self.q)
    // CHECK: witness_method $Self, #P.p!getter
    // CHECK: function_ref @$s7ranking15genericOptionalyyxSglF
    genericOptional(self.p)
    // CHECK: witness_method $Self, #P.q!getter
    // CHECK: function_ref @$s7ranking15genericOptionalyyxSglF
    genericOptional(self.q)
    // CHECK: witness_method $Self, #P.p!getter
    // CHECK: function_ref @$s7ranking17genericNoOptionalyyxlF
    genericNoOptional(self.p)
    // CHECK: witness_method $Self, #P.q!getter
    // CHECK: function_ref @$s7ranking17genericNoOptionalyyxlF
    genericNoOptional(self.q)
  }
}

//--------------------------------------------------------------------

func f0<T>(_ x: T) {}

// FIXME: Lookup breaks if these come after f1!
class A { 
  init() {} 
};
class B : A { 
  override init() { super.init() } 
}

func f1(_ a: A) -> A { return a }
func f1(_ b: B) -> B { return b }

func testDerived(b: B) {
  // CHECK-LABEL: sil hidden [ossa] @$s7ranking11testDerived1byAA1BC_tF
  // CHECK: function_ref @$s7ranking2f1yAA1BCADF
  // CHECK: function_ref @$s7ranking2f0yyxlF
  f0(f1(b))
  // CHECK: end sil function '$s7ranking11testDerived1byAA1BC_tF'
}

protocol X {
  var foo: Int { get }
  var bar: Int { get }
  func baz() -> Int
  subscript(foo: String) -> Int { get }
}

class Y {
  var foo: Int = 0
  func baz() -> Int { return foo }
  subscript(foo: String) -> Int { return 0 }
}
extension Y {
  var bar: Int { return foo }
}

protocol Z : Y {
  var foo: Int { get }
  var bar: Int { get }
  func baz() -> Int
  subscript(foo: String) -> Int { get }
}

class GenericClass<T> {
  var foo: T
  init(_ foo: T) { self.foo = foo }
  func baz() -> T { return foo }
}
extension GenericClass {
  var bar: T { return foo }
  subscript(foo: String) -> Int { return 0 }
}

// Make sure we favour the class implementation over the protocol requirement.

// CHECK-LABEL: sil hidden [ossa] @$s7ranking32testGenericPropertyProtocolClassyyxAA1YCRbzAA1XRzlF
func testGenericPropertyProtocolClass<T : X & Y>(_ t: T) {
  _ = t.foo   // CHECK: class_method {{%.*}} : $Y, #Y.foo!getter
  _ = t.bar   // CHECK: function_ref @$s7ranking1YC3barSivg
  _ = t.baz() // CHECK: class_method {{%.*}} : $Y, #Y.baz
  _ = t[""]   // CHECK: class_method {{%.*}} : $Y, #Y.subscript!getter
}

// CHECK-LABEL: sil hidden [ossa] @$s7ranking36testExistentialPropertyProtocolClassyyAA1X_AA1YCXcF
func testExistentialPropertyProtocolClass(_ t: X & Y) {
  _ = t.foo   // CHECK: class_method {{%.*}} : $Y, #Y.foo!getter
  _ = t.bar   // CHECK: function_ref @$s7ranking1YC3barSivg
  _ = t.baz() // CHECK: class_method {{%.*}} : $Y, #Y.baz
  _ = t[""]   // CHECK: class_method {{%.*}} : $Y, #Y.subscript!getter
}

// CHECK-LABEL: sil hidden [ossa] @$s7ranking46testGenericPropertySubclassConstrainedProtocolyyxAA1ZRzlF
func testGenericPropertySubclassConstrainedProtocol<T : Z>(_ t: T) {
  _ = t.foo   // CHECK: class_method {{%.*}} : $Y, #Y.foo!getter
  _ = t.bar   // CHECK: function_ref @$s7ranking1YC3barSivg
  _ = t.baz() // CHECK: class_method {{%.*}} : $Y, #Y.baz
  _ = t[""]   // CHECK: class_method {{%.*}} : $Y, #Y.subscript!getter
}

// CHECK-LABEL: sil hidden [ossa] @$s7ranking50testExistentialPropertySubclassConstrainedProtocolyyAA1Z_pF
func testExistentialPropertySubclassConstrainedProtocol(_ t: Z) {
  _ = t.foo   // CHECK: class_method {{%.*}} : $Y, #Y.foo!getter
  _ = t.bar   // CHECK: function_ref @$s7ranking1YC3barSivg
  _ = t.baz() // CHECK: class_method {{%.*}} : $Y, #Y.baz
  _ = t[""]   // CHECK: class_method {{%.*}} : $Y, #Y.subscript!getter
}

// CHECK-LABEL: sil hidden [ossa] @$s7ranking43testExistentialPropertyProtocolGenericClassyyAA1X_AA0fG0CySiGXcF
func testExistentialPropertyProtocolGenericClass(_ t: GenericClass<Int> & X) {
  _ = t.foo   // CHECK: class_method {{%.*}} : $GenericClass<Int>, #GenericClass.foo!getter
  _ = t.bar   // CHECK: function_ref @$s7ranking12GenericClassC3barxvg
  _ = t.baz() // CHECK: class_method {{%.*}} : $GenericClass<Int>, #GenericClass.baz
  _ = t[""]   // CHECK: function_ref @$s7ranking12GenericClassCySiSScig
}

// CHECK-LABEL: sil hidden [ossa] @$s7ranking43testExistentialPropertyProtocolGenericClassyyAA1X_AA0fG0CySSGXcF
func testExistentialPropertyProtocolGenericClass(_ t: GenericClass<String> & X) {
  _ = t.foo   // CHECK: class_method {{%.*}} : $GenericClass<String>, #GenericClass.foo!getter
  _ = t.bar   // CHECK: function_ref @$s7ranking12GenericClassC3barxvg
  _ = t.baz() // CHECK: class_method {{%.*}} : $GenericClass<String>, #GenericClass.baz
  _ = t[""]   // CHECK: function_ref @$s7ranking12GenericClassCySiSScig
}

extension X where Self : Y {
  // CHECK-LABEL: sil hidden [ossa] @$s7ranking1XPA2A1YCRbzrlE32testGenericPropertyProtocolClassyyxF
  func testGenericPropertyProtocolClass(_ x: Self) {
    _ = self.foo   // CHECK: class_method {{%.*}} : $Y, #Y.foo!getter
    _ = self.bar   // CHECK: function_ref @$s7ranking1YC3barSivg
    _ = self.baz() // CHECK: class_method {{%.*}} : $Y, #Y.baz
    _ = self[""]   // CHECK: class_method {{%.*}} : $Y, #Y.subscript!getter
  }
}

extension X where Self : GenericClass<Int> {
  // CHECK-LABEL: sil hidden [ossa] @$s7ranking1XPA2A12GenericClassCySiGRbzrlE04testb16PropertyProtocolbC0yyxF
  func testGenericPropertyProtocolGenericClass(_ x: Self) {
    _ = self.foo   // CHECK: class_method {{%.*}} : $GenericClass<Int>, #GenericClass.foo!getter
    _ = self.bar   // CHECK: function_ref @$s7ranking12GenericClassC3barxvg
    _ = self.baz() // CHECK: class_method {{%.*}} : $GenericClass<Int>, #GenericClass.baz
    _ = self[""]   // CHECK: function_ref @$s7ranking12GenericClassCySiSScig
  }
}

extension X where Self : GenericClass<String> {
  // CHECK-LABEL: sil hidden [ossa] @$s7ranking1XPA2A12GenericClassCySSGRbzrlE04testb16PropertyProtocolbC0yyxF
  func testGenericPropertyProtocolGenericClass(_ x: Self) {
    _ = self.foo   // CHECK: class_method {{%.*}} : $GenericClass<String>, #GenericClass.foo!getter
    _ = self.bar   // CHECK: function_ref @$s7ranking12GenericClassC3barxvg
    _ = self.baz() // CHECK: class_method {{%.*}} : $GenericClass<String>, #GenericClass.baz
    _ = self[""]   // CHECK: function_ref @$s7ranking12GenericClassCySiSScig
  }
}

//--------------------------------------------------------------------
// Constructor-specific ranking
//--------------------------------------------------------------------

// We have a special ranking rule that only currently applies to constructors,
// and compares the concrete parameter types.

protocol Q {
  init()
}

struct S1<T : Q> {
  // We want to prefer the non-optional init over the optional init here.
  init(_ x: T = .init()) {}
  init(_ x: T? = nil) {}

  // CHECK-LABEL: sil hidden [ossa] @$s7ranking2S1V11testRankingACyxGyt_tcfC
  init(testRanking: Void) {
    // CHECK: function_ref @$s7ranking2S1VyACyxGxcfC : $@convention(method) <τ_0_0 where τ_0_0 : Q> (@in τ_0_0, @thin S1<τ_0_0>.Type) -> S1<τ_0_0>
    self.init()
  }

  // CHECK-LABEL: sil hidden [ossa] @$s7ranking2S1V15testInitRankingyyF
  func testInitRanking() {
    // CHECK: function_ref @$s7ranking2S1VyACyxGxcfC : $@convention(method) <τ_0_0 where τ_0_0 : Q> (@in τ_0_0, @thin S1<τ_0_0>.Type) -> S1<τ_0_0>
    _ = S1<T>()
  }
}

protocol R {}
extension Array : R {}
extension Int : R {}

struct S2 {
  init(_ x: R) {}
  init(_ x: Int...) {}

  // CHECK-LABEL: sil hidden [ossa] @$s7ranking2S2V15testInitRankingyyF
  func testInitRanking() {
    // We currently prefer the non-variadic init due to having
    // "less effective parameters", and we don't compare the types for ranking due
    // to the difference in variadic-ness.
    // CHECK: function_ref @$s7ranking2S2VyAcA1R_pcfC : $@convention(method) (@in any R, @thin S2.Type) -> S2
    _ = S2(0)
  }
}

// Very cursed: As a holdover from how we used to represent function inputs,
// we rank these as tuples and consider (x:x:) to be a subtype of (x:y:). Seems
// unlikely this is being relied on in the real world, but let's at least have
// it as a test case to track its behavior.
struct S3 {
  init(x _: Int = 0, y _: Int = 0) {}
  init(x _: Int = 0, x _: Int = 0) {}

  func testInitRanking() {
    // CHECK: function_ref @$s7ranking2S3V1xAdCSi_SitcfC : $@convention(method) (Int, Int, @thin S3.Type) -> S3
    _ = S3()
  }
}

// Also another consequence of having ranked as tuples: we prefer the unlabeled
// init here.
struct S4 {
  init(x: Int = 0, y: Int = 0) {}
  init(_ x: Int = 0, _ y: Int = 0) {}

  // CHECK-LABEL: sil hidden [ossa] @$s7ranking2S4V15testInitRankingyyF
  func testInitRanking() {
    // CHECK: function_ref @$s7ranking2S4VyACSi_SitcfC : $@convention(method) (Int, Int, @thin S4.Type) -> S4
    _ = S4()
  }
}

// rdar://84279742 – Make sure we prefer the unlabeled init here.
struct S5 {
  init(x: Int...) {}
  init(_ x: Int...) {}

  // CHECK-LABEL: sil hidden [ossa] @$s7ranking2S5V15testInitRankingyyF
  func testInitRanking() {
    // CHECK: function_ref @$s7ranking2S5VyACSid_tcfC : $@convention(method) (@owned Array<Int>, @thin S5.Type) -> S5
    _ = S5()
  }
}

// We should also prefer the unlabeled case here.
struct S6 {
  init(_: Int = 0, x: Int...) {}
  init(_: Int = 0, _: Int...) {}

  // CHECK-LABEL: sil hidden [ossa] @$s7ranking2S6V15testInitRankingyyF
  func testInitRanking() {
    // CHECK: function_ref @$s7ranking2S6VyACSi_SidtcfC : $@convention(method) (Int, @owned Array<Int>, @thin S6.Type) -> S6
    _ = S6()
  }
}

// However subtyping rules take precedence over labeling rules, so we should
// prefer the labeled init here.
struct S7 {
  init(x: Int...) {}
  init(_: Int?...) {}

  // CHECK-LABEL: sil hidden [ossa] @$s7ranking2S7V15testInitRankingyyF
  func testInitRanking() {
    // CHECK: function_ref @$s7ranking2S7V1xACSid_tcfC : $@convention(method) (@owned Array<Int>, @thin S7.Type) -> S7
    _ = S7()
  }
}

// Subtyping rules also let us prefer the Int... init here.
struct S8 {
  init(_: Int?...) {}
  init(_: Int...) {}

  // CHECK-LABEL: sil hidden [ossa] @$s7ranking2S8V15testInitRankingyyF
  func testInitRanking() {
    // CHECK: function_ref @$s7ranking2S8VyACSid_tcfC : $@convention(method) (@owned Array<Int>, @thin S8.Type) -> S8
    _ = S8()
  }
}

//--------------------------------------------------------------------
// Pointer conversions
//--------------------------------------------------------------------

struct UnsafePointerStruct {
  // CHECK-LABEL: sil hidden [ossa] @$s7ranking19UnsafePointerStructVyACSPyxGSgclufC : $@convention(method) <U> (Optional<UnsafePointer<U>>, @thin UnsafePointerStruct.Type) -> UnsafePointerStruct
  init<U>(_ from: UnsafePointer<U>) {}
  init<U>(_ from: UnsafePointer<U>?) {
    // CHECK: function_ref @$s7ranking19UnsafePointerStructVyACSPyxGclufC : $@convention(method) <τ_0_0> (UnsafePointer<τ_0_0>, @thin UnsafePointerStruct.Type) -> UnsafePointerStruct
    self.init(from!)
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s7ranking22useUnsafePointerStructyySPyxGlF : $@convention(thin) <U> (UnsafePointer<U>) -> ()
func useUnsafePointerStruct<U>(_ ptr: UnsafePointer<U>) {
  // CHECK: function_ref @$s7ranking19UnsafePointerStructVyACSPyxGclufC : $@convention(method) <τ_0_0> (UnsafePointer<τ_0_0>, @thin UnsafePointerStruct.Type) -> UnsafePointerStruct
  let _: UnsafePointerStruct = UnsafePointerStruct(ptr)
}

/// Archetype vs. non-archetype (expect placeholder)

protocol SignalProtocol {
  associatedtype Element
  associatedtype Error: Swift.Error
}

struct Signal<Element, Error: Swift.Error>: SignalProtocol {
  init<S: Sequence>(sequence: S) where S.Iterator.Element == Element {
  }
}

extension SignalProtocol where Element: SignalProtocol, Element.Error == Error {
  typealias InnerElement = Element.Element

  func flatten() -> Signal<InnerElement, Error> {
    fatalError()
  }
}

extension SignalProtocol where Element: SignalProtocol, Error == Never {
  func flatten() -> Signal<Element.Element, Element.Error> {
    fatalError()
  }
}

func no_ambiguity_error_vs_never<Element, Error>(_ signals: [Signal<Element, Error>]) -> Signal<Element, Error> {
  return Signal(sequence: signals).flatten() // Ok
}

// Test to make sure that previous favoring behavior is maintained and @autoclosure makes a difference.
func test_no_ambiguity_with_autoclosure(x: Int) {
  func test(_ condition: Bool, file: StaticString = #file, line: UInt = #line) {}
  func test(_ condition: @autoclosure () -> Bool, file: StaticString = #file, line: UInt = #line) {}

  test(x >= 0) // Ok
}
