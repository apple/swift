// RUN: %target-typecheck-verify-swift
// rdar://81228221

@resultBuilder
struct Builder {
  static func buildBlock(_ components: Int...) -> Int { 0 }
  static func buildEither(first component: Int) -> Int { 0 }
  static func buildEither(second component: Int) -> Int { 0 }
  static func buildOptional(_ component: Int?) -> Int { 0 }
  static func buildArray(_ components: [Int]) -> Int { 0 }
}

@Builder
func foo(_ x: String) -> Int {
  if .random() {
    switch x {
    case 1: // expected-error {{expression pattern of type 'Int' cannot match values of type 'String'}}
      0
    default:
      1
    }
  }
}

@Builder
func bar(_ x: String) -> Int {
  switch 0 {
  case 0:
    switch x {
    case 1: // expected-error {{expression pattern of type 'Int' cannot match values of type 'String'}}
      0
    default:
      1
    }
  default:
    0
  }
}

@Builder
func baz(_ x: String) -> Int {
  do {
    switch x {
    case 1: // expected-error {{expression pattern of type 'Int' cannot match values of type 'String'}}
      0
    default:
      1
    }
  }
}

@Builder
func qux(_ x: String) -> Int {
  for _ in 0 ... 0 {
    switch x {
    case 1: // expected-error {{expression pattern of type 'Int' cannot match values of type 'String'}}
      0
    default:
      1
    }
  }
}
