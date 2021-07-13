// RUN: %target-typecheck-verify-swift -target %target-cpu-apple-macosx10.50

// REQUIRES: OS=macosx

@available(*, unavailable)
@resultBuilder
struct UnavailableBuilder {
// expected-note@-1 {{'UnavailableBuilder' has been explicitly marked unavailable here}}
  static func buildBlock() {}
}

@UnavailableBuilder public func usesUnavailableBuilder() {}
// expected-error@-1 {{'UnavailableBuilder' is unavailable}}

enum Either<T,U> {
  case first(T)
  case second(U)
}

@resultBuilder
struct TupleBuilder {
// expected-note@-1{{add 'buildLimitedAvailability(_:)' to the result builder 'TupleBuilder' to erase type information for less-available types}}{{22-22=\n    static func buildLimitedAvailability(_ component: <#Component#>) -> <#Component#> {\n      <#code#>\n    \}}}
// expected-note@-2{{add 'buildLimitedAvailability(_:)' to the result builder 'TupleBuilder' to erase type information for less-available types}}{{22-22=\n    static func buildLimitedAvailability(_ component: <#Component#>) -> <#Component#> {\n      <#code#>\n    \}}}
  static func buildBlock<T1>(_ t1: T1) -> (T1) {
    return (t1)
  }

  static func buildBlock<T1, T2>(_ t1: T1, _ t2: T2) -> (T1, T2) {
    return (t1, t2)
  }

  static func buildBlock<T1, T2, T3>(_ t1: T1, _ t2: T2, _ t3: T3)
      -> (T1, T2, T3) {
    return (t1, t2, t3)
  }

  static func buildBlock<T1, T2, T3, T4>(_ t1: T1, _ t2: T2, _ t3: T3, _ t4: T4)
      -> (T1, T2, T3, T4) {
    return (t1, t2, t3, t4)
  }

  static func buildBlock<T1, T2, T3, T4, T5>(
    _ t1: T1, _ t2: T2, _ t3: T3, _ t4: T4, _ t5: T5
  ) -> (T1, T2, T3, T4, T5) {
    return (t1, t2, t3, t4, t5)
  }

  static func buildDo<T>(_ value: T) -> T { return value }
  static func buildIf<T>(_ value: T?) -> T? { return value }

  static func buildEither<T,U>(first value: T) -> Either<T,U> {
    return .first(value)
  }
  static func buildEither<T,U>(second value: U) -> Either<T,U> {
    return .second(value)
  }
}

func tuplify<T>(_ cond: Bool, @TupleBuilder body: (Bool) -> T) {
  print(body(cond))
}

@available(OSX, introduced: 10.9)
func globalFuncAvailableOn10_9() -> Int { return 9 }

@available(OSX, introduced: 10.51)
func globalFuncAvailableOn10_51() -> Int { return 10 }

@available(OSX, introduced: 10.52)
struct Only10_52 { }

@available(OSX, introduced: 10.52)
func globalFuncAvailableOn10_52() -> Only10_52 { .init() }

tuplify(true) { cond in
  globalFuncAvailableOn10_9()
  if #available(OSX 10.51, *) {
    globalFuncAvailableOn10_51()
    tuplify(false) { cond2 in
      if cond, #available(OSX 10.52, *) { 
        // expected-warning@-1{{result builder 'TupleBuilder' does not implement 'buildLimitedAvailability'; this code may crash on earlier versions of the OS}}
        cond2
        globalFuncAvailableOn10_52()
      } else if true {
        globalFuncAvailableOn10_52() // expected-error{{'globalFuncAvailableOn10_52()' is only available in macOS 10.52 or newer}}
        // expected-note@-1{{add 'if #available' version check}}
      } else if false {
        globalFuncAvailableOn10_52() // expected-error{{'globalFuncAvailableOn10_52()' is only available in macOS 10.52 or newer}}
        // expected-note@-1{{add 'if #available' version check}}
      } else {
        globalFuncAvailableOn10_52() // expected-error{{'globalFuncAvailableOn10_52()' is only available in macOS 10.52 or newer}}
        // expected-note@-1{{add 'if #available' version check}}
      }
      if cond, #unavailable(OSX 10.52) { 
        // expected-warning@-1{{result builder 'TupleBuilder' does not implement 'buildLimitedAvailability'; this code may crash on earlier versions of the OS}}
        cond2
        globalFuncAvailableOn10_52() // expected-error{{'globalFuncAvailableOn10_52()' is only available in macOS 10.52 or newer}}
        // expected-note@-1{{add 'if #available' version check}}
      } else if true {
        globalFuncAvailableOn10_52()
      } else if false {
        globalFuncAvailableOn10_52()
      } else {
        globalFuncAvailableOn10_52()
      }
    }
  }
}

// Function builder that can perform type erasure for #available.
@resultBuilder
struct TupleBuilderAvailability {
  static func buildBlock<T1>(_ t1: T1) -> (T1) {
    return (t1)
  }

  static func buildBlock<T1, T2>(_ t1: T1, _ t2: T2) -> (T1, T2) {
    return (t1, t2)
  }

  static func buildBlock<T1, T2, T3>(_ t1: T1, _ t2: T2, _ t3: T3)
      -> (T1, T2, T3) {
    return (t1, t2, t3)
  }

  static func buildBlock<T1, T2, T3, T4>(_ t1: T1, _ t2: T2, _ t3: T3, _ t4: T4)
      -> (T1, T2, T3, T4) {
    return (t1, t2, t3, t4)
  }

  static func buildBlock<T1, T2, T3, T4, T5>(
    _ t1: T1, _ t2: T2, _ t3: T3, _ t4: T4, _ t5: T5
  ) -> (T1, T2, T3, T4, T5) {
    return (t1, t2, t3, t4, t5)
  }

  static func buildDo<T>(_ value: T) -> T { return value }
  static func buildIf<T>(_ value: T?) -> T? { return value }

  static func buildEither<T,U>(first value: T) -> Either<T,U> {
    return .first(value)
  }
  static func buildEither<T,U>(second value: U) -> Either<T,U> {
    return .second(value)
  }

  static func buildLimitedAvailability<T>(_ value: T) -> Any {
    return value
  }
}

func tuplifyWithAvailabilityErasure<T>(_ cond: Bool, @TupleBuilderAvailability body: (Bool) -> T) {
  print(body(cond))
}

tuplifyWithAvailabilityErasure(true) { cond in
  if cond, #available(OSX 10.52, *) {
    globalFuncAvailableOn10_52()
  }

  if cond, #unavailable(OSX 10.52) {
    cond
  } else {
    globalFuncAvailableOn10_52()
  }
}
