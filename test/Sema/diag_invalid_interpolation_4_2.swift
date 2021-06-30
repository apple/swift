// RUN: %target-typecheck-verify-swift -swift-version 4.2

var x = 0
_ = "\(&x)"
// expected-error@-1 {{'&' used with non-inout argument of type 'Int'}}

_ = "\(y: &x)"
// expected-error@-1 {{'&' used with non-inout argument of type 'Int'}}
// expected-warning@-2 {{labeled interpolations will not be ignored in Swift 5}}
// expected-note@-3 {{remove 'y' label to keep current behavior}}

_ = "\(x, y: &x)"
// expected-error@-1 {{extra argument 'y' in call}}
// expected-error@-2 2{{use of extraneous '&'}}
