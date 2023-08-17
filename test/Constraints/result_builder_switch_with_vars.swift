// RUN: %target-swift-frontend -typecheck -dump-ast %s | %FileCheck %s

enum Either<T,U> {
  case first(T)
  case second(U)
}

@resultBuilder
struct TupleBuilder {
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

  static func buildEither<T,U>(first value: T) -> Either<T,U> {
    return .first(value)
  }
  static func buildEither<T,U>(second value: U) -> Either<T,U> {
    return .second(value)
  }
}

enum E {
case test(a: String, b: Int)
}

func tuplify<T>(@TupleBuilder body: (E) throws -> T) rethrows {
}

tuplify {
  switch $0 {
  // CHECK: (case_body_variables
  // CHECK-NEXT: (var_decl implicit {{.*}} "a" interface type='String' let readImpl=stored immutable)
  // CHECK-NEXT: (var_decl implicit {{.*}} "b" interface type='Int' let readImpl=stored immutable)
  case let .test(a, b):
    a
    b
  }

  switch $0 {
    // CHECK: (case_body_variables
    // CHECK-NEXT: (var_decl implicit {{.*}} "a" interface type='String' let readImpl=stored immutable)
    // CHECK-NEXT: (var_decl implicit {{.*}} "b" interface type='Int' let readImpl=stored immutable)
  case .test(let a, let b):
    a
    b
  }

  switch $0 {
    // CHECK: (case_body_variables
    // CHECK-NEXT: (var_decl implicit {{.*}} "value" interface type='(a: String, b: Int)' let readImpl=stored immutable)
  case let .test((value)):
    value.a
  }

  switch $0 {
    // CHECK: (case_body_variables
    // CHECK-NEXT: (var_decl implicit {{.*}} "value" interface type='(a: String, b: Int)' let readImpl=stored immutable)
  case let .test(value):
    value.a
  }

  switch $0 {
    // CHECK: (case_body_variables
    // CHECK-NEXT: (var_decl implicit {{.*}} "value" interface type='(a: String, b: Int)' let readImpl=stored immutable)
  case .test(let value):
    value.a
  }
}
