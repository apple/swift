// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %sourcekitd-test \
// RUN:   -shell -- echo '## State 1' == \
// RUN:   -req=cursor -pos=6:21 %t/file.swift -- %t/file.swift == \
// RUN:   -shell -- echo '## State 2' == \
// RUN:   -shell -- cp %t/State2.swift %t/file.swift == \
// RUN:   -req=cursor -pos=7:21 %t/file.swift -- %t/file.swift == \
// RUN:   -shell -- echo '## State 3' == \
// RUN:   -shell -- cp %t/State3.swift %t/file.swift == \
// RUN:   -req=cursor -pos=5:21 %t/file.swift -- %t/file.swift == \
// RUN:   -shell -- echo '## State 4' == \
// RUN:   -shell -- cp %t/State4.swift %t/file.swift == \
// RUN:   -req=cursor -pos=5:19 %t/file.swift -- %t/file.swift == \
// RUN:   -shell -- echo '## State 5' == \
// RUN:   -shell -- cp %t/State5.swift %t/file.swift == \
// RUN:   -req=cursor -pos=5:19 %t/file.swift -- %t/file.swift == \
// RUN:   -shell -- echo '## State 6' == \
// RUN:   -shell -- cp %t/State6.swift %t/file.swift == \
// RUN:   -req=cursor -pos=5:19 %t/file.swift -- %t/file.swift > %t/response.txt
// RUN: %FileCheck %s < %t/response.txt

// CHECK-LABEL: ## State 1
// CHECK: source.lang.swift.ref.function.free
// CHECK: DID REUSE AST CONTEXT: 0
// CHECK-LABEL: ## State 2
// CHECK: source.lang.swift.ref.function.free
// CHECK: DID REUSE AST CONTEXT: 1
// CHECK-LABEL: ## State 3
// CHECK: source.lang.swift.ref.function.free
// CHECK: DID REUSE AST CONTEXT: 1
// CHECK-LABEL: ## State 4
// CHECK: source.lang.swift.ref.function.free
// CHECK: DID REUSE AST CONTEXT: 1
// CHECK-LABEL: ## State 5
// CHECK: source.lang.swift.ref.function.free
// CHECK: DID REUSE AST CONTEXT: 0
// CHECK-LABEL: ## State 6
// CHECK: source.lang.swift.ref.function.free
// CHECK: DID REUSE AST CONTEXT: 1

//--- file.swift
func bar() -> Int { return 1 }
func bar() -> String { return "" }

func foo() {
  let inFunctionA = bar()
  let inFunctionB = bar()
}

//--- State2.swift
func bar() -> Int { return 1 }
func bar() -> String { return "" }

func foo() {
  let newlyAddedMember: Int = bar()
  let inFunctionA = bar()
  let inFunctionB = bar()
}

//--- State3.swift
func bar() -> Int { return 1 }
func bar() -> String { return "" }

func foo() {
  let inFunctionB = bar()
}

//--- State4.swift
func bar() -> Int { return 1 }
func bar() -> String { return "" }

func foo() {
  let myNewName = bar()
}

//--- State5.swift
func bar() -> Int { return 1 }
func bar() -> String { return "" }

func foo(param: Int) {
  let myNewName = bar()
}

//--- State6.swift
func bar() -> Int { return 1 }
func bar() -> String { return "" }

func foo(param: Int) {
  let myNewName = bar() + bar()
}
