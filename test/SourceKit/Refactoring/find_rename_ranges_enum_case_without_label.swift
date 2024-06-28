// RUN: %empty-directory(%t)
// RUN: split-file %s %t
// RUN: %sourcekitd-test -req=find-rename-ranges -rename-spec %t/rename-spec.json %t/input.swift | %FileCheck %s

// CHECK: source.edit.kind.active:
// CHECK-NEXT:  2:8-2:14 source.refactoring.range.kind.basename
// CHECK-NEXT:  2:15-2:15 source.refactoring.range.kind.call-argument-combined arg-index=0
// CHECK-NEXT: source.edit.kind.active:
// CHECK-NEXT:  6:14-6:20 source.refactoring.range.kind.basename
// CHECK-NEXT:  6:21-6:21 source.refactoring.range.kind.call-argument-combined arg-index=0

//--- input.swift
enum MyEnum {
  case myCase(String)
}

func test() {
  _ = MyEnum.myCase("abc")
}

//--- rename-spec.json

[
  {
    "key.name": "myCase(_:)",
    "key.locations": [
      {
        "key.line": 2,
        "key.column": 8,
        "key.nametype": source.syntacticrename.definition
      }
    ]
  },
  {
    "key.name": "myCase(_:)",
    "key.locations": [
      {
        "key.line": 6,
        "key.column": 14,
        "key.nametype": source.syntacticrename.call
      }
    ]
  }
]
