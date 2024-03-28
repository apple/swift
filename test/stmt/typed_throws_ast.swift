// RUN: %target-swift-frontend %s -dump-ast | %FileCheck %s

enum MyError: Error {
case failed
case epicFailed
}

enum HomeworkError: Error {
case dogAteIt
case forgot
}

var homework: String {
  get throws(HomeworkError) {
    throw .dogAteIt
  }
}

func printOrFail(_ message: String) throws(MyError) {
}

// CHECK-LABEL: func_decl{{.*}}"throwsAnything()"
func throwsAnything() throws {
  // CHECK: do_catch_stmt{{.*}}throws
  do {
    // CHECK: declref_expr{{.*}}throws(HomeworkError to any Error)
    let hw = try homework
    // CHECK: call_expr{{.*}}throws(MyError to any Error)
    try printOrFail(hw)
  } catch let e as HomeworkError {
    // swallow this error
    _ = e
  } // implicit rethrow

  // CHECK: force_try_expr{{.*}}thrown_error="MyError"
  try! printOrFail("boom")
  // CHECK: optional_try_expr{{.*}}thrown_error="MyError"
  try? printOrFail("ssshhhhh")
}

func doesNotThrow() { }

func throwsNothing() {
  // CHECK-LABEL: func_decl{{.*}}"throwsNothing()"

  // CHECK: force_try_expr{{.*}}thrown_error="Never"
  try! doesNotThrow()

  // CHECK: optional_try_expr{{.*}}thrown_error="Never"
  try? doesNotThrow()
}
