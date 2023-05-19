import errors

func testInvalidGlobalCursor() {
  invalidGlobalMissingInit = ""
}

func testInvalidFuncCursor() {
  invalidPartialFunc()
}

func testInvalidStructCursor() {
  let foo: InvalidStruct
  foo.memberB
}

func testInvalidStructMemberCompletion() {
  let foo: InvalidStruct
  foo.#^INVALID-MEMBER^#
  // INVALID-MEMBER-DAG: Decl[InstanceVar]/CurrNominal:      memberA[#Int#];
  // INVALID-MEMBER-DAG: Decl[InstanceVar]/CurrNominal:      memberB[#<<error type>>#];
  // INVALID-MEMBER-DAG: Decl[InstanceVar]/CurrNominal:      memberC[#<<error type>>#];
  // INVALID-MEMBER-DAG: Decl[InstanceVar]/CurrNominal:      memberD[#<<error type>>#];
  // INVALID-MEMBER-DAG: Decl[InstanceVar]/CurrNominal:      memberE[#<<error type>>#];
  // INVALID-MEMBER-DAG: Decl[InstanceMethod]/Super:         add({#<<error type>>#})[#Void#];
  // INVALID-MEMBER-DAG: Decl[InstanceMethod]/Super:         get()[#InvalidStruct.Item#];
  // INVALID-MEMBER-DAG: Decl[InstanceMethod]/Super:         set({#item: InvalidStruct.Item#})[#Void#];
}

func testInvalidTypeCompletion() {
  let foo: #^INVALID-TYPE^#;
  // INVALID-TYPE-DAG: Decl[Enum]/OtherModule[errors]:     InvalidEnum[#InvalidEnum#];
  // INVALID-TYPE-DAG: Decl[Class]/OtherModule[errors]:    InvalidClass[#InvalidClass#];
  // INVALID-TYPE-DAG: Decl[Struct]/OtherModule[errors]:   InvalidGenericStruct[#InvalidGenericStruct#];
  // INVALID-TYPE-DAG: Decl[Struct]/OtherModule[errors]:   InvalidStruct[#InvalidStruct#];
  // INVALID-TYPE-DAG: Decl[TypeAlias]/OtherModule[errors]: InvalidAlias[#InvalidAlias#];
  // INVALID-TYPE-DAG: Decl[Class]/OtherModule[errors]:    InvalidClassSub1[#InvalidClassSub1#];
  // INVALID-TYPE-DAG: Decl[Class]/OtherModule[errors]:    InvalidClassSub2[#InvalidClassSub2#];
  // INVALID-TYPE-DAG: Decl[Protocol]/OtherModule[errors]: InvalidProtocol[#InvalidProtocol#];
}

func testInvalidTopLevelCompletion() {
  #^INVALID-TOP^#
  // INVALID-TOP-DAG: Decl[Enum]/OtherModule[errors]:     InvalidEnum[#InvalidEnum#];
  // INVALID-TOP-DAG: Decl[Class]/OtherModule[errors]:    InvalidClass[#InvalidClass#];
  // INVALID-TOP-DAG: Decl[FreeFunction]/OtherModule[errors]: invalidGenericFuncBody({#param: T#})[#T#];
  // INVALID-TOP-DAG: Decl[FreeFunction]/OtherModule[errors]: invalidPartialFunc()[#Void#];
  // INVALID-TOP-DAG: Decl[FreeFunction]/OtherModule[errors]: invalidFuncBody()[#Void#];
  // INVALID-TOP-DAG: Decl[GlobalVar]/OtherModule[errors]: invalidGlobalClosureBody[#<<error type>>#];
  // INVALID-TOP-DAG: Decl[FreeFunction]/OtherModule[errors]: invalidFuncSignature()[#Void#];
  // INVALID-TOP-DAG: Decl[GlobalVar]/OtherModule[errors]: invalidGlobalMissingInit[#String#];
  // INVALID-TOP-DAG: Decl[Struct]/OtherModule[errors]:   InvalidGenericStruct[#InvalidGenericStruct#];
  // INVALID-TOP-DAG: Decl[Struct]/OtherModule[errors]:   InvalidStruct[#InvalidStruct#];
  // INVALID-TOP-DAG: Decl[FreeFunction]/OtherModule[errors]: typeUsesFunc({#pe: InvalidEnum#}, {#pa: <<error type>>#}, {#pp: any InvalidProtocol#}, {#ps: InvalidStruct#}, {#pg: <<error type>>#}, {#pc: InvalidClass#})[#Int#];
  // INVALID-TOP-DAG: Decl[GlobalVar]/OtherModule[errors]: invalidGlobalKeypath[#InvalidStruct.Type#];
  // INVALID-TOP-DAG: Decl[TypeAlias]/OtherModule[errors]: InvalidAlias[#InvalidAlias#];
  // INVALID-TOP-DAG: Decl[FreeFunction]/OtherModule[errors]: invalidGenericFuncType({#param: T#})[#<<error type>>#];
  // INVALID-TOP-DAG: Decl[FreeFunction]/OtherModule[errors]: invalidFuncType()[#<<error type>>#];
  // INVALID-TOP-DAG: Decl[GlobalVar]/OtherModule[errors]: invalidGlobalClosureType[#() -> ()#];
  // INVALID-TOP-DAG: Decl[Class]/OtherModule[errors]:    InvalidClassSub1[#InvalidClassSub1#];
  // INVALID-TOP-DAG: Decl[Class]/OtherModule[errors]:    InvalidClassSub2[#InvalidClassSub2#];
  // INVALID-TOP-DAG: Decl[Protocol]/OtherModule[errors]/Flair[RareType]: InvalidProtocol[#InvalidProtocol#];
  // INVALID-TOP-DAG: Decl[FreeFunction]/OtherModule[errors]: invalidFuncThrows()[' throws'][#<<error type>>#];
}

// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -emit-module -experimental-allow-module-with-compiler-errors -primary-file %S/Inputs/errors-a.swift %S/Inputs/errors-b.swift %S/Inputs/errors-c.swift -module-name errors -o %t/errors.a.swiftmodule
// RUN: %target-swift-frontend -emit-module -experimental-allow-module-with-compiler-errors %S/Inputs/errors-a.swift -primary-file %S/Inputs/errors-b.swift %S/Inputs/errors-c.swift -module-name errors -o %t/errors.b.swiftmodule
// RUN: %target-swift-frontend -emit-module -experimental-allow-module-with-compiler-errors %S/Inputs/errors-a.swift %S/Inputs/errors-b.swift -primary-file %S/Inputs/errors-c.swift -module-name errors -o %t/errors.c.swiftmodule
// RUN: %target-swift-frontend -merge-modules -emit-module -experimental-allow-module-with-compiler-errors %t/errors.a.swiftmodule %t/errors.b.swiftmodule %t/errors.c.swiftmodule -module-name errors -o %t/errors.swiftmodule

// Read the module back in to make sure it can be deserialized
// RUN: %target-swift-ide-test -print-module -source-filename dummy -module-to-print errors -I %t -allow-compiler-errors | %FileCheck %s
// CHECK: typealias InvalidAlias = <<error type>>
// CHECK: class InvalidClass : <<error type>>, InvalidProtocol
// CHECK: var classMemberA: <<error type>>
// CHECK: init(param1: <<error type>>, param2: <<error type>>)
// CHECK: convenience init()
// CHECK: convenience init(param: <<error type>>)
// CHECK: class InvalidClassSub1 : InvalidClass
// CHECK: var classMemberB: <<error type>>
// CHECK: init(param1: <<error type>>, param2: <<error type>>)
// CHECK: convenience init()
// CHECK: class InvalidClassSub2 : InvalidClass
// CHECK: var classMemberC: <<error type>>
// CHECK: convenience init()
// CHECK: enum InvalidEnum
// CHECK: case enumeratorA
// CHECK: case enumeratorB
// CHECK: case enumeratorC
// CHECK: struct InvalidGenericStruct<T, U>
// CHECK: var genericMemberA: <<error type>>
// CHECK: protocol InvalidProtocol
// CHECK: associatedtype Item
// CHECK: mutating func add(_: <<error type>>)
// CHECK: func get() -> Self.Item
// CHECK: func set(item: Self.Item)
// CHECK: struct InvalidStruct : <<error type>>, InvalidProtocol
// CHECK: typealias Item = <<error type>>
// CHECK: let memberA: Int
// CHECK: let memberB: <<error type>>
// CHECK: var memberC: <<error type>>
// CHECK: lazy var memberD: <<error type>>
// CHECK: var memberE: <<error type>>
// CHECK: mutating func set(item: <<error type>>)
// CHECK: func invalidFuncBody()
// CHECK: func invalidFuncSignature()
// CHECK: func invalidFuncThrows() throws
// CHECK: func invalidFuncType() -> <<error type>>
// CHECK: func invalidGenericFuncBody<T>(param: T) -> T
// CHECK: func invalidGenericFuncType<T>(param: T) -> <<error type>>
// CHECK: invalidGlobalClosureBody:
// CHECK: invalidGlobalClosureType:
// CHECK: invalidGlobalKeypath:
// CHECK: invalidGlobalMissingInit: String
// CHECK: func invalidPartialFunc()
// CHECK: func typeUsesFunc

// Check completions
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t-completions -I %t -allow-compiler-errors

// Check cursor info for the various symbols
// RUN: %sourcekitd-test -req=cursor -pos=4:3 %s -- -Xfrontend -experimental-allow-module-with-compiler-errors -I %t -target %target-triple %s | %FileCheck %s -check-prefix=CHECK-GLOBAL
// CHECK-GLOBAL: source.lang.swift.ref.var.global
// CHECK-GLOBAL: invalidGlobalMissingInit

// RUN: %sourcekitd-test -req=cursor -pos=8:3 %s -- -Xfrontend -experimental-allow-module-with-compiler-errors -I %t -target %target-triple %s | %FileCheck %s -check-prefix=CHECK-FUNC
// CHECK-FUNC: source.lang.swift.ref.function.free
// CHECK-FUNC: invalidPartialFunc

// RUN: %sourcekitd-test -req=cursor -pos=12:12 %s -- -Xfrontend -experimental-allow-module-with-compiler-errors -I %t -target %target-triple %s | %FileCheck %s -check-prefix=CHECK-STRUCT
// CHECK-STRUCT: source.lang.swift.ref.struct
// CHECK-STRUCT: InvalidStruct

// Currently doesn't work for any members with invalid types, even within the same module: rdar://71514163
// RUN: %sourcekitd-test -req=cursor -pos=13:7 %s -- -Xfrontend -experimental-allow-module-with-compiler-errors -I %t -target %target-triple %s | not %FileCheck %s -check-prefix=CHECK-MEMBER
// CHECK-MEMBER: source.lang.swift.ref.var.instance
// CHECK-MEMBER: memberB
