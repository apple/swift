// RUN: %target-swift-frontend -O -emit-ir  %s | %FileCheck %s
// RUN: %target-swift-frontend -Osize -emit-ir  %s | %FileCheck %s

// RUN: %empty-directory(%t) 
// RUN: %target-build-swift -O -module-name=test %s -o %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s -check-prefix=CHECK-OUTPUT

// REQUIRES: executable_test,swift_stdlib_no_asserts
// REQUIRES: CPU=arm64 || CPU=x86_64
// REQUIRES: swift_in_compiler

// This is an end-to-end test to ensure that the optimizer generates
// optimal code for static String variables.

public struct S {
  // CHECK: {{^@"}}[[SMALL:.*smallstr.*pZ]]" ={{.*}} constant {{.*}} inttoptr
  public static let smallstr = "abc123a"
  // CHECK: {{^@"}}[[LARGE:.*largestr.*pZ]]" ={{.*}} constant {{.*}} inttoptr {{.*}} add
  public static let largestr = "abc123asd3sdj3basfasdf"
  // CHECK: {{^@"}}[[UNICODE:.*unicodestr.*pZ]]" ={{.*}} constant {{.*}} inttoptr {{.*}} add
  public static let unicodestr = "❄️gastroperiodyni"
  // CHECK: {{^@"}}[[EMPTY:.*emptystr.*pZ]]" ={{.*}} constant {{.*}} inttoptr
  public static let emptystr = ""
}

// unsafeMutableAddressor for S.smallstr
// CHECK: define {{.*smallstr.*}}u"
// CHECK-NEXT: entry:
// CHECK-NEXT:   ret {{.*}} @"[[SMALL]]"
// CHECK-NEXT: }

// getter for S.smallstr
// CHECK: define {{.*smallstr.*}}gZ"
// CHECK-NEXT: entry:
// CHECK-NEXT:   ret {{.*}}
// CHECK-NEXT: }

// unsafeMutableAddressor for S.largestr
// CHECK: define {{.*largestr.*}}u"
// CHECK-NEXT: entry:
// CHECK-NEXT:   ret {{.*}} @"[[LARGE]]"
// CHECK-NEXT: }

// getter for S.largestr
// CHECK: define {{.*largestr.*}}gZ"
// CHECK-NEXT: entry:
// CHECK-NEXT:   ret {{.*}}
// CHECK-NEXT: }

// unsafeMutableAddressor for S.unicodestr
// CHECK: define {{.*unicodestr.*}}u"
// CHECK-NEXT: entry:
// CHECK-NEXT:   ret {{.*}} @"[[UNICODE]]"
// CHECK-NEXT: }

// getter for S.unicodestr
// CHECK: define {{.*unicodestr.*}}gZ"
// CHECK-NEXT: entry:
// CHECK-NEXT:   ret {{.*}}
// CHECK-NEXT: }

// unsafeMutableAddressor for S.emptystr
// CHECK: define {{.*emptystr.*}}u"
// CHECK-NEXT: entry:
// CHECK-NEXT:   ret {{.*}} @"[[EMPTY]]"
// CHECK-NEXT: }

// getter for S.emptystr
// CHECK: define {{.*emptystr.*}}gZ"
// CHECK-NEXT: entry:
// CHECK-NEXT:   ret {{.*}}
// CHECK-NEXT: }

// CHECK-LABEL: define {{.*}}get_smallstr
// CHECK:      entry:
// CHECK-NEXT:   ret {{.*}}
// CHECK-NEXT: }
@inline(never)
public func get_smallstr() -> String {
  return S.smallstr
}

// CHECK-LABEL: define {{.*}}get_largestr
// CHECK:      entry:
// CHECK-NEXT:   ret {{.*}}
// CHECK-NEXT: }
@inline(never)
public func get_largestr() -> String {
  return S.largestr
}

// CHECK-LABEL: define {{.*}}get_unicodestr
// CHECK:      entry:
// CHECK-NEXT:   ret {{.*}}
// CHECK-NEXT: }
@inline(never)
public func get_unicodestr() -> String {
  return S.unicodestr
}

// CHECK-LABEL: define {{.*}}get_emptystr
// CHECK:      entry:
// CHECK-NEXT:   ret {{.*}}
// CHECK-NEXT: }
@inline(never)
public func get_emptystr() -> String {
  return S.emptystr
}

// Really load the globals from their addresses.
@_optimize(none)
func print_strings_from_addressors() {
  print(S.smallstr)
  print(S.largestr)
  print(S.unicodestr)
  print("<\(S.emptystr)>")
}

@inline(never)
func testit() {
  // Also check if the generated code is correct.
  
  // CHECK-OUTPUT: abc123a
  // CHECK-OUTPUT: abc123asd3sdj3basfasdf
  // CHECK-OUTPUT: ❄️gastroperiodyni
  // CHECK-OUTPUT: <>
  print(get_smallstr())
  print(get_largestr())
  print(get_unicodestr())
  print("<\(get_emptystr())>")
  
  // CHECK-OUTPUT: abc123a
  // CHECK-OUTPUT: abc123asd3sdj3basfasdf
  // CHECK-OUTPUT: ❄️gastroperiodyni
  // CHECK-OUTPUT: <>
  print_strings_from_addressors()
}

testit()
