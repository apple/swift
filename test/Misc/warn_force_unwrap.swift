// RUN: %target-swift-frontend -lint force_unwrap_usage -typecheck -verify -c %s

var a: String?

print(a!) // expected-warning {{consider using an alternative to a forced unwrap}}

