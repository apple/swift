// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

// KW_RETURN: Keyword[return]/None: return{{; name=.+$}}
// KW_NO_RETURN-NOT: Keyword[return]

// KW_IN: Keyword[in]/None: in{{; name=.+$}}
// KW_NO_IN-NOT: Keyword[in]

// KW_DECL-DAG: Keyword[class]/None: class{{; name=.+$}}
// KW_DECL-DAG: Keyword/None: actor{{; name=.+$}}
// KW_DECL-DAG: Keyword/None: convenience{{; name=.+$}}
// KW_DECL-DAG: Keyword[deinit]/None: deinit{{; name=.+$}}
// KW_DECL-DAG: Keyword/None: dynamic{{; name=.+$}}
// KW_DECL-DAG: Keyword[enum]/None: enum{{; name=.+$}}
// KW_DECL-DAG: Keyword[extension]/None: extension{{; name=.+$}}
// KW_DECL-DAG: Keyword/None: final{{; name=.+$}}
// KW_DECL-DAG: Keyword[func]/None: func{{; name=.+$}}
// KW_DECL-DAG: Keyword[import]/None: import{{; name=.+$}}
// KW_DECL-DAG: Keyword/None: infix{{; name=.+$}}
// KW_DECL-DAG: Keyword[init]/None: init{{; name=.+$}}
// KW_DECL-DAG: Keyword[internal]/None: internal{{; name=.+$}}
// KW_DECL-DAG: Keyword/None: lazy{{; name=.+$}}
// KW_DECL-DAG: Keyword[let]/None: let{{; name=.+$}}
// KW_DECL-DAG: Keyword/None: mutating{{; name=.+$}}
// KW_DECL-DAG: Keyword/None: nonmutating{{; name=.+$}}
// KW_DECL-DAG: Keyword[operator]/None: operator{{; name=.+$}}
// KW_DECL-DAG: Keyword/None: optional{{; name=.+$}}
// KW_DECL-DAG: Keyword/None: override{{; name=.+$}}
// KW_DECL-DAG: Keyword/None: postfix{{; name=.+$}}
// KW_DECL-DAG: Keyword/None: prefix{{; name=.+$}}
// KW_DECL-DAG: Keyword[precedencegroup]: precedencegroup{{; name=.+$}}
// KW_DECL-DAG: Keyword[private]/None: private{{; name=.+$}}
// KW_DECL-DAG: Keyword[protocol]/None: protocol{{; name=.+$}}
// KW_DECL-DAG: Keyword[public]/None: public{{; name=.+$}}
// KW_DECL-DAG: Keyword/None: required{{; name=.+$}}
// KW_DECL-DAG: Keyword[static]/None: static{{; name=.+$}}
// KW_DECL-DAG: Keyword[struct]/None: struct{{; name=.+$}}
// KW_DECL-DAG: Keyword[subscript]/None: subscript{{; name=.+$}}
// KW_DECL-DAG: Keyword[typealias]/None: typealias{{; name=.+$}}
// KW_DECL-DAG: Keyword/None: unowned{{; name=.+$}}
// KW_DECL-DAG: Keyword[var]/None: var{{; name=.+$}}
// KW_DECL-DAG: Keyword/None: weak{{; name=.+$}}

// KW_DECL_PROTOCOL-DAG: Keyword[class]/None/Flair[RareKeyword]: class{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword/None/Flair[RareKeyword]: actor{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword/None: convenience{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword[deinit]/None: deinit{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword/None: dynamic{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword[enum]/None/Flair[RareKeyword]: enum{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword[extension]/None/Flair[RareKeyword]: extension{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword/None: final{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword[func]/None: func{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword[import]/None/Flair[RareKeyword]: import{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword/None: infix{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword[init]/None: init{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword[internal]/None: internal{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword/None: lazy{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword[let]/None: let{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword/None: mutating{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword/None: nonmutating{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword[operator]/None/Flair[RareKeyword]: operator{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword/None: optional{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword/None: override{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword/None: postfix{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword/None: prefix{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword[precedencegroup]/None/Flair[RareKeyword]: precedencegroup{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword[private]/None: private{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword[protocol]/None/Flair[RareKeyword]: protocol{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword[public]/None: public{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword/None: required{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword[static]/None: static{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword[struct]/None/Flair[RareKeyword]: struct{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword[subscript]/None: subscript{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword[typealias]/None: typealias{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword/None: unowned{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword[var]/None: var{{; name=.+$}}
// KW_DECL_PROTOCOL-DAG: Keyword/None: weak{{; name=.+$}}

// KW_DECL_TYPECONTEXT-DAG: Keyword[class]/None: class{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword/None: actor{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword/None: convenience{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword[deinit]/None: deinit{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword/None: dynamic{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword[enum]/None: enum{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword[extension]/None/Flair[RareKeyword]: extension{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword/None: final{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword[func]/None: func{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword[import]/None/Flair[RareKeyword]: import{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword/None: infix{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword[init]/None: init{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword[internal]/None: internal{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword/None: lazy{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword[let]/None: let{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword/None: mutating{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword/None: nonmutating{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword[operator]/None/Flair[RareKeyword]: operator{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword/None: optional{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword/None: override{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword/None: postfix{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword/None: prefix{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword[precedencegroup]/None/Flair[RareKeyword]: precedencegroup{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword[private]/None: private{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword[protocol]/None/Flair[RareKeyword]: protocol{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword[public]/None: public{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword/None: required{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword[static]/None: static{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword[struct]/None: struct{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword[subscript]/None: subscript{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword[typealias]/None: typealias{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword/None: unowned{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword[var]/None: var{{; name=.+$}}
// KW_DECL_TYPECONTEXT-DAG: Keyword/None: weak{{; name=.+$}}


//
// Declaration keywords.
//
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[class]/None: class{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword/None: actor{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword/None: convenience{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[deinit]/None: deinit{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword/None: dynamic{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[enum]/None: enum{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[extension]/None: extension{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword/None: final{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[func]/None: func{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[import]/None: import{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword/None: infix{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[init]/None: init{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[internal]/None: internal{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword/None: lazy{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[let]/None: let{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword/None: mutating{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword/None: nonmutating{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[operator]/None: operator{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword/None: optional{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword/None: override{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword/None: postfix{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[precedencegroup]/None: precedencegroup{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword/None: prefix{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[private]/None: private{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[protocol]/None: protocol{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[public]/None: public{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword/None: required{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[static]/None: static{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[struct]/None: struct{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[subscript]/None: subscript{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[typealias]/None: typealias{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword/None: unowned{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[var]/None: var{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword/None: weak{{; name=.+$}}
//
// Statement keywords.
//
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[if]/None: if{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[do]/None: do{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[else]/None: else{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[for]/None: for{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[while]/None: while{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[break]/None: break{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[continue]/None: continue{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[fallthrough]/None: fallthrough{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[switch]/None: switch{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[case]/None: case{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[default]/None: default{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[where]/None: where{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[catch]/None: catch{{; name=.+$}}
//
// Misc.
//
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[throw]/None: throw{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[try]/None: try{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[try]/None: try!{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Keyword[try]/None: try?{{; name=.+$}}
//
// Literals
//
// KW_DECL_STMT_TOPLEVEL-DAG: Literal[Boolean]/None: false[#Bool#]{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Literal[Boolean]/None: true[#Bool#]{{; name=.+$}}
// KW_DECL_STMT_TOPLEVEL-DAG: Literal[Nil]/None: nil{{; name=.+$}}

//
// Declaration keywords.
//
// KW_DECL_STMT-DAG: Keyword[class]/None/Flair[RareKeyword]: class{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword/None/Flair[RareKeyword]: convenience{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[deinit]/None/Flair[RareKeyword]: deinit{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword/None/Flair[RareKeyword]: dynamic{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[enum]/None/Flair[RareKeyword]: enum{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[extension]/None/Flair[RareKeyword]: extension{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword/None/Flair[RareKeyword]: final{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[func]/None: func{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[import]/None/Flair[RareKeyword]: import{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword/None/Flair[RareKeyword]: infix{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[init]/None/Flair[RareKeyword]: init{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[internal]/None/Flair[RareKeyword]: internal{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword/None/Flair[RareKeyword]: lazy{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[let]/None: let{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword/None: mutating{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword/None: nonmutating{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[operator]/None/Flair[RareKeyword]: operator{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword/None/Flair[RareKeyword]: optional{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword/None/Flair[RareKeyword]: override{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword/None/Flair[RareKeyword]: postfix{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[precedencegroup]/None/Flair[RareKeyword]: precedencegroup{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword/None/Flair[RareKeyword]: prefix{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[private]/None/Flair[RareKeyword]: private{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[protocol]/None/Flair[RareKeyword]: protocol{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[public]/None/Flair[RareKeyword]: public{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword/None/Flair[RareKeyword]: required{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[static]/None/Flair[RareKeyword]: static{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[struct]/None/Flair[RareKeyword]: struct{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[subscript]/None/Flair[RareKeyword]: subscript{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[typealias]/None: typealias{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword/None: unowned{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[var]/None: var{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword/None: weak{{; name=.+$}}
//
// Statement keywords.
//
// KW_DECL_STMT-DAG: Keyword[if]/None: if{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[do]/None: do{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[else]/None: else{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[for]/None: for{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[while]/None: while{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[break]/None: break{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[continue]/None: continue{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[fallthrough]/None: fallthrough{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[switch]/None: switch{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[case]/None: case{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[default]/None: default{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[where]/None: where{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[catch]/None: catch{{; name=.+$}}
//
// Misc.
//
// KW_DECL_STMT-DAG: Keyword[throw]/None: throw{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[try]/None: try{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[try]/None: try!{{; name=.+$}}
// KW_DECL_STMT-DAG: Keyword[try]/None: try?{{; name=.+$}}
//
// Literals
//
// KW_DECL_STMT-DAG: Literal[Boolean]/None: false[#Bool#]{{; name=.+$}}
// KW_DECL_STMT-DAG: Literal[Boolean]/None: true[#Bool#]{{; name=.+$}}
// KW_DECL_STMT-DAG: Literal[Nil]/None: nil{{; name=.+$}}


//
// Expressions
//
// KW_EXPR-DAG: Keyword[try]/None: try{{; name=.+$}}
// KW_EXPR-DAG: Keyword[try]/None: try!{{; name=.+$}}
// KW_EXPR-DAG: Keyword[try]/None: try?{{; name=.+$}}
//
// let and var
//
// KW_LETVAR-DAG: Keyword[let]/None: let{{; name=.+$}}
// KW_LETVAR-DAG: Keyword[var]/None: var{{; name=.+$}}
//
// KW_LETVAR_NEG-NOT: Keyword[let]/None: let{{; name=.+$}}
// KW_LETVAR_NEG-NOT: Keyword[var]/None: var{{; name=.+$}}
//
// Literals
//
// KW_EXPR-DAG: Literal[Boolean]/None{{(/TypeRelation\[Convertible\])?}}: false[#Bool#]{{; name=.+$}}
// KW_EXPR-DAG: Literal[Boolean]/None{{(/TypeRelation\[Convertible\])?}}: true[#Bool#]{{; name=.+$}}
// KW_EXPR-DAG: Literal[Nil]/None: nil{{; name=.+$}}

//
// Declaration keywords
//
// KW_EXPR_NEG-NOT: Keyword{{.*}}class
// KW_EXPR_NEG-NOT: Keyword{{.*}}convenience
// KW_EXPR_NEG-NOT: Keyword{{.*}}mutating
// KW_EXPR_NEG-NOT: Keyword{{.*}}weak
//
// Statement keywords
//
// KW_EXPR_NEG-NOT: Keyword{{.*}}throw
// KW_EXPR_NEG-NOT: Keyword{{.*}}while
// KW_EXPR_NEG-NOT: Keyword{{.*}}switch
// KW_EXPR_NEG-NOT: Keyword{{.*}}catch
// KW_EXPR_NEG-NOT: Keyword{{.*}}break

#^TOP_LEVEL_1?check=KW_DECL_STMT_TOPLEVEL;check=KW_NO_RETURN;check=KW_NO_IN^#

for _ in 1...10 {
  #^TOP_LEVEL_2?check=KW_DECL_STMT;check=KW_NO_RETURN;check=KW_NO_IN^#
}

if true {} #^TOP_LEVEL_AFTER_IF_1?check=KW_DECL_STMT_TOPLEVEL;check=KW_NO_RETURN;check=KW_NO_IN^#
if true {} 
#^TOP_LEVEL_AFTER_IF_2?check=KW_DECL_STMT_TOPLEVEL;check=KW_NO_RETURN;check=KW_NO_IN^#


if true {} else #^TOP_LEVEL_AFTER_IF_ELSE_1?check=AFTER_IF_ELSE^# {}

// AFTER_IF_ELSE: Begin completions, 1 items
// AFTER_IF_ELSE: Keyword[if]/None: if;

func testAfterIf1() {
  if true {} #^AFTER_IF_1?check=KW_DECL_STMT;check=KW_RETURN;check=KW_NO_IN^#
}
func testAfterIfElse1() {
  if true {} else #^AFTER_IF_ELSE_1?check=AFTER_IF_ELSE^# {}
}

func testInFuncBody1() {
  #^IN_FUNC_BODY_1?check=KW_DECL_STMT;check=KW_RETURN;check=KW_NO_IN^#
}

struct InStructFunc {
  func testInFuncBody2() {
    #^IN_FUNC_BODY_2?check=KW_DECL_STMT;check=KW_RETURN;check=KW_NO_IN^#
  }
}

enum InEnumFunc {
  func testInFuncBody3() {
    #^IN_FUNC_BODY_3?check=KW_DECL_STMT;check=KW_RETURN;check=KW_NO_IN^#
  }
}

class InClassFunc {
  func testInFuncBody4() {
    #^IN_FUNC_BODY_4?check=KW_DECL_STMT;check=KW_RETURN;check=KW_NO_IN^#
  }
}

class InClassFunc {
  class Nested {
    func testInFuncBody5() {
      #^IN_FUNC_BODY_5?check=KW_DECL_STMT;check=KW_RETURN;check=KW_NO_IN^#
    }
  }
}

func testInClosure1() {
  { #^IN_CLOSURE_1?check=KW_DECL_STMT;check=KW_RETURN;check=KW_IN^# }
}
func testInClosure2() {
  { #^IN_CLOSURE_2?check=KW_DECL_STMT;check=KW_RETURN;check=KW_IN^#
}
struct InVarClosureInit {
  let x = { #^IN_CLOSURE_3?check=KW_DECL_STMT;check=KW_RETURN;check=KW_IN^# }()
}

{ #^IN_CLOSURE_4?check=KW_DECL_STMT;check=KW_RETURN;check=KW_IN^# }

struct InSubscript {
  subscript(x: Int) -> Int { #^IN_SUBSCRIPT_1?check=KW_DECL_STMT;check=KW_RETURN;check=KW_NO_IN^# }
}

struct InInit {
  init?() { #^IN_INIT_1?check=KW_DECL_STMT;check=KW_RETURN;check=KW_NO_IN^# }
}

struct InStruct {
  #^IN_NOMINAL_DECL_1?check=KW_DECL_TYPECONTEXT^#
}

enum InEnum {
  #^IN_NOMINAL_DECL_2?check=KW_DECL_TYPECONTEXT^#
}

class InClass {
  #^IN_NOMINAL_DECL_3?check=KW_DECL_TYPECONTEXT^#
}

protocol InProtocol {
  #^IN_NOMINAL_DECL_4?check=KW_DECL_PROTOCOL^#
}

struct AfterOtherKeywords1 {
  public #^IN_NOMINAL_DECL_5?check=KW_DECL_TYPECONTEXT^#
}

struct AfterOtherKeywords2 {
  mutating #^IN_NOMINAL_DECL_6?check=KW_DECL_TYPECONTEXT^#
}

class AfterOtherKeywords3 {
  override #^IN_NOMINAL_DECL_7?check=KW_DECL_TYPECONTEXT^#
}

class AfterOtherKeywords4 {
  public override #^IN_NOMINAL_DECL_8?check=KW_DECL_TYPECONTEXT^#
}

extension InStruct {
  #^IN_NOMINAL_DECL_9?check=KW_DECL_TYPECONTEXT^#
}

extension InProtocol {
  #^IN_NOMINAL_DECL_10?check=KW_DECL_TYPECONTEXT^#
}

class SuperSuperClass {
   func f1() {
    #^SUPER_KEYWORD0^#
// SUPER_KEYWORD0-NOT: Keyword{{.*}}super
  }
}

class SuperClass : SuperSuperClass {
   func f2() {
    #^SUPER_KEYWORD1^#
  }
// SUPER_KEYWORD1: Keyword[super]/CurrNominal:                       super[#SuperSuperClass#]; name=super{{$}}
}

class SubClass : SuperClass {
  func f3() {
    #^SUPER_KEYWORD2^#
  }
// SUPER_KEYWORD2: Keyword[super]/CurrNominal:                       super[#SuperClass#]; name=super{{$}}
}

extension SubClass {
  func f4() {
    #^SUPER_KEYWORD3^#
  }
// SUPER_KEYWORD3: Keyword[super]/CurrNominal:                       super[#SuperClass#]; name=super{{$}}
}

func inExpr1() {
  (#^EXPR_1?check=KW_EXPR;check=KW_LETVAR;check=KW_EXPR_NEG^#)
}
func inExpr2() {
  let x = #^EXPR_2?check=KW_EXPR;check=KW_LETVAR;check=KW_EXPR_NEG^#
}
func inExpr3() {
  if #^EXPR_3?check=KW_EXPR;check=KW_LETVAR;check=KW_EXPR_NEG^# {}
}
func inExpr4() {
  let x = 1
  x + #^EXPR_4?check=KW_EXPR;check=KW_LETVAR;check=KW_EXPR_NEG^#
}
func inExpr5() {
  var x: Int
  x = #^EXPR_5?check=KW_EXPR;check=KW_LETVAR;check=KW_EXPR_NEG^#
}
func inExpr6() -> Int {
  // Make sure we don't recommend 'let' and 'var' here.
  return #^EXPR_6?check=KW_EXPR;check=KW_EXPR_NEG;check=KW_LETVAR_NEG^#
}
func inExpr7() {
  // Make sure we don't recommend 'let' and 'var' here.
  for x in #^EXPR_7?check=KW_EXPR;check=KW_EXPR_NEG;check=KW_LETVAR_NEG^#
}

func inSwitch(val: Int) {
  switch val {
  #^SWITCH_TOP?check=KW_CASE^#
  case 1:
    foo()
  #^SWITCH_IN_CASE?check=KW_CASE^#
  }
// KW_CASE-DAG: Keyword[case]/None:                 case; name=case
// KW_CASE-DAG: Keyword[default]/None:              default; name=default
}
func testContextualType() {
  let _: UInt32 = #^CONTEXT_UINT32^#
// CONTEXT_UINT32: Begin completions
// CONTEXT_UINT32: End completions

  let _: StaticString = #^CONTEXT_STATICSTRING^#
// CONTEXT_STATICSTRING: Begin completions
// CONTEXT_STATICSTRING: End completions
}

class Base {
  func foo() {}
}
class Derivied: Base {
  override func foo() {
    #^OVERRIDE^#
// OVERRIDE-DAG: Keyword[super]/CurrNominal/Flair[CommonKeyword]: super[#Base#]; name=super
  }
}
