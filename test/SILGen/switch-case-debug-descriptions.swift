// RUN: %target-swift-emit-silgen -module-name switch %s -Xllvm -sil-print-debuginfo | %FileCheck %s -check-prefix=SCOPE

enum E {
  case one(String)
  case two(String)
  case three(Int)
}

func test1(_ e: E) {
  switch e {                                  // SCOPE: sil_scope [[test1_switch:[0-9]+]] {{.*}}:[[@LINE]]:3
  case .one(let payload), .two(let payload):  // SCOPE-NEXT: sil_scope [[test1_case1:[0-9]+]] {{.*}}:[[@LINE]]:3 parent [[test1_switch]]
    print(payload)                            // SCOPE-NEXT: sil_scope                        {{.*}}:[[@LINE]]:5 parent [[test1_case1]]
  case .three(let payload):                   // SCOPE-NEXT: sil_scope [[test1_case2:[0-9]+]] {{.*}}:[[@LINE]]:3 parent [[test1_switch]]
    print(payload)
  }
}

func test2(_ e: E) {
  switch e {                                  // SCOPE: sil_scope [[test2_switch:[0-9]+]] {{.*}}:[[@LINE]]:3
  case .one(let x):                           // SCOPE-NEXT: sil_scope [[test2_case1:[0-9]+]] {{.*}}:[[@LINE]]:3 parent [[test2_switch]]
    print(x)                                  // SCOPE-NEXT: sil_scope {{.*}}:[[@LINE]]:5 parent [[test2_case1]]
  case .two(let x):                           // SCOPE-NEXT: sil_scope [[test2_case2:[0-9]+]] {{.*}}:[[@LINE]]:3 parent [[test2_switch]]
    print(x)                                  // SCOPE-NEXT: sil_scope {{.*}}:[[@LINE]]:5 parent [[test2_case2]]
  case .three(let x):                         // SCOPE-NEXT: sil_scope [[test2_case3:[0-9]+]] {{.*}}:[[@LINE]]:3 parent [[test2_switch]]
    print(x)
  }
}

func test3(_ e: E) {
  switch e {                                  // SCOPE: sil_scope [[test3_switch:[0-9]+]] {{.*}}:[[@LINE]]:3
  case .one:                                  // SCOPE-NEXT: sil_scope [[test3_case1:[0-9]+]] {{.*}}:[[@LINE]]:3 parent [[test3_switch]]
    print(1)                                  // SCOPE-NEXT: sil_scope {{.*}}:[[@LINE]]:5 parent [[test3_case1]]
  case .three(let x):                         // SCOPE-NEXT: sil_scope [[test3_case2:[0-9]+]] {{.*}}:[[@LINE]]:3 parent [[test3_switch]]
    print(x)                                  // SCOPE-NEXT: sil_scope {{.*}}:[[@LINE]]:5 parent [[test3_case2]]
  default:                                    // SCOPE-NEXT: sil_scope [[test3_case3:[0-9]+]] {{.*}}:[[@LINE]]:3 parent [[test3_switch]]
    print("error")                            // SCOPE-NEXT: sil_scope {{.*}}:[[@LINE]]:5 parent [[test3_case3]]
  }
}

func test4(_ e: E) {
  switch e {                                  // SCOPE: sil_scope [[test4_switch:[0-9]+]] {{.*}}:[[@LINE]]:3
  case .one(let x):                           // SCOPE-NEXT: sil_scope [[test4_case1:[0-9]+]] {{.*}}:[[@LINE]]:3 parent [[test4_switch]]
    print(x)                                  // SCOPE-NEXT: sil_scope {{.*}}:[[@LINE]]:5 parent [[test4_case1]]
    fallthrough
  case .two(let x):                           // SCOPE-NEXT: sil_scope [[test4_case2:[0-9]+]] {{.*}}:[[@LINE]]:3 parent [[test4_switch]]
    print(x)                                  // SCOPE-NEXT:                                    {{.*}}:[[@LINE]]:5 parent [[test4_case2]]
    fallthrough
  default:                                    // SCOPE-NEXT: sil_scope [[test4_default:[0-9]+]] {{.*}}:[[@LINE]]:3 parent [[test4_switch]]
    print("default")                          // SCOPE:      sil_scope [[test4_default1:[0-9]+]] {{.*}}:[[@LINE]]:5
                                              // SCOPE: string_literal utf8 "default", {{.*}} scope [[test4_default1]]
  }
}

test1(E.one("payload1"))
test2(E.two("payload2"))
test3(E.three(3))
test4(E.one("payload1"))
