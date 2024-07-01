// Casts.swift - Tests for conversion between types.
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
// -----------------------------------------------------------------------------
///
/// Contains tests for non-trapping type conversions reported by users.
///
// -----------------------------------------------------------------------------
// RUN: %empty-directory(%t)
//
// RUN: %target-build-swift -swift-version 5 -g -Onone  -module-name a %s -o %t/a.swift5.Onone.out
// RUN: %target-codesign %t/a.swift5.Onone.out
// RUN: %target-run %t/a.swift5.Onone.out
//
// RUN: %target-build-swift -swift-version 5 -g -O  -module-name a %s -o %t/a.swift5.O.out
// RUN: %target-codesign %t/a.swift5.O.out
// RUN: %target-run %t/a.swift5.O.out
//
// REQUIRES: executable_test
// REQUIRES: concurrency
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

import StdlibUnittest
#if _runtime(_ObjC)
import Foundation
#endif

func blackhole<T>(_ t: T) { }

private func runtimeCast<T,U>(_ from: T, to: U.Type) -> U? {
  return from as? U
}

let CastsTests = TestSuite("Casts")

// https://github.com/apple/swift/issues/43043
// Missing release for some types after failed conversion
CastsTests.test("No leak for failed tuple casts") {
    let t: Any = (1, LifetimeTracked(0))
    expectFalse(t is Any.Type)
}

protocol P {}
class ErrClass : Error { }

// https://github.com/apple/swift/issues/43009
CastsTests.test("No overrelease of existential boxes in failed casts") {
    // We fail casts of an existential box repeatedly to ensure it does not get
    // over-released.
    func bar<T>(_ t: T) {
        for _ in 0..<10 {
            if case let a as P = t {
                _ = a
            }
        }
    }

    let err: Error = ErrClass()
    bar(err)
}

extension Int : P {}

/// https://github.com/apple/swift/issues/50204
/// Inconsistent optional casting behaviour with generics
///
/// Runtime failed to unwrap multiple levels of `Optional` when casting.
CastsTests.test("Multi-level optionals can be casted") {
  func testSuccess<From, To>(_ x: From, from: From.Type, to: To.Type) {
    expectNotNil(x as? To)
  }
  func testFailure<From, To>(_ x: From, from: From.Type, to: To.Type) {
    expectNil(x as? To)
  }
  testSuccess(42, from: Int?.self, to: Int.self)
  testSuccess(42, from: Int??.self, to: Int.self)
  testSuccess(42, from: Int???.self, to: Int.self)
  testSuccess(42, from: Int???.self, to: Int?.self)
  testSuccess(42, from: Int???.self, to: Int??.self)
  testSuccess(42, from: Int???.self, to: Int???.self)
  testFailure(42, from: Int?.self, to: String.self)
  testFailure(42, from: Int??.self, to: String.self)
  testFailure(42, from: Int???.self, to: String.self)
}

/// https://github.com/apple/swift/issues/52251
/// `Optional<T>.none` not casting to `Optional<U>.none` in generic context
CastsTests.test("Optional<T>.none can be casted to Optional<U>.none in generic context") {
  func test<T>(_ type: T.Type) -> T? {
    return Any?.none as? T
  }

  expectEqual(type(of: test(Bool.self)), Bool?.self)
  expectEqual(type(of: test(Bool?.self)), Bool??.self)
}

/// https://github.com/apple/swift/issues/46456
/// Failure to cast from ObjC existential without going through `AnyObject`
#if _runtime(_ObjC)
protocol P2 {}
CastsTests.test("Cast from ObjC existential to Protocol") {
  if #available(SwiftStdlib 5.3, *) {
    struct S: P2 {}

    class ObjCWrapper {
      @objc dynamic let any: Any = S()
      init() {}
    }
    let a = ObjCWrapper().any
    expectTrue(a is P2)
    // The following cast failed in the above issue (everything else here
    // succeeded).
    expectNotNil(a as? P2)
    expectNotNil(a as? S)
    let b = a as AnyObject
    expectTrue(a is P2)
    expectNotNil(b as? P2)
    expectNotNil(b as? S)
  }
}
#endif

protocol P3 {}
CastsTests.test("Cast from Swift existential to Protocol") {
  struct S: P3 {}
  class SwiftWrapper {
    let any: Any = S()
    init() {}
  }
  let a = SwiftWrapper().any
  expectTrue(a is P3)
  expectNotNil(a as? P3)
  expectNotNil(a as? S)
  let b = a as AnyObject
  expectTrue(b is P3)
  expectNotNil(b as? P3)
  expectNotNil(b as? S)
}

/// Another test for https://github.com/apple/swift/issues/46456
/// User type in a `_SwiftValue` in an `Optional<Any>` not casting to a
/// protocol
///
/// Note: This uses the (misnamed) `_bridgeAnythingToObjectiveC` so it can
/// test these paths on Linux as well.
protocol P4 {}
CastsTests.test("struct -> Obj-C -> Protocol") {
  struct SFUUUHREEEEEFFFF: P4 {
    let value: Int
    let tracker = LifetimeTracked(13)
  }

  let a: P4 = SFUUUHREEEEEFFFF(value: 13)

  let b = _bridgeAnythingToObjectiveC(a)
  let d = b as? Any
  let e = d as? P4
  expectNotNil(e)
}

#if _runtime(_ObjC)
extension CFBitVector : P {
  static func makeImmutable(from values: Array<UInt8>) -> CFBitVector {
    return CFBitVectorCreate(/*allocator:*/ nil, values, values.count * 8)
  }
}

extension CFMutableBitVector {
  static func makeMutable(from values: Array<UInt8>) -> CFMutableBitVector {
    return CFBitVectorCreateMutableCopy(
      /*allocator:*/ nil,
      /*capacity:*/ 0,
      CFBitVector.makeImmutable(from: values))
  }
}

func isP<T>(_ t: T) -> Bool {
  return t is P
}

// https://github.com/apple/swift/issues/44896
CastsTests.test("Dynamic casts of CF types to protocol existentials")
.skip(.custom({
      !_isDebugAssertConfiguration()
    },
    reason: "This test behaves unpredictably in optimized mode."))
.code {
  expectTrue(isP(10 as Int))
  if #available(SwiftStdlib 5.5, *) {
    expectTrue(isP(CFBitVector.makeImmutable(from: [10, 20])))
    expectTrue(isP(CFMutableBitVector.makeMutable(from: [10, 20])))
  }
}
#endif

// https://github.com/apple/swift/issues/47129
protocol P_47129 {}
if #available(SwiftStdlib 5.5, *) {
CastsTests.test("Any(Optional(T)) -> Protocol") {
  struct S: P_47129 {
    let tracker = LifetimeTracked(13)
  }

  let a = S()
  let b: S? = a
  let c = b as? Any
  let d = c as? P_47129
  expectNotNil(d)
}
}

// rdar://27108240 (Optional casting bug (crash))
protocol Key {
  associatedtype Value
}
CastsTests.test("Cast to associated type") {
  // Helper function to bypass compiler cast optimizations
  func runtimeCast<From, To> (_ x: From, to: To.Type) -> To? {
    return x as? To
  }
  struct StringKey : Key {
    typealias Value = String?
  }
  var string: String?
  func value<K: Key>(forKey key: K.Type) {
    let b = runtimeCast(string, to: K.Value.self)
    expectNotNil(b)
    let c = string as? K.Value
    expectNotNil(c)
  }
  value(forKey: StringKey.self)
}

#if _runtime(_ObjC)
// rdar://36288786 (Swift metatype stored in an Objective-C id property can't be typecast back to its original type)
CastsTests.test("Store Swift metatype in ObjC property and cast back to Any.Type") {
  class MyObj {
    var sVar: Any? = nil
    @objc dynamic var objcVar: Any? = nil
  }

  let a = MyObj()

  // Double values
  a.sVar = 1.234
  a.objcVar = 1.234

  let sValue1 = a.sVar as? Double
  let objcValue1 = a.objcVar as? Double
  expectEqual(sValue1, objcValue1)

  // Swift types
  let b = Bool.self
  a.sVar = b
  a.objcVar = b

  let sValue2 = a.sVar as? Any.Type
  let objcValue2 = a.objcVar as? Any.Type
  expectTrue(sValue2 == b)
  if #available(SwiftStdlib 5.5, *) {
    expectTrue(sValue2 == objcValue2)
    expectTrue(objcValue2 == b)
  }
}
#endif

/// rdar://37793004
/// https://github.com/apple/swift/issues/49597
CastsTests.test("Cast enum back from AnyHashable") {
  enum E {
    case a
  }

  // This works as expected.
  let str: AnyHashable = "hello"
  expectNotNil(str as? String) // Optional("hello")
  expectNotNil(str as? String as Any) // Optional("hello")

  // This doesn't.
  let ea: AnyHashable = E.a
  expectNotNil(ea as? E)
  expectNotNil(ea as? E as Any)
  expectEqual((ea as? E), E.a)
}

/// rdar://39415812
/// https://github.com/apple/swift/issues/49975
/// Failure to see through boxed `_SwiftValue` when casting from `@objc` Type
#if _runtime(_ObjC)
@objc(Exporter)
protocol Exporter: NSObjectProtocol {
  var type: Any { get }
  func export(item: Any) -> String?
}
CastsTests.test("Casts from @objc Type") {
  struct User { var name: String }

  final class UserExporter: NSObject, Exporter {
    var type: Any { return User.self }
    func export(item: Any) -> String? {
      let u = item as? User
      return u?.name
    }
  }

  let user = User(name: "Kermit")
  let exporter: Exporter = UserExporter()

  if #available(SwiftStdlib 5.5, *) {
    expectTrue(exporter.type is User.Type)
  }
  expectNotNil(exporter.export(item: user))
}
#endif

#if _runtime(_ObjC)
// rdar://44467533 (Swift main branch: conditional casts for _ObjectiveCBridgeable miscompile in swift-corelibs-foundation)
CastsTests.test("Conditional NSNumber -> Bool casts") {
  let x = NSNumber(value: -1) as? Bool
  expectNil(x)
}
#endif

/// rdar://45217461
/// https://github.com/apple/swift/issues/51469
/// Type check operator (`is`) fails for `Any!` variable holding an `Error`
/// (struct) value
if #available(SwiftStdlib 5.5, *) {
CastsTests.test("Casts from Any(struct) to Error") {
  struct MyError: Error { }

  let a: Any! = MyError()
  let b: Any = a
  expectTrue(b is Error)
}
}

CastsTests.test("Cast failure for Any! holding Error struct") {
  struct MyError: Error {}
  let a: Any! = MyError()
  let b: Any = a
  expectTrue(b is Error)
}

#if _runtime(_ObjC)
// rdar://15494623 (Handle dynamic cast to archetype bound to ObjC existential)
CastsTests.test("Dynamic cast to ObjC protocol") {
  func genericCast<T>(x: NSObject, _: T.Type) -> T? {
    return x as? T
  }

  let n: NSNumber = 1
  let copying = genericCast(x: n, NSCopying.self)
  expectNotNil(copying)
}
#endif

// https://github.com/apple/swift/issues/48681
if #available(macOS 11.3, iOS 14.5, tvOS 14.5, watchOS 7.4, *) {
CastsTests.test("Nil handling for Optionals and Arrays") {
  func check(_ arg: Int??) -> String {
    switch arg {
    case .none:
      return ".none"
    case .some(.none):
      return ".some(.none)"
    case .some(.some):
      return ".some(.some)"
    }
  }

  let x: Int? = .none
  let y: [Int?] = [.none]

  let a = x as Int??
  let b = (x as? Int??)!
  let b2 = runtimeCast(x, to: Int??.self)!
  let c = Int?.none as Int??
  let d = (Int?.none as? Int??)!
  let d2 = runtimeCast(Int?.none, to: Int??.self)!
  let e = (y as [Int??]).first!
  let f = (y as? [Int??])!.first!
  let f2 = runtimeCast(y, to: [Int??].self)!.first!
  let g = ([Int?.none] as [Int??]).first!
  let h = ([Int?.none] as? [Int??])!.first!
  let h2 = runtimeCast([Int?.none], to: [Int??].self)!.first!

  // Original reporter believes all of these should be .some(.none)
  expectEqual(".some(.none)", check(a)) // Xcode 9.0: .some(.none)
  expectEqual(".some(.none)", check(b)) // Xcode 9.0: .some(.none)
  expectEqual(".some(.none)", check(b2))
  expectEqual(".some(.none)", check(c)) // Xcode 9.0: .some(.none)
  expectEqual(".some(.none)", check(d)) // Xcode 9.0: .some(.none)
  expectEqual(".some(.none)", check(d2))
  expectEqual(".some(.none)", check(e)) // Xcode 9.0: .none
  expectEqual(".some(.none)", check(f)) // Xcode 9.0: .none
  expectEqual(".some(.none)", check(f2))
  expectEqual(".some(.none)", check(g)) // Xcode 9.0: .some(.none)
  expectEqual(".some(.none)", check(h)) // Xcode 9.0: .none
  expectEqual(".some(.none)", check(h2))
}
}

protocol SwiftProtocol {}
CastsTests.test("Swift Protocol Metatypes don't self-conform") {
  let a = SwiftProtocol.self
  // `is P.Protocol` tests whether the argument is a subtype of P.
  // In particular, the protocol identifier `P.self` is such a subtype.
  if #available(SwiftStdlib 5.5, *) {
    expectNotNil(runtimeCast(a, to: SwiftProtocol.Protocol.self)) // Fixed by rdar://58991956
  }
  expectNotNil(a as? SwiftProtocol.Protocol)
  expectTrue(a is SwiftProtocol.Protocol)
  blackhole(a as! SwiftProtocol.Protocol) // Should not trap

  // `is P.Type` tests conformance to P.  Protocols cannot conform to
  // protocols, so these always fail.
  expectNil(runtimeCast(a, to: SwiftProtocol.Type.self))
  expectNil(a as? SwiftProtocol.Type)
  expectFalse(a is SwiftProtocol.Type)
}

CastsTests.test("Self-conformance for Any.self") {
  let b = Any.self
  expectNotNil(runtimeCast(b, to: Any.Protocol.self))
  blackhole(b as! Any.Protocol) // Should not trap
  expectTrue(b is Any.Protocol)
  expectNotNil(b as? Any.Protocol)

  // Unlike most other protocols, Any.self does conform to Any
  expectNotNil(runtimeCast(b, to: Any.Type.self))
  expectNotNil(b as? Any.Type)
  expectTrue(b is Any.Type)
  blackhole(b as! Any.Type)
}

// rdar://59067748 (Error Protocol should self-conform in optimized casts)
CastsTests.test("Self-conformance for Error.self")
.skip(.custom({
      !_isDebugAssertConfiguration()
    },
    reason: "Cast optimizer breaks this test"))
.code {
  let c = Error.self
  expectNotNil(runtimeCast(c, to: Error.Protocol.self))
  expectNotNil(c as? Error.Protocol)
  expectTrue(c is Error.Protocol)
  blackhole(c as! Error.Protocol)

  // Unlike most other protocols, Error.self does conform to Error
  expectNotNil(runtimeCast(c, to: Error.Type.self))
  expectFailure { expectNotNil(c as? Error.Type) }
  expectFailure { expectTrue(c is Error.Type) }
  // blackhole(c as! Error.Type) // Should not trap, but currently does
}

// rdar://59067556 (Obj-C Protocol Metatypes should self-conform)
#if _runtime(_ObjC)
@objc protocol ObjCProtocol {}
CastsTests.test("ObjC Protocol Metatypes self-conform") {
  let a = ObjCProtocol.self
  if #available(SwiftStdlib 5.5, *) {
    expectNotNil(runtimeCast(a, to: ObjCProtocol.Protocol.self))
  }
  expectNotNil(a as? ObjCProtocol.Protocol)
  expectTrue(a is ObjCProtocol.Protocol)
  blackhole(a as! ObjCProtocol.Protocol)

  // Unlike Swift protocols, ObjC protocols do conform to themselves
  expectFailure { expectNotNil(runtimeCast(a, to: ObjCProtocol.Type.self)) }
  expectFailure { expectNotNil(a as? ObjCProtocol.Type) }
  expectFailure { expectTrue(a is ObjCProtocol.Type) }
  // blackhole(a as! ObjCProtocol.Type) // Should not trap, but currently does
}
#endif

#if _runtime(_ObjC)
protocol NewStringProtocol {}
extension String: NewStringProtocol { }
CastsTests.test("String/NSString extension compat") {
  let x: Any = NSString()
  expectFailure { expectNotNil(runtimeCast(x, to: NewStringProtocol.self)) }
  expectFailure { expectNotNil(x as? NewStringProtocol) }
}
#endif

// https://github.com/apple/swift/issues/44608
protocol P_44608 {}
if #available(SwiftStdlib 5.5, *) {
CastsTests.test("Cast Any(Optional(class)) to Protocol type") {
  class Foo: P_44608 { }

  let optionalFoo : Foo? = Foo()
  let anyValue: Any = optionalFoo

  let foo1 = anyValue as? Foo
  expectNotNil(foo1)

  let foo2 = anyValue as? P_44608
  expectNotNil(foo2)

  let foo3 = runtimeCast(anyValue, to: Foo.self)
  expectNotNil(foo3)

  let foo4 = runtimeCast(anyValue, to: P_44608.self)
  expectNotNil(foo4)
}
}

CastsTests.test("Cast from Any? to Existential") {
  let a = Float(1) as Any as? Float
  expectNotNil(a)

  let b = Float(1) as Any as? CustomStringConvertible
  expectNotNil(b)

  let c = Optional.some(Float(1)) as Any as? Float
  expectNotNil(c)

  let d = Optional.some(Float(1)) as Any as? CustomStringConvertible
  expectNotNil(d)
}

// https://github.com/apple/swift/issues/45505
#if _runtime(_ObjC)
CastsTests.test("Dict value casting") {
  var dict: [AnyHashable: String] = [:]
  dict["Key"] = "Value"
  expectNotNil(dict["Key"] as? NSString)
  expectNotNil(runtimeCast(dict["Key"], to: NSString.self))
}
#endif

// https://github.com/apple/swift-corelibs-foundation/issues/3279
#if _runtime(_ObjC)
CastsTests.test("String coercions should work on Linux") {
  let a = "abc" as Substring as NSString
  let b = "abc" as NSString
  expectEqual(a, b)

  let c = "abc" as Substring
  let d = c as? NSString
  let e = "abc" as? NSString
  expectEqual(d, e)

  let f = runtimeCast(d, to: NSString.self)
  expectEqual(e, f)
}
#endif

class ClassInt: Equatable, Hashable {
  private var tracker = LifetimeTracked(77)
  static func == (lhs: ClassInt, rhs: ClassInt) -> Bool {return true}
  func hash(into hasher: inout Hasher) {}
}
CastsTests.test("AnyHashable(Class) -> Obj-C -> Class") {
  let a = ClassInt()
  let b = runtimeCast(a, to: AnyHashable.self)!
  let c = _bridgeAnythingToObjectiveC(b)
  let d = /* SwiftValueBox(AnyHashable(ClassInt)) */ c as? ClassInt
  expectNotNil(d)
  let d2 = runtimeCast(c, to: ClassInt.self)
  expectNotNil(d2)
  let e = runtimeCast(/* SwiftValueBox(AnyHashable(ClassInt)) */ c, to: ClassInt.self)
  expectNotNil(e)
}

// rdar://58999120
// https://github.com/apple/swift-corelibs-foundation/issues/3274
#if _runtime(_ObjC)
CastsTests.test("Error -> NSError -> Protocol transitivity") {
  enum NonConformingError: Error {
  case ok
  }

  let nonConformingError: Error = NonConformingError.ok

  // NSError conforms to CustomStringConvertible, so ...
  let conformingError = nonConformingError as? NSError
  expectTrue(conformingError is CustomStringConvertible)
  expectNotNil(conformingError as? CustomStringConvertible)

  // Our error type does not conform directly, but should conform
  // indirectly because of NSError...
  // Note: Known broken in both runtime and compiler.
  expectFailure { expectTrue(nonConformingError is CustomStringConvertible) }
  expectFailure { expectNotNil(nonConformingError as? CustomStringConvertible) }
}
#endif

#if _runtime(_ObjC)
CastsTests.test("Runtime crash casting Obj-C object to Obj-C protocol (rdar://16449805)") {
  // FIXME:  The reported crash was for `NSPoint(x:0, y:0) as? NSCoding`,
  // but NSPoint seems to not be available on 32-bit platforms.
  expectNotNil(NSString() as? NSCoding)
}
#endif

CastsTests.test("Casting Swift Error-conforming types to Error existentials") {
  enum Foo: Error {
  case OK
  case Broken
  }
  let a = Foo.Broken
  let b = a as? Error
  expectNotNil(b)
  let c = b as? Foo
  expectNotNil(c)
  let d = Foo.self as? Error.Type
  expectNotNil(d)
}

#if _runtime(_ObjC)
CastsTests.test("Casting NSError <-> Error") {
  @objc class Bar: NSError {
    init() {super.init(domain: "Bar", code: 99)}
    required init?(coder: NSCoder) {super.init(coder: coder)}
  }
  let e = Bar.self as? Error.Type
  expectNotNil(e)
  let f = Bar.self as? Bar.Type
  expectNotNil(f)
  let g = Bar() as? Error
  expectNotNil(g)
}
#endif

// Foundation's JSON handling makes heavy use of passing Any? inside of Any
// existentials.  That inspired the following three checks:
CastsTests.test("[Any(Any?)] -> [Any?] should prefer unwrapping source") {
  let a: Any? = nil
  let b: [Any] = [a as Any]
  let c = b as? [Any?]
  let d = c!
  let e = d[0]
  expectNil(e)
}

CastsTests.test("Any(Any?) -> Any? should prefer unwrapping source") {
  let a: Any? = nil
  let b: Any = a
  let c = b as? Any?
  let d = c!
  expectNil(d)
}

#if _runtime(_ObjC)
CastsTests.test("NSNull?.none -> Any? should set outer nil") {
  let a: NSNull? = nil
  let b = a as? Any?
  let c = b!
  expectNil(c)
}
#endif

if #available(macOS 11.3, iOS 14.5, tvOS 14.5, watchOS 7.4, *) {
CastsTests.test("Int??.some(nil) => Int??? should inject naturally") {
  let a: Int?? = .some(nil)
  let b = a as? Int???
  let c = b!
  let d = c!
  let e = d!
  expectNil(e)
}
}

if #available(macOS 11.3, iOS 14.5, tvOS 14.5, watchOS 7.4, *) {
CastsTests.test("Int??.some(nil) => String??? should inject naturally") {
  let a: Int?? = .some(nil)
  let b = runtimeCast(a, to: String???.self)
  let c = b!
  let d = c!
  let e = d!
  expectNil(e)
}
}

if #available(macOS 11.3, iOS 14.5, tvOS 14.5, watchOS 7.4, *) {
CastsTests.test("Int??.some(nil) => Any??? should inject naturally") {
  let a: Int?? = .some(nil)
  let b = a as? Any???
  let c = b!
  let d = c!
  let e = d!
  expectNil(e)
}
}

#if _runtime(_ObjC)
CastsTests.test("NSString -> String fast path") {
  let a = "short" as NSString
  expectNotNil(a as? String)
  let b = runtimeCast(a, to: String.self)
  expectNotNil(b)

  let c = "Long (adj) -- extended, large, the opposite of short" as NSString
  expectNotNil(c as? String)
  let d = runtimeCast(c, to: String.self)
  expectNotNil(d)

  let e = NSMutableString("not read-only")
  expectNotNil(e as? String)
  let f = runtimeCast(e, to: String.self)
  expectNotNil(f)

  let g = CFStringCreateWithCString(nil, "hello, world", CFStringBuiltInEncodings.UTF8.rawValue)
  expectNotNil(g as? String)
  let h = runtimeCast(g, to: String.self)
  expectNotNil(h)

  let i = CFStringCreateMutable(nil, 0)
  expectNotNil(i as? String)
  let j = runtimeCast(i, to: String.self)
  expectNotNil(j)
}
#endif

// This fails in optimized builds because after inlining `runtimeCast`,
// the resulting SIL cast operation is left in a form that IRGen can't
// correctly handle.
//CastsTests.test("Optimized metatype -> AnyObject cast") {
//  struct StructInt { }
//  let a = StructInt.self
//  let b = runtimeCast(a, to: AnyObject.self)
//  expectNotNil(b)
//}

CastsTests.test("Any.Protocol") {
  class C {}
  struct S {}
  func isAnyProtocol<T>(_ type: T.Type) -> Bool {
    let result = T.self is Any.Protocol
		if result {
			// `as!` should succeed if `is` does
			blackhole(T.self as! Any.Protocol)
		}
    return result
  }
  func isAnyType<T>(_ type: T.Type) -> Bool {
    return T.self is Any.Type
  }
  func isType<T,U>(_ type: T.Type, to: U.Type) -> Bool {
    return T.self is U.Type
  }

  expectTrue(Int.self is Any.Type)
  expectNotNil(Int.self as? Any.Type)
  expectTrue(isAnyType(Int.self))
  expectFalse(Int.self is Any.Protocol)
  expectNil(Int.self as? Any.Protocol)
  expectFalse(isAnyProtocol(Int.self))
  expectFalse(isType(Int.self, to: Any.self))

  expectTrue(C.self is Any.Type)
  expectNotNil(C.self as? Any.Type)
  expectTrue(isAnyType(C.self))
  expectFalse(C.self is Any.Protocol)
  expectNil(C.self as? Any.Protocol)
  expectFalse(isAnyProtocol(C.self))
  expectFalse(isType(C.self, to: Any.self))

  expectTrue(S.self is Any.Type)
  expectNotNil(S.self as? Any.Type)
  expectTrue(isAnyType(S.self))
  expectFalse(S.self is Any.Protocol)
  expectNil(S.self as? Any.Protocol)
  expectFalse(isAnyProtocol(S.self))
  expectFalse(isType(S.self, to: Any.self))

  expectTrue(Any.self is Any.Type)
  expectNotNil(Any.self as? Any.Type)
  expectTrue(isAnyType(Any.self))
  expectTrue(Any.self is Any.Protocol)
  expectNotNil(Any.self as? Any.Protocol)
  expectTrue(isAnyProtocol(Any.self))
  expectTrue(isType(Any.self, to: Any.self))

  expectTrue(Any?.self is Any.Type)
  expectNotNil(Any?.self as? Any.Type)
  expectTrue(isAnyType(Any?.self))
  expectFalse(Any?.self is Any.Protocol)
  expectNil(Any?.self as? Any.Protocol)
  expectFalse(isAnyProtocol(Any?.self))
  expectFalse(isType(Any?.self, to: Any.self))
}

CastsTests.test("Async function types") {
  let asyncFnType: Any.Type = (() async -> Void).self
  let fnType: Any.Type = (() -> Void).self

  expectTrue(fnType is (() -> Void).Type)
  expectTrue(asyncFnType is (() async -> Void).Type)
  expectFalse(fnType is (() async -> Void).Type)
  expectFalse(asyncFnType is (() -> Void).Type)
}

// `Optional<Int>` is Hashable, so it must cast to AnyHashable,
// even if it contains a nil.  (This was broken in 5.3 and earlier,
// but was fixed by the new dynamic cast runtime.)
CastsTests.test("Optional nil -> AnyHashable") {
  let a : Int? = nil
  expectNotNil(a as? AnyHashable)
}

#if _runtime(_ObjC)
// See below for notes about missing Linux functionality
// that prevents us from running this test there.
CastsTests.test("AnyObject.Type -> AnyObject") {
  class C {}
  let a = C.self
  let b = a as? AnyObject.Type
  expectNotNil(b)
  // Note: On macOS, the following cast generates a call to
  // `swift_dynamicCastMetatypeToObjectConditional` That function is currently
  // unimplemented on Linux, so this cast always fails on Linux.
  let c = b as? AnyObject
  expectNotNil(c)
  // Note: The following cast currently succeeds on Linux only by stuffing the
  // source into a `__SwiftValue` container, which breaks the checks below.
  let d = runtimeCast(b, to: AnyObject.self)
  expectNotNil(d)
  let e = c as? C.Type
  expectNotNil(e)
  let f = runtimeCast(d, to: C.Type.self)
  expectNotNil(f)
  // Verify that the round-trip casts yield exactly the same pointer.  In
  // particular, none of the casts above should fall back on stuffing the source
  // into a `__SwiftValue` container.
  expectTrue(c! === a)
  expectTrue(d! === a)
  expectTrue(e! === a)
  expectTrue(f! === a)
}
#endif

// https://github.com/apple/swift/issues/56209
protocol Fruit {}
CastsTests.test("Generic type validation") {
  func check<A, B>(a: A.Type, b: B.Type) -> Bool {
    return (a is B.Type)
  }
  struct Apple: Fruit {}
  expectFalse(check(a: Apple.self, b: Fruit.self))
  expectFalse(Apple.self is Fruit.Protocol)
  expectTrue(Apple.self is Fruit.Type)
}

// https://github.com/apple/swift/issues/48829
protocol A {}
CastsTests.test("Cast from Any to Optional<Protocol>") {
  struct B: A {}

  // If we have an optional instance, stored as an `Any`
  let b: A? = B()
  let c = b as Any

  // This fails to cast, should succeed.
  let d = c as? A
  expectNotNil(d)

  // There is a workaround, but not ideal.
  func cast<T, U>(_ t: T, to: U.Type) -> U? {
    return t as? U
  }
  let f = cast(c, to: Any?.self) as? A
  expectNotNil(f)
}

protocol SuperProtocol{}
CastsTests.test("Casting Objects retained from KeyPaths to Protocols is not working properly") {
  // This is the simplified reproduction from rdar://59844232 which doesn't
  // actually use KeyPaths
  class SubClass : SuperProtocol{}
  let value = SubClass() as Any? as Any

  expectNotNil(value as? SubClass)
  expectNotNil(value as? SuperProtocol)
}

// https://github.com/apple/swift/issues/54462
// FIXME: Known to still be broken, but we can document the issue here.
#if _runtime(_ObjC)
public protocol SomeProtocol {}
extension NSString: SomeProtocol {}
CastsTests.test("NSDictionary -> Dictionary casting") {
  // Create NSDictionary with one entry
  var a = NSMutableDictionary()
  a[NSString("key")] = NSString("value")

  let v = NSString("value")
  let v2 = v as? SomeProtocol
  expectNotNil(v2)

  // Test casting of the dictionary
  let b = a as? [String:SomeProtocol]
  expectFailure { expectNotNil(b) } // Expect non-nil, but see nil
  let c = a as? [String:Any]
  expectNotNil(c)  // Non-nil (as expected)
  let d = c as? [String:SomeProtocol]
  expectNotNil(d) // Non-nil (as expected)
}
#endif

// Casting optionals to AnyHashable is a little peculiar
// TODO: It would be nice if AnyHashable(Optional("Foo")) == AnyHashable("Foo")
// (including as dictionary keys).  That would make this a lot less confusing.
CastsTests.test("Optional cast to AnyHashable") {
  let d: [String?: String] = ["FooKey": "FooValue", nil: "NilValue"]
  // In Swift 5.3, this cast DOES unwrap the non-nil key
  // We've deliberately tried to preserve that behavior in Swift 5.4
  let d2 = d as [AnyHashable: String]

  // After https://github.com/apple/swift/issues/51550 all four of the following
  // should work:
  let d3 = d2["FooKey" as String? as AnyHashable]
  expectNil(d3)
  let d4 = d2["FooKey" as String?]
  expectNil(d4)
  let d5 = d2["FooKey"]
  expectNotNil(d5)
  let d6 = d2["FooKey" as AnyHashable]
  expectNotNil(d6)

  // The nil key should be preserved and still function
  let d7 = d2[String?.none as AnyHashable]
  expectNotNil(d7)

  // Direct casts via the runtime unwrap the optional
  let a: String = "Foo"
  let ah: AnyHashable = a
  let b: String? = a
  let bh = runtimeCast(b, to: AnyHashable.self)
  expectEqual(bh, ah)

  // Direct casts that don't go through the runtime don't unwrap the optional
  // This is inconsistent with the runtime cast behavior above.  We should
  // probably change the runtime behavior above to work the same as this,
  // but that should wait until https://github.com/apple/swift/issues/51550
  // lands.
  let x: String = "Baz"
  let xh = x as AnyHashable
  let y: String? = x
  let yh = y as AnyHashable // Doesn't unwrap the optional
  // xh is AnyHashable("Baz")
  // yh is AnyHashable(Optional("Baz"))
  expectNotEqual(xh, yh)
}

// Repeatedly casting to AnyHashable should still test equal.
// (This was broken for a while because repeatedly casting to
// AnyHashable could end up with multiple nested AnyHashables.)
// rdar://75180619
CastsTests.test("Recursive AnyHashable") {
  struct P: Hashable {
    var x: Int
  }
  struct S {
    var x: AnyHashable?
    init<T: Hashable>(_ x: T?) {
      self.x = x
    }
  }
  let p = P(x: 0)
  let hp = p as AnyHashable?
  print(hp.debugDescription)
  let s = S(hp)
  print(s.x.debugDescription)
  expectEqual(s.x, hp)
  expectEqual(s.x, p)
}

// rdar://78224322
// https://github.com/apple/swift/issues/56987
#if _runtime(_ObjC)
CastsTests.test("Do not overuse __SwiftValue")
.skip(.custom({
  if #available(SwiftStdlib 5.9, *) { return false } else { return true }
}, reason: "Requires stdlib from Swift 5.9 or later"))
.code {
  struct Bar {}
  // This used to succeed because of overeager __SwiftValue
  // boxing (and __SwiftValue does satisfy NSCopying)
  expectFalse(Bar() is NSCopying)
  expectNil(runtimeCast(Bar(), to: NSCopying.self))
  expectFalse(Bar() as Any is NSCopying)
  expectNil(runtimeCast(Bar() as Any, to: NSCopying.self))

  // `Bar() as! AnyObject` gets boxed as a __SwiftValue.
  // __SwiftValue does conform to NSCopying, but that should
  // not be visible here.
  let anyBar = Bar() as! AnyObject
  expectNil(runtimeCast(anyBar, to: NSCopying.self))
  expectFalse(anyBar is NSCopying)

  class Foo {}
  // Foo does not conform to NSCopying
  // (This used to succeed due to over-eager __SwiftValue boxing)
  expectFalse(Foo() is NSCopying)
  expectNil(runtimeCast(Foo(), to: NSCopying.self))
  expectFalse(Foo() as Any is NSCopying)
  expectNil(runtimeCast(Foo() as Any, to: NSCopying.self))

  // A type that really does conform should cast to NSCopying
  class Foo2: NSCopying {
    func copy(with: NSZone?) -> Any { return self }
  }
  expectTrue(Foo2() is NSCopying)
  expectNotNil(runtimeCast(Foo2(), to: NSCopying.self))
  expectTrue(Foo2() is AnyObject)
  expectNotNil(runtimeCast(Foo2(), to: AnyObject.self))
}
#endif

#if _runtime(_ObjC)
CastsTests.test("Artificial subclass protocol conformance") {
  class SwiftClass: NSObject {}
  let subclass: AnyClass = objc_allocateClassPair(SwiftClass.self,
                                                  "ArtificialSwiftSubclass", 0)!
  objc_registerClassPair(subclass)
  expectFalse(subclass is P.Type)
}
#endif

CastsTests.test("Do not overuse __SwiftValue (non-ObjC)") {
  struct Bar {}
  // This should succeed because this is what __SwiftValue boxing is for
  expectTrue(Bar() is AnyObject)
  expectTrue(Bar() as Any is AnyObject)
  let a = Bar() as Any as! AnyObject
  expectTrue(a is Bar)

  class Foo {}
  // Any class type can be cast to AnyObject
  expectTrue(Foo() is AnyObject)
  let b = Foo() as! AnyObject
  expectTrue(b is Foo)

  // As above, but force use of runtime casting
  expectNotNil(runtimeCast(Bar(), to: AnyObject.self))
  expectNotNil(runtimeCast(Bar() as Any, to: AnyObject.self))
  expectNotNil(runtimeCast(a, to: Bar.self))
  expectNotNil(runtimeCast(Foo(), to: AnyObject.self))
  expectNotNil(runtimeCast(b, to: Foo.self))
}

CastsTests.test("Don't put AnyHashable inside AnyObject") {
  class C: Hashable {
    func hash(into hasher: inout Hasher) {}
    static func ==(lhs: C, rhs: C) -> Bool { true }
  }
  let a = C()
  let b = AnyHashable(a)
  let c = a as! AnyObject
  expectTrue(a === c)
  let d = c as! C
  expectTrue(a === d)
}

#if _runtime(_ObjC)
// We currently (as of Jan 2024) bridge NSSet to Swift with `x as!
// Set<NSObject>`, which in turn demands that __SwiftValue successfully cast to
// NSObject.
// So this nonsensical behavior can probably never be fixed.
// (It's nonsense because it implies that every Swift object is derived
// from NSObject.)  See PR #68952 for an early attempt to change it which
// had to be reverted.
CastsTests.test("__SwiftValue should not be obvious to `is`")
.xfail(.always("Probably can never be fixed"))
.code {
  struct S {}
  let s = S() as AnyObject
  expectFalse(s is NSObject)
}
#endif

// See above for reasons why this might need to remain broken forever,
// though I do have some hope for it.
CastsTests.test("type(of:) should look through __SwiftValue")
.xfail(.always("Known to be broken"))
.code {
  struct S {}
  let s = S() as AnyObject
  let t = "\(type(of: s))"
  expectEqual(t, "S")  // Fails: currently says `__SwiftValue`
}

#if _runtime(_ObjC)
@objc protocol P106973771 {
  func sayHello()
}
@objc protocol P106973771a { }
extension NSObject : P106973771a {}
CastsTests.test("Class metatype values should not cast to Obj-C existentials") {
  @objc class C106973771: NSObject, P106973771 {
    func sayHello() { print("Hello") }
  }

  // A class instance clearly conforms to this protocol
  expectTrue(C106973771() is any P106973771)
  expectNotNil(runtimeCast(C106973771(), to: (any P106973771).self))
  // But the metatype definitely does not
  expectFalse(C106973771.self is any P106973771)
  expectNil(C106973771.self as? any P106973771)
  expectNil(runtimeCast(C106973771.self, to: (any P106973771).self))
  // The following will crash if the cast succeeds
  (C106973771.self as? any P106973771)?.sayHello()

  // We should also follow ObjC rules for
  // ObjC metatypes:
  expectTrue(C106973771.self is any NSObjectProtocol)
  expectNotNil(runtimeCast(C106973771.self, to: (any NSObjectProtocol).self))
  expectTrue(C106973771.self is NSObject.Type)
  expectNotNil(runtimeCast(C106973771.self, to: NSObject.Type.self))
  // FIXME: Optimizer breaks this in -O and -Onone builds
  // expectTrue(C106973771.self is NSObject)
  // FIXME: Optimizer breaks this in -O builds
  // expectNotNil(runtimeCast(C106973771.self, to: NSObject.self))
  expectTrue(C106973771.self as AnyObject is any P106973771a)
  expectNotNil(runtimeCast(C106973771.self as AnyObject, to: (any P106973771a).self))
  expectTrue(C106973771.self is any P106973771a)
  expectNotNil(runtimeCast(C106973771.self, to: (any P106973771a).self))
}
#endif

runAllTests()
