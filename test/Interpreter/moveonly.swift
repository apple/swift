// RUN: %target-run-simple-swift(-O -Xfrontend -sil-verify-all)
// this test takes a long time, so run this once with optimizations on.

// REQUIRES: executable_test

import StdlibUnittest

defer { runAllTests() }

var Tests = TestSuite("MoveOnlyTests")

@_moveOnly
struct FD {
  var a = LifetimeTracked(0)

  deinit {
  }
}

Tests.test("simple deinit called once") {
  do {
    let s = FD()
  }
  expectEqual(0, LifetimeTracked.instances)
}

Tests.test("ref element addr destroyed once") {
  class CopyableKlass {
    var fd = FD()
  }

  func assignCopyableKlass(_ x: CopyableKlass) {
    x.fd = FD()
  }

  do {
    let x = CopyableKlass()
    assignCopyableKlass(x)
  }
  expectEqual(0, LifetimeTracked.instances)
}

var global = FD()

Tests.test("global destroyed once") {
  do {
    global = FD()
  }
  expectEqual(0, LifetimeTracked.instances)
}

@_moveOnly
struct FD2 {
  var field = 5
  static var count = 0
  init() { FD2.count += 1 }
  deinit {
    FD2.count -= 1
    print("In deinit!")
  }
  func use() {}
}

Tests.test("deinit not called in init when assigned") {
  class FDHaver {
    var fd: FD2

    init() {
      self.fd = FD2()
    }
  }

  class FDHaver2 {
    var fd: FD2

    init() {
      self.fd = FD2()
      self.fd = FD2()
      self.fd = FD2()
      self.fd.use()
    }
  }

  do {
    let haver = FDHaver()
    let _ = haver
  }
  do {
    let haver = FDHaver2()
    let _ = haver
  }
  expectEqual(0, FD2.count)
}

Tests.test("empty struct") {
  @_moveOnly
  struct EmptyStruct {
    func doSomething() {}
    var value: Bool { false }
  }

  let e = EmptyStruct()
  e.doSomething()
  if e.value {
    let _ = consume e
  }
}

protocol P {
   var name: String { get }
}

Tests.test("AddressOnly") {
    class Klass : P {
        var name: String { "myName" }
    }

    @_moveOnly
    struct S<T : P> {
        var t: T
    }

    let e = S(t: Klass())
    expectEqual(e.t.name, "myName")

    func testGeneric<T : P>(_ x: borrowing S<T>) {
        expectEqual(x.t.name, "myName")
    }
    testGeneric(e)

    if e.t.name.count == 5 {
        let _ = consume e
    }
}

