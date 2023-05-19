// RUN: %target-typecheck-verify-swift

// rdar://107651291 – Make sure we don't crash
func foo(xs: [String: [String]], ys: [String: [String]]) {
  for (key, value) in xs {
    guard let ys = ys.first(where: { $0.key == key }) else { return }
    for (a, b) in zip(xs, ys) {}
    // expected-error@-1 {{type 'Dictionary<String, [String]>.Element' (aka '(key: String, value: Array<String>)') cannot conform to 'Sequence'}}
    // expected-note@-2 {{only concrete types such as structs, enums and classes can conform to protocols}}
    // expected-note@-3 {{required by referencing instance method 'next()'}}
  }
}
