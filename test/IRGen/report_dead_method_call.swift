// RUN: %empty-directory(%t)

// We compile with -O (optimizations) and -disable-access-control (which
// allows use to "call" methods that are removed by dead code elimination).
// RUN: %target-build-swift -parse-as-library %S/Inputs/report_dead_method_call/main.swift %s -O -Xfrontend -disable-access-control -Xfrontend -disable-availability-checking -o %t/report_dead_method_call

// The private, unused methods are optimized away. The test calls these
// methods anyway (since it has overridden the access control), so we
// expect them to produce "Fatal error: Call of deleted method" when run.
// RUN: %target-codesign %t/report_dead_method_call
// RUN: %target-run %t/report_dead_method_call

// REQUIRES: executable_test
// REQUIRES: concurrency
// UNSUPPORTED: freestanding
// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime

// UNSUPPORTED: swift_test_mode_optimize_none_with_implicit_dynamic
// UNSUPPORTED: swift_test_mode_optimize_with_implicit_dynamic

private protocol PrivateProto {
	func abc()
	func abcAsync() async
}

struct PrivateStructC : PrivateProto {
	func abc() {}
	func abcAsync() async {}
}

struct Container {

	private var p: PrivateProto = PrivateStructC()
}

@inline(never)
func callProto() {
	testProto(Container())
}

@inline(never)
func callProtoAsync() async {
	await testProtoAsync(Container())
}

private class Base {
	func def() {}
	func defAsync() async {}
}

private class Derived : Base {
	override func def() {}
	override func defAsync() async {}
}

struct ClassContainer {

	private var p: Base = Derived()
}

@inline(never)
func callClass() {
	testClass(ClassContainer())
}

@inline(never)
func callClassAsync() async {
	await testClassAsync(ClassContainer())
}

public class PublicBase {
	private func ghi() { }

  private func ghiAsync() async {}
}

