// RUN: %batch-code-completion

func foo(x: #^FUNC_PARAM?check=SPECIFIER^#) {
  let fn1 = { (x: #CLOSURE_PARAM?check=SPECIFIER#) in }
  let fn2 = { (x: consuming #CLOSURE_PARAM2?check=SPECIFIER#) in }
  let fn3: (#^CLOSURE_PARAM_TY_COMPLETE?check=SPECIFIER^#) -> Void
  let fn4: (borrowing #^CLOSURE_PARAM_TY_COMPLETE2?check=SPECIFIER^#) -> Void
  let fn5: (#^CLOSURE_PARAM_TY_INCOMPLETE?check=SPECIFIER^#
  let fn6: (inout #^CLOSURE_PARAM_TY_INCOMPLETE2?check=SPECIFIER^#
}

func bar(_ x: borrowing #^FUNC_PARAM_2?check=SPECIFIER^#) {}

struct S {
  init(x: #^INIT_PARAM?check=SPECIFIER^#) {}
  subscript(x: #SUBSCRIPT_PARAM?check=SPECIFIER#) -> #^SUBSCRIPT_RET?check=RESULT;check=RESULT_NOT^# {}
}

// Don't complete for enum cases.
enum E {
  case e(#^ENUM_CASE_TY?check=SPECIFIER_NOT^#)
  case f(x: #^ENUM_CASE_LABELED_TY?check=SPECIFIER_NOT^#)
}

// Don't complete the parameter specifiers for a variable type.
//
// Note that we will still complete 'sending' here, even though it isn't
// currently supported for computed properties (it is supported for functions
// and subscripts though).
let x: #^VAR_TY?check=RESULT;check=RESULT_NOT^#
var y: #^VAR_TY2?check=RESULT;check=RESULT_NOT^#

// Or for a return type.
func bar() -> #^RESULT_TY?check=RESULT;check=RESULT_NOT^# {}

// SPECIFIER-DAG: Keyword[inout]/None: inout; name=inout
// SPECIFIER-DAG: Keyword/None: consuming; name=consuming
// SPECIFIER-DAG: Keyword/None: borrowing; name=borrowing
// SPECIFIER-DAG: Keyword/None: isolated; name=isolated
// SPECIFIER-DAG: Keyword/None: sending; name=sending

// SPECIFIER_NOT-NOT: Keyword[inout]/None: inout
// SPECIFIER_NOT-NOT: Keyword/None: consuming
// SPECIFIER_NOT-NOT: Keyword/None: borrowing
// SPECIFIER_NOT-NOT: Keyword/None: isolated
// SPECIFIER_NOT-NOT: Keyword/None: sending

// RESULT_NOT-NOT: Keyword[inout]/None: inout
// RESULT_NOT-NOT: Keyword/None: consuming
// RESULT_NOT-NOT: Keyword/None: borrowing
// RESULT_NOT-NOT: Keyword/None: isolated

// RESULT-DAG: Keyword/None: sending
