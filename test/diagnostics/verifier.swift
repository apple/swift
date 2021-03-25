// RUN: %empty-directory(%t)
// RUN: not %target-swift-frontend -typecheck -verify -serialize-diagnostics-path %t/serialized.dia -emit-fixits-path %t/fixits %s 2>&1 | %FileCheck %s
// RUN: %FileCheck %s -check-prefix CHECK-SERIALIZED <%t/serialized.dia
// RUN: %FileCheck %s -check-prefix CHECK-FIXITS <%t/fixits

// Wrong message
let x: Int = "hello, world!" // expected-error {{foo bar baz}}
// CHECK-NOT: error: cannot convert value of type 'String' to specified type 'Int'
// CHECK: error: incorrect message found

// Wrong column
let y: Int = "hello, world!" // expected-error@:49 {{cannot convert value of type}}
// CHECK: message found at column 14 but was expected to appear at column 49

// Wrong fix-it
let z: Int = "hello, world!" as Any
// expected-error@-1 {{cannot convert value of type}} {{3-3=foobarbaz}}
// CHECK: expected fix-it not seen; actual fix-it seen: {{[{][{]}}36-36= as! Int{{[}][}]}}

// Expected no fix-it
let a: Bool = "hello, world!" as Any
// expected-error@-1 {{cannot convert value of type}} {{none}}
// CHECK: expected no fix-its; actual fix-it seen: {{[{][{]}}37-37= as! Bool{{[}][}]}}

// Unexpected error
_ = foo()
// CHECK: unexpected error produced: cannot find 'foo' in scope

extension (Int, Int) {} // expected-error {{non-nominal type '(Int, Int)' cannot be extended}} {{educational-notes=foo-bar-baz}}
// CHECK: error: expected educational note(s) not seen; actual educational note(s): {{[{][{]}}educational-notes=nominal-types{{[}][}]}}

extension (Bool, Int) {} // expected-error {{non-nominal type '(Bool, Int)' cannot be extended}} {{educational-notes=nominal-types}} {{educational-notes=nominal-types}}
// CHECK: error: each verified diagnostic may only have one {{[{][{]}}educational-notes=<#notes#>{{[}][}]}} declaration

extension (Bool, Bool) {} // expected-error {{non-nominal type '(Bool, Bool)' cannot be extended}} {{educational-notes=nominal-types,foo-bar-baz}}
// CHECK: error: expected educational note(s) not seen; actual educational note(s): {{[{][{]}}educational-notes=nominal-types{{[}][}]}}

// Verify the serialized diags have the right magic at the top.
// CHECK-SERIALIZED: DIA

// Ensure the verifier doesn't interfere with -emit-fixits-path.
// CHECK-FIXITS: {
// CHECK-FIXITS: "file":
// CHECK-FIXITS: "offset":
// CHECK-FIXITS: "text": " as! Int",
// CHECK-FIXITS: },
