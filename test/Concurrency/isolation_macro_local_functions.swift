// RUN: %target-swift-frontend -emit-silgen %s -disable-availability-checking

public func hasIsolatedParam<T>(isolation: isolated (any Actor)? = #isolation) async -> T {}

func callee<T>(_: @autoclosure () -> T, _: @autoclosure () -> T) {}

func outer() async {
  func inner() async -> String {
    let x = #isolation
    return await hasIsolatedParam()
  }

  var value = await inner()
  callee(value, "hi")
}
