// RUN: %empty-directory(%t)
// RUN: split-file %s %t
// RUN: %sourcekitd-test -req=find-rename-ranges -rename-spec %t/rename-spec.json %t/input.swift | %FileCheck %s

// CHECK: source.edit.kind.active:
// CHECK-NEXT:  2:8-2:14 source.refactoring.range.kind.basename
// CHECK-NEXT:  2:15-2:20 source.refactoring.range.kind.decl-argument-label arg-index=0
// CHECK-NEXT:  2:20-2:20 source.refactoring.range.kind.parameter-and-whitespace arg-index=0
// CHECK-NEXT: source.edit.kind.active:
// CHECK-NEXT:  6:14-6:20 source.refactoring.range.kind.basename
// CHECK-NEXT:  6:21-6:26 source.refactoring.range.kind.call-argument-label arg-index=0
// CHECK-NEXT:  6:26-6:28 source.refactoring.range.kind.call-argument-colon arg-index=0

//--- input.swift
enum MyEnum {
  case myCase(label: String)
}

func test() {
  _ = MyEnum.myCase(label: "abc")
}

//--- rename-spec.json

[
  {
    "key.name": "myCase(label:)",
    "key.locations": [
      {
        "key.line": 2,
        "key.column": 8,
        "key.nametype": source.syntacticrename.definition
      }
    ]
  },
  {
    "key.name": "myCase(label:)",
    "key.locations": [
      {
        "key.line": 6,
        "key.column": 14,
        "key.nametype": source.syntacticrename.call
      }
    ]
  }
]
