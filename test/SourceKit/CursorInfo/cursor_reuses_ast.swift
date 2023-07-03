// RUN: %empty-directory(%t)
// RUN: touch %t/empty.swift %t/func.swift

// Check that cursor info re-uses the underlying AST if it's able to based
// off edit snapshots.

// RUN: %sourcekitd-test \
// RUN:   -shell -- echo '## State 1' == \
// RUN:   -req=open -text-input %t/empty.swift %t/func.swift -- %t/func.swift == \
// RUN:   -req=edit -offset=0 -length=0 -replace="func foo() {}" -req-opts=enablesyntaxmap=0,enablesubstructure=0,enablediagnostics=0,syntactic_only=1 %t/func.swift -- %t/func.swift == \
// RUN:   -req=cursor -offset=5 %t/func.swift -- %t/func.swift == \
// RUN:   -shell -- echo '## State 2' == \
// RUN:   -req=edit -offset=0 -length=0 -replace="/* some comment */ " -req-opts=enablesyntaxmap=0,enablesubstructure=0,enablediagnostics=0,syntactic_only=1 %t/func.swift -- %t/func.swift == \
// RUN:   -req=cursor -offset=24 %t/func.swift -- %t/func.swift | %FileCheck %s

// CHECK: ## State 1
// CHECK: source.lang.swift.decl.function.free
// CHECK: foo()
// CHECK: DID REUSE AST CONTEXT: 0
// CHECK: ## State 2
// CHECK: source.lang.swift.decl.function.free
// CHECK: foo()
// CHECK: DID REUSE AST CONTEXT: 1
