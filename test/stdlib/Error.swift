// RUN: %empty-directory(%t)
// RUN: %target-build-swift -o %t/Error -DPTR_SIZE_%target-ptrsize -module-name main %/s
// RUN: %target-codesign %t/Error
// RUN: %target-run %t/Error
// REQUIRES: executable_test
// REQUIRES: reflection

import StdlibUnittest

func shouldCheckErrorLocation() -> Bool {
  // Location information for runtime traps is only emitted in debug builds.
  guard _isDebugAssertConfiguration() else { return false }
  // The runtime error location format changed after the 5.3 release.
  // (https://github.com/apple/swift/pull/34665)
  if #available(macOS 11.3, iOS 14.5, tvOS 14.5, watchOS 7.4, *) {
    return true
  } else {
    return false
  }
}

var ErrorTests = TestSuite("Error")

var NoisyErrorLifeCount = 0
var NoisyErrorDeathCount = 0

protocol OtherProtocol {
  var otherProperty: String { get }
}

protocol OtherClassProtocol : AnyObject {
  var otherClassProperty: String { get }
}

class NoisyError : Error, OtherProtocol, OtherClassProtocol {
  init() { NoisyErrorLifeCount += 1 }
  deinit { NoisyErrorDeathCount += 1 }

  let _domain = "NoisyError"
  let _code = 123

  let otherProperty = "otherProperty"
  let otherClassProperty = "otherClassProperty"
}

ErrorTests.test("erasure") {
  NoisyErrorLifeCount = 0
  NoisyErrorDeathCount = 0
  do {
    let e: Error = NoisyError()

    expectEqual(e._domain, "NoisyError")
    expectEqual(e._code, 123)
  }
  expectEqual(NoisyErrorDeathCount, NoisyErrorLifeCount)
}

ErrorTests.test("reflection") {
  NoisyErrorLifeCount = 0
  NoisyErrorDeathCount = 0
  do {
    let ne = NoisyError()
    let e: Error = ne

    var neDump = "", eDump = ""
    dump(ne, to: &neDump)
    dump(e, to: &eDump)

    expectEqual(eDump, neDump)
  }
  expectEqual(NoisyErrorDeathCount, NoisyErrorLifeCount)
}

ErrorTests.test("dynamic casts") {
  NoisyErrorLifeCount = 0
  NoisyErrorDeathCount = 0
  do {
    let ne = NoisyError()
    let e: Error = ne

    expectTrue(e as! NoisyError === ne)
    expectEqual((e as! OtherClassProtocol).otherClassProperty, "otherClassProperty")
    expectEqual((e as! OtherProtocol).otherProperty, "otherProperty")

    let op: OtherProtocol = ne
    expectEqual((op as! Error)._domain, "NoisyError")
    expectEqual((op as! Error)._code, 123)

    let ocp: OtherClassProtocol = ne
    expectEqual((ocp as! Error)._domain, "NoisyError")
    expectEqual((ocp as! Error)._code, 123)

    // Do the same with rvalues, so we exercise the
    // take-on-success/destroy-on-failure paths.

    expectEqual(((NoisyError() as Error) as! NoisyError)._domain, "NoisyError")
    expectEqual(((NoisyError() as Error) as! OtherClassProtocol).otherClassProperty, "otherClassProperty")
    expectEqual(((NoisyError() as Error) as! OtherProtocol).otherProperty, "otherProperty")

    expectEqual(((NoisyError() as OtherProtocol) as! Error)._domain, "NoisyError")
    expectEqual(((NoisyError() as OtherProtocol) as! Error)._code, 123)

    expectEqual(((NoisyError() as OtherClassProtocol) as! Error)._domain, "NoisyError")
    expectEqual(((NoisyError() as OtherClassProtocol) as! Error)._code, 123)
  }
  expectEqual(NoisyErrorDeathCount, NoisyErrorLifeCount)
}

struct DefaultStruct : Error { }
class DefaultClass : Error { }

ErrorTests.test("default domain and code") {
  expectEqual(DefaultStruct()._domain, "main.DefaultStruct")
  expectEqual(DefaultStruct()._code, 1)
  expectEqual(DefaultClass()._domain, "main.DefaultClass")
  expectEqual(DefaultClass()._code, 1)
}

enum SillyError: Error { case JazzHands }

#if !os(WASI)
// Trap tests aren't available on WASI.
ErrorTests.test("try!")
  .skip(.custom({ _isFastAssertConfiguration() },
                reason: "trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches(shouldCheckErrorLocation()
                        ? "'try!' expression unexpectedly raised an error: "
                          + "main.SillyError.JazzHands"
                        : "")
  .code {
    expectCrashLater()
    let _: () = try! { throw SillyError.JazzHands }()
}

ErrorTests.test("try!/location")
  .skip(.custom({ _isFastAssertConfiguration() },
                reason: "trap is not guaranteed to happen in -Ounchecked"))
  .crashOutputMatches(shouldCheckErrorLocation()
                        ? "main/Error.swift:\(#line + 4)"
                        : "")
  .code {
    expectCrashLater()
    let _: () = try! { throw SillyError.JazzHands }()
}
#endif

ErrorTests.test("try?") {
  var value = try? { () throws -> Int in return 1 }()
  expectType(Optional<Int>.self, &value)
  expectEqual(Optional(1), value)

  expectNil(try? { () throws -> Int in throw SillyError.JazzHands }())
}

enum LifetimeError : Error {
  case MistakeOfALifetime(LifetimeTracked, yearsIncarcerated: Int)
}

ErrorTests.test("existential in lvalue") {
  expectEqual(0, LifetimeTracked.instances)
  do {
    var e: Error?
    do {
      throw LifetimeError.MistakeOfALifetime(LifetimeTracked(0),
                                             yearsIncarcerated: 25)
    } catch {
      e = error
    }
    expectEqual(1, LifetimeTracked.instances)
    expectEqual(0, e?._code)
  }
  expectEqual(0, LifetimeTracked.instances)
}

enum UnsignedError: UInt, Error {
#if PTR_SIZE_64
case negativeOne = 0xFFFFFFFFFFFFFFFF
#elseif PTR_SIZE_32
case negativeOne = 0xFFFFFFFF
#else
#error ("Unknown pointer size")
#endif
}

ErrorTests.test("unsigned raw value") {
  let negOne: Error = UnsignedError.negativeOne
  expectEqual(-1, negOne._code)
}

ErrorTests.test("test dealloc empty error box") {
  struct Foo<T>: Error { let value: T }

  func makeFoo<T>() throws -> Foo<T> {
    throw Foo(value: "makeFoo throw error")
  }

  func makeError<T>(of: T.Type) throws -> Error {
    return try makeFoo() as Foo<T>
  }

  do {
    _ = try makeError(of: Int.self)
  } catch let foo as Foo<String> {
    expectEqual(foo.value, "makeFoo throw error")
  } catch {
    expectUnreachableCatch(error)
  }
}

#if !os(WASI)
var errors: [Error] = []

@inline(never)
func throwNegativeOne() throws {
  throw UnsignedError.negativeOne
}

@inline(never)
func throwJazzHands() throws {
  throw SillyError.JazzHands
}

// Error isn't allowed in a @convention(c) function when ObjC interop is
// not available, so pass it through an UnsafeRawPointer.
@available(SwiftStdlib 5.8, *)
@_silgen_name("_swift_setWillThrowHandler")
func setWillThrowHandler(
    _ handler: (@convention(c) (UnsafeRawPointer) -> Void)?
)

ErrorTests.test("willThrow") {
  guard #available(SwiftStdlib 5.8, *) else {
    return
  }
  setWillThrowHandler {
    errors.append(unsafeBitCast($0, to: Error.self))
  }
  defer {
    setWillThrowHandler(nil)
  }
  expectTrue(errors.isEmpty)
  do {
    try throwNegativeOne()
  } catch {}
  expectEqual(UnsignedError.self, type(of: errors.last!))

  do {
    try throwJazzHands()
  } catch {}
  expectEqual(2, errors.count)
  expectEqual(SillyError.self, type(of: errors.last!))
}
#endif

runAllTests()

