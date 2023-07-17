func foo(a a: String) {}
func foo(a a: Int) {}
func foo(b b: Int) {}

func test() {
  let x = 1

}

// RUN: %sourcekitd-test -req=complete -req-opts=hidelowpriority=0 -pos=7:1 %s -- %s > %t.orig
// RUN: %sourcekitd-test -req=complete -req-opts=hidelowpriority=0,sort.byname=0 -pos=7:1 %s -- %s > %t.orig.off
// RUN: %FileCheck -check-prefix=NAME_SORTED %s < %t.orig
// RUN: %FileCheck -check-prefix=NAME_UNSORTED %s < %t.orig.off
// RUN: not %diff -u %t.orig %t.orig.off

// NAME_SORTED: key.name: "foo(a:)"
// NAME_SORTED-NOT: key.name:
// NAME_SORTED: key.name: "foo(a:)"
// NAME_SORTED-NOT: key.name:
// NAME_SORTED: key.name: "foo(b:)"
// NAME_SORTED: key.name: "test()"
// NAME_SORTED: key.name: "x"

// NAME_UNSORTED-DAG: key.description: "x"
// NAME_UNSORTED-DAG: key.description: "foo(a: String)"
// NAME_UNSORTED-DAG: key.description: "foo(a: Int)"
// NAME_UNSORTED-DAG: key.description: "foo(b: Int)"

// RUN: %sourcekitd-test -req=complete.open -pos=7:1 -req-opts=hidelowpriority=0,hideunderscores=0 %s -- %s > %t.default
// RUN: %sourcekitd-test -req=complete.open -pos=7:1 -req-opts=sort.byname=0,hidelowpriority=0,hideunderscores=0 %s -- %s > %t.on
// RUN: %sourcekitd-test -req=complete.open -pos=7:1 -req-opts=sort.byname=1,hidelowpriority=0,hideunderscores=0 %s -- %s > %t.off
// RUN: %FileCheck -check-prefix=CONTEXT %s < %t.default
// RUN: %FileCheck -check-prefix=NAME_SORTED %s < %t.off
// FIXME: rdar://problem/20109989 non-deterministic sort order
// RUN-disabled: diff %t.on %t.default
// RUN: %FileCheck -check-prefix=CONTEXT %s < %t.on

// CONTEXT: key.kind: source.lang.swift.decl
// CONTEXT-NEXT: key.name: "x"
// CONTEXT-NOT: key.name:
// CONTEXT: key.name: "foo(a:)"
// CONTEXT-NOT: key.name:
// CONTEXT: key.name: "foo(a:)"
// CONTEXT-NOT: key.name:
// CONTEXT: key.name: "foo(b:)"
// CONTEXT-NOT: key.name:
// CONTEXT: key.name: "test()"
// CONTEXT: key.name: "complete_sort_order"

// RUN: %complete-test -tok=STMT_0 %s | %FileCheck %s -check-prefix=STMT
func test1() {
  #^STMT_0^#
}
// STMT: let
// STMT: var
// STMT: if
// STMT: for
// STMT: while
// STMT: return
// STMT: func
// STMT: foo(a: Int)

// RUN: %complete-test -tok=STMT_1 %s | %FileCheck %s -check-prefix=STMT_1
func test5() {
  var retLocal: Int
  #^STMT_1,r,ret,retur,return^#
}
// STMT_1-LABEL: Results for filterText: r [
// STMT_1-NEXT:    return
// STMT_1-NEXT:    retLocal
// STMT_1-NEXT:    repeat
// STMT_1-NEXT:    required
// STMT_1: ]
// STMT_1-LABEL: Results for filterText: ret [
// STMT_1-NEXT:    return
// STMT_1-NEXT:    retLocal
// STMT_1:         repeat
// STMT_1: ]
// STMT_1-LABEL: Results for filterText: retur [
// STMT_1-NEXT:    return
// STMT_1: ]
// STMT_1-LABEL: Results for filterText: return [
// STMT_1-NEXT:    return
// STMT_1: ]

// RUN: %complete-test -top=0 -tok=EXPR_0 %s | %FileCheck %s -check-prefix=EXPR
func test2() {
  (#^EXPR_0^#)
}
// EXPR: 0
// EXPR: "abc"
// EXPR: true
// EXPR: false
// EXPR: #colorLiteral(red: Float, green: Float, blue: Float, alpha: Float)
// EXPR: #imageLiteral(resourceName: String)
// EXPR: [values]
// EXPR: [key: value]
// EXPR: (values)
// EXPR: nil
// EXPR: foo(a: Int)

// Top 1
// RUN: %complete-test -top=1 -tok=EXPR_1 %s | %FileCheck %s -check-prefix=EXPR_TOP_1
func test3(x: Int) {
  let y = x
  let z = x
  let zzz = x
  (#^EXPR_1^#)
}
// EXPR_TOP_1: x
// EXPR_TOP_1: 0
// EXPR_TOP_1: "abc"
// EXPR_TOP_1: true
// EXPR_TOP_1: false
// EXPR_TOP_1: #colorLiteral(red: Float, green: Float, blue: Float, alpha: Float)
// EXPR_TOP_1: #imageLiteral(resourceName: String)
// EXPR_TOP_1: [values]
// EXPR_TOP_1: [key: value]
// EXPR_TOP_1: (values)
// EXPR_TOP_1: nil
// EXPR_TOP_1: y
// EXPR_TOP_1: z
// EXPR_TOP_1: zzz

// Test where there are fewer results than 'top'.
// RUN: %complete-test -top=1000 -tok=FEW_1 %s | %FileCheck %s -check-prefix=FEW_1
func test3b() -> Int {
  return #^FEW_1^#
}
// FEW_1: test3b()
// FEW_1: Int
// FEW_1: 0

// Top 3
// RUN: %complete-test -top=3 -tok=EXPR_2 %s | %FileCheck %s -check-prefix=EXPR_TOP_3
func test4(x: Int) {
  let y = x
  let z = x
  let zzz = x
  (#^EXPR_2^#)
}
// EXPR_TOP_3: x
// EXPR_TOP_3: y
// EXPR_TOP_3: z
// EXPR_TOP_3: 0
// EXPR_TOP_3: "abc"
// EXPR_TOP_3: true
// EXPR_TOP_3: false
// EXPR_TOP_3: #colorLiteral(red: Float, green: Float, blue: Float, alpha: Float)
// EXPR_TOP_3: #imageLiteral(resourceName: String)
// EXPR_TOP_3: [values]
// EXPR_TOP_3: [key: value]
// EXPR_TOP_3: (values)
// EXPR_TOP_3: nil
// EXPR_TOP_3: zzz

// Top 3 with type matching
// RUN: %complete-test -top=3 -tok=EXPR_3 %s | %FileCheck %s -check-prefix=EXPR_TOP_3_TYPE_MATCH
func test4(x: Int) {
  let y: String = ""
  let z: String = y
  let zzz = x
  let bar: Int = #^EXPR_3^#
}
// EXPR_TOP_3_TYPE_MATCH: x
// EXPR_TOP_3_TYPE_MATCH: zzz
// EXPR_TOP_3_TYPE_MATCH: 0
// EXPR_TOP_3_TYPE_MATCH: y
// EXPR_TOP_3_TYPE_MATCH: z

// RUN: %complete-test -tok=VOID_1 %s | %FileCheck %s -check-prefix=VOID_1
// RUN: %complete-test -tok=VOID_1 %s -raw | %FileCheck %s -check-prefix=VOID_1_RAW
func test6() {
  func foo1() {}
  func foo2() -> Int {}
  func foo3() -> String {}
  let x: Int
  x = #^VOID_1,,foo^#
}
// VOID_1-LABEL: Results for filterText:  [
// VOID_1-NOT: foo1
// VOID_1: foo2()
// VOID_1-NOT: foo1
// VOID_1: foo3()
// VOID_1-NOT: foo1
// VOID_1: ]
// VOID_1-LABEL: Results for filterText: foo [
// VOID_1: foo2()
// VOID_1: foo3()
// VOID_1: foo1()
// VOID_1: ]

// VOID_1_RAW: key.name: "foo1()",
// VOID_1_RAW-NEXT: key.sourcetext: "foo1()",
// VOID_1_RAW-NEXT: key.description: "foo1()",
// VOID_1_RAW-NEXT: key.typename: "Void",
// VOID_1_RAW-NEXT: key.context: source.codecompletion.context.local,
// VOID_1_RAW-NEXT: key.num_bytes_to_erase: 0,



// RUN: %complete-test -tok=CASE_0 %s | %FileCheck %s -check-prefix=CASE_0
func test7() {
  struct CaseSensitiveCheck {
    var member: Int = 0
  }
  let caseSensitiveCheck = CaseSensitiveCheck()
  #^CASE_0,caseSensitiveCheck,CaseSensitiveCheck^#
}
// CASE_0: Results for filterText: caseSensitiveCheck [
// CASE_0: caseSensitiveCheck
// CASE_0: CaseSensitiveCheck
// CASE_0: caseSensitiveCheck.
// CASE_0: ]
// CASE_0: Results for filterText: CaseSensitiveCheck [
// CASE_0: CaseSensitiveCheck
// CASE_0: caseSensitiveCheck
// CASE_0: CaseSensitiveCheck(
// CASE_0: ]

// RUN: %complete-test -tok=CALLARG_1 %s | %FileCheck %s -check-prefix=CALLARG
// RUN: %complete-test -tok=CALLARG_2 %s | %FileCheck %s -check-prefix=CALLARG
func test8() {
    struct CallArgumentTest {
        init(_ arg: String) {}
        init(label arg: Int) {}
        func argTest(_ arg: String) {}
        func argTest(label arg: Int) {}
    }
    var stringVal: String = "";
    var intVal: Int = 1

    _ = CallArgumentTest(#^CALLARG_1^#)
    func methodTest(obj: CallArgumentTest) {
      obj.argTest(#^CALLARG_2^#)
    }
// CALLARG: (arg: String)
// CALLARG: (label: Int)
// CALLARG: intVal
// CALLARG: stringVal
// CALLARG: String
}
