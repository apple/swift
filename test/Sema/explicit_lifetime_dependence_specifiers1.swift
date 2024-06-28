// RUN: %target-typecheck-verify-swift -disable-availability-checking -enable-experimental-feature NonescapableTypes
// REQUIRES: asserts

struct Container {
  let ptr: UnsafeRawBufferPointer
}

struct AnotherBufferView : ~Escapable {
  let ptr: UnsafeRawBufferPointer
  init(_ ptr: UnsafeRawBufferPointer) -> dependsOn(ptr) Self {
    self.ptr = ptr
  }
}

struct BufferView : ~Escapable {
  let ptr: UnsafeRawBufferPointer
  init(_ ptr: UnsafeRawBufferPointer) -> dependsOn(ptr) Self {
    self.ptr = ptr
  }
  @_unsafeNonescapableResult
  init(independent ptr: UnsafeRawBufferPointer) {
    self.ptr = ptr
  }
  init(_ ptr: UnsafeRawBufferPointer, _ arr: borrowing Array<Int>) -> dependsOn(arr) Self {
    self.ptr = ptr
    return self
  }
  init(_ ptr: UnsafeRawBufferPointer, _ arr: borrowing Array<Double>) -> dependsOn(arr) Self {
    self.ptr = ptr
  }
  // TODO: Once Optional is ~Escapable, the error will go away
  init?(_ ptr: UnsafeRawBufferPointer, _ arr: borrowing Array<Float>) -> dependsOn(arr) Self? { // expected-error{{lifetime dependence can only be specified on ~Escapable results}}
    if (Int.random(in: 1..<100) == 0) {
      return nil
    }
    self.ptr = ptr
  }
  init?(_ ptr: UnsafeRawBufferPointer, _ arr: borrowing Array<String>) -> dependsOn(arr) Self? { // expected-error{{lifetime dependence can only be specified on ~Escapable results}}
    if (Int.random(in: 1..<100) == 0) {
      return nil
    }
    self.ptr = ptr
    return self
  }
  init(_ ptr: UnsafeRawBufferPointer, _ arr: borrowing Array<Character>) -> dependsOn(self) Self { // expected-error{{invalid lifetime dependence on self in an initializer}}
    self.ptr = ptr
  }
  init(_ ptr: UnsafeRawBufferPointer, _ arr: Array<Bool>) -> dependsOn(scoped arr) Self { // expected-error{{invalid use of scoped lifetime dependence with consuming ownership}}
    self.ptr = ptr
  }
  init(_ ptr: UnsafeRawBufferPointer, _ arr: Array<UInt>) -> dependsOn(arr) Self { // expected-error{{invalid use of lifetime dependence on an Escapable parameter with consuming ownership}}
    self.ptr = ptr
  }
  init(_ ptr: UnsafeRawBufferPointer, _ abv: AnotherBufferView) -> dependsOn(abv) Self {
    self.ptr = ptr
  }

  consuming func consume() -> dependsOn(scoped self) BufferView {
    return BufferView(independent: self.ptr)
  }

  func get() -> dependsOn(self) Self { // expected-note{{'get()' previously declared here}}
    return self
  }

  func get() -> dependsOn(scoped self) Self { // expected-error{{invalid redeclaration of 'get()'}}
    return self
  }
}

struct MutableBufferView : ~Escapable, ~Copyable {
  let ptr: UnsafeMutableRawBufferPointer
  init(_ ptr: UnsafeMutableRawBufferPointer) -> dependsOn(ptr) Self {
    self.ptr = ptr
  }
}

class Klass {}

struct WrapperStruct {
  let k: Klass
}

func invalidLifetimeDependenceOnEscapableResult(_ w: borrowing WrapperStruct) -> dependsOn(w) Klass { // expected-error{{lifetime dependence can only be specified on ~Escapable results}}
  return w.k
}

func incorrectSelfInvalidLifetimeDependence(_ x: borrowing BufferView) -> dependsOn(self) BufferView { // expected-error{{invalid lifetime dependence specifier on non-existent self}}
  return BufferView(x.ptr)
}

func incorrectParamNameInvalidLifetimeDependence(_ x: borrowing BufferView) -> dependsOn(y) BufferView { // expected-error{{invalid parameter name specified 'y'}}
  return BufferView(x.ptr)
}

func duplicateParamInvalidLifetimeDependence1(_ x: borrowing BufferView) -> dependsOn(x, x) BufferView { // expected-error{{duplicate lifetime dependence specifier}}
  return BufferView(x.ptr)
}

func duplicateParamInvalidLifetimeDependence2(_ x: borrowing BufferView) -> dependsOn(x) dependsOn(x) BufferView { // expected-error{{duplicate lifetime dependence specifier}}
  return BufferView(x.ptr)
}

func duplicateParamInvalidLifetimeDependence3(_ x: borrowing BufferView) -> dependsOn(x) dependsOn(x) BufferView { // expected-error{{duplicate lifetime dependence specifier}}
  return BufferView(x.ptr)
}

func consumingParamInvalidLifetimeDependence1(_ x: consuming BufferView) -> dependsOn(scoped x) BufferView {
  return BufferView(x.ptr)
}

func borrowingParamInvalidLifetimeDependence1(_ x: borrowing BufferView) -> dependsOn(x) BufferView {
  return BufferView(x.ptr)
}

func implicitBorrowingParamLifetimeDependence1(_ x: BufferView) -> dependsOn(x) BufferView {
  return BufferView(x.ptr)
}

func implicitBorrowingParamLifetimeDependence2(_ x: BufferView) -> dependsOn(scoped x) BufferView {
  return BufferView(x.ptr)
}

func inoutParamLifetimeDependence1(_ x: inout BufferView) -> dependsOn(x) BufferView {
  return BufferView(x.ptr)
}

func inoutParamLifetimeDependence2(_ x: inout BufferView) -> dependsOn(scoped x) BufferView {
  return BufferView(x.ptr)
}

func invalidSpecifierPosition1(_ x: borrowing dependsOn(x) BufferView) -> BufferView { // expected-error{{lifetime dependence specifiers may only be used on result of functions, methods, initializers}}
  return BufferView(x.ptr)
}

func invalidSpecifierPosition2(_ x: borrowing BufferView) -> BufferView { 
  let y: dependsOn(x) x // expected-error{{lifetime dependence specifiers may only be used on result of functions, methods, initializers}}
  return BufferView(y.ptr)
}

func invalidTupleLifetimeDependence(_ x: inout BufferView) -> (dependsOn(x) BufferView, BufferView) { // expected-error{{lifetime dependence specifiers cannot be applied to tuple elements}}
  return (BufferView(x.ptr), BufferView(x.ptr))
}

struct Wrapper : ~Escapable {
  let view: BufferView
  init(_ view: consuming BufferView) {
    self.view = view
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

  borrowing func borrowingMethodLifetimeDependence1() -> dependsOn(self) BufferView { 
    return view
  }

  borrowing func borrowingMethodLifetimeDependence2() -> dependsOn(scoped self) BufferView {
    return view
  }

  consuming func consumingMethodLifetimeDependence1() -> dependsOn(self) BufferView {
    return view
  }

  consuming func consumingMethodInvalidLifetimeDependence1() -> dependsOn(scoped self) BufferView {
    return view
  }

  mutating func mutatingMethodLifetimeDependence1() -> dependsOn(self) BufferView {
    return view
  }

  mutating func mutatingMethodLifetimeDependence2() -> dependsOn(scoped self) BufferView {
    return view
  } 
}

public struct GenericBufferView<Element> : ~Escapable {
  public typealias Index = Int
  public typealias Pointer = UnsafePointer<Element>

  public let baseAddress: Pointer
  public let count: Int

  public init<Storage>(unsafeBuffer: UnsafeBufferPointer<Element>,
                       storage: borrowing Storage)
    -> dependsOn(storage) Self {
    let baseAddress = unsafeBuffer.baseAddress!
    self = GenericBufferView<Element>(baseAddress: baseAddress,
                                      count: unsafeBuffer.count)
    return self
  }
  // unsafe private API
  init(baseAddress: Pointer, count: Int) -> dependsOn(baseAddress) Self {
    precondition(count >= 0, "Count must not be negative")
    self.baseAddress = baseAddress
    self.count = count
  } 
}

func derive(_ x: BufferView) -> dependsOn(x) BufferView { // expected-note{{'derive' previously declared here}}
  return BufferView(x.ptr)
}

func derive(_ x: BufferView) -> dependsOn(scoped x) BufferView { // expected-error{{invalid redeclaration of 'derive'}}
  return BufferView(x.ptr)
}

struct RawBufferView {
  let ptr: UnsafeRawBufferPointer
  init(_ ptr: UnsafeRawBufferPointer) {
    self.ptr = ptr
  }
}

extension RawBufferView {
  mutating func zeroBufferView() throws -> BufferView { // expected-error{{cannot infer lifetime dependence on a self which is BitwiseCopyable & Escapable}}
    return BufferView(ptr)
  }
}

func immortalConflict(immortal: UnsafeRawBufferPointer ) -> dependsOn(immortal) BufferView { // expected-error{{conflict between the parameter name and immortal keyword}}
  return BufferView(immortal)
}

