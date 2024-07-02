// RUN: %target-typecheck-verify-swift -swift-version 5

protocol P {}

protocol Q {}

struct S1: P {}

struct S2: Q {}

@resultBuilder
struct Builder1 {
  // expected-note@+1 {{where 'T' = 'S2'}}
  static func buildExpression<T: P>(_ t: T) -> T {
    t
  }

  static func buildBlock<T>(_ components: T...) -> T {
    return components[0]
  }
}

@resultBuilder
struct Builder2 {
  // expected-note@+1 {{where 'T' = 'S1'}}
  static func buildExpression<T: Q>(_ t: T) -> T {
    t
  }

  static func buildBlock<T>(_ components: T...) -> T {
    return components[0]
  }
}


func builder<T: P>(@Builder1 closure: () -> T) -> some P {
  closure()
}
func builder<T: Q>(@Builder2 closure: () -> T) -> some Q {
  closure()
}

func ambiguous() {
  builder {
    // expected-note@+1 {{static method 'buildExpression' requires that 'S1' conform to 'Q'}}
    S1()
    // expected-error@+2 {{found multiple potential errors}}
    // expected-note@+1 {{static method 'buildExpression' requires that 'S2' conform to 'P'}}
    S2()
  }
}
