func bar() -> Int { return 1 }
func bar() -> String { return "" }

// RUN: %sourcekitd-test -req=cursor -pos=%(line + 2):7 %s -- %s == -req=cursor -pos=%(line + 3):7 %s -- %s | %FileCheck %s --check-prefix IN-FUNCTION
func foo() {
  _ = bar()
  _ = bar()
}

// IN-FUNCTION: source.lang.swift.ref.function.free
// IN-FUNCTION-NEXT: bar
// IN-FUNCTION: DID REUSE AST CONTEXT: 0
// IN-FUNCTION: source.lang.swift.ref.function.free
// IN-FUNCTION-NEXT: bar
// IN-FUNCTION-NEXT: bar
// IN-FUNCTION: DID REUSE AST CONTEXT: 1

// RUN: %sourcekitd-test -req=cursor -pos=%(line + 3):9 %s -- %s == -req=cursor -pos=%(line + 4):9 %s -- %s | %FileCheck %s --check-prefix IN-INSTANCE-METHOD
struct MyStruct {
  func test() {
    _ = bar()
    _ = bar()
  }
}

// IN-INSTANCE-METHOD: source.lang.swift.ref.function.free
// IN-INSTANCE-METHOD-NEXT: bar
// IN-INSTANCE-METHOD: DID REUSE AST CONTEXT: 0
// IN-INSTANCE-METHOD: source.lang.swift.ref.function.free
// IN-INSTANCE-METHOD-NEXT: bar
// IN-INSTANCE-METHOD: DID REUSE AST CONTEXT: 1
