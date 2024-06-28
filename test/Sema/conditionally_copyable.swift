// RUN: %target-typecheck-verify-swift

struct G<T: ~Copyable>: ~Copyable {}

extension G: Copyable where T: Copyable {}

protocol Base {}
protocol Derived: Base {}

// Normally we would require the conditional conformance 'G: Base' to be
// explicitly declared, but that would break source compatibility if G
// used to be unconditionally Copyable.

extension G: Derived {} // expected-note {{requirement from conditional conformance of 'G<NotCopyable>' to 'Base'}}

struct NotCopyable: ~Copyable {}
struct IsCopyable {}

func f<T: Base>(_: T.Type) {}

f(G<NotCopyable>.self)  // expected-error {{global function 'f' requires that 'NotCopyable' conform to 'Copyable'}}
f(G<IsCopyable>.self)  // accepted
