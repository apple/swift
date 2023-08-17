// RUN: %target-typecheck-verify-swift

struct Value {
  var data: String
}

func testAnyKeyPath<T>(field: AnyKeyPath, _: UnsafePointer<T>) {}
func testPartialKeyPath<T>(field: PartialKeyPath<T>, _: UnsafePointer<T>) {}

struct Test {
  func test(ptr: UnsafePointer<Int64>) {
    ptr.withMemoryRebound(to: Value.self, capacity: 2) { newPtr in
      for idx in 0 ..< 2 {
        var elt = newPtr[idx]
        testAnyKeyPath(field: \Value.data, &elt)
        testPartialKeyPath(field: \Value.data, &elt)
      }
    }
  }
}
