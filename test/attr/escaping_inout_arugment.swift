// RUN: %target-typecheck-verify-swift

let _: (_ v: inout @escaping () -> Void) -> ()
// expected-error@-1 {{inout expression is implicitly escaping}}{{20-30=}}

func m(v: inout @escaping () -> Void) {}
// expected-error@-1 {{inout expression is implicitly escaping}}{{17-27=}}
