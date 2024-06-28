// RUN: %target-typecheck-verify-swift -disable-availability-checking -enable-experimental-feature NonescapableTypes -enable-builtin-module
// REQUIRES: asserts

import Builtin

struct BufferView : ~Escapable {
  let ptr: UnsafeRawBufferPointer
  init(_ ptr: UnsafeRawBufferPointer) -> dependsOn(ptr) Self {
    self.ptr = ptr
  }
  // TODO:  -> dependsOn(ptr) Self
  @_unsafeNonescapableResult
  init?(_ ptr: UnsafeRawBufferPointer, _ i: Int) {
    if (i % 2 == 0) {
      return nil
    } 
    self.ptr = ptr
  }
  init(_ ptr: UnsafeRawBufferPointer, _ a: borrowing Array<Int>) -> dependsOn(a) Self {
    self.ptr = ptr
    return self
  }
  init(_ ptr: UnsafeRawBufferPointer, _ a: consuming Wrapper) -> dependsOn(a) Self {
    self.ptr = ptr
    return self
  }
  init(_ ptr: UnsafeRawBufferPointer, _ a: consuming Wrapper, b: borrowing Array<Int>) -> dependsOn(a) dependsOn(scoped b) Self {
    self.ptr = ptr
    return self
  }
  init(_ ptr: UnsafeRawBufferPointer, _ a: borrowing Array<Double>, b: borrowing Array<Int>, c: Double) -> dependsOn(scoped a) dependsOn(b) Int { // expected-error{{expected Self return type for initializers with lifetime dependence specifiers}}
    self.ptr = ptr
    return 0
  }
  /* TODO: Enable this test once Optional is ~Escapable
  init?(_ ptr: UnsafeRawBufferPointer, _ a: borrowing Array<Int>, _ i: Int) -> dependsOn(a) Self? {
    if (i % 2 == 0) {
      self.ptr = ptr
      return self
    }
    return nil
  }
  */
}

struct MutableBufferView : ~Escapable, ~Copyable {
  let ptr: UnsafeMutableRawBufferPointer
  init(_ ptr: UnsafeMutableRawBufferPointer) -> dependsOn(ptr) Self {
    self.ptr = ptr
  }
}

func testBasic() {
  let capacity = 4
  let a = Array(0..<capacity)
  a.withUnsafeBytes {
    let view = BufferView($0)
    let derivedView = derive(view)
    let newView = consumeAndCreate(derivedView)
    use(newView)    
  }
}

func derive(_ x: borrowing BufferView) -> dependsOn(x) BufferView {
  return BufferView(x.ptr)
}

func consumeAndCreate(_ x: consuming BufferView) -> dependsOn(x) BufferView {
  return BufferView(x.ptr)
}

func use(_ x: borrowing BufferView) {}

func deriveMultiView1(_ x: borrowing BufferView, _ y: borrowing BufferView) -> dependsOn(x, y) BufferView {
  if (Int.random(in: 1..<100) % 2 == 0) {
    return BufferView(x.ptr)
  }
  return BufferView(y.ptr)
}

func deriveMultiView2(_ x: borrowing BufferView, _ y: borrowing BufferView) -> dependsOn(x) dependsOn(y) BufferView {
  if (Int.random(in: 1..<100) % 2 == 0) {
    return BufferView(x.ptr)
  }
  return BufferView(y.ptr)
}

func consumeAndCreateMultiView1(_ x: consuming BufferView, _ y: consuming BufferView) -> dependsOn(x, y) BufferView {
  if (Int.random(in: 1..<100) % 2 == 0) {
    return BufferView(x.ptr)
  }
  return BufferView(y.ptr)
}

func consumeAndCreateMultiView2(_ x: consuming BufferView, _ y: consuming BufferView) -> dependsOn(x) dependsOn(y) BufferView {
  if (Int.random(in: 1..<100) % 2 == 0) {
    return BufferView(x.ptr)
  }
  return BufferView(y.ptr)
}

func mixedMultiView(_ x: consuming BufferView, _ y: borrowing BufferView) -> dependsOn(x) dependsOn(y) BufferView {
  if (Int.random(in: 1..<100) % 2 == 0) {
    return BufferView(x.ptr)
  }
  return BufferView(y.ptr)
}

func modifiedViewDependsOnInput(_ x: inout MutableBufferView) -> dependsOn(x) MutableBufferView {
  return MutableBufferView(x.ptr)
}

func modifiedViewDependsOnParent(_ x: inout MutableBufferView) -> dependsOn(x) MutableBufferView {
  return MutableBufferView(x.ptr)
}

func invalidSpecifier1(_ x: borrowing BufferView) -> dependsOn BufferView { // expected-error{{expected '(' after lifetime dependence specifier}}
  return BufferView(x.ptr)
}

func invalidSpecifier2(_ x: borrowing BufferView) -> dependsOn() BufferView {// expected-error{{expected identifier, index or self in lifetime dependence specifier}}
  return BufferView(x.ptr)
}

func invalidSpecifier3(_ x: borrowing BufferView) -> dependsOn(*) BufferView { // expected-error{{expected identifier, index or self in lifetime dependence specifier}}
  return BufferView(x.ptr)
} 

// TODO: Diagnose using param indices on func decls in sema
func invalidSpecifier4(_ x: borrowing BufferView) -> dependsOn(self) BufferView { // expected-error{{invalid lifetime dependence specifier on non-existent self}}
  return BufferView(x.ptr)
}

struct Wrapper : ~Escapable {
  let view: BufferView
  init(_ view: consuming BufferView) -> dependsOn(view) Self {
    self.view = view
    return self
  }
  borrowing func getView1() -> dependsOn(self) BufferView {
    return view
  }

  consuming func getView2() -> dependsOn(self) BufferView {
    return view
  }

  mutating func getView3() -> dependsOn(self) BufferView {
    return view
  }

  borrowing func getView4() -> dependsOn(self) BufferView {
    return view
  }
}

enum FakeOptional<Wrapped: ~Escapable>: ~Escapable {
  case none, some(Wrapped)
}

extension FakeOptional: Escapable where Wrapped: Escapable {}

extension FakeOptional where Wrapped: ~Escapable {
  init(nilLiteral: ()) -> dependsOn(immortal) Self {
    self = .none
  }
}
