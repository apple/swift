// The StdlibUnittest test suite is placed here because it contains
// expressions that are only allowed at the top level in files named
// "main.swift".

import StdlibUnittest

@inline(never)
func testProto(_ c: Container) {
	// call the dead witness method abc()
	c.p.abc()
}

@inline(never)
func testProtoAsync(_ c: Container) async {
	// call the dead witness method abcAsync()
	await c.p.abcAsync()
}

@inline(never)
func testClass(_ c: ClassContainer) {
	// call the dead vtable method def()
	c.p.def()
}

@inline(never)
func testClassAsync(_ c: ClassContainer) async {
	// call the dead vtable method defAsync()
	await c.p.defAsync()
}

public class PublicDerived : PublicBase {
	// The vtable of PublicDerived contains a reference to PublicBase.ghi()
}

@inline(never)
func callPublicClass() {
	testPublicClass(PublicDerived())
}

@inline(never)
func testPublicClass(_ c: PublicBase) {
	// call the dead private vtable method ghi()
	c.ghi()
}

@inline(never)
func callPublicClassAsync() async {
	await testPublicClassAsync(PublicDerived())
}

@inline(never)
func testPublicClassAsync(_ c: PublicBase) async {
	// call the dead private vtable method ghiAsync()
	await c.ghiAsync()
}

@main struct Main {
  static func main() async {

    let tests = TestSuite("ReportDeadMethodCall")

    tests.test("Call class") {
      expectCrashLater(withMessage: "Fatal error: Call of deleted method")
      callClass()
    }

    tests.test("Call class async") {
      // TODO: it should crash with the error message and not with sigsegv
      expectCrashLater()
      await callClassAsync()
    }

    tests.test("Call proto") {
      expectCrashLater(withMessage: "Fatal error: Call of deleted method")
      callProto()
    }

    tests.test("Call proto async") {
      // TODO: it should crash with the error message and not with sigsegv
      expectCrashLater(withMessage: "")
      await callProtoAsync()
    }

    tests.test("Call public class") {
      expectCrashLater(withMessage: "Fatal error: Call of deleted method")
      callPublicClass()
    }

    tests.test("Call public class async") {
      expectCrashLater(withMessage: "Fatal error: Call of deleted method")
      await callPublicClassAsync()
    }

    await runAllTestsAsync()
  }
}  

