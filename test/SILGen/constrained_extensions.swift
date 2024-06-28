
// RUN: %target-swift-emit-silgen -module-name constrained_extensions -primary-file %s | %FileCheck %s
// RUN: %target-swift-emit-sil -module-name constrained_extensions -O -primary-file %s > /dev/null
// RUN: %target-swift-emit-ir -module-name constrained_extensions -primary-file %s > /dev/null

extension Array where Element == Int {
  // CHECK-LABEL: sil [ossa] @$sSa22constrained_extensionsSiRszlE1xSaySiGyt_tcfC : $@convention(method) (@thin Array<Int>.Type) -> @owned Array<Int>
  public init(x: ()) {
    self.init()
  }

  // CHECK-LABEL: sil [ossa] @$sSa22constrained_extensionsSiRszlE16instancePropertySivg : $@convention(method) (@guaranteed Array<Int>) -> Int
  // CHECK-LABEL: sil [ossa] @$sSa22constrained_extensionsSiRszlE16instancePropertySivs : $@convention(method) (Int, @inout Array<Int>) -> ()
  // CHECK-LABEL: sil [transparent] [serialized] [ossa] @$sSa22constrained_extensionsSiRszlE16instancePropertySivM : $@yield_once @convention(method) (@inout Array<Int>) -> @yields @inout Int

  public var instanceProperty: Element {
    get {
      return self[0]
    }
    set {
      self[0] = newValue
    }
  }

  // CHECK-LABEL: sil [ossa] @$sSa22constrained_extensionsSiRszlE14instanceMethodSiyF : $@convention(method) (@guaranteed Array<Int>) -> Int
  public func instanceMethod() -> Element {
    return instanceProperty
  }

  // CHECK-LABEL: sil [ossa] @$sSa22constrained_extensionsSiRszlE14instanceMethod1eS2i_tF : $@convention(method) (Int, @guaranteed Array<Int>) -> Int
  public func instanceMethod(e: Element) -> Element {
    return e
  }

  // CHECK-LABEL: sil [ossa] @$sSa22constrained_extensionsSiRszlE14staticPropertySivgZ : $@convention(method) (@thin Array<Int>.Type) -> Int
  public static var staticProperty: Element {
    return Array(x: ()).instanceProperty
  }

  // CHECK-LABEL: sil [ossa] @$sSa22constrained_extensionsSiRszlE12staticMethodSiyFZ : $@convention(method) (@thin Array<Int>.Type) -> Int
  public static func staticMethod() -> Element {
    return staticProperty
  }

  // CHECK-LABEL: sil non_abi [serialized] [ossa] @$sSa22constrained_extensionsSiRszlE12staticMethod1eS2iSg_tFZfA_ : $@convention(thin) () -> Optional<Int>
  // CHECK-LABEL: sil [ossa] @$sSa22constrained_extensionsSiRszlE12staticMethod1eS2iSg_tFZ : $@convention(method) (Optional<Int>, @thin Array<Int>.Type) -> Int
  public static func staticMethod(e: Element? = (nil)) -> Element {
    return e!
  }

  // CHECK-LABEL: sil [ossa] @$sSa22constrained_extensionsSiRszlEySiyt_tcig : $@convention(method) (@guaranteed Array<Int>) -> Int
  public subscript(i: ()) -> Element {
    return self[0]
  }

  // CHECK-LABEL: sil [ossa] @$sSa22constrained_extensionsSiRszlE21inoutAccessOfPropertyyyF : $@convention(method) (@inout Array<Int>) -> ()
  public mutating func inoutAccessOfProperty() {
    func increment(x: inout Element) {
      x += 1
    }

    increment(x: &instanceProperty)
  }
}

extension Dictionary where Key == Int {
  // CHECK-LABEL: sil [ossa] @$sSD22constrained_extensionsSiRszrlE1xSDySiq_Gyt_tcfC : $@convention(method) <Key, Value where Key == Int> (@thin Dictionary<Int, Value>.Type) -> @owned Dictionary<Int, Value> {
  public init(x: ()) {
    self.init()
  }

  // CHECK-LABEL: sil [ossa] @$sSD22constrained_extensionsSiRszrlE16instancePropertyq_vg : $@convention(method) <Key, Value where Key == Int> (@guaranteed Dictionary<Int, Value>) -> @out Value
  // CHECK-LABEL: sil [ossa] @$sSD22constrained_extensionsSiRszrlE16instancePropertyq_vs : $@convention(method) <Key, Value where Key == Int> (@in Value, @inout Dictionary<Int, Value>) -> ()
  // CHECK-LABEL: sil [transparent] [serialized] [ossa] @$sSD22constrained_extensionsSiRszrlE16instancePropertyq_vM : $@yield_once @convention(method) <Key, Value where Key == Int> (@inout Dictionary<Int, Value>) -> @yields @inout Value
  public var instanceProperty: Value {
    get {
      return self[0]!
    }
    set {
      self[0] = newValue
    }
  }

  // CHECK-LABEL: sil [ossa] @$sSD22constrained_extensionsSiRszrlE14instanceMethodq_yF : $@convention(method) <Key, Value where Key == Int> (@guaranteed Dictionary<Int, Value>) -> @out Value
  public func instanceMethod() -> Value {
    return instanceProperty
  }

  // CHECK-LABEL: sil [ossa] @$sSD22constrained_extensionsSiRszrlE14instanceMethod1vq_q__tF : $@convention(method) <Key, Value where Key == Int> (@in_guaranteed Value, @guaranteed Dictionary<Int, Value>) -> @out Value
  public func instanceMethod(v: Value) -> Value {
    return v
  }

  // CHECK-LABEL: sil [ossa] @$sSD22constrained_extensionsSiRszrlE12staticMethodSiyFZ : $@convention(method) <Key, Value where Key == Int> (@thin Dictionary<Int, Value>.Type) -> Int
  public static func staticMethod() -> Key {
    return staticProperty
  }

  // CHECK-LABEL: sil [ossa] @$sSD22constrained_extensionsSiRszrlE14staticPropertySivgZ : $@convention(method) <Key, Value where Key == Int> (@thin Dictionary<Int, Value>.Type) -> Int
  public static var staticProperty: Key {
    return 0
  }

  // CHECK-LABEL: sil non_abi [serialized] [ossa] @$sSD22constrained_extensionsSiRszrlE12staticMethod1k1vq_SiSg_q_SgtFZfA_ : $@convention(thin) <Key, Value where Key == Int> () -> Optional<Int>
  // CHECK-LABEL: sil non_abi [serialized] [ossa] @$sSD22constrained_extensionsSiRszrlE12staticMethod1k1vq_SiSg_q_SgtFZfA0_ : $@convention(thin) <Key, Value where Key == Int> () -> @out Optional<Value>
  // CHECK-LABEL: sil [ossa] @$sSD22constrained_extensionsSiRszrlE12staticMethod1k1vq_SiSg_q_SgtFZ : $@convention(method) <Key, Value where Key == Int> (Optional<Int>, @in_guaranteed Optional<Value>, @thin Dictionary<Int, Value>.Type) -> @out Value
  public static func staticMethod(k: Key? = (nil), v: Value? = (nil)) -> Value {
    return v!
  }

  // CHECK-LABEL: sil [ossa] @$sSD22constrained_extensionsSiRszrlE17callsStaticMethodq_yFZ : $@convention(method) <Key, Value where Key == Int> (@thin Dictionary<Int, Value>.Type) -> @out Value
  public static func callsStaticMethod() -> Value {
    return staticMethod()
  }

  // CHECK-LABEL: sil [ossa] @$sSD22constrained_extensionsSiRszrlE16callsConstructorq_yFZ : $@convention(method) <Key, Value where Key == Int> (@thin Dictionary<Int, Value>.Type) -> @out Value
  public static func callsConstructor() -> Value {
    return Dictionary(x: ()).instanceMethod()
  }

  // CHECK-LABEL: sil [ossa] @$sSD22constrained_extensionsSiRszrlEyq_yt_tcig : $@convention(method) <Key, Value where Key == Int> (@guaranteed Dictionary<Int, Value>) -> @out Value
  public subscript(i: ()) -> Value {
    return self[0]!
  }

  // CHECK-LABEL: sil [ossa] @$sSD22constrained_extensionsSiRszrlE21inoutAccessOfPropertyyyF : $@convention(method) <Key, Value where Key == Int> (@inout Dictionary<Int, Value>) -> ()
  public mutating func inoutAccessOfProperty() {
    func increment(x: inout Value) { }

    increment(x: &instanceProperty)
  }
}

public class GenericClass<X, Y> {}

extension GenericClass where Y == () {
  // CHECK-LABEL: sil [ossa] @$s22constrained_extensions12GenericClassCAAytRs_rlE5valuexvg : $@convention(method) <X, Y where Y == ()> (@guaranteed GenericClass<X, ()>) -> @out X
  // CHECK-LABEL: sil [ossa] @$s22constrained_extensions12GenericClassCAAytRs_rlE5valuexvs : $@convention(method) <X, Y where Y == ()> (@in X, @guaranteed GenericClass<X, ()>) -> ()
  // CHECK-LABEL: sil [transparent] [serialized] [ossa] @$s22constrained_extensions12GenericClassCAAytRs_rlE5valuexvM : $@yield_once @convention(method) <X, Y where Y == ()> (@guaranteed GenericClass<X, ()>) -> @yields @inout X
  public var value: X {
    get { while true {} }
    set {}
  }

  // CHECK-LABEL: sil [ossa] @$s22constrained_extensions12GenericClassCAAytRs_rlE5emptyytvg : $@convention(method) <X, Y where Y == ()> (@guaranteed GenericClass<X, ()>) -> ()
  // CHECK-LABEL: sil [ossa] @$s22constrained_extensions12GenericClassCAAytRs_rlE5emptyytvs : $@convention(method) <X, Y where Y == ()> (@guaranteed GenericClass<X, ()>) -> ()
  // CHECK-LABEL: sil [transparent] [serialized] [ossa] @$s22constrained_extensions12GenericClassCAAytRs_rlE5emptyytvM : $@yield_once @convention(method) <X, Y where Y == ()> (@guaranteed GenericClass<X, ()>) ->  @yields @inout ()
  public var empty: Y {
    get { return () }
    set {}
  }

  // CHECK-LABEL: sil [ossa] @$s22constrained_extensions12GenericClassCAAytRs_rlEyxyt_tcig : $@convention(method) <X, Y where Y == ()> (@guaranteed GenericClass<X, ()>) -> @out X
  // CHECK-LABEL: sil [ossa] @$s22constrained_extensions12GenericClassCAAytRs_rlEyxyt_tcis : $@convention(method) <X, Y where Y == ()> (@in X, @guaranteed GenericClass<X, ()>) -> ()
  // CHECK-LABEL: sil [transparent] [serialized] [ossa] @$s22constrained_extensions12GenericClassCAAytRs_rlEyxyt_tciM : $@yield_once @convention(method) <X, Y where Y == ()> (@guaranteed GenericClass<X, ()>) ->  @yields @inout X
  public subscript(_: Y) -> X {
    get { while true {} }
    set {}
  }

  // CHECK-LABEL: sil [ossa] @$s22constrained_extensions12GenericClassCAAytRs_rlEyyxcig : $@convention(method) <X, Y where Y == ()> (@in_guaranteed X, @guaranteed GenericClass<X, ()>) -> ()
  // CHECK-LABEL: sil [ossa] @$s22constrained_extensions12GenericClassCAAytRs_rlEyyxcis : $@convention(method) <X, Y where Y == ()> (@in X, @guaranteed GenericClass<X, ()>) -> ()
  // CHECK-LABEL: sil [transparent] [serialized] [ossa] @$s22constrained_extensions12GenericClassCAAytRs_rlEyyxciM : $@yield_once @convention(method) <X, Y where Y == ()> (@in_guaranteed X, @guaranteed GenericClass<X, ()>) ->  @yields @inout ()
  public subscript(_: X) -> Y {
    get { while true {} }
    set {}
  }
}

protocol VeryConstrained {}

struct AnythingGoes<T> {
  // CHECK-LABEL: sil hidden [transparent] [ossa] @$s22constrained_extensions12AnythingGoesV13meaningOfLifexSgvpfi : $@convention(thin) <T> () -> @out Optional<T>
  var meaningOfLife: T? = nil
}

extension AnythingGoes where T : VeryConstrained {
  // CHECK-LABEL: sil hidden [ossa] @$s22constrained_extensions12AnythingGoesVA2A15VeryConstrainedRzlE13fromExtensionACyxGyt_tcfC : $@convention(method) <T where T : VeryConstrained> (@thin AnythingGoes<T>.Type) -> @out AnythingGoes<T> {

  // CHECK: [[RESULT:%.*]] = struct_element_addr {{%.*}} : $*AnythingGoes<T>, #AnythingGoes.meaningOfLife
  // CHECK: [[INIT:%.*]] = function_ref @$s22constrained_extensions12AnythingGoesV13meaningOfLifexSgvpfi : $@convention(thin) <τ_0_0> () -> @out Optional<τ_0_0>
  // CHECK: apply [[INIT]]<T>([[RESULT]]) : $@convention(thin) <τ_0_0> () -> @out Optional<τ_0_0>
  // CHECK: return
  init(fromExtension: ()) {}
}

extension Array where Element == Int {
  struct Nested {
    // CHECK-LABEL: sil hidden [transparent] [ossa] @$sSa22constrained_extensionsSiRszlE6NestedV1eSiSgvpfi : $@convention(thin) () -> Optional<Int>
    var e: Element? = nil

    // CHECK-LABEL: sil hidden [ossa] @$sSa22constrained_extensionsSiRszlE6NestedV10hasDefault1eySiSg_tFfA_ : $@convention(thin) () -> Optional<Int>
    // CHECK-LABEL: sil hidden [ossa] @$sSa22constrained_extensionsSiRszlE6NestedV10hasDefault1eySiSg_tF : $@convention(method) (Optional<Int>, @inout Array<Int>.Nested) -> ()
    mutating func hasDefault(e: Element? = (nil)) {
      self.e = e
    }
  }
}

extension Array where Element == AnyObject {
  class NestedClass {
    // CHECK-LABEL: sil hidden [ossa] @$sSa22constrained_extensionsyXlRszlE11NestedClassCfd : $@convention(method) (@guaranteed Array<AnyObject>.NestedClass) -> @owned Builtin.NativeObject
    // CHECK-LABEL: sil hidden [ossa] @$sSa22constrained_extensionsyXlRszlE11NestedClassCfD : $@convention(method) (@owned Array<AnyObject>.NestedClass) -> ()
    deinit { }

    // CHECK-LABEL: sil hidden [exact_self_class] [ossa] @$sSa22constrained_extensionsyXlRszlE11NestedClassCACyyXl_GycfC : $@convention(method) (@thick Array<AnyObject>.NestedClass.Type) -> @owned Array<AnyObject>.NestedClass
    // CHECK-LABEL: sil hidden [ossa] @$sSa22constrained_extensionsyXlRszlE11NestedClassCACyyXl_Gycfc : $@convention(method) (@owned Array<AnyObject>.NestedClass) -> @owned Array<AnyObject>.NestedClass
  }

  class DerivedClass : NestedClass {
    // CHECK-LABEL: sil hidden [transparent] [ossa] @$sSa22constrained_extensionsyXlRszlE12DerivedClassC1eyXlSgvpfi : $@convention(thin) () -> @owned Optional<AnyObject>
    // CHECK-LABEL: sil hidden [ossa] @$sSa22constrained_extensionsyXlRszlE12DerivedClassCfE : $@convention(method) (@guaranteed Array<AnyObject>.DerivedClass) -> ()
    var e: Element? = nil
  }

  enum NestedEnum {
    case hay
    case grain

    func makeHay() -> NestedEnum {
      return .hay
    }
  }
}

func referenceNestedTypes() {
  _ = Array<AnyObject>.NestedClass()
  _ = Array<AnyObject>.DerivedClass()
}

struct S<T> {
  struct X {}
}

// CHECK-LABEL: sil hidden [ossa] @$s22constrained_extensions1SVAASiRszlEyAC1XVySi_GSicir : $@yield_once @convention(method) (Int, S<Int>) -> @yields S<Int>.X
extension S<Int> {
  subscript(index:Int) -> X {
    _read {
      fatalError()
    }
  }
}

// https://github.com/swiftlang/swift/issues/74648

protocol HasX {
  var x: Int { get set }
}

extension S: HasX where T == Int {
  var x: Int {
    get { 3 }
    set { }
  }
}

// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s22constrained_extensions1SVyxGAA4HasXAASiRszlAaEP1xSivMTW : $@yield_once @convention(witness_method: HasX) @substituted <τ_0_0> (@inout τ_0_0) -> @yields @inout Int for <S<Int>> {
// CHECK: [[WITNESS_FN:%.*]] = function_ref @$s22constrained_extensions1SVAASiRszlE1xSivM : $@yield_once @convention(method) (@inout S<Int>) -> @yields @inout Int
// CHECK: begin_apply [[WITNESS_FN]](%0) : $@yield_once @convention(method) (@inout S<Int>) -> @yields @inout Int
// CHECK: yield
// CHECK: unwind
