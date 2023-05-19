// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -disable-objc-attr-requires-foundation-module -o %t %S/Inputs/AnyObject/foo_swift_module.swift
// RUN: %target-swift-frontend -emit-module -disable-objc-attr-requires-foundation-module -o %t %S/Inputs/AnyObject/bar_swift_module.swift
// RUN: cp %S/Inputs/AnyObject/baz_clang_module.h %t
// RUN: cp %S/Inputs/AnyObject/module.map %t

// RUN: %target-swift-ide-test -code-completion -source-filename %s -I %t -disable-objc-attr-requires-foundation-module -code-completion-token=DL_FUNC_PARAM_NO_DOT_1 > %t.dl.txt
// RUN: %FileCheck %s -check-prefix=DL_INSTANCE_NO_DOT < %t.dl.txt
// RUN: %FileCheck %s -check-prefix=GLOBAL_NEGATIVE < %t.dl.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -I %t -disable-objc-attr-requires-foundation-module -code-completion-token=DL_FUNC_PARAM_DOT_1 > %t.dl.txt
// RUN: %FileCheck %s -check-prefix=DL_INSTANCE_DOT < %t.dl.txt
// RUN: %FileCheck %s -check-prefix=GLOBAL_NEGATIVE < %t.dl.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -I %t -disable-objc-attr-requires-foundation-module -code-completion-token=DL_VAR_NO_DOT_1 > %t.dl.txt
// RUN: %FileCheck %s -check-prefix=DL_INSTANCE_NO_DOT < %t.dl.txt
// RUN: %FileCheck %s -check-prefix=GLOBAL_NEGATIVE < %t.dl.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -I %t -disable-objc-attr-requires-foundation-module -code-completion-token=DL_VAR_DOT_1 > %t.dl.txt
// RUN: %FileCheck %s -check-prefix=DL_INSTANCE_DOT < %t.dl.txt
// RUN: %FileCheck %s -check-prefix=GLOBAL_NEGATIVE < %t.dl.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -I %t -disable-objc-attr-requires-foundation-module -code-completion-token=DL_RETURN_VAL_NO_DOT_1 > %t.dl.txt
// RUN: %FileCheck %s -check-prefix=DL_INSTANCE_NO_DOT < %t.dl.txt
// RUN: %FileCheck %s -check-prefix=GLOBAL_NEGATIVE < %t.dl.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -I %t -disable-objc-attr-requires-foundation-module -code-completion-token=DL_RETURN_VAL_DOT_1 > %t.dl.txt
// RUN: %FileCheck %s -check-prefix=DL_INSTANCE_DOT < %t.dl.txt
// RUN: %FileCheck %s -check-prefix=GLOBAL_NEGATIVE < %t.dl.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -I %t -disable-objc-attr-requires-foundation-module -code-completion-token=DL_CALL_RETURN_VAL_NO_DOT_1 > %t.dl.txt
// RUN: %FileCheck %s -check-prefix=TLOC_MEMBERS_NO_DOT < %t.dl.txt
// RUN: %FileCheck %s -check-prefix=GLOBAL_NEGATIVE < %t.dl.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -I %t -disable-objc-attr-requires-foundation-module -code-completion-token=DL_CALL_RETURN_VAL_DOT_1 > %t.dl.txt
// RUN: %FileCheck %s -check-prefix=TLOC_MEMBERS_DOT < %t.dl.txt
// RUN: %FileCheck %s -check-prefix=GLOBAL_NEGATIVE < %t.dl.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -I %t -disable-objc-attr-requires-foundation-module -code-completion-token=DL_FUNC_NAME_1 > %t.dl.txt
// RUN: %FileCheck %s -check-prefix=DL_FUNC_NAME_1 < %t.dl.txt
// RUN: %FileCheck %s -check-prefix=GLOBAL_NEGATIVE < %t.dl.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -I %t -disable-objc-attr-requires-foundation-module -code-completion-token=DL_FUNC_NAME_PAREN_1 > %t.dl.txt
// RUN: %FileCheck %s -check-prefix=DL_FUNC_NAME_PAREN_1 < %t.dl.txt
// RUN: %FileCheck %s -check-prefix=GLOBAL_NEGATIVE < %t.dl.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -I %t -disable-objc-attr-requires-foundation-module -code-completion-token=DL_FUNC_NAME_BANG_1 > %t.dl.txt
// RUN: %FileCheck %s -check-prefix=DL_FUNC_NAME_BANG_1 < %t.dl.txt
// RUN: %FileCheck %s -check-prefix=GLOBAL_NEGATIVE < %t.dl.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -I %t -disable-objc-attr-requires-foundation-module -code-completion-token=DL_CLASS_NO_DOT_1 > %t.dl.txt
// RUN: %FileCheck %s -check-prefix=DL_CLASS_NO_DOT < %t.dl.txt
// RUN: %FileCheck %s -check-prefix=GLOBAL_NEGATIVE < %t.dl.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -I %t -disable-objc-attr-requires-foundation-module -code-completion-token=DL_CLASS_DOT_1 > %t.dl.txt
// RUN: %FileCheck %s -check-prefix=DL_CLASS_DOT < %t.dl.txt
// RUN: %FileCheck %s -check-prefix=GLOBAL_NEGATIVE < %t.dl.txt

// RUN: %target-swift-ide-test -code-completion -source-filename %s -I %t -disable-objc-attr-requires-foundation-module -code-completion-token=INITIALIZE_PAREN | %FileCheck %s -check-prefix=INITIALIZE_PAREN
// RUN: %target-swift-ide-test -code-completion -source-filename %s -I %t -disable-objc-attr-requires-foundation-module -code-completion-token=GLOBAL_WITHINIT -code-complete-inits-in-postfix-expr | %FileCheck %s -check-prefix=GLOBAL_WITHINIT

// REQUIRES: objc_interop

import foo_swift_module
import class bar_swift_module.Bar_ImportedObjcClass
import baz_clang_module

//===---
//===--- Helper types that are used in this test.
//===---

@objc class Base {}
@objc class Derived : Base {}

protocol Foo { func foo() }
protocol Bar { func bar() }

//===---
//===--- Types that contain members accessible by dynamic lookup.
//===---

// GLOBAL_NEGATIVE-NOT: ERROR

// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[bar_swift_module]: .bar_ImportedObjcClass_InstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceVar]/OtherModule[bar_swift_module]:    .bar_ImportedObjcClass_Property1[#Int?#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   .base1_InstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   .base1_InstanceFunc2!({#(a): Derived#})[#Void#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   .base1_InstanceFunc3!({#(a): Derived#})[#Void#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   .base1_InstanceFunc4!()[#Base#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceVar]/OtherModule[swift_ide_test]:      .base1_Property1[#Int?#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceVar]/OtherModule[swift_ide_test]:      .base1_Property2[#Base?#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[baz_clang_module]: .baz_Class_InstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[baz_clang_module]: .baz_Protocol_InstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[foo_swift_module]: .foo_Nested1_ObjcInstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceVar]/OtherModule[foo_swift_module]:    .foo_Nested1_Property1[#Int?#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[foo_swift_module]: .foo_Nested2_ObjcInstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceVar]/OtherModule[foo_swift_module]:    .foo_Nested2_Property1[#Int?#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[foo_swift_module]: .foo_TopLevelClass_ObjcInstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceVar]/OtherModule[foo_swift_module]:    .foo_TopLevelClass_ObjcProperty1[#Int?#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[foo_swift_module]: .foo_TopLevelObjcClass_InstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceVar]/OtherModule[foo_swift_module]:    .foo_TopLevelObjcClass_Property1[#Int?#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[foo_swift_module]: .foo_TopLevelObjcProtocol_InstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceVar]/OtherModule[foo_swift_module]:    .foo_TopLevelObjcProtocol_Property1[#Int?#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   .nested1_ObjcInstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceVar]/OtherModule[swift_ide_test]:      .nested1_Property1[#Int?#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   .nested2_ObjcInstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceVar]/OtherModule[swift_ide_test]:      .nested2_Property[#Int?#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   .returnsObjcClass!({#(i): Int#})[#TopLevelObjcClass#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   .topLevelClass_ObjcInstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceVar]/OtherModule[swift_ide_test]:      .topLevelClass_ObjcProperty1[#Int?#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   .topLevelObjcClass_InstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceVar]/OtherModule[swift_ide_test]:      .topLevelObjcClass_Property1[#Int?#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   .topLevelObjcProtocol_InstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[InstanceVar]/OtherModule[swift_ide_test]:      .topLevelObjcProtocol_Property1[#Int?#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[Subscript]/OtherModule[bar_swift_module]:      [{#(i): Bar_ImportedObjcClass#}][#Int?#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[Subscript]/OtherModule[foo_swift_module]:      [{#(i): any Foo_TopLevelObjcProtocol#}][#Int?#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[Subscript]/OtherModule[swift_ide_test]:        [{#(i): Int16#}][#Int?#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[Subscript]/OtherModule[foo_swift_module]:      [{#(i): Int32#}][#Int?#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[Subscript]/OtherModule[foo_swift_module]:      [{#(i): Int64#}][#Int?#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[Subscript]/OtherModule[swift_ide_test]:        [{#(i): Int8#}][#Int?#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[Subscript]/OtherModule[swift_ide_test]:        [{#(i): TopLevelObjcClass#}][#Int?#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[Subscript]/OtherModule[swift_ide_test]:        [{#(i): any TopLevelObjcProtocol#}][#Int?#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[Subscript]/OtherModule[baz_clang_module]:      [{#(idx): Int32#}][#Any??#]{{; name=.+$}}
// DL_INSTANCE_NO_DOT-DAG: Decl[Subscript]/OtherModule[baz_clang_module]:      [{#(key): Any!#}][#Any??#]{{; name=.+$}}
// GLOBAL_NEGATIVE-NOT:.objectAtIndexedSubscript

// DL_INSTANCE_DOT-DAG: Decl[InstanceMethod]/OtherModule[bar_swift_module]: bar_ImportedObjcClass_InstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceVar]/OtherModule[bar_swift_module]:    bar_ImportedObjcClass_Property1[#Int?#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   base1_InstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   base1_InstanceFunc2!({#(a): Derived#})[#Void#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   base1_InstanceFunc3!({#(a): Derived#})[#Void#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   base1_InstanceFunc4!()[#Base#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceVar]/OtherModule[swift_ide_test]:      base1_Property1[#Int?#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceVar]/OtherModule[swift_ide_test]:      base1_Property2[#Base?#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceMethod]/OtherModule[baz_clang_module]: baz_Class_InstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceVar]/OtherModule[baz_clang_module]:    baz_Class_Property1[#Baz_Class!?#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceVar]/OtherModule[baz_clang_module]:    baz_Class_Property2[#Baz_Class!?#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceMethod]/OtherModule[baz_clang_module]: baz_Protocol_InstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceMethod]/OtherModule[foo_swift_module]: foo_Nested1_ObjcInstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceVar]/OtherModule[foo_swift_module]:    foo_Nested1_Property1[#Int?#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceMethod]/OtherModule[foo_swift_module]: foo_Nested2_ObjcInstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceVar]/OtherModule[foo_swift_module]:    foo_Nested2_Property1[#Int?#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceMethod]/OtherModule[foo_swift_module]: foo_TopLevelClass_ObjcInstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceVar]/OtherModule[foo_swift_module]:    foo_TopLevelClass_ObjcProperty1[#Int?#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceMethod]/OtherModule[foo_swift_module]: foo_TopLevelObjcClass_InstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceVar]/OtherModule[foo_swift_module]:    foo_TopLevelObjcClass_Property1[#Int?#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceMethod]/OtherModule[foo_swift_module]: foo_TopLevelObjcProtocol_InstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceVar]/OtherModule[foo_swift_module]:    foo_TopLevelObjcProtocol_Property1[#Int?#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   nested1_ObjcInstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceVar]/OtherModule[swift_ide_test]:      nested1_Property1[#Int?#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   nested2_ObjcInstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceVar]/OtherModule[swift_ide_test]:      nested2_Property[#Int?#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   returnsObjcClass!({#(i): Int#})[#TopLevelObjcClass#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   topLevelClass_ObjcInstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceVar]/OtherModule[swift_ide_test]:      topLevelClass_ObjcProperty1[#Int?#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   topLevelObjcClass_InstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceVar]/OtherModule[swift_ide_test]:      topLevelObjcClass_Property1[#Int?#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   topLevelObjcProtocol_InstanceFunc1!()[#Void#]{{; name=.+$}}
// DL_INSTANCE_DOT-DAG: Decl[InstanceVar]/OtherModule[swift_ide_test]:      topLevelObjcProtocol_Property1[#Int?#]{{; name=.+$}}

// DL_CLASS_NO_DOT-DAG: Decl[StaticMethod]/OtherModule[bar_swift_module]:   .bar_ImportedObjcClass_ClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[bar_swift_module]: .bar_ImportedObjcClass_InstanceFunc1({#(self): Bar_ImportedObjcClass#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   .base1_InstanceFunc1({#(self): Base1#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   .base1_InstanceFunc2({#(self): Base1#})[#(Derived) -> Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   .base1_InstanceFunc3({#(self): Base1#})[#(Derived) -> Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   .base1_InstanceFunc4({#(self): Base1#})[#() -> Base#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[StaticMethod]/OtherModule[baz_clang_module]:   .baz_Class_ClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[baz_clang_module]: .baz_Class_InstanceFunc1({#(self): Baz_Class#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[StaticMethod]/OtherModule[baz_clang_module]:   .baz_Protocol_ClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[baz_clang_module]: .baz_Protocol_InstanceFunc1({#(self): Baz_Protocol#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[StaticMethod]/OtherModule[foo_swift_module]:   .foo_Nested1_ObjcClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[foo_swift_module]: .foo_Nested1_ObjcInstanceFunc1({#(self): Foo_ContainerForNestedClass1.Foo_Nested1#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[StaticMethod]/OtherModule[foo_swift_module]:   .foo_Nested2_ObjcClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[foo_swift_module]: .foo_Nested2_ObjcInstanceFunc1({#(self): Foo_ContainerForNestedClass2.Foo_Nested2#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[StaticMethod]/OtherModule[foo_swift_module]:   .foo_TopLevelClass_ObjcClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[foo_swift_module]: .foo_TopLevelClass_ObjcInstanceFunc1({#(self): Foo_TopLevelClass#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[StaticMethod]/OtherModule[foo_swift_module]:   .foo_TopLevelObjcClass_ClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[foo_swift_module]: .foo_TopLevelObjcClass_InstanceFunc1({#(self): Foo_TopLevelObjcClass#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[StaticMethod]/OtherModule[foo_swift_module]:   .foo_TopLevelObjcProtocol_ClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[foo_swift_module]: .foo_TopLevelObjcProtocol_InstanceFunc1({#(self): Foo_TopLevelObjcProtocol#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[StaticMethod]/OtherModule[swift_ide_test]:     .nested1_ObjcClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   .nested1_ObjcInstanceFunc1({#(self): ContainerForNestedClass1.Nested1#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[StaticMethod]/OtherModule[swift_ide_test]:     .nested2_ObjcClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   .nested2_ObjcInstanceFunc1({#(self): ContainerForNestedClass2.Nested2#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   .returnsObjcClass({#(self): TopLevelObjcClass#})[#(Int) -> TopLevelObjcClass#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[StaticMethod]/OtherModule[swift_ide_test]:     .topLevelClass_ObjcClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   .topLevelClass_ObjcInstanceFunc1({#(self): TopLevelClass#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[StaticMethod]/OtherModule[swift_ide_test]:     .topLevelObjcClass_ClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   .topLevelObjcClass_InstanceFunc1({#(self): TopLevelObjcClass#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[StaticMethod]/OtherModule[swift_ide_test]:     .topLevelObjcProtocol_ClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_NO_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   .topLevelObjcProtocol_InstanceFunc1({#(self): TopLevelObjcProtocol#})[#() -> Void#]{{; name=.+$}}

// DL_CLASS_DOT-DAG: Decl[StaticMethod]/OtherModule[bar_swift_module]:   bar_ImportedObjcClass_ClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[InstanceMethod]/OtherModule[bar_swift_module]: bar_ImportedObjcClass_InstanceFunc1({#(self): Bar_ImportedObjcClass#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   base1_InstanceFunc1({#(self): Base1#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   base1_InstanceFunc2({#(self): Base1#})[#(Derived) -> Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   base1_InstanceFunc3({#(self): Base1#})[#(Derived) -> Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   base1_InstanceFunc4({#(self): Base1#})[#() -> Base#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[StaticMethod]/OtherModule[baz_clang_module]:   baz_Class_ClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[InstanceMethod]/OtherModule[baz_clang_module]: baz_Class_InstanceFunc1({#(self): Baz_Class#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[StaticMethod]/OtherModule[baz_clang_module]:   baz_Protocol_ClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[InstanceMethod]/OtherModule[baz_clang_module]: baz_Protocol_InstanceFunc1({#(self): Baz_Protocol#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[StaticMethod]/OtherModule[foo_swift_module]:   foo_Nested1_ObjcClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[InstanceMethod]/OtherModule[foo_swift_module]: foo_Nested1_ObjcInstanceFunc1({#(self): Foo_ContainerForNestedClass1.Foo_Nested1#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[StaticMethod]/OtherModule[foo_swift_module]:   foo_Nested2_ObjcClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[InstanceMethod]/OtherModule[foo_swift_module]: foo_Nested2_ObjcInstanceFunc1({#(self): Foo_ContainerForNestedClass2.Foo_Nested2#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[StaticMethod]/OtherModule[foo_swift_module]:   foo_TopLevelClass_ObjcClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[InstanceMethod]/OtherModule[foo_swift_module]: foo_TopLevelClass_ObjcInstanceFunc1({#(self): Foo_TopLevelClass#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[StaticMethod]/OtherModule[foo_swift_module]:   foo_TopLevelObjcClass_ClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[InstanceMethod]/OtherModule[foo_swift_module]: foo_TopLevelObjcClass_InstanceFunc1({#(self): Foo_TopLevelObjcClass#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[StaticMethod]/OtherModule[foo_swift_module]:   foo_TopLevelObjcProtocol_ClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[InstanceMethod]/OtherModule[foo_swift_module]: foo_TopLevelObjcProtocol_InstanceFunc1({#(self): Foo_TopLevelObjcProtocol#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[StaticMethod]/OtherModule[swift_ide_test]:     nested1_ObjcClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   nested1_ObjcInstanceFunc1({#(self): ContainerForNestedClass1.Nested1#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[StaticMethod]/OtherModule[swift_ide_test]:     nested2_ObjcClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   nested2_ObjcInstanceFunc1({#(self): ContainerForNestedClass2.Nested2#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   returnsObjcClass({#(self): TopLevelObjcClass#})[#(Int) -> TopLevelObjcClass#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[StaticMethod]/OtherModule[swift_ide_test]:     topLevelClass_ObjcClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   topLevelClass_ObjcInstanceFunc1({#(self): TopLevelClass#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[StaticMethod]/OtherModule[swift_ide_test]:     topLevelObjcClass_ClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   topLevelObjcClass_InstanceFunc1({#(self): TopLevelObjcClass#})[#() -> Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[StaticMethod]/OtherModule[swift_ide_test]:     topLevelObjcProtocol_ClassFunc1()[#Void#]{{; name=.+$}}
// DL_CLASS_DOT-DAG: Decl[InstanceMethod]/OtherModule[swift_ide_test]:   topLevelObjcProtocol_InstanceFunc1({#(self): TopLevelObjcProtocol#})[#() -> Void#]{{; name=.+$}}

// TLOC_MEMBERS_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .returnsObjcClass({#(i): Int#})[#TopLevelObjcClass#]{{; name=.+$}}
// TLOC_MEMBERS_NO_DOT-DAG: Decl[InstanceMethod]/CurrNominal: .topLevelObjcClass_InstanceFunc1()[#Void#]{{; name=.+$}}
// TLOC_MEMBERS_NO_DOT-DAG: Decl[Subscript]/CurrNominal:      [{#(i): Int8#}][#Int#]{{; name=.+$}}
// TLOC_MEMBERS_NO_DOT-DAG: Decl[InstanceVar]/CurrNominal:    .topLevelObjcClass_Property1[#Int#]{{; name=.+$}}
// TLOC_MEMBERS_NO_DOT-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: === {#AnyObject?#}[#Bool#];
// TLOC_MEMBERS_NO_DOT-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: !== {#AnyObject?#}[#Bool#];
// TLOC_MEMBERS_NO_DOT-DAG: Keyword[self]/CurrNominal: .self[#TopLevelObjcClass#]; name=self

// TLOC_MEMBERS_DOT-DAG: Keyword[self]/CurrNominal: self[#TopLevelObjcClass#]; name=self
// TLOC_MEMBERS_DOT-DAG: Decl[InstanceMethod]/CurrNominal: returnsObjcClass({#(i): Int#})[#TopLevelObjcClass#]{{; name=.+$}}
// TLOC_MEMBERS_DOT-DAG: Decl[InstanceMethod]/CurrNominal: topLevelObjcClass_InstanceFunc1()[#Void#]{{; name=.+$}}
// TLOC_MEMBERS_DOT-DAG: Decl[InstanceVar]/CurrNominal:    topLevelObjcClass_Property1[#Int#]{{; name=.+$}}

// FIXME: Properties in Clang modules.
// There's a test already: baz_Protocol_Property1.
// Blocked by: rdar://15136550 Properties in protocols not implemented

@objc class TopLevelObjcClass {
  @objc func returnsObjcClass(_ i: Int) -> TopLevelObjcClass {}

  @objc func topLevelObjcClass_InstanceFunc1() {}
  @objc class func topLevelObjcClass_ClassFunc1() {}
  @objc subscript(i: Int8) -> Int {
    get {
      return 0
    }
  }
  @objc var topLevelObjcClass_Property1: Int
}

@objc class TopLevelObjcClass_DuplicateMembers {
  @objc func topLevelObjcClass_InstanceFunc1() {}
  @objc class func topLevelObjcClass_ClassFunc1() {}
  @objc subscript(i: Int8) -> Int {
    get {
      return 0
    }
  }
  @objc var topLevelObjcClass_Property1: Int
}

class TopLevelClass {
  @objc func topLevelClass_ObjcInstanceFunc1() {}
  @objc class func topLevelClass_ObjcClassFunc1() {}
  @objc subscript (i: Int16) -> Int {
    get {
      return 0
    }
  }
  @objc var topLevelClass_ObjcProperty1: Int

  func ERROR() {}
  typealias ERROR = Int
  subscript (i: ERROR) -> Int {
    get {
      return 0
    }
  }
  var ERROR_Property: Int
}

@objc protocol TopLevelObjcProtocol {
  @objc func topLevelObjcProtocol_InstanceFunc1()
  @objc class func topLevelObjcProtocol_ClassFunc1()
  @objc subscript (i: TopLevelObjcClass) -> Int { get set }
  @objc var topLevelObjcProtocol_Property1: Int { get set }
}

class ContainerForNestedClass1 {
  class Nested1 {
    @objc func nested1_ObjcInstanceFunc1() {}
    @objc class func nested1_ObjcClassFunc1() {}
    @objc var nested1_Property1: Int

    func ERROR() {}
    typealias ERROR = Int
    subscript (i: ERROR) -> Int {
      get {
        return 0
      }
    }
    var ERROR_Property: Int
  }
  func ERROR() {}
}

struct ContainerForNestedClass2 {
  class Nested2 {
    @objc func nested2_ObjcInstanceFunc1() {}
    @objc class func nested2_ObjcClassFunc1() {}
    @objc subscript (i: TopLevelObjcProtocol) -> Int {
      get {
        return 0
      }
    }
    @objc var nested2_Property: Int

    func ERROR() {}
    var ERROR_Property: Int
  }
  func ERROR() {}
}

class GenericContainerForNestedClass1<T> {
  class Nested3 {
    func ERROR1() {}
    func ERROR2() {}
    class func ERROR3() {}
    typealias ERROR = Int
    subscript (i: ERROR) -> Int {
      get {
        return 0
      }
    }
    var ERROR_Property: Int
  }
  func ERROR() {}
}

struct GenericContainerForNestedClass2<T> {
  class Nested3 {
    func ERROR1() {}
    func ERROR2() {}
    class func ERROR3() {}
    typealias ERROR = Int
    subscript (i: ERROR) -> Int {
      get {
        return 0
      }
    }
    var ERROR_Property: Int
  }
  func ERROR() {}
}

@objc class Base1 {
  @objc func base1_InstanceFunc1() {}

  @objc func base1_InstanceFunc2(_ a: Derived) {}

  @objc func base1_InstanceFunc3(_ a: Derived) {}

  @objc func base1_InstanceFunc4() -> Base {}

  @objc var base1_Property1: Int

  @objc var base1_Property2: Base
}

@objc class Derived1 : Base1 {
  @objc func base1_InstanceFunc1() {}

  @objc func base1_InstanceFunc2(_ a: Derived) {}

  @objc func base1_InstanceFunc3(_ a: Base) {}

  @objc func base1_InstanceFunc4() -> Derived {}

  @objc var base1_Property1: Int {
    get {
      return 0
    }
    set {}
  }

  @objc var base1_Property2: Derived {
    get {
      return Derived()
    }
    set {}
  }
}

func returnsAnyObject() -> AnyObject {
  return TopLevelClass()
}

func testAnyObject1(_ dl: AnyObject) {
  dl#^DL_FUNC_PARAM_NO_DOT_1^#
}

func testAnyObject2(_ dl: AnyObject) {
  dl.#^DL_FUNC_PARAM_DOT_1^#
}

func testAnyObject3() {
  var dl: AnyObject = TopLevelClass()
  dl#^DL_VAR_NO_DOT_1^#
}

func testAnyObject4() {
  var dl: AnyObject = TopLevelClass()
  dl.#^DL_VAR_DOT_1^#
}

func testAnyObject5() {
  returnsAnyObject()#^DL_RETURN_VAL_NO_DOT_1^#
}

func testAnyObject6() {
  returnsAnyObject().#^DL_RETURN_VAL_DOT_1^#
}

func testAnyObject7(_ dl: AnyObject) {
  dl.returnsObjcClass!(42)#^DL_CALL_RETURN_VAL_NO_DOT_1^#
}

func testAnyObject8(_ dl: AnyObject) {
  dl.returnsObjcClass!(42).#^DL_CALL_RETURN_VAL_DOT_1^#
}

func testAnyObject9() {
  // FIXME: this syntax is not implemented yet.
  // dl.returnsObjcClass?(42)#^DL_CALL_RETURN_OPTIONAL_NO_DOT_1^#
}

func testAnyObject10() {
  // FIXME: this syntax is not implemented yet.
  // dl.returnsObjcClass?(42).#^DL_CALL_RETURN_OPTIONAL_DOT_1^#
}

func testAnyObject11(_ dl: AnyObject) {
  dl.returnsObjcClass#^DL_FUNC_NAME_1^#
}
// FIXME: it would be nice if we produced a call pattern here.
// DL_FUNC_NAME_1:     Begin completions

func testAnyObject11_(_ dl: AnyObject) {
  dl.returnsObjcClass!(#^DL_FUNC_NAME_PAREN_1^#
}
// DL_FUNC_NAME_PAREN_1-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ['(']{#(i): Int#}[')'][#TopLevelObjcClass#]{{; name=.+$}}

func testAnyObject12(_ dl: AnyObject) {
  dl.returnsObjcClass.#^DL_FUNC_NAME_DOT_1^#
}
// FIXME: it would be nice if we produced a call pattern here.

func testAnyObject13(_ dl: AnyObject) {
  dl.returnsObjcClass!#^DL_FUNC_NAME_BANG_1^#
}
// DL_FUNC_NAME_BANG_1-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ({#(i): Int#})[#TopLevelObjcClass#]
// DL_FUNC_NAME_BANG_1-DAG: Keyword[self]/CurrNominal: .self[#(Int) -> TopLevelObjcClass#]; name=self

func testAnyObject14() {
  // FIXME: this syntax is not implemented yet.
  // dl.returnsObjcClass?#^DL_FUNC_QUESTION_1^#
}

func testAnyObjectClassMethods1(_ dl: AnyObject) {
  type(of: dl)#^DL_CLASS_NO_DOT_1^#
}

func testAnyObjectClassMethods2(_ dl: AnyObject) {
  type(of: dl).#^DL_CLASS_DOT_1^#
}

func testAnyObjectInitialize() {
  AnyObject(#^INITIALIZE_PAREN^#)
// INITIALIZE_PAREN-NOT: Flair[ArgLabels]
// INITIALIZE_PAREN-NOT: name=int:
}

func testGlobalInitializer() {
  #^GLOBAL_WITHINIT^#
// GLOBAL_WITHINIT-NOT: name=AnyObject(
// GLOBAL_WITHINIT-DAG: Decl[TypeAlias]/OtherModule[Swift]/IsSystem: AnyObject[#Builtin.AnyObject#]; name=AnyObject
// GLOBAL_WITHINIT-NOT: name=AnyObject(
}
