// RUN: %target-swift-frontend -emit-sil -unavailable-decl-optimization=none -verify %s
// RUN: %target-swift-frontend -emit-sil -unavailable-decl-optimization=complete -verify %s

public enum HasUnavailableElement {
  case available

  @available(*, unavailable)
  case unavailable
}

public func markUsed<T>(_ t: T) {}

public func testSwitch(_ e: HasUnavailableElement) {
  switch e {
  case .available:
    ()
  case .unavailable:
    let x: Int // expected-note {{constant defined here}}
    markUsed(x) // expected-error {{constant 'x' used before being initialized}}
  }
}

public func testIfCase(_ e: HasUnavailableElement) {
  if case .unavailable = e {
    let x: Int // expected-note {{constant defined here}}
    markUsed(x) // expected-error {{constant 'x' used before being initialized}}
  }
}

public func testInitSwitch() {
  struct S {
    var x: Int // expected-note {{'self.x' not initialized}}

    init(_ e: HasUnavailableElement) {
      switch e {
      case .available:
        x = 1
      case .unavailable:
        ()
      }
    } // expected-error {{return from initializer without initializing all stored properties}}
  }
}
