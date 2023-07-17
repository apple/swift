class Foo {}
class Base {
  init() {}
  func base() {}
}
class FooBar {
  init() {}
  init(x: Foo) {}
  static func fooBar() {}
  static func fooBaz() {}
  func method() {}
  let prop: FooBar { return FooBar() }
  subscript(x: Foo) -> Foo {}
}

enum E1 {
  case one
  case two
}

// Note: this test uses line:column inputs as a workaround for a complete-test limitation.
func test010(x: E1, y: FooBar) {
  switch x {
    case .:
      break
    case two:
      y.
  }
}

// RUN: %sourcekitd-test -req=complete.open -pos=24:11 -req-opts=filtertext=on %s -- %s | %FileCheck %s -check-prefix=INNER_POSTFIX_0a
// INNER_POSTFIX_0a-NOT: key.description: "one{{.+}}"
// INNER_POSTFIX_0a: key.description: "one",{{$}}
// INNER_POSTFIX_0a-NOT: key.description: "one{{.+}}"

// RUN: %sourcekitd-test -req=complete.open -pos=24:11 -req-opts=filtertext=one %s -- %s | %FileCheck %s -check-prefix=INNER_POSTFIX_0b
// INNER_POSTFIX_0b-NOT: key.description: "one{{.+}}"
// INNER_POSTFIX_0b: key.description: "one",{{$}}
// INNER_POSTFIX_0b: key.description: "one.",{{$}}
// INNER_POSTFIX_0b: key.description: "one==",{{$}}
// INNER_POSTFIX_0b: key.description: "one!=",{{$}}
// INNER_POSTFIX_0b: key.description: "one~=",{{$}}
// INNER_POSTFIX_0b-NOT: key.description: "one{{.+}}"

// RUN: %sourcekitd-test -req=complete.open -pos=27:9 -req-opts=filtertext=pro %s -- %s | %FileCheck %s -check-prefix=INNER_POSTFIX_1
// INNER_POSTFIX_1-NOT: key.description: "prop{{.+}}"
// INNER_POSTFIX_1: key.description: "prop",{{$}}
// INNER_POSTFIX_1-NOT: key.description: "prop{{.+}}"


func test001() {
  #^TOP_LEVEL_0,fo,foo,foob,foobar^#
}
// RUN: %complete-test %s -no-fuzz -group=none -add-inner-results -tok=TOP_LEVEL_0 | %FileCheck %s -check-prefix=TOP_LEVEL_0
// TOP_LEVEL_0-LABEL: Results for filterText: fo [
// TOP_LEVEL_0-NEXT:   for
// TOP_LEVEL_0-NEXT:   Foo
// TOP_LEVEL_0-NEXT:   FooBar
// TOP_LEVEL_0: ]

// TOP_LEVEL_0-LABEL: Results for filterText: foo [
// TOP_LEVEL_0-NEXT:   Foo
// TOP_LEVEL_0-NEXT:   Foo.
// TOP_LEVEL_0-NEXT:   Foo(
// TOP_LEVEL_0-NEXT:   FooBar
// TOP_LEVEL_0-NEXT:   Foo.self
// TOP_LEVEL_0-NEXT:   Foo.Type
// TOP_LEVEL_0-NEXT:   Foo()
// TOP_LEVEL_0-NEXT: ]

// TOP_LEVEL_0-LABEL: Results for filterText: foob [
// TOP_LEVEL_0-NEXT:   FooBar
// TOP_LEVEL_0-NEXT: ]

// TOP_LEVEL_0-LABEL: Results for filterText: foobar [
// TOP_LEVEL_0-NEXT:   FooBar
// TOP_LEVEL_0-NEXT:   FooBar.
// TOP_LEVEL_0-NEXT:   FooBar(
// TOP_LEVEL_0-NEXT:   FooBar.self
// TOP_LEVEL_0-NEXT:   FooBar.Type
// TOP_LEVEL_0-NEXT:   FooBar()
// TOP_LEVEL_0-NEXT:   FooBar(x: Foo)
// TOP_LEVEL_0-NEXT:   FooBar.fooBar()
// TOP_LEVEL_0-NEXT:   FooBar.fooBaz()
// TOP_LEVEL_0: ]

func test002(abc: FooBar, abd: Base) {
  #^TOP_LEVEL_1,ab,abc,abd^#
}
// RUN: %complete-test %s -no-fuzz -group=none -add-inner-results -tok=TOP_LEVEL_1 | %FileCheck %s -check-prefix=TOP_LEVEL_1
// TOP_LEVEL_1-LABEL: Results for filterText: ab [
// TOP_LEVEL_1-NEXT:   abc
// TOP_LEVEL_1-NEXT:   abd
// TOP_LEVEL_1: ]

// TOP_LEVEL_1-LABEL: Results for filterText: abc [
// TOP_LEVEL_1-NEXT:   abc
// TOP_LEVEL_1-NEXT:   abc.
// TOP_LEVEL_1-NEXT:   abc===
// TOP_LEVEL_1-NEXT:   abc!==
// TOP_LEVEL_1-NEXT:   abc.self
// TOP_LEVEL_1-NEXT:   abc.method()
// TOP_LEVEL_1-NEXT:   abc.prop
// TOP_LEVEL_1-NEXT: ]

// TOP_LEVEL_1-LABEL: Results for filterText: abd [
// TOP_LEVEL_1-NEXT:   abd
// TOP_LEVEL_1-NEXT:   abd.
// TOP_LEVEL_1-NEXT:   abd===
// TOP_LEVEL_1-NEXT:   abd!==
// TOP_LEVEL_1-NEXT:   abd.self
// TOP_LEVEL_1-NEXT:   abd.base()
// TOP_LEVEL_1-NEXT: ]

func test003(x: FooBar) {
  x.#^FOOBAR_QUALIFIED,pro,prop,prop.^#
}
// RUN: %complete-test %s -group=none -add-inner-results -tok=FOOBAR_QUALIFIED | %FileCheck %s -check-prefix=FOOBAR_QUALIFIED
// FOOBAR_QUALIFIED-LABEL: Results for filterText: pro [
// FOOBAR_QUALIFIED-NEXT:   prop
// FOOBAR_QUALIFIED-NEXT: ]

// FOOBAR_QUALIFIED-LABEL: Results for filterText: prop [
// FOOBAR_QUALIFIED-NEXT:   prop
// FOOBAR_QUALIFIED-NEXT:   prop.
// FOOBAR_QUALIFIED:   prop.method()
// FOOBAR_QUALIFIED-NEXT:   prop.prop
// FOOBAR_QUALIFIED-NEXT: ]

// Just don't explode.  We generally expect to get a new session here.
// FOOBAR_QUALIFIED-LABEL: Results for filterText: prop. [
// FOOBAR_QUALIFIED-NEXT: ]

// RUN: %complete-test %s -group=none -no-inner-results -inner-operators -tok=FOOBAR_QUALIFIED | %FileCheck %s -check-prefix=FOOBAR_QUALIFIED_OP
// FOOBAR_QUALIFIED_OP-LABEL: Results for filterText: pro [
// FOOBAR_QUALIFIED_OP-NEXT:   prop
// FOOBAR_QUALIFIED_OP-NEXT: ]
// FOOBAR_QUALIFIED_OP-LABEL: Results for filterText: prop [
// FOOBAR_QUALIFIED_OP-NEXT:   prop
// FOOBAR_QUALIFIED_OP-NEXT:   prop.
// FOOBAR_QUALIFIED_OP: ]

// RUN: %complete-test %s -group=none -add-inner-results -no-inner-operators -tok=FOOBAR_QUALIFIED | %FileCheck %s -check-prefix=FOOBAR_QUALIFIED_NOOP
// FOOBAR_QUALIFIED_NOOP-LABEL: Results for filterText: pro [
// FOOBAR_QUALIFIED_NOOP-NEXT:   prop
// FOOBAR_QUALIFIED_NOOP-NEXT: ]
// FOOBAR_QUALIFIED_NOOP-LABEL: Results for filterText: prop [
// FOOBAR_QUALIFIED_NOOP-NEXT:   prop
// FOOBAR_QUALIFIED_NOOP-NEXT:   prop.self
// FOOBAR_QUALIFIED_NOOP-NEXT:   prop.method()
// FOOBAR_QUALIFIED_NOOP-NEXT:   prop.prop
// FOOBAR_QUALIFIED_NOOP-NEXT: ]

// RUN: %complete-test %s -group=none -no-include-exact-match -add-inner-results -no-inner-operators -tok=FOOBAR_QUALIFIED | %FileCheck %s -check-prefix=FOOBAR_QUALIFIED_NOEXACT
// FOOBAR_QUALIFIED_NOEXACT-LABEL: Results for filterText: prop [
// FOOBAR_QUALIFIED_NOEXACT-NEXT:   prop.self
// FOOBAR_QUALIFIED_NOEXACT-NEXT:   prop.method()
// FOOBAR_QUALIFIED_NOEXACT-NEXT:   prop.prop
// FOOBAR_QUALIFIED_NOEXACT-NEXT: ]

func test004() {
  FooBar#^FOOBAR_POSTFIX^#
}
// RUN: %complete-test %s -group=none -no-inner-results -inner-operators -tok=FOOBAR_POSTFIX | %FileCheck %s -check-prefix=FOOBAR_POSTFIX_OP
// FOOBAR_POSTFIX_OP: {{^}}.{{$}}
// FOOBAR_POSTFIX_OP: {{^}}({{$}}

func test005(x: FooBar) {
  x#^FOOBAR_INSTANCE_POSTFIX^#
}
// RUN: %complete-test %s -group=none -no-inner-results -inner-operators -tok=FOOBAR_INSTANCE_POSTFIX | %FileCheck %s -check-prefix=FOOBAR_INSTANCE_POSTFIX_OP
// FOOBAR_INSTANCE_POSTFIX_OP: .
// FIXME: We should probably just have '[' here - rdar://22702955
// FOOBAR_INSTANCE_POSTFIX_OP: [x: Foo]

func test005(x: Base?) {
  x#^OPTIONAL_POSTFIX^#
}
// RUN: %complete-test %s -group=none -no-inner-results -inner-operators -tok=OPTIONAL_POSTFIX | %FileCheck %s -check-prefix=OPTIONAL_POSTFIX_OP
// OPTIONAL_POSTFIX_OP: .
// OPTIONAL_POSTFIX_OP: ?.

// RUN: %complete-test %s -group=none -no-inner-results -inner-operators -tok=KEYWORD_0 | %FileCheck %s -check-prefix=KEYWORD_0
func test006() {
  #^KEYWORD_0,for^#
}
// KEYWORD_0-NOT: for_
// KEYWORD_0-NOT: fortest
// KEYWORD_0-NOT: for.

enum E0 {
  case case0
}

// RUN: %complete-test %s -group=none -no-inner-results -no-inner-operators -tok=LEADING_DOT_0 | %FileCheck %s -check-prefix=LEADING_NODOT_E0
// RUN: %complete-test %s -group=none -no-inner-results -inner-operators -tok=LEADING_DOT_0 | %FileCheck %s -check-prefix=LEADING_DOT_E0
func test007() {
  var e: E0
  e = #^LEADING_DOT_0^#
}
// LEADING_NODOT_E0-NOT: .
// LEADING_DOT_E0: .

struct WithLeading {
  static var foo: WithLeading = WithLeading()
}

// RUN: %complete-test %s -group=none -no-inner-results -inner-operators -tok=LEADING_DOT_1 | %FileCheck %s -check-prefix=LEADING_DOT_S
func test009() {
  var e: WithLeading
  e = #^LEADING_DOT_1^#
}
// FIXME: should have leading dot.
// LEADING_DOT_S-NOT: .
