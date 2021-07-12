// RUN: %target-swift-frontend -dump-parse %s | %FileCheck %s

// CR
_ = """
"""
//CHECK: string_literal_expr {{.*}} "" encoding=utf8

_ = """
  test
  """
//CHECK: string_literal_expr {{.*}} "test" encoding=utf8

// CR+LF
_ = """
    """
//CHECK: string_literal_expr {{.*}} "" encoding=utf8

_ = """
  test
  """
//CHECK: string_literal_expr {{.*}} "test" encoding=utf8

// CR+LF
_ = """
    """
//CHECK: string_literal_expr {{.*}} "" encoding=utf8
_ = """
  test
  test
  """
//CHECK: string_literal_expr {{.*}} "test\ntest" encoding=utf8

// LF+CR
_ = """

    foo

    foo

    """
//CHECK: string_literal_expr {{.*}} "\nfoo\n\nfoo\n" encoding=utf8

// LF+CR+LF
_ = """

    foo

    foo

    """
//CHECK: string_literal_expr {{.*}} "\nfoo\n\nfoo\n" encoding=utf8

// Mixed no-indent.
_ = """
<LF
<LF
<CR
<CR+LF
"""
//CHECK: string_literal_expr {{.*}} "<LF\n<LF\n<CR\n<CR+LF" encoding=utf8

// Mixed indent.
_ = """
	 <LF
	 <LF
	 <CR
	 <CR+LF
	 """
//CHECK: string_literal_expr {{.*}} "<LF\n<LF\n<CR\n<CR+LF" encoding=utf8

// Empty line CR, CR+LF, LF.
_ = """
   foo



   bar
   """
//CHECK: string_literal_expr {{.*}} "foo\n\n\n\nbar" encoding=utf8
