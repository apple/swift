// RUN: %target-run-simple-swift(-Xfrontend -enable-experimental-concurrency -parse-as-library %import-libdispatch)

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: libdispatch

// rdar://76038845
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

@available(SwiftStdlib 5.5, *)
enum TL {
  @TaskLocal
  static var number: Int = 0
}

@available(SwiftStdlib 5.5, *)
@discardableResult
func printTaskLocal<V>(
    _ key: TaskLocal<V>,
    _ expected: V? = nil,
    file: String = #file, line: UInt = #line
) -> V? {
  let value = key.get()
  print("\(key) (\(value)) at \(file):\(line)")
  if let expected = expected {
    assert("\(expected)" == "\(value)",
        "Expected [\(expected)] but found: \(value), at \(file):\(line)")
  }
  return expected
}

// ==== ------------------------------------------------------------------------

@available(SwiftStdlib 5.5, *)
func async_let_nested() async {
  print("TL: \(TL.$number)")
  
  _ = printTaskLocal(TL.$number) // CHECK: TaskLocal<Int>(defaultValue: 0) (0)
  async let x1: () = TL.$number.withValue(2) {
    async let x2 = printTaskLocal(TL.$number) // CHECK: TaskLocal<Int>(defaultValue: 0) (2)

    @Sendable
    func test() async {
      printTaskLocal(TL.$number) // CHECK: TaskLocal<Int>(defaultValue: 0) (2)
      async let x31 = printTaskLocal(TL.$number) // CHECK: TaskLocal<Int>(defaultValue: 0) (2)
      _ = await x31
    }
    async let x3: () = test()

    _ = await x2
    await x3
  }

  _ = await x1
  printTaskLocal(TL.$number) // CHECK: TaskLocal<Int>(defaultValue: 0) (0)
}

@available(SwiftStdlib 5.5, *)
func async_let_nested_skip_optimization() async {
  async let x1: Int? = TL.$number.withValue(2) {
    async let x2: Int? = { () async -> Int? in
      async let x3: Int? = { () async -> Int? in
        async let x4: Int? = { () async -> Int? in
          async let x5: Int? = { () async -> Int? in
            // assert(TL.$number == 2)
            async let xx = printTaskLocal(TL.$number) // CHECK: TaskLocal<Int>(defaultValue: 0) (2)
            return await xx
          }()
          return await x5
        }()
        return await x4
      }()
      return await x3
    }()
    return await x2
  }

  _ = await x1
}

@available(SwiftStdlib 5.5, *)
@main struct Main {
  static func main() async {
    await async_let_nested()
    await async_let_nested_skip_optimization()
  }
}
