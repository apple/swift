// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/src)
// RUN: %empty-directory(%t/sdk)
// RUN: split-file %s %t/src

// RUN: %target-swift-frontend -emit-module %t/src/PublicModule.swift \
// RUN:   -module-name PublicModule -swift-version 5 \
// RUN:   -emit-module-path %t/sdk/PublicModule.swiftmodule

// RUN: %target-swift-frontend -typecheck %t/src/Client.swift \
// RUN:   -module-name Client -I %t/sdk

//--- PublicModule.swift
public struct Test<T> {
  private var _x: T

  public var x: T {
    @storageRestrictions(initializes: _x)
    init {
      _x = newValue
    }

    get { _x }
    set { _x = newValue }
  }

  public init(data: T) {
    self.x = data
  }
}

public struct TestMulti<T, U> {
  private var _a: T
  private var _c: U

  public var b: Int

  public var data: (T, U) {
    @storageRestrictions(initializes: _a, _c, accesses: b)
    init {
      _a = newValue.0
      _c = newValue.1
      b = 0
    }

    get { (_a, _c) }
  }

  public init(data: (T, U), b: Int) {
    self.b = b
    self.data = data
  }
}

//--- Client.swift
import PublicModule

let test1 = Test<[Int]>(data: [1, 2, 3])
_ = test1.x

let test2 = TestMulti(data: ("Question", 42), b: -1)
_ = print("\(test2.data), \(test2.b)")
