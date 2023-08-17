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

@#^CLASS_ATTR?check=NOMINAL_ATTR^# class C {}
@#^EXTRA_FILTER?check=NOMINAL_ATTR^#IB class C2 {}
@#^ENUM_ATTR?check=NOMINAL_ATTR^# enum E {}
@#^STRUCT_ATTR?check=NOMINAL_ATTR^# struct S{}
// NOMINAL_ATTR-NOT: freestanding
// NOMINAL_ATTR-NOT: AttachedAccessorMacro
// NOMINAL_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedMemberMacro; name=AttachedMemberMacro
// NOMINAL_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedMemberMacroWithArgs({#arg1: Int#}); name=AttachedMemberMacroWithArgs
// NOMINAL_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedMemberAttributeMacro; name=AttachedMemberAttributeMacro
// NOMINAL_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedPeerMacro; name=AttachedPeerMacro
// NOMINAL_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedConformanceMacro; name=AttachedConformanceMacro
// NOMINAL_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: EverythingMacro; name=EverythingMacro

@#^FUNC_ATTR?check=DECL_ATTR^# func method() {}
struct MethodAttrs {
  @#^INIT_ATTR?check=DECL_ATTR^# init() {}
  @#^DEINIT_ATTR?check=DECL_ATTR^# deinit{}
  @#^METHOD_ATTR?check=DECL_ATTR^# func method() {}
}
// DECL_ATTR-NOT: freestanding
// DECL_ATTR-NOT: AttachedAccessorMacro
// DECL_ATTR-NOT: AttachedMemberMacro
// DECL_ATTR-NOT: AttachedMemberMacroWithArgs
// DECL_ATTR-NOT: AttachedConformanceMacro
// DECL_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedPeerMacro; name=AttachedPeerMacro
// DECL_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: EverythingMacro; name=EverythingMacro

@#^GLOBAL_ATTR?check=VAR_ATTR^# var globalVar
struct PropAttr {
  @#^PROP_ATTR?check=VAR_ATTR^# var propVar
  func localAttr() {
    @#^LOCAL_ATTR?check=VAR_ATTR^# var localVar
  }
}
// VAR_ATTR-NOT: freestanding
// VAR_ATTR-NOT: AttachedMemberMacro
// VAR_ATTR-NOT: AttachedMemberMacroWithArgs
// VAR_ATTR-NOT: AttachedMemberAttributeMacro
// VAR_ATTR-NOT: AttachedConformanceMacro
// VAR_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedAccessorMacro; name=AttachedAccessorMacro
// VAR_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedPeerMacro; name=AttachedPeerMacro
// VAR_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: EverythingMacro; name=EverythingMacro

func paramAttr(@#^PARAM_ATTR?check=PARAM_ATTR^#) {}
func paramAttr2(@#^PARAM2_ATTR?check=PARAM_ATTR^# arg: Int) {}
// TODO: These should both be PARAM_ATTR
func takeNoArgClosure(_: (Int) -> Void) {
  takeClosure { @#^NO_ARG_CLOSURE_ATTR?check=INDEPENDENT_ATTR^# in
    print("x")
  }
}
func takeNoArgClosure(_: () -> Void) {
  takeClosure { @#^CLOSURE_ATTR?check=INDEPENDENT_ATTR^# in
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
// PARAM_ATTR-NOT: EverythingMacro

##^TOP_LEVEL_FREESTANDING?check=ALL_FREESTANDING^#
func nestedFreestanding() {
  ##^TOP_NESTED_FREESTANDING?check=ALL_FREESTANDING^#
}
// ALL_FREESTANDING-NOT: Attached
// ALL_FREESTANDING-DAG: Decl[Macro]/{{.*}}: freestandingDeclMacro; name=freestandingDeclMacro
// ALL_FREESTANDING-DAG: Decl[Macro]/{{.*}}: freestandingCodeItemMacro; name=freestandingCodeItemMacro
// ALL_FREESTANDING-DAG: Decl[Macro]/{{.*}}: freestandingExprIntMacro[#Int#]; name=freestandingExprIntMacro
// ALL_FREESTANDING-DAG: Decl[Macro]/{{.*}}: freestandingExprStringMacro[#String#]; name=freestandingExprStringMacro
// ALL_FREESTANDING-DAG: Decl[Macro]/{{.*}}: freestandingExprTMacro({#(value): T#})[#T#]; name=freestandingExprTMacro(:)
// ALL_FREESTANDING-DAG: Decl[Macro]/{{.*}}: EverythingMacro; name=EverythingMacro

func exprFreestanding(arg: Int) {
  _ = arg + ##^EXPR_FREESTANDING^#
}
// EXPR_FREESTANDING-NOT: freestandingDeclMacro
// EXPR_FREESTANDING-NOT: freestandingCodeItemMacro
// EXPR_FREESTANDING-NOT: Attached
// EXPR_FREESTANDING-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: freestandingExprIntMacro[#Int#]; name=freestandingExprIntMacro
// EXPR_FREESTANDING-DAG: Decl[Macro]/{{.*}}: freestandingExprStringMacro[#String#]; name=freestandingExprStringMacro
// EXPR_FREESTANDING-DAG: Decl[Macro]/{{.*}}: freestandingExprTMacro({#(value): T#})[#T#]; name=freestandingExprTMacro(:)
// TODO: This should be invalid in both same module and across modules
// EXPR_FREESTANDING-DAG: Decl[Macro]/{{.*}}: EverythingMacro; name=EverythingMacro

struct NestedFreestanding {
  ##^TYPE_NESTED_FREESTANDING?check=ITEM_FREESTANDING^#
}
// ITEM_FREESTANDING-NOT: Attached
// ITEM_FREESTANDING-NOT: freestandingExpr
// ITEM_FREESTANDING-NOT: freestandingCodeItemMacro
// ITEM_FREESTANDING-DAG: Decl[Macro]/{{.*}}: freestandingDeclMacro; name=freestandingDeclMacro
// ITEM_FREESTANDING-DAG: Decl[Macro]/{{.*}}: EverythingMacro; name=EverythingMacro


@AttachedMemberMacroWithEnumArgs(.#^ATTACHED_MACRO_ARG^#)
struct AttachedMacroArg {}

// ATTACHED_MACRO_ARG-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: up[#Direction#]; name=up
// ATTACHED_MACRO_ARG-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: down[#Direction#]; name=down

@AttachedMemberMacroWithMultipleArgs(first: 1, #^ATTACHED_MACRO_SECOND_ARG_LABEL^#)
struct AttachedMacroSecondArgLabel {}

// ATTACHED_MACRO_SECOND_ARG_LABEL: Pattern/Local/Flair[ArgLabels]:     {#second: Int#}[#Int#]; name=second:


struct LastMember {
  @#^LAST_MEMBER_ATTR?check=INDEPENDENT_ATTR^#
}
@#^INDEPENDENT?check=INDEPENDENT_ATTR^#
// INDEPENDENT_ATTR-NOT: freestandingExprMacro
// INDEPENDENT_ATTR-NOT: freestandingDeclMacro
// INDEPENDENT_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedAccessorMacro; name=AttachedAccessorMacro
// INDEPENDENT_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedMemberMacro; name=AttachedMemberMacro
// INDEPENDENT_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedMemberMacroWithArgs({#arg1: Int#}); name=AttachedMemberMacroWithArgs
// INDEPENDENT_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedMemberAttributeMacro; name=AttachedMemberAttributeMacro
// INDEPENDENT_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedPeerMacro; name=AttachedPeerMacro
// INDEPENDENT_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: AttachedConformanceMacro; name=AttachedConformanceMacro
// INDEPENDENT_ATTR-DAG: Decl[Macro]/{{.*}}/TypeRelation[Convertible]: EverythingMacro; name=EverythingMacro
