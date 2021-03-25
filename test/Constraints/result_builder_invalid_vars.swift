// RUN: %target-typecheck-verify-swift

@resultBuilder
struct DummyBuilder { // expected-note 5 {{struct 'DummyBuilder' declared here}}
  static func buildBlock<T>(_ t: T) -> T {
    return t
  }
}

func dummy<T>(@DummyBuilder _: () -> T) {}

dummy {
  var computedVar: Int { return 123 } // expected-error {{closure containing a declaration cannot be used with result builder 'DummyBuilder'}}
  ()
}

dummy {
  lazy var lazyVar: Int = 123 // expected-error {{closure containing a declaration cannot be used with result builder 'DummyBuilder'}}
  ()
}

dummy {
  var observedVar: Int = 123 { // expected-error {{closure containing a declaration cannot be used with result builder 'DummyBuilder'}}
    didSet {}
  }

  ()
}

dummy {
  var observedVar: Int = 123 { // expected-error {{closure containing a declaration cannot be used with result builder 'DummyBuilder'}}
    willSet {}
  }

  ()
}

@propertyWrapper struct Wrapper {
  var wrappedValue: Int
}

dummy {
  @Wrapper var wrappedVar: Int = 123 // expected-error {{closure containing a declaration cannot be used with result builder 'DummyBuilder'}}
  ()
}

dummy {
  @resultBuilder var attributedVar: Int = 123 // expected-error {{@resultBuilder' attribute cannot be applied to this declaration}}
  ()
}
