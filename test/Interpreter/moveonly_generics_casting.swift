// RUN: %target-run-simple-swift(-Xfrontend -sil-verify-all) | %FileCheck %s
// RUN: %target-run-simple-swift(-O -Xfrontend -sil-verify-all) | %FileCheck %s

// REQUIRES: executable_test

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

protocol P {
  func speak()
}

extension P {
  func speak() { print("hello") }
}

struct Noncopyable: ~Copyable {}
struct Ordinary {}

struct Dog<T: ~Copyable>: Copyable {}
extension Dog: P where T: Copyable {}

enum Cat<Left: ~Copyable, Right: ~Copyable>: Copyable {
    case meows
    init() { self = .meows }
}
extension Cat: P where Left: Copyable, Right: ~Copyable {}

struct ConditionallyCopyable<T: ~Copyable>: ~Copyable {
  var value: Int = 17
}
extension ConditionallyCopyable: Copyable where T: Copyable { }

// FIXME: Not yet supported
// struct VariadicCopyable<each T: ~Copyable>: Copyable { }
// extension VariadicCopyable: P where repeat each T: Copyable { }

func attemptCall(_ a: Any) {
  if let value = a as? P {
    value.speak()
    return
  }
  print("failed to cast (attemptCall)")
}

@_silgen_name("swift_getExtendedFunctionTypeMetadata")
func _getExtendedFunctionTypeMetadata(
    flags: UInt, differentiabilityKind: UInt,
    parameterTypes: UnsafePointer<Any.Type>?,
    parameterFlags: UnsafePointer<UInt32>?,
    resultType: Any.Type, globalActorType: Any.Type? = nil,
    extendedFlags: UInt32, thrownErrorType: Any.Type? = nil) -> Any.Type


defer { main() }
func main() {
  // CHECK: hello
  attemptCall(Dog<Ordinary>())

  // CHECK: failed to cast (attemptCall)
  attemptCall(Dog<Noncopyable>())

  // CHECK: failed to cast (attemptCall)
  attemptCall(Dog<ConditionallyCopyable<Noncopyable>>())

  // CHECK: hello
  attemptCall(Cat<Ordinary, Noncopyable>())

  // CHECK: failed to cast (attemptCall)
  attemptCall(Cat<Noncopyable, Ordinary>())

   // CHECK: failed to cast (attemptCall)
  attemptCall(Cat<Noncopyable, ConditionallyCopyable<Ordinary>>())

  // CHECK-FIXME: hello
  // attemptCall(VariadicCopyable<Ordinary, ConditionallyCopyable<Ordinary>>())

  // CHECK-FIXME: failed to cast (attemptCall)
  // attemptCall(VariadicCopyable<Ordinary, ConditionallyCopyable<Noncopyable>>())

  // CHECK: tuple types
  print("tuple types")

  // CHECK: hello
  attemptCall(Dog<(Ordinary, Ordinary)>())

  // CHECK-FIXME: failed to cast (attemptCall)
  // FIXME: Requires the ability to create such tuple types
  // attemptCall(Dog<(Ordinary, Noncopyable)>())

  // CHECK: metatype types
  print("metatype types")

  // CHECK: hello
  attemptCall(Dog<Ordinary.Type>())

  // CHECK: hello
  attemptCall(Dog<Noncopyable.Type>())

  // CHECK: function types
  print("function types")

  attemptCall(Dog<(Ordinary) -> Noncopyable>())

  // This is a nonmovable function type, which cannot currently be
  // expressed in the language.
  let noncopyableFnType = _getExtendedFunctionTypeMetadata(
    flags: 0x04000000 | 0x80000000,
    differentiabilityKind: 0,
    parameterTypes: nil,
    parameterFlags: nil,
    resultType: Void.self,
    extendedFlags: 0x1 << 16)

  // CHECK: failed to cast (attemptCall)
  func doFuncCall<F>(_: F.Type) {
    attemptCall(Dog<F>())
  }
  _openExistential(noncopyableFnType, do: doFuncCall)

  // CHECK: existential types
  print("existential types")

  // CHECK: hello
  attemptCall(Dog<Any>())

  // CHECK-FIXME: failed to cast (attemptCall)
  // FIXME crashes: attemptCall(Dog<any ~Copyable>())

  // CHECK: cast succeeded
  test_radar124171788(.nothing)
}

// coverage for rdar://124171788
enum Maybe<Wrapped: ~Copyable>: ~Copyable {
  case just(Wrapped)
  case nothing
}
extension Maybe: Copyable where Wrapped: Copyable {}
extension Maybe: CustomDebugStringConvertible {
  var debugDescription: String {
    "cast succeeded"
  }
}
func test_radar124171788(_ v: Maybe<Int>) {
  if let foo = v as? CustomDebugStringConvertible {
    print("\(foo.debugDescription)")
    return
  }
  print("failed to cast (test_radar124171788)")
}
