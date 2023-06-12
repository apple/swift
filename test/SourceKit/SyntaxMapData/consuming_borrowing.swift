func foo(a: consuming Int, b: borrowing Int, c: _const Int) {}

// RUN: %sourcekitd-test -req=syntax-map %s | %FileCheck %s

// CHECK: key.syntaxmap: [
// CHECK-NEXT: {
// CHECK-NEXT:   key.kind: source.lang.swift.syntaxtype.keyword,
// CHECK-NEXT:   key.offset: 0,
// CHECK-NEXT:   key.length: 4
// CHECK-NEXT: },
// CHECK-NEXT: {
// CHECK-NEXT:   key.kind: source.lang.swift.syntaxtype.identifier,
// CHECK-NEXT:   key.offset: 5,
// CHECK-NEXT:   key.length: 3
// CHECK-NEXT: },
// CHECK-NEXT: {
// CHECK-NEXT:   key.kind: source.lang.swift.syntaxtype.identifier,
// CHECK-NEXT:   key.offset: 9,
// CHECK-NEXT:   key.length: 1
// CHECK-NEXT: },
// CHECK-NEXT: {
// CHECK-NEXT:   key.kind: source.lang.swift.syntaxtype.keyword,
// CHECK-NEXT:   key.offset: 12,
// CHECK-NEXT:   key.length: 9
// CHECK-NEXT: },
// CHECK-NEXT: {
// CHECK-NEXT:   key.kind: source.lang.swift.syntaxtype.typeidentifier,
// CHECK-NEXT:   key.offset: 22,
// CHECK-NEXT:   key.length: 3
// CHECK-NEXT: },
// CHECK-NEXT: {
// CHECK-NEXT:   key.kind: source.lang.swift.syntaxtype.identifier,
// CHECK-NEXT:   key.offset: 27,
// CHECK-NEXT:   key.length: 1
// CHECK-NEXT: },
// CHECK-NEXT: {
// CHECK-NEXT:   key.kind: source.lang.swift.syntaxtype.keyword,
// CHECK-NEXT:   key.offset: 30,
// CHECK-NEXT:   key.length: 9
// CHECK-NEXT: },
// CHECK-NEXT: {
// CHECK-NEXT:   key.kind: source.lang.swift.syntaxtype.typeidentifier,
// CHECK-NEXT:   key.offset: 40,
// CHECK-NEXT:   key.length: 3
// CHECK-NEXT: },
// CHECK-NEXT: {
// CHECK-NEXT:   key.kind: source.lang.swift.syntaxtype.identifier,
// CHECK-NEXT:   key.offset: 45,
// CHECK-NEXT:   key.length: 1
// CHECK-NEXT: },
// CHECK-NEXT: {
// CHECK-NEXT:   key.kind: source.lang.swift.syntaxtype.keyword,
// CHECK-NEXT:   key.offset: 48,
// CHECK-NEXT:   key.length: 6
// CHECK-NEXT: },
// CHECK-NEXT: {
// CHECK-NEXT:   key.kind: source.lang.swift.syntaxtype.typeidentifier,
// CHECK-NEXT:   key.offset: 55,
// CHECK-NEXT:   key.length: 3
// CHECK-NEXT: },
