// RUN: %target-swift-frontend -emit-sil -swift-version 6 -disable-availability-checking -verify %s -o /dev/null -parse-as-library

// READ THIS: This test is testing specifically behavior around global actor
// isolated types that are nonsendable. This is a bit of a corner case so we use
// a separate test case from the main global actor test case.

// REQUIRES: concurrency
// REQUIRES: asserts

////////////////////////
// MARK: Declarations //
////////////////////////

class NonSendableKlass {}
final class SendableKlass : Sendable {}

actor CustomActorInstance {}

@globalActor
struct CustomActor {
  static let shared = CustomActorInstance()
}

func transferToNonIsolated<T>(_ t: T) async {}
@MainActor func transferToMainActor<T>(_ t: T) async {}
@CustomActor func transferToCustomActor<T>(_ t: T) async {}
func useValue<T>(_ t: T) {}
func useValueAsync<T>(_ t: T) async {}
@MainActor func useValueMainActor<T>(_ t: T) {}
@MainActor func mainActorFunction() {}

var booleanFlag: Bool { false }
@MainActor var mainActorIsolatedGlobal = NonSendableKlass()
@CustomActor var customActorIsolatedGlobal = NonSendableKlass()

@MainActor
class NonSendableGlobalActorIsolatedKlass {
  var k = NonSendableKlass()
  var p: (any GlobalActorIsolatedProtocol)? = nil
  var p2: OtherProtocol? = nil
}

@available(*, unavailable)
extension NonSendableGlobalActorIsolatedKlass: Sendable {}

@MainActor
final class FinalNonSendableGlobalActorIsolatedKlass {
  var k = NonSendableKlass()
  var p: (any GlobalActorIsolatedProtocol)? = nil
  var p2: OtherProtocol? = nil
}

@available(*, unavailable)
extension FinalNonSendableGlobalActorIsolatedKlass: Sendable {}


@MainActor
struct NonSendableGlobalActorIsolatedStruct {
  var k = NonSendableKlass()
  var p: (any GlobalActorIsolatedProtocol)? = nil
  var p2: OtherProtocol? = nil
}

@available(*, unavailable)
extension NonSendableGlobalActorIsolatedStruct: Sendable {}

@MainActor protocol GlobalActorIsolatedProtocol {
  var k: NonSendableKlass { get }
  var p: GlobalActorIsolatedProtocol { get }
  var p2: OtherProtocol { get }
}

protocol OtherProtocol {
  var k: NonSendableKlass { get }
}

@MainActor
enum NonSendableGlobalActorIsolatedEnum {
  case first
  case second(NonSendableKlass)
  case third(SendableKlass)
  case fourth(GlobalActorIsolatedProtocol)
  case fifth(OtherProtocol)
}

@available(*, unavailable)
extension NonSendableGlobalActorIsolatedEnum: Sendable {}

/////////////////
// MARK: Tests //
/////////////////

extension NonSendableGlobalActorIsolatedStruct {
  mutating func test() {
    _ = self.k
  }

  mutating func test2() -> NonSendableKlass {
    self.k
  }

  mutating func test3() -> sending NonSendableKlass {
    self.k
  } // expected-error {{sending 'self.k' risks causing data races}}
  // expected-note @-1 {{main actor-isolated 'self.k' cannot be a 'sending' result. main actor-isolated uses may race with caller uses}}

  mutating func test4() -> (any GlobalActorIsolatedProtocol)? {
    self.p
  }

  // TODO: Should error here.
  mutating func test5() -> sending (any GlobalActorIsolatedProtocol)? {
    self.p
  }

  mutating func test6() -> (any OtherProtocol)? {
    self.p2
  }

  // TODO: Should error here.
  mutating func test7() -> sending (any OtherProtocol)? {
    self.p2
  }
}

extension NonSendableGlobalActorIsolatedEnum {
  mutating func test() {
    if case let .fourth(x) = self {
      print(x)
    }
    switch self {
    case .first:
      break
    case .second(let x):
      print(x)
      break
    case .third(let x):
      print(x)
      break
    case .fourth(let x):
      print(x)
      break
    case .fifth(let x):
      print(x)
      break
    }
  }

  mutating func test2() -> (any GlobalActorIsolatedProtocol)? {
    guard case let .fourth(x) = self else {
      return nil
    }
    return x
  }

  // TODO: This should error.
  mutating func test3() -> sending (any GlobalActorIsolatedProtocol)? {
    guard case let .fourth(x) = self else {
      return nil
    }
    return x
  }

  mutating func test3() -> sending NonSendableKlass? {
    guard case let .second(x) = self else {
      return nil
    }
    return x
  } // expected-error {{sending 'x.some' risks causing data races}}
  // expected-note @-1 {{main actor-isolated 'x.some' cannot be a 'sending' result. main actor-isolated uses may race with caller uses}}
}

extension NonSendableGlobalActorIsolatedKlass {
  func test() {
    _ = self.k
  }

  func test2() -> NonSendableKlass {
    self.k
  }

  func test3() -> sending NonSendableKlass {
    self.k
  } // expected-error {{sending 'self.k' risks causing data races}}
  // expected-note @-1 {{main actor-isolated 'self.k' cannot be a 'sending' result. main actor-isolated uses may race with caller uses}}

  func test4() -> (any GlobalActorIsolatedProtocol)? {
    self.p
  }

  // TODO: Should error here.
  func test5() -> sending (any GlobalActorIsolatedProtocol)? {
    self.p
  }

  func test6() -> (any OtherProtocol)? {
    self.p2
  }

  // TODO: Should error here.
  func test7() -> sending (any OtherProtocol)? {
    self.p2
  }
}

extension FinalNonSendableGlobalActorIsolatedKlass {
  func test() {
    _ = self.k
  }

  func test2() -> NonSendableKlass {
    self.k
  }

  func test3() -> sending NonSendableKlass {
    self.k
  } // expected-error {{sending 'self.k' risks causing data races}}
  // expected-note @-1 {{main actor-isolated 'self.k' cannot be a 'sending' result. main actor-isolated uses may race with caller uses}}

  func test4() -> (any GlobalActorIsolatedProtocol)? {
    self.p
  }

  // TODO: Should error here.
  func test5() -> sending (any GlobalActorIsolatedProtocol)? {
    self.p
  }

  func test6() -> (any OtherProtocol)? {
    self.p2
  }

  // TODO: Should error here.
  func test7() -> sending (any OtherProtocol)? {
    self.p2
  }
}

extension GlobalActorIsolatedProtocol {
  mutating func test() {
    _ = self.k
  }

  mutating func test2() -> NonSendableKlass {
    self.k
  }

  mutating func test3() -> sending NonSendableKlass {
    self.k
  } // expected-error {{sending 'self.k' risks causing data races}}
  // expected-note @-1 {{main actor-isolated 'self.k' cannot be a 'sending' result. main actor-isolated uses may race with caller uses}}

  mutating func test4() -> (any GlobalActorIsolatedProtocol)? {
    self.p
  }

  // TODO: Should error here.
  mutating func test5() -> sending (any GlobalActorIsolatedProtocol)? {
    self.p
  }

  mutating func test6() -> (any OtherProtocol)? {
    self.p2
  }

  // TODO: Should error here.
  mutating func test7() -> sending (any OtherProtocol)? {
    self.p2
  }
}
