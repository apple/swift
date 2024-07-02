// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/mods)
// RUN: %empty-directory(%t/co-same)
// RUN: %empty-directory(%t/co-across)
// RUN: split-file %s %t

// Completion in the current file/module
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t/co-same

// Completion across modules
// RUN: %target-swift-frontend -emit-module %t/MacroDefinitions.swift -o %t/mods/ -experimental-allow-module-with-compiler-errors
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %t/MacroUses.swift -filecheck %raw-FileCheck -completion-output-dir %t/co-across -I %t/mods -D SEPARATED

//--- MacroDefinitions.swift
@freestanding(expression)
public macro freestandingExprIntMacro() -> Int

@freestanding(expression)
public macro freestandingExprStringMacro() -> String

@freestanding(expression)
public macro freestandingExprTMacro<T>(_ value: T) -> T

@freestanding(declaration)
public macro freestandingDeclMacro()

@freestanding(codeItem)
public macro freestandingCodeItemMacro()

@attached(accessor)
public macro AttachedAccessorMacro()

@attached(member)
public macro AttachedMemberMacro()

@attached(member)
public macro AttachedMemberMacroWithArgs(arg1: Int)

public enum Direction {
  case up, down
}

@attached(member)
public macro AttachedMemberMacroWithEnumArgs(_ direction: Direction)

@attached(member)
public macro AttachedMemberMacroWithMultipleArgs(first: Int, second: Int)

@attached(memberAttribute)
public macro AttachedMemberAttributeMacro()

@attached(peer)
public macro AttachedPeerMacro()

@attached(extension)
public macro AttachedConformanceMacro()

@attached(body)
public macro BodyMacro()

@freestanding(expression)
@freestanding(declaration)
@attached(accessor)
@attached(member)
@attached(memberAttribute)
@attached(peer)
@attached(extension)
public macro EverythingMacro()

//--- MacroUses.swift
#if SEPARATED
import MacroDefinitions
#endif

@#^CLASS_ATTR?check=NOMINAL_ATTR;check=NOMINAL_ATTR_NOT^# class C {}
@#^EXTRA_FILTER?check=NOMINAL_ATTR;check=NOMINAL_ATTR_NOT^#IB class C2 {}
@#^ENUM_ATTR?check=NOMINAL_ATTR;check=NOMINAL_ATTR_NOT^# enum E {}
@#^STRUCT_ATTR?check=NOMINAL_ATTR;check=NOMINAL_ATTR_NOT^# struct S{}
// NOMINAL_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedMemberMacro[#Member Macro#]; name=AttachedMemberMacro
// NOMINAL_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedMemberMacroWithArgs({#arg1: Int#})[#Member Macro#]; name=AttachedMemberMacroWithArgs
// NOMINAL_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedMemberAttributeMacro[#Member Attribute Macro#]; name=AttachedMemberAttributeMacro
// NOMINAL_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedPeerMacro[#Peer Macro#]; name=AttachedPeerMacro
// NOMINAL_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedConformanceMacro[#Extension Macro#]; name=AttachedConformanceMacro
// NOMINAL_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: EverythingMacro[#Expression Macro, Declaration Macro, Accessor Macro, Member Attribute Macro, Member Macro, Peer Macro, Extension Macro#]; name=EverythingMacro
//
// NOMINAL_ATTR_NOT-NOT: freestanding
// NOMINAL_ATTR_NOT-NOT: AttachedAccessorMacro
// NOMINAL_ATTR_NOT-NOT: BodyMacro

@#^FUNC_ATTR?check=FUNC_ATTR;check=FUNC_ATTR_NOT^# func method() {}
struct MethodAttrs {
  var x: Int {
    @#^ACCESSOR_ATTR?check=FUNC_ATTR;check=FUNC_ATTR_NOT^# get { 0 }
  }
  @#^INIT_ATTR?check=FUNC_ATTR;check=FUNC_ATTR_NOT^# init() {}
  @#^DEINIT_ATTR?check=FUNC_ATTR;check=FUNC_ATTR_NOT^# deinit{}
  @#^METHOD_ATTR?check=FUNC_ATTR;check=FUNC_ATTR_NOT^# func method() {}
}
// FUNC_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedPeerMacro[#Peer Macro#]; name=AttachedPeerMacro
// FUNC_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: BodyMacro[#Body Macro#]; name=BodyMacro
// FUNC_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: EverythingMacro[#Expression Macro, Declaration Macro, Accessor Macro, Member Attribute Macro, Member Macro, Peer Macro, Extension Macro#]; name=EverythingMacro
//
// FUNC_ATTR_NOT-NOT: freestanding
// FUNC_ATTR_NOT-NOT: AttachedAccessorMacro
// FUNC_ATTR_NOT-NOT: AttachedMemberMacro
// FUNC_ATTR_NOT-NOT: AttachedMemberMacroWithArgs
// FUNC_ATTR_NOT-NOT: AttachedConformanceMacro

@#^GLOBAL_ATTR?check=VAR_ATTR;check=VAR_ATTR_NOT^# var globalVar
struct PropAttr {
  @#^PROP_ATTR?check=VAR_ATTR;check=VAR_ATTR_NOT^# var propVar
  func localAttr() {
    @#^LOCAL_ATTR?check=VAR_ATTR;check=VAR_ATTR_NOT^# var localVar
  }
}
// VAR_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedAccessorMacro[#Accessor Macro#]; name=AttachedAccessorMacro
// VAR_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedPeerMacro[#Peer Macro#]; name=AttachedPeerMacro
// VAR_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: EverythingMacro[#Expression Macro, Declaration Macro, Accessor Macro, Member Attribute Macro, Member Macro, Peer Macro, Extension Macro#]; name=EverythingMacro
//
// VAR_ATTR_NOT-NOT: freestanding
// VAR_ATTR_NOT-NOT: AttachedMemberMacro
// VAR_ATTR_NOT-NOT: AttachedMemberMacroWithArgs
// VAR_ATTR_NOT-NOT: AttachedMemberAttributeMacro
// VAR_ATTR_NOT-NOT: AttachedConformanceMacro
// VAR_ATTR_NOT-NOT: BodyMacro

func paramAttr(@#^PARAM_ATTR?check=PARAM_ATTR^#) {}
func paramAttr2(@#^PARAM2_ATTR?check=PARAM_ATTR^# arg: Int) {}
// TODO: These should both be PARAM_ATTR
func takeNoArgClosure(_: (Int) -> Void) {
  takeClosure { @#^NO_ARG_CLOSURE_ATTR?check=INDEPENDENT_ATTR;check=INDEPENDENT_ATTR_NOT^# in
    print("x")
  }
}
func takeNoArgClosure(_: () -> Void) {
  takeClosure { @#^CLOSURE_ATTR?check=INDEPENDENT_ATTR;check=INDEPENDENT_ATTR_NOT^# in
    print("x")
  }
}
// PARAM_ATTR-NOT: freestanding
// PARAM_ATTR-NOT: AttachedAccessorMacro
// PARAM_ATTR-NOT: AttachedMemberMacro
// PARAM_ATTR-NOT: AttachedMemberMacroWithArgs
// PARAM_ATTR-NOT: AttachedMemberAttributeMacro
// PARAM_ATTR-NOT: AttachedPeerMacro
// PARAM_ATTR-NOT: AttachedConformanceMacro
// PARAM_ATTR-NOT: BodyMacro
// PARAM_ATTR-NOT: EverythingMacro

##^TOP_LEVEL_FREESTANDING?check=ALL_FREESTANDING;check=ALL_FREESTANDING_NOT^#
func nestedFreestanding() {
  ##^TOP_NESTED_FREESTANDING?check=ALL_FREESTANDING;check=ALL_FREESTANDING_NOT^#
}
// ALL_FREESTANDING-DAG: Decl[Macro]/{{.*}}: freestandingDeclMacro[#Declaration Macro#]; name=freestandingDeclMacro
// ALL_FREESTANDING-DAG: Decl[Macro]/{{.*}}: freestandingCodeItemMacro[#Code Item Macro#]; name=freestandingCodeItemMacro
// ALL_FREESTANDING-DAG: Decl[Macro]/{{.*}}: freestandingExprIntMacro[#Int#]; name=freestandingExprIntMacro
// ALL_FREESTANDING-DAG: Decl[Macro]/{{.*}}: freestandingExprStringMacro[#String#]; name=freestandingExprStringMacro
// ALL_FREESTANDING-DAG: Decl[Macro]/{{.*}}: freestandingExprTMacro({#(value): T#})[#T#]; name=freestandingExprTMacro(:)
// ALL_FREESTANDING-DAG: Decl[Macro]/{{.*}}: EverythingMacro[#Expression Macro, Declaration Macro, Accessor Macro, Member Attribute Macro, Member Macro, Peer Macro, Extension Macro#]; name=EverythingMacro
//
// ALL_FREESTANDING_NOT-NOT: Attached
// ALL_FREESTANDING_NOT-NOT: BodyMacro

func exprFreestanding(arg: Int) {
  _ = arg + ##^EXPR_FREESTANDING?check=EXPR_FREESTANDING;check=EXPR_FREESTANDING_NOT^#
}
// EXPR_FREESTANDING-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: freestandingExprIntMacro[#Int#]; name=freestandingExprIntMacro
// EXPR_FREESTANDING-DAG: Decl[Macro]/{{.*}}: freestandingExprStringMacro[#String#]; name=freestandingExprStringMacro
// EXPR_FREESTANDING-DAG: Decl[Macro]/{{.*}}: freestandingExprTMacro({#(value): T#})[#T#]; name=freestandingExprTMacro(:)
// TODO: This should be invalid in both same module and across modules
// EXPR_FREESTANDING-DAG: Decl[Macro]/{{.*}}: EverythingMacro[#Expression Macro, Declaration Macro, Accessor Macro, Member Attribute Macro, Member Macro, Peer Macro, Extension Macro#]; name=EverythingMacro
//
// EXPR_FREESTANDING_NOT-NOT: freestandingDeclMacro
// EXPR_FREESTANDING_NOT-NOT: freestandingCodeItemMacro
// EXPR_FREESTANDING_NOT-NOT: Attached
// EXPR_FREESTANDING_NOT-NOT: BodyMacro

struct NestedFreestanding {
  ##^TYPE_NESTED_FREESTANDING?check=ITEM_FREESTANDING;check=ITEM_FREESTANDING_NOT^#
}
// ITEM_FREESTANDING-DAG: Decl[Macro]/{{.*}}: freestandingDeclMacro[#Declaration Macro#]; name=freestandingDeclMacro
// ITEM_FREESTANDING-DAG: Decl[Macro]/{{.*}}: EverythingMacro[#Expression Macro, Declaration Macro, Accessor Macro, Member Attribute Macro, Member Macro, Peer Macro, Extension Macro#]; name=EverythingMacro
//
// ITEM_FREESTANDING_NOT-NOT: Attached
// ITEM_FREESTANDING_NOT-NOT: freestandingExpr
// ITEM_FREESTANDING_NOT-NOT: freestandingCodeItemMacro
// ITEM_FREESTANDING_NOT-NOT: BodyMacro

@AttachedMemberMacroWithEnumArgs(.#^ATTACHED_MACRO_ARG^#)
struct AttachedMacroArg {}

// ATTACHED_MACRO_ARG-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: up[#Direction#]; name=up
// ATTACHED_MACRO_ARG-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: down[#Direction#]; name=down

@AttachedMemberMacroWithMultipleArgs(first: 1, #^ATTACHED_MACRO_SECOND_ARG_LABEL^#)
struct AttachedMacroSecondArgLabel {}

// ATTACHED_MACRO_SECOND_ARG_LABEL: Pattern/Local/Flair[ArgLabels]:     {#second: Int#}[#Int#]; name=second:


struct LastMember {
  @#^LAST_MEMBER_ATTR?check=INDEPENDENT_ATTR;check=INDEPENDENT_ATTR_NOT^#
}
@#^INDEPENDENT?check=INDEPENDENT_ATTR;check=INDEPENDENT_ATTR_NOT^#
// INDEPENDENT_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedAccessorMacro[#Accessor Macro#]; name=AttachedAccessorMacro
// INDEPENDENT_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedMemberMacro[#Member Macro#]; name=AttachedMemberMacro
// INDEPENDENT_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedMemberMacroWithArgs({#arg1: Int#})[#Member Macro#]; name=AttachedMemberMacroWithArgs
// INDEPENDENT_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedMemberAttributeMacro[#Member Attribute Macro#]; name=AttachedMemberAttributeMacro
// INDEPENDENT_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedPeerMacro[#Peer Macro#]; name=AttachedPeerMacro
// INDEPENDENT_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedConformanceMacro[#Extension Macro#]; name=AttachedConformanceMacro
// INDEPENDENT_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: BodyMacro[#Body Macro#]; name=BodyMacro
// INDEPENDENT_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: EverythingMacro[#Expression Macro, Declaration Macro, Accessor Macro, Member Attribute Macro, Member Macro, Peer Macro, Extension Macro#]; name=EverythingMacro
//
// INDEPENDENT_ATTR_NOT-NOT: freestandingExprMacro
// INDEPENDENT_ATTR_NOT-NOT: freestandingDeclMacro
