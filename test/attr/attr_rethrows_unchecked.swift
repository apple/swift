// RUN: %target-typecheck-verify-swift

func alwaysThrows() throws {}

func unsafeRethrowsBad(_ fn: () throws -> ()) rethrows {
  try alwaysThrows()
  // expected-error@-1 {{call can throw, but the error is not handled; a function declared 'rethrows' may only throw if its parameter does}}
}

@_rethrowsUnchecked
func unsafeRethrowsGood(_ fn: () throws -> ()) rethrows {
  try alwaysThrows()
}
