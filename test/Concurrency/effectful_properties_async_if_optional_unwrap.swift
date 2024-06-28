// RUN: %target-swift-frontend -parse-as-library -emit-sil -verify %s

// REQUIRES: concurrency

@available(SwiftStdlib 5.1, *)
struct Kappa {
  var maybeData: Int? {
    get async { 12 }
  }

  func take() -> Int? { // expected-note 3{{add 'async' to function 'take()' to make it asynchronous}}

    if let data = maybeData { // expected-error{{'async' property access in a function that does not support concurrency}}
      return data
    }

    let x = maybeData // expected-error{{'async' property access in a function that does not support concurrency}}
      _ = x

    if let maybeData { // expected-error{{'async' property access in a function that does not support concurrency}}
      return maybeData
    }

    return nil
  }

  func takeAsync() async -> Int? {

    if let data = maybeData { // expected-error{{expression is 'async' but is not marked with 'await'}}
      // expected-note@-1{{property access is 'async'}}
      return data
    }

    let x = maybeData // expected-error{{expression is 'async' but is not marked with 'await'}}
    // expected-note@-1{{property access is 'async'}}
    _ = x

    if let maybeData { // expected-error{{expression is 'async' but is not marked with 'await'}}
      // expected-note@-1{{property access is 'async'}}
      return maybeData
    }

    return nil
  }

  func illegalSyntax() async -> Int? {
    // This is NOT valid syntax and we reject it properly,
    // though we could try to could give a better message
    if let await maybeData { // expected-error{{unwrap condition requires a valid identifier}}
      // expected-error@-1{{pattern variable binding cannot appear in an expression}}
      // expected-warning@-2{{no 'async' operations occur within 'await' expression}}
      return maybeData // expected-error{{expression is 'async' but is not marked with 'await'}}
      // expected-note@-1{{property access is 'async'}}
    }

  }
}
