// RUN: %empty-directory(%t)
// RUN: %target-build-swift -import-objc-header %S/Inputs/tail_allocated_c_array.h -swift-version 5 -g %s -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out
// REQUIRES: executable_test
// UNSUPPORTED: freestanding

@_spi(ObservableRerootKeyPath)
import Swift

import StdlibUnittest

var keyPath = TestSuite("key paths")

final class C<T> {
  var x: Int
  var y: LifetimeTracked?
  var z: T
  let immutable: String
  private(set) var secretlyMutable: String

  var computed: T {
    get {
      return z
    }
    set {
      z = newValue
    }
  }

  init(x: Int, y: LifetimeTracked?, z: T) {
    self.x = x
    self.y = y
    self.z = z
    self.immutable = "\(x) \(y) \(z)"
    self.secretlyMutable = immutable
  }
}

struct Point: Equatable {
  var x: Double
  var y: Double
  var trackLifetime = LifetimeTracked(123)
  let hypotenuse: Double
  private(set) var secretlyMutableHypotenuse: Double
  
  init(x: Double, y: Double) {
    self.x = x
    self.y = y
    hypotenuse = x*x + y*y
    secretlyMutableHypotenuse = x*x + y*y
  }
  
  static func ==(a: Point, b: Point) -> Bool {
    return a.x == b.x && a.y == b.y
  }
}

struct S<T: Equatable>: Equatable {
  var x: Int
  var y: LifetimeTracked?
  var z: T
  var p: Point
  var c: C<T>
  
  static func ==(a: S, b: S) -> Bool {
    return a.x == b.x
      && a.y === b.y
      && a.z == b.z
      && a.p == b.p
      && a.c === b.c
  }
}

final class ComputedA {
  var readOnly: ComputedB { fatalError() }
  var nonmutating: ComputedB {
    get { fatalError() }
    set { fatalError() }
  }
  var reabstracted: () -> () = {}
}

struct ComputedB {
  var readOnly: ComputedA { fatalError() }
  var mutating: ComputedA { 
    get { fatalError() }
    set { fatalError() }
  }
  var nonmutating: ComputedA {
    get { fatalError() }
    nonmutating set { fatalError() }
  }
  var reabstracted: () -> () = {}
}

typealias Tuple<T: Equatable, U> = (S<T>, C<U>)

keyPath.test("key path in-place instantiation") {
  for _ in 1...2 {
    let s_x = (\S<Int>.x as AnyKeyPath) as! WritableKeyPath<S<Int>, Int>
    let s_y = (\S<Int>.y as AnyKeyPath) as! WritableKeyPath<S<Int>, LifetimeTracked?>
    let s_z = (\S<Int>.z as AnyKeyPath) as! WritableKeyPath<S<Int>, Int>
    let s_p = (\S<Int>.p as AnyKeyPath) as! WritableKeyPath<S<Int>, Point>
    let s_p_x = (\S<Int>.p.x as AnyKeyPath) as! WritableKeyPath<S<Int>, Double>
    let s_p_y = (\S<Int>.p.y as AnyKeyPath) as! WritableKeyPath<S<Int>, Double>
    let s_c = (\S<Int>.c as AnyKeyPath) as! WritableKeyPath<S<Int>, C<Int>>
    let s_c_x = (\S<Int>.c.x as AnyKeyPath) as! ReferenceWritableKeyPath<S<Int>, Int>

    let t_0s = (\Tuple<Int, Int>.0 as AnyKeyPath) as! WritableKeyPath<Tuple<Int, Int>, S<Int>>
    let t_1c = (\Tuple<Int, Int>.1 as AnyKeyPath) as! WritableKeyPath<Tuple<Int, Int>, C<Int>>
    let t_0s_x = (\Tuple<Int, Int>.0.x as AnyKeyPath) as! WritableKeyPath<Tuple<Int, Int>, Int>
    let t_0s_p_hypotenuse = (\Tuple<Int, Int>.0.p.hypotenuse as AnyKeyPath) as! KeyPath<Tuple<Int, Int>, Double>
    let t_1c_x = (\Tuple<Int, Int>.1.x as AnyKeyPath) as! ReferenceWritableKeyPath<Tuple<Int, Int>, Int>
    let t_1c_immutable = (\Tuple<Int, Int>.1.immutable as AnyKeyPath) as! KeyPath<Tuple<Int, Int>, String>

    let c_x = (\C<Int>.x as AnyKeyPath) as! ReferenceWritableKeyPath<C<Int>, Int>
    let s_c_x_2 = s_c.appending(path: c_x)

    expectEqual(s_c_x, s_c_x_2)
    expectEqual(s_c_x_2, s_c_x)
    expectEqual(s_c_x.hashValue, s_c_x_2.hashValue)

    let t_1c_x_2 = t_1c.appending(path: c_x)

    expectEqual(t_1c_x, t_1c_x_2)
    expectEqual(t_1c_x_2, t_1c_x)
    expectEqual(t_1c_x.hashValue, t_1c_x_2.hashValue)

    let point_x = (\Point.x as AnyKeyPath) as! WritableKeyPath<Point, Double>
    let point_y = (\Point.y as AnyKeyPath) as! WritableKeyPath<Point, Double>

    let s_p_x_2 = s_p.appending(path: point_x)
    let s_p_y_2 = s_p.appending(path: point_y)

    expectEqual(s_p_x, s_p_x_2)
    expectEqual(s_p_x_2, s_p_x)
    expectEqual(s_p_x_2.hashValue, s_p_x.hashValue)
    expectEqual(s_p_y, s_p_y_2)
    expectEqual(s_p_y_2, s_p_y)
    expectEqual(s_p_y_2.hashValue, s_p_y.hashValue)

    let ca_readOnly = (\ComputedA.readOnly as AnyKeyPath) as! KeyPath<ComputedA, ComputedB>
    let ca_nonmutating = (\ComputedA.nonmutating as AnyKeyPath) as! ReferenceWritableKeyPath<ComputedA, ComputedB>
    let ca_reabstracted = (\ComputedA.reabstracted as AnyKeyPath) as! ReferenceWritableKeyPath<ComputedA, () -> ()>

    let cb_readOnly = (\ComputedB.readOnly as AnyKeyPath) as! KeyPath<ComputedB, ComputedA>
    let cb_mutating = (\ComputedB.mutating as AnyKeyPath) as! WritableKeyPath<ComputedB, ComputedA>
    let cb_nonmutating = (\ComputedB.nonmutating as AnyKeyPath) as! ReferenceWritableKeyPath<ComputedB, ComputedA>
    let cb_reabstracted = (\ComputedB.reabstracted as AnyKeyPath) as! WritableKeyPath<ComputedB, () -> ()>
  
    let ca_readOnly_mutating = (\ComputedA.readOnly.mutating as AnyKeyPath) as! KeyPath<ComputedA, ComputedA>
    let cb_mutating_readOnly = (\ComputedB.mutating.readOnly as AnyKeyPath) as! KeyPath<ComputedB, ComputedB>
    let ca_readOnly_nonmutating = (\ComputedA.readOnly.nonmutating as AnyKeyPath) as! ReferenceWritableKeyPath<ComputedA, ComputedA>
    let cb_readOnly_reabstracted = (\ComputedB.readOnly.reabstracted as AnyKeyPath) as! ReferenceWritableKeyPath<ComputedB, () -> ()>

    let ca_readOnly_mutating2 = ca_readOnly.appending(path: cb_mutating)
    expectEqual(ca_readOnly_mutating, ca_readOnly_mutating2)
    expectEqual(ca_readOnly_mutating2, ca_readOnly_mutating)
    expectEqual(ca_readOnly_mutating.hashValue, ca_readOnly_mutating2.hashValue)

    let cb_mutating_readOnly2 = cb_mutating.appending(path: ca_readOnly)
    expectEqual(cb_mutating_readOnly, cb_mutating_readOnly2)
    expectEqual(cb_mutating_readOnly2, cb_mutating_readOnly)
    expectEqual(cb_mutating_readOnly.hashValue, cb_mutating_readOnly2.hashValue)

    let ca_readOnly_nonmutating2 = ca_readOnly.appending(path: cb_nonmutating)
    expectEqual(ca_readOnly_nonmutating, ca_readOnly_nonmutating2)
    expectEqual(ca_readOnly_nonmutating2, ca_readOnly_nonmutating)
    expectEqual(ca_readOnly_nonmutating.hashValue,
                ca_readOnly_nonmutating2.hashValue)

    let cb_readOnly_reabstracted2 = cb_readOnly.appending(path: ca_reabstracted)
    expectEqual(cb_readOnly_reabstracted,
                cb_readOnly_reabstracted2)
    expectEqual(cb_readOnly_reabstracted2,
                cb_readOnly_reabstracted)
    expectEqual(cb_readOnly_reabstracted2.hashValue,
                cb_readOnly_reabstracted.hashValue)
  }
}

keyPath.test("key path generic instantiation") {
  func testWithGenericParam<T: Equatable>(_: T.Type) -> ReferenceWritableKeyPath<S<T>, Int> {
    for i in 1...2 {
      let s_x = (\S<T>.x as AnyKeyPath) as! WritableKeyPath<S<T>, Int>
      let s_y = (\S<T>.y as AnyKeyPath) as! WritableKeyPath<S<T>, LifetimeTracked?>
      let s_z = (\S<T>.z as AnyKeyPath) as! WritableKeyPath<S<T>, T>
      let s_p = (\S<T>.p as AnyKeyPath) as! WritableKeyPath<S<T>, Point>
      let s_p_x = (\S<T>.p.x as AnyKeyPath) as! WritableKeyPath<S<T>, Double>
      let s_p_y = (\S<T>.p.y as AnyKeyPath) as! WritableKeyPath<S<T>, Double>
      let s_c = (\S<T>.c as AnyKeyPath) as! WritableKeyPath<S<T>, C<T>>
      let s_c_x = (\S<T>.c.x as AnyKeyPath) as! ReferenceWritableKeyPath<S<T>, Int>

      let t_0s = (\Tuple<T, T>.0 as AnyKeyPath) as! WritableKeyPath<Tuple<T, T>, S<T>>
      let t_1c = (\Tuple<T, T>.1 as AnyKeyPath) as! WritableKeyPath<Tuple<T, T>, C<T>>
      let t_0s_x = (\Tuple<T, T>.0.x as AnyKeyPath) as! WritableKeyPath<Tuple<T, T>, Int>
      let t_0s_p_hypotenuse = (\Tuple<T, T>.0.p.hypotenuse as AnyKeyPath) as! KeyPath<Tuple<T, T>, Double>
      let t_1c_x = (\Tuple<T, T>.1.x as AnyKeyPath) as! ReferenceWritableKeyPath<Tuple<T, T>, Int>
      let t_1c_immutable = (\Tuple<T, T>.1.immutable as AnyKeyPath) as! KeyPath<Tuple<T, T>, String>

      let c_x = (\C<T>.x as AnyKeyPath) as! ReferenceWritableKeyPath<C<T>, Int>
      let s_c_x_2 = s_c.appending(path: c_x)

      expectEqual(s_c_x, s_c_x_2)
      expectEqual(s_c_x_2, s_c_x)
      expectEqual(s_c_x.hashValue, s_c_x_2.hashValue)

      let t_1c_x_2 = t_1c.appending(path: c_x)

      expectEqual(t_1c_x, t_1c_x_2)
      expectEqual(t_1c_x_2, t_1c_x)
      expectEqual(t_1c_x.hashValue, t_1c_x_2.hashValue)

      let point_x = (\Point.x as AnyKeyPath) as! WritableKeyPath<Point, Double>
      let point_y = (\Point.y as AnyKeyPath) as! WritableKeyPath<Point, Double>

      let s_p_x_2 = s_p.appending(path: point_x)
      let s_p_y_2 = s_p.appending(path: point_y)

      expectEqual(s_p_x, s_p_x_2)
      expectEqual(s_p_x_2, s_p_x)
      expectEqual(s_p_x_2.hashValue, s_p_x.hashValue)
      expectEqual(s_p_y, s_p_y_2)
      expectEqual(s_p_y_2, s_p_y)
      expectEqual(s_p_y_2.hashValue, s_p_y.hashValue)

      if i == 2 { return s_c_x }
    }
    fatalError()
  }
  let s_c_x_int = testWithGenericParam(Int.self)
  let s_c_x_int2 = \S<Int>.c.x
  expectEqual(s_c_x_int, s_c_x_int2)

  let s_c_x_string = testWithGenericParam(String.self)
  let s_c_x_string2 = \S<String>.c.x
  expectEqual(s_c_x_string, s_c_x_string2)

  let s_c_x_lt = testWithGenericParam(LifetimeTracked.self)
  let s_c_x_lt2 = \S<LifetimeTracked>.c.x
  expectEqual(s_c_x_lt, s_c_x_lt2)
}

protocol P {}

struct TestComputed: P {
  static var numNonmutatingSets = 0
  static var numMutatingSets = 0

  static func resetCounts() {
    numNonmutatingSets = 0
    numMutatingSets = 0
  }

  var canary = LifetimeTracked(0)

  var readonly: LifetimeTracked {
    return LifetimeTracked(1)
  }
  var nonmutating: LifetimeTracked {
    get {
      return LifetimeTracked(2)
    }
    nonmutating set { TestComputed.numNonmutatingSets += 1 }
  }
  var mutating: LifetimeTracked {
    get {
      return LifetimeTracked(3)
    }
    set {
      canary = newValue
    }
  }
}

extension P {
  var readonlyProtoExt: Self { return self }
  var mutatingProtoExt: Self {
    get { return self }
    set { self = newValue }
  }
}

keyPath.test("computed properties") {
  var test = TestComputed()

  do {
    let tc_readonly = \TestComputed.readonly
    expectTrue(test[keyPath: tc_readonly] !== test[keyPath: tc_readonly])
    expectEqual(test[keyPath: tc_readonly].value,
                test[keyPath: tc_readonly].value)
  }

  do {
    let tc_nonmutating = \TestComputed.nonmutating
    expectTrue(test[keyPath: tc_nonmutating] !== test[keyPath: tc_nonmutating])
    expectEqual(test[keyPath: tc_nonmutating].value,
                test[keyPath: tc_nonmutating].value)
    TestComputed.resetCounts()
    test[keyPath: tc_nonmutating] = LifetimeTracked(4)
    expectEqual(TestComputed.numNonmutatingSets, 1)
  }

  do {
    let tc_mutating = \TestComputed.mutating
    expectTrue(test[keyPath: tc_mutating] !== test[keyPath: tc_mutating])
    expectEqual(test[keyPath: tc_mutating].value,
                test[keyPath: tc_mutating].value)
    let newObject = LifetimeTracked(5)
    test[keyPath: tc_mutating] = newObject
    expectTrue(test.canary === newObject)
  }

  do {
    let tc_readonlyProtoExt = \TestComputed.readonlyProtoExt
    expectTrue(test.canary === test[keyPath: tc_readonlyProtoExt].canary)
  }

  do {
    let tc_mutatingProtoExt = \TestComputed.mutatingProtoExt
    expectTrue(test.canary === test[keyPath: tc_mutatingProtoExt].canary)
    let oldTest = test
    test[keyPath: tc_mutatingProtoExt] = TestComputed()
    expectTrue(oldTest.canary !== test.canary)
    expectTrue(test.canary === test[keyPath: tc_mutatingProtoExt].canary)
  }
}

class AB {
}
class ABC: AB, ABCProtocol {
  var a = LifetimeTracked(1)
  var b = LifetimeTracked(2)
  var c = LifetimeTracked(3)
  subscript(x: Int) -> Int {
    get { return x + 27 }
    set { }
  }
}

protocol ABCProtocol {
  var a: LifetimeTracked { get }
  var b: LifetimeTracked { get set }
  var c: LifetimeTracked { get nonmutating set }
  subscript(x: Int) -> Int { get set }
}

keyPath.test("dynamically-typed application") {
  let cPaths = [\ABC.a, \ABC.b, \ABC.c]

  let subject = ABC()

  do {
    let fields = cPaths.map { subject[keyPath: $0] }
    expectTrue(fields[0] as! AnyObject === subject.a)
    expectTrue(fields[1] as! AnyObject === subject.b)
    expectTrue(fields[2] as! AnyObject === subject.c)
  }

  let erasedSubject: AB = subject
  let erasedPaths: [AnyKeyPath] = cPaths
  let wrongSubject = AB()

  do {
    let fields = erasedPaths.map { erasedSubject[keyPath: $0] }
    expectTrue(fields[0]! as! AnyObject === subject.a)
    expectTrue(fields[1]! as! AnyObject === subject.b)
    expectTrue(fields[2]! as! AnyObject === subject.c)

    let wrongFields = erasedPaths.map { wrongSubject[keyPath: $0] }
    expectTrue(wrongFields[0] == nil)
    expectTrue(wrongFields[1] == nil)
    expectTrue(wrongFields[2] == nil)
  }

  var protoErasedSubject: ABCProtocol = subject
  let protoErasedPathA = \ABCProtocol.a
  let protoErasedPathB = \ABCProtocol.b
  let protoErasedPathC = \ABCProtocol.c
  let protoErasedSubscript = \ABCProtocol[100]

  do {
    expectTrue(protoErasedSubject.a ===
                  protoErasedSubject[keyPath: protoErasedPathA])

    let newB = LifetimeTracked(4)
    expectTrue(protoErasedSubject.b ===
                  protoErasedSubject[keyPath: protoErasedPathB])
    protoErasedSubject[keyPath: protoErasedPathB] = newB
    expectTrue(protoErasedSubject.b ===
                  protoErasedSubject[keyPath: protoErasedPathB])
    expectTrue(protoErasedSubject.b === newB)

    let newC = LifetimeTracked(5)
    expectTrue(protoErasedSubject.c ===
                  protoErasedSubject[keyPath: protoErasedPathC])
    protoErasedSubject[keyPath: protoErasedPathC] = newC
    expectTrue(protoErasedSubject.c ===
                  protoErasedSubject[keyPath: protoErasedPathC])
    expectTrue(protoErasedSubject.c === newC)

    expectTrue(protoErasedSubject[keyPath: protoErasedSubscript] == 127)
  }
}

struct TestOptional {
  var origin: Point?
  var questionableCanary: LifetimeTracked? = LifetimeTracked(123)

  init(origin: Point?) {
    self.origin = origin
  }
}

keyPath.test("optional force-unwrapping") {
  let origin_x = \TestOptional.origin!.x
  let canary = \TestOptional.questionableCanary!

  var value = TestOptional(origin: Point(x: 3, y: 4))

  expectEqual(value[keyPath: origin_x], 3)
  expectEqual(value.origin!.x, 3)

  value[keyPath: origin_x] = 5

  expectEqual(value[keyPath: origin_x], 5)
  expectEqual(value.origin!.x, 5)

  expectTrue(value[keyPath: canary] === value.questionableCanary)
  let newCanary = LifetimeTracked(456)
  value[keyPath: canary] = newCanary
  expectTrue(value[keyPath: canary] === newCanary)
  expectTrue(value.questionableCanary === newCanary)
}

#if !os(WASI)
// Trap tests aren't available on WASI.
keyPath.test("optional force-unwrapping trap") {
  let origin_x = \TestOptional.origin!.x
  var value = TestOptional(origin: nil)

  expectCrashLater()
  _ = value[keyPath: origin_x]
}
#endif

struct TestOptional2 {
  var optional: TestOptional?
}

keyPath.test("optional chaining") {
  let origin_x = \TestOptional.origin?.x
  let canary = \TestOptional.questionableCanary?.value
  
  let withPoint = TestOptional(origin: Point(x: 3, y: 4))
  expectEqual(withPoint[keyPath: origin_x]!, 3)
  expectEqual(withPoint[keyPath: canary]!, 123)

  let withoutPoint = TestOptional(origin: nil)
  expectNil(withoutPoint[keyPath: origin_x])

  let optional2: TestOptional2? = TestOptional2(optional: withPoint)
  let optional2_optional = \TestOptional2?.?.optional
  expectEqual(optional2[keyPath: optional2_optional]!.origin!.x, 3)
  expectEqual(optional2[keyPath: optional2_optional]!.origin!.y, 4)
}

func makeKeyPathInGenericContext<T>(of: T.Type)
    -> ReferenceWritableKeyPath<C<T>, T> {
  return \C<T>.computed
}

keyPath.test("computed generic key paths") {
  let path = makeKeyPathInGenericContext(of: LifetimeTracked.self)
  let z = LifetimeTracked(456)
  let c = C(x: 42, y: LifetimeTracked(123), z: z)

  expectTrue(c[keyPath: path] === z)

  let z2 = LifetimeTracked(789)
  c[keyPath: path] = z2
  expectTrue(c[keyPath: path] === z2)
  expectTrue(c.z === z2)

  let path2 = makeKeyPathInGenericContext(of: LifetimeTracked.self)

  expectEqual(path, path2)
  expectEqual(path.hashValue, path2.hashValue)

  let pathNonGeneric = \C<LifetimeTracked>.computed
  expectEqual(path, pathNonGeneric)
  expectEqual(path.hashValue, pathNonGeneric.hashValue)

  let valuePath = path.appending(path: \LifetimeTracked.value)

  expectEqual(c[keyPath: valuePath], 789)

  let valuePathNonGeneric = pathNonGeneric.appending(path: \LifetimeTracked.value)
  expectEqual(valuePath, valuePathNonGeneric)
  expectEqual(valuePath.hashValue, valuePathNonGeneric.hashValue)
}

var numberOfMutatingWritebacks = 0
var numberOfNonmutatingWritebacks = 0

struct NoisyWriteback {
  var canary = LifetimeTracked(246)

  var mutating: LifetimeTracked {
    get { return canary }
    set { numberOfMutatingWritebacks += 1 }
  }

  var nonmutating: LifetimeTracked {
    get { return canary }
    nonmutating set { numberOfNonmutatingWritebacks += 1 }
  }
}

keyPath.test("read-only accesses don't trigger writebacks") {
  var x = NoisyWriteback()
  x = NoisyWriteback() // suppress "never mutated" warnings

  let wkp = \NoisyWriteback.mutating
  let rkp = \NoisyWriteback.nonmutating

  numberOfMutatingWritebacks = 0
  numberOfNonmutatingWritebacks = 0
  _ = x[keyPath: wkp]
  _ = x[keyPath: rkp]

  expectEqual(x[keyPath: wkp].value, 246)
  expectEqual(x[keyPath: rkp].value, 246)

  expectEqual(numberOfMutatingWritebacks, 0)
  expectEqual(numberOfNonmutatingWritebacks, 0)

  let y = x
  _ = y[keyPath: wkp]
  _ = y[keyPath: rkp]

  expectEqual(y[keyPath: wkp].value, 246)
  expectEqual(y[keyPath: rkp].value, 246)

  expectEqual(numberOfMutatingWritebacks, 0)
  expectEqual(numberOfNonmutatingWritebacks, 0)
}

var nestedWritebackLog = 0

struct NoisyNestingWriteback {
  var value: Int

  var nested: NoisyNestingWriteback {
    get {
      return NoisyNestingWriteback(value: value + 1)
    }
    set {
      nestedWritebackLog = nestedWritebackLog << 8 | newValue.value
      value = newValue.value - 1
    }
  }
}

keyPath.test("writebacks nest properly") {
  var test = NoisyNestingWriteback(value: 0)
  nestedWritebackLog = 0
  test.nested.nested.nested.value = 0x38
  expectEqual(nestedWritebackLog, 0x383736)

  nestedWritebackLog = 0
  let kp = \NoisyNestingWriteback.nested.nested.nested
  test[keyPath: kp].value = 0x38
  expectEqual(nestedWritebackLog, 0x383736)
}

struct IUOWrapper {
  var wrapped: IUOWrapped!
}

struct IUOWrapped {
  var value: Int
}

keyPath.test("IUO and key paths") {
  var subject = IUOWrapper(wrapped: IUOWrapped(value: 1989))
  let kp1 = \IUOWrapper.wrapped.value

  expectEqual(subject[keyPath: kp1], 1989)
  subject[keyPath: kp1] = 1738
  expectEqual(subject[keyPath: kp1], 1738)
  expectEqual(subject.wrapped.value, 1738)

  let kp2 = \IUOWrapper.wrapped!.value

  expectEqual(kp1, kp2)
  expectEqual(kp1.hashValue, kp2.hashValue)
}

struct SubscriptResult<T: Hashable, U: Hashable> {
  var canary = LifetimeTracked(3333)
  var left: T
  var right: U

  init(left: T, right: U) {
    self.left = left
    self.right = right
  }

  subscript(left: T) -> Bool {
    return self.left == left
  }
  subscript(right: U) -> Bool {
    return self.right == right
  }
}

struct Subscripts<T: Hashable> {
  var canary = LifetimeTracked(4444)

  subscript<U: Hashable>(x: T, y: U) -> SubscriptResult<T, U> {
    return SubscriptResult(left: x, right: y)
  }

  subscript(x: Int, y: Int) -> Int {
    return x + y
  }
}

struct KeyA: Hashable {
  var canary = LifetimeTracked(1111)
  var value: String

  init(value: String) { self.value = value }

  static func ==(a: KeyA, b: KeyA) -> Bool { return a.value == b.value }
  func hash(into hasher: inout Hasher) {
    hasher.combine(value)
  }
}
struct KeyB: Hashable {
  var canary = LifetimeTracked(2222)

  var value: Int

  init(value: Int) { self.value = value }

  static func ==(a: KeyB, b: KeyB) -> Bool { return a.value == b.value }
  func hash(into hasher: inout Hasher) {
    hasher.combine(value)
  }
}

func fullGenericContext<T: Hashable, U: Hashable>(x: T, y: U) -> KeyPath<Subscripts<T>, SubscriptResult<T, U>> {
  return \Subscripts<T>.[x, y]
}

func halfGenericContext<U: Hashable>(x: KeyA, y: U) -> KeyPath<Subscripts<KeyA>, SubscriptResult<KeyA, U>> {
  return \Subscripts<KeyA>.[x, y]
}

func nonGenericContext(x: KeyA, y: KeyB) -> KeyPath<Subscripts<KeyA>, SubscriptResult<KeyA, KeyB>> {
  return \Subscripts<KeyA>.[x, y]
}

keyPath.test("subscripts") {
  let a = fullGenericContext(x: KeyA(value: "hey"), y: KeyB(value: 1738))
  let b = halfGenericContext(x: KeyA(value: "hey"), y: KeyB(value: 1738))
  let c = nonGenericContext(x: KeyA(value: "hey"), y: KeyB(value: 1738))

  expectEqual(a, b)
  expectEqual(a, c)
  expectEqual(b, a)
  expectEqual(b, c)
  expectEqual(c, a)
  expectEqual(c, b)
  expectEqual(a.hashValue, b.hashValue)
  expectEqual(a.hashValue, c.hashValue)
  expectEqual(b.hashValue, a.hashValue)
  expectEqual(b.hashValue, c.hashValue)
  expectEqual(c.hashValue, a.hashValue)
  expectEqual(c.hashValue, b.hashValue)

  let base = Subscripts<KeyA>()

  let kp2 = \SubscriptResult<KeyA, KeyB>.[KeyA(value: "hey")]

  for kp in [a, b, c] {
    let projected = base[keyPath: kp]
    expectEqual(projected.left.value, "hey")
    expectEqual(projected.right.value, 1738)

    expectEqual(projected[keyPath: kp2], true)

    let kp12 =
      \Subscripts<KeyA>.[KeyA(value: "hey"), KeyB(value: 1738)][KeyA(value: "hey")]

    let kp12a = kp.appending(path: kp2)

    expectEqual(kp12, kp12a)
    expectEqual(kp12a, kp12)
    expectEqual(kp12.hashValue, kp12a.hashValue)
  }

  let ints = \Subscripts<KeyA>.[17, 38]
  let ints2 = \Subscripts<KeyA>.[17, 38]
  let ints3 = \Subscripts<KeyA>.[38, 17]
  expectEqual(base[keyPath: ints], 17 + 38)

  expectEqual(ints, ints2)
  expectEqual(ints2, ints)
  expectNotEqual(ints, ints3)
  expectNotEqual(ints2, ints3)
  expectNotEqual(ints3, ints)
  expectNotEqual(ints3, ints2)

  expectEqual(ints.hashValue, ints2.hashValue)

  let ints_be = ints.appending(path: \Int.bigEndian)

  expectEqual(base[keyPath: ints_be], (17 + 38).bigEndian)
}

struct NonOffsetableProperties {
  // observers
  var x: Int { didSet {} }
  // reabstracted
  var y: () -> ()
  // computed
  var z: Int { return 0 }
}

struct TupleProperties {
  // unlabeled
  var a: (Int, String)
  // labeled
  let b: (x: String, y: Int)
  // reference writable
  let c: (m: C<Int>, n: C<String>)
}

func getIdentityKeyPathOfType<T>(_: T.Type) -> KeyPath<T, T> {
  return \.self
}

keyPath.test("offsets") {
  let SLayout = MemoryLayout<S<Int>>.self
  expectNotNil(SLayout.offset(of: \S<Int>.x))
  expectNotNil(SLayout.offset(of: \S<Int>.y))
  expectNotNil(SLayout.offset(of: \S<Int>.z))
  expectNotNil(SLayout.offset(of: \S<Int>.p))
  expectNotNil(SLayout.offset(of: \S<Int>.p.x))
  expectNotNil(SLayout.offset(of: \S<Int>.p.y))
  expectNotNil(SLayout.offset(of: \S<Int>.c))
  expectNil(SLayout.offset(of: \S<Int>.c.x))

  let NOPLayout = MemoryLayout<NonOffsetableProperties>.self
  expectNil(NOPLayout.offset(of: \NonOffsetableProperties.x))
  expectNil(NOPLayout.offset(of: \NonOffsetableProperties.y))
  expectNil(NOPLayout.offset(of: \NonOffsetableProperties.z))

  expectEqual(SLayout.offset(of: \.self), 0)
  expectEqual(SLayout.offset(of: getIdentityKeyPathOfType(S<Int>.self)), 0)

  let TPLayout = MemoryLayout<TupleProperties>.self
  expectEqual(TPLayout.offset(of: \TupleProperties.self), 0)
  expectEqual(TPLayout.offset(of: \TupleProperties.a), 0)
  expectEqual(TPLayout.offset(of: \TupleProperties.a.0), 0)
  expectEqual(TPLayout.offset(of: \TupleProperties.a.1), MemoryLayout<Int>.size)
  expectEqual(TPLayout.offset(of: \TupleProperties.b), MemoryLayout<(Int, String)>.size)
  expectEqual(TPLayout.offset(of: \TupleProperties.b.x), MemoryLayout<(Int, String)>.size)
  expectEqual(TPLayout.offset(of: \TupleProperties.b.y), MemoryLayout<(Int, String, String)>.size)
  expectEqual(TPLayout.offset(of: \TupleProperties.c), MemoryLayout<(Int, String, String, Int)>.size)
  expectEqual(TPLayout.offset(of: \TupleProperties.c.m), MemoryLayout<(Int, String, String, Int)>.size)
  expectEqual(TPLayout.offset(of: \TupleProperties.c.n), MemoryLayout<(Int, String, String, Int, C<Int>)>.size)

  let TLayout = MemoryLayout<Tuple<Int, Int>>.self
  expectEqual(TLayout.offset(of: \Tuple<Int, Int>.self), 0)
  expectEqual(TLayout.offset(of: \Tuple<Int, Int>.0), 0)
  expectEqual(TLayout.offset(of: \Tuple<Int, Int>.0.x), 0)
  expectEqual(TLayout.offset(of: \Tuple<Int, Int>.1), SLayout.size)
}

keyPath.test("identity key path") {
  var x = LifetimeTracked(1738)

  let id = \LifetimeTracked.self
  expectTrue(x === x[keyPath: id])

  let newX = LifetimeTracked(679)
  x[keyPath: id] = newX
  expectTrue(x === newX)

  let id2 = getIdentityKeyPathOfType(LifetimeTracked.self)
  expectEqual(id, id2)
  expectEqual(id.hashValue, id2.hashValue)
  expectNotNil(id2 as? WritableKeyPath)

  let id3 = id.appending(path: id2)
  expectEqual(id, id3)
  expectEqual(id.hashValue, id3.hashValue)
  expectNotNil(id3 as? WritableKeyPath)

  let valueKey = \LifetimeTracked.value
  let valueKey2 = id.appending(path: valueKey)
  let valueKey3 = (valueKey as KeyPath).appending(path: \Int.self)

  expectEqual(valueKey, valueKey2)
  expectEqual(valueKey.hashValue, valueKey2.hashValue)
  expectEqual(valueKey, valueKey3)
  expectEqual(valueKey.hashValue, valueKey3.hashValue)

  expectEqual(x[keyPath: valueKey2], 679)
  expectEqual(x[keyPath: valueKey3], 679)
}

keyPath.test("tuple key path") {
  let t0 = \TupleProperties.a.0
  expectNotNil(t0 as? KeyPath<TupleProperties, Int>)
  expectNotNil(t0 as? WritableKeyPath<TupleProperties, Int>)
  expectNil(t0 as? ReferenceWritableKeyPath<TupleProperties, Int>)

  let t1 = \TupleProperties.a.1
  expectNotNil(t1 as? KeyPath<TupleProperties, String>)
  expectNotNil(t1 as? WritableKeyPath<TupleProperties, String>)
  expectNil(t1 as? ReferenceWritableKeyPath<TupleProperties, String>)

  let t2 = \TupleProperties.b.x
  expectNotNil(t2 as? KeyPath<TupleProperties, String>)
  expectNil(t2 as? WritableKeyPath<TupleProperties, String>)
  expectNil(t2 as? ReferenceWritableKeyPath<TupleProperties, String>)

  let t3 = \TupleProperties.b.y
  expectNotNil(t3 as? KeyPath<TupleProperties, Int>)
  expectNil(t3 as? WritableKeyPath<TupleProperties, Int>)
  expectNil(t3 as? ReferenceWritableKeyPath<TupleProperties, Int>)

  let t4 = \TupleProperties.c.m
  expectNotNil(t4 as? KeyPath<TupleProperties, C<Int>>)
  expectNil(t4 as? WritableKeyPath<TupleProperties, C<Int>>)
  expectNil(t4 as? ReferenceWritableKeyPath<TupleProperties, C<Int>>)

  let t5 = \TupleProperties.c.n.z
  expectNotNil(t5 as? KeyPath<TupleProperties, String>)
  expectNotNil(t5 as? WritableKeyPath<TupleProperties, String>)
  expectNotNil(t5 as? ReferenceWritableKeyPath<TupleProperties, String>)
}

keyPath.test("tuple key path execution") {
  typealias T0 = (Int, String)
  typealias T1 = (x: Int, y: String)

  let kp_t0_0 = \T0.0
  let kp_t0_1 = \T0.1

  let kp_t1_x = \T1.x
  let kp_t1_y = \T1.y
  let kp_t1_0 = \T1.0
  let kp_t1_1 = \T1.1

  var tuple0 = (1, "Hello")
  let tuple1 = (x: 2, y: "World")
  let tuple2 = (a: 3, b: "String")

  // in some cases, tuple key paths are interchangeable

  expectEqual(tuple0[keyPath: kp_t0_0], 1)
  expectEqual(tuple0[keyPath: kp_t1_x], 1)
  expectEqual(tuple0[keyPath: kp_t1_0], 1)

  expectEqual(tuple0[keyPath: kp_t0_1], "Hello")
  expectEqual(tuple0[keyPath: kp_t1_y], "Hello")
  expectEqual(tuple0[keyPath: kp_t1_1], "Hello")


  expectEqual(tuple1[keyPath: kp_t0_0], 2)
  expectEqual(tuple1[keyPath: kp_t1_x], 2)
  expectEqual(tuple1[keyPath: kp_t1_0], 2)

  expectEqual(tuple1[keyPath: kp_t0_1], "World")
  expectEqual(tuple1[keyPath: kp_t1_y], "World")
  expectEqual(tuple1[keyPath: kp_t1_1], "World")


  expectEqual(tuple2[keyPath: kp_t0_0], 3)
  //expectEqual(tuple2[keyPath: kp_t1_x], 3)
  //expectEqual(tuple2[keyPath: kp_t1_0], 3)

  expectEqual(tuple2[keyPath: kp_t0_1], "String")
  //expectEqual(tuple2[keyPath: kp_t1_y], "String")
  //expectEqual(tuple2[keyPath: kp_t1_1], "String")


  tuple0[keyPath: kp_t0_1] = "Another String"
  expectEqual(tuple0[keyPath: kp_t0_1], "Another String")
}

keyPath.test("tuple key path execution (generic)") {
  struct Container<T, U> {
    let x: (T, U)
    var y: (a: T, b: U)
  }

  func generic<A: Equatable, B: Equatable>(a: A, b: B) {
    typealias T = (A, B)

    let kp_t_0 = \T.0
    let kp_t_1 = \T.1
 
    let kp_c_x = \Container<A, B>.x
    let kp_c_x_0 = kp_c_x.appending(path: kp_t_0)
    let kp_c_x_1 = kp_c_x.appending(path: kp_t_1)

    let kp_c_y_a = \Container<A, B>.y.a
    let kp_c_y_b = \Container<A, B>.y.b
 

    let tuple = (a, b)
    let container = Container(x: (a, b), y: (a, b))


    expectEqual(tuple[keyPath: kp_t_0], tuple.0)
    expectEqual(tuple[keyPath: kp_t_1], tuple.1)

    expectEqual(container[keyPath: kp_c_x_0], container.x.0)
    expectEqual(container[keyPath: kp_c_x_1], container.x.1)

    expectEqual(container[keyPath: kp_c_y_a], container.y.0)
    expectEqual(container[keyPath: kp_c_y_b], container.y.1)
  }

  generic(a: 13, b: "Hello Tuples")
  generic(a: "Tuples Hello", b: 31)
}

keyPath.test("let-ness") {
  expectNil(\C<Int>.immutable as? ReferenceWritableKeyPath)
  expectNotNil(\C<Int>.secretlyMutable as? ReferenceWritableKeyPath)
  expectNil(\Point.hypotenuse as? WritableKeyPath)
  expectNotNil(\Point.secretlyMutableHypotenuse as? WritableKeyPath)
}

keyPath.test("key path literal closures") {
  // Property access
  let fnX: (C<String>) -> Int = \C<String>.x
  let fnY: (C<String>) -> LifetimeTracked? = \C<String>.y
  let fnZ: (C<String>) -> String = \C<String>.z
  let fnImmutable: (C<String>) -> String = \C<String>.immutable
  let fnSecretlyMutable: (C<String>) -> String = \C<String>.secretlyMutable
  let fnComputed: (C<String>) -> String = \C<String>.computed
  
  let lifetime = LifetimeTracked(249)
  let base = C(x: 1, y: lifetime, z: "SE-0249")

  expectEqual(1, fnX(base))
  expectEqual(lifetime, fnY(base))
  expectEqual("SE-0249", fnZ(base))
  expectEqual("1 Optional(249) SE-0249", fnImmutable(base))
  expectEqual("1 Optional(249) SE-0249", fnSecretlyMutable(base))
  expectEqual("SE-0249", fnComputed(base))
  
  // Subscripts
  var callsToComputeIndex = 0
  func computeIndexWithSideEffect(_ i: Int) -> Int {
    callsToComputeIndex += 1
    return -i
  }
  
  let fnSubscriptResultA: (Subscripts<String>) -> SubscriptResult<String, Int>
    = \Subscripts<String>.["A", computeIndexWithSideEffect(13)]
  let fnSubscriptResultB: (Subscripts<String>) -> SubscriptResult<String, Int>
    = \Subscripts<String>.["B", computeIndexWithSideEffect(42)]
  
  let subscripts = Subscripts<String>()
  
  expectEqual("A", fnSubscriptResultA(subscripts).left)
  expectEqual(-13, fnSubscriptResultA(subscripts).right)
  
  expectEqual("B", fnSubscriptResultB(subscripts).left)
  expectEqual(-42, fnSubscriptResultB(subscripts).right)
  
  // Did we compute the indices once per closure construction, or once per
  // closure application?
  expectEqual(2, callsToComputeIndex)

  // rdar://problem/59445486
  let variadicFn: (String...) -> Int = \.count
  expectEqual(3, variadicFn("a", "b", "c"))
}

// https://github.com/apple/swift/issues/48651

protocol P_48651 {}
struct S_48651<ValueType> {}
extension P_48651 {
    var asString: String? {
        return self as? String
    }
}
extension S_48651 where ValueType: P_48651 {
    func doSomething() {
        _ = \ValueType.asString?.endIndex
    }
}
extension Int: P_48651 {}

keyPath.test("optional chaining component that needs generic instantiation") {
  S_48651<Int>().doSomething()
}

// Nested generics.
protocol HasAssoc {
  associatedtype A
}

struct Outer<T, U> {
  struct Middle<V, W, X> {
  }
}

extension Outer.Middle where V: HasAssoc, U == V.A, W == X {
  struct Inner<Y: Hashable> {
    func foo() ->  AnyKeyPath {
      return \[Y?: [U]].values
    }
  }
}

extension Double: HasAssoc {
  typealias A = Float
}

keyPath.test("nested generics") {
  let nested = Outer<Int, Float>.Middle<Double, String, String>.Inner<UInt>()
  let nestedKeyPath = nested.foo()
  typealias DictType = [UInt? : [Float]]
  expectTrue(nestedKeyPath is KeyPath<DictType, DictType.Values>)
}

keyPath.test("tail allocated c array") {
  let offset = MemoryLayout<foo>.offset(of: \foo.tailallocatedarray)!
  expectEqual(4, offset)
}

keyPath.test("ReferenceWritableKeyPath statically typed as WritableKeyPath") {
  let inner = C<Int>(x: 42, y: nil, z: 43)
  var outer = C<C<Int>>(x: 44, y: nil, z: inner)
  let keyPath = \C<C<Int>>.z.x
  let upcastKeyPath = keyPath as WritableKeyPath

  expectEqual(outer[keyPath: keyPath], 42)
  outer[keyPath: keyPath] = 43
  expectEqual(outer[keyPath: keyPath], 43)

  expectEqual(outer[keyPath: upcastKeyPath], 43)
  outer[keyPath: upcastKeyPath] = 44
  expectEqual(outer[keyPath: upcastKeyPath], 44)

  func setWithInout<T>(_ lhs: inout T, _ rhs: T) { lhs = rhs }

  expectEqual(outer[keyPath: keyPath], 44)
  setWithInout(&outer[keyPath: keyPath], 45);
  expectEqual(outer[keyPath: keyPath], 45)

  expectEqual(outer[keyPath: upcastKeyPath], 45)
  setWithInout(&outer[keyPath: upcastKeyPath], 46)
  expectEqual(outer[keyPath: upcastKeyPath], 46)
}

struct Dog {
  var name: String
  var age: Int
}

if #available(SwiftStdlib 5.9, *) {
  keyPath.test("_createOffsetBasedKeyPath") {
    let dogAgeKp = _createOffsetBasedKeyPath(
      root: Dog.self,
      value: Int.self,
      offset: MemoryLayout<String>.size
    ) as? KeyPath<Dog, Int>

    expectNotNil(dogAgeKp)

    let sparky = Dog(name: "Sparky", age: 7)

    expectEqual(sparky[keyPath: dogAgeKp!], 7)
  }
}

class RerootedSuper {
  var x = "hello world"
}

class RerootedSub0: RerootedSuper {}
class RerootedSub1: RerootedSub0 {}

if #available(SwiftStdlib 5.9, *) {
  keyPath.test("_rerootKeyPath") {
    let x = \RerootedSub1.x

    let superValue = RerootedSuper()
    let sub0 = RerootedSub0()
    let sub1 = RerootedSub1()

    let sub0Kp = _rerootKeyPath(x, to: RerootedSub0.self)

    expectTrue(type(of: sub0Kp) == ReferenceWritableKeyPath<RerootedSub0, String>.self)

    let superKp = _rerootKeyPath(x, to: RerootedSuper.self)

    expectTrue(type(of: superKp) == ReferenceWritableKeyPath<RerootedSuper, String>.self)

    let x0 = sub1[keyPath: sub0Kp] as! String
    expectEqual(x0, "hello world")

    let x1 = sub1[keyPath: superKp] as! String
    expectEqual(x1, "hello world")

    let x2 = sub0[keyPath: sub0Kp] as! String
    expectEqual(x2, "hello world")

    let x3 = sub0[keyPath: superKp] as! String
    expectEqual(x3, "hello world")

    let x4 = superValue[keyPath: superKp] as! String
    expectEqual(x4, "hello world")
  }
}

runAllTests()

