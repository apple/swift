// RUN: %target-typecheck-verify-swift -parse-stdlib
// tests the ability to switch over an enum with same named cases that differ in associated value labels and types

struct A {}
struct C {}

enum B {
    case a(A)
    case a(a: A)
    case a(b: A)
    case a(c: C)
}

let a: B = .a(b: A())

switch a {
case .a(_): break
case .a(c: _): break
default: break
}

switch a {
case .a(a: _): break
case .a(c: _): break
default: break
}

switch a {
case .a(b: _): break
case .a(c: _): break
default: break
}
