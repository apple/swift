// RUN: not %target-swiftc_driver %s 2>&1 | %FileCheck -check-prefix=DEFAULT %s
// RUN: not %target-swiftc_driver -warnings-as-errors %s 2>&1 | %FileCheck -check-prefix=WERR %s
// RUN: not %target-swiftc_driver -suppress-warnings %s 2>&1 | %FileCheck -check-prefix=NOWARN %s

// RUN: not %target-swiftc_driver -suppress-warnings -warnings-as-errors %s 2>&1 | %FileCheck -check-prefix=FLAGS_CONFLICT %s
// FLAGS_CONFLICT: error: conflicting options '-warnings-as-errors' and '-suppress-warnings'

func foo() {
	let x = 1
	x
// DEFAULT: warning: expression of type 'Int' is unused
// WERR:    error: expression of type 'Int' is unused
// NOWARN-NOT:  warning: expression of type 'Int' is unused
}

func bar() {
	foo()
// To help anchor the checks, have an error. Put it inside a later function, to help make sure it comes after
	xyz
// DEFAULT: error: cannot find 'xyz' in scope
// WERR:    error: cannot find 'xyz' in scope
// NOWARN:  error: cannot find 'xyz' in scope
}
