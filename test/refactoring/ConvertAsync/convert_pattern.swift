// REQUIRES: concurrency

// RUN: %empty-directory(%t)

enum E : Error { case e }

func anyCompletion(_ completion: @escaping (Any?, Error?) -> Void) {}
func anyResultCompletion(_ completion: @escaping (Result<Any?, Error>) -> Void) {}

func stringTupleParam(_ completion: @escaping ((String, String)?, Error?) -> Void) {}
func stringTupleParam() async throws -> (String, String) {}

func stringTupleResult(_ completion: @escaping (Result<(String, String), Error>) -> Void) {}
func stringTupleResult() async throws -> (String, String) {}

func mixedTupleResult(_ completion: @escaping (Result<((Int, Float), String), Error>) -> Void) {}
func mixedTupleResult() async throws -> ((Int, Float), String) {}

func multipleTupleParam(_ completion: @escaping ((String, String)?, (Int, Int)?, Error?) -> Void) {}
func multipleTupleParam() async throws -> ((String, String), (Int, Int)) {}

func testPatterns() async throws {
  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=INLINE %s
  stringTupleParam { strs, err in
    guard let (str1, str2) = strs else { return }
    print(str1, str2)
  }

  // INLINE:      let (str1, str2) = try await stringTupleParam()
  // INLINE-NEXT: print(str1, str2)

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=INLINE-VAR %s
  stringTupleParam { strs, err in
    guard var (str1, str2) = strs else { return }
    print(str1, str2)
  }

  // INLINE-VAR:      var (str1, str2) = try await stringTupleParam()
  // INLINE-VAR-NEXT: print(str1, str2)

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=INLINE-BLANK %s
  stringTupleParam { strs, err in
    guard var (_, str2) = strs else { return }
    print(str2)
  }

  // INLINE-BLANK:      var (_, str2) = try await stringTupleParam()
  // INLINE-BLANK-NEXT: print(str2)

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=INLINE-TYPED %s
  stringTupleParam { strs, err in
    guard let (str1, str2): (String, String) = strs else { return }
    print(str1, str2)
  }

  // INLINE-TYPED:      let (str1, str2) = try await stringTupleParam()
  // INLINE-TYPED-NEXT: print(str1, str2)

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=INLINE-CASE %s
  stringTupleParam { strs, err in
    guard case (let str1, var str2)? = strs else { return }
    print(str1, str2)
  }

  // INLINE-CASE:      var (str1, str2) = try await stringTupleParam()
  // INLINE-CASE-NEXT: print(str1, str2)

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=INLINE-CASE-TYPED %s
  stringTupleParam { strs, err in
    guard case let (str1, str2)?: (String, String)? = strs else { return }
    print(str1, str2)
  }

  // INLINE-CASE-TYPED:      let (str1, str2) = try await stringTupleParam()
  // INLINE-CASE-TYPED-NEXT: print(str1, str2)

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=OUT-OF-LINE %s
  stringTupleParam { strs, err in
    guard let (str1, str2) = strs else { return }
    print(str1, str2, strs!)
  }

  // OUT-OF-LINE:      let strs = try await stringTupleParam()
  // OUT-OF-LINE-NEXT: let (str1, str2) = strs
  // OUT-OF-LINE-NEXT: print(str1, str2, strs)

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=OUT-OF-LINE-VAR %s
  stringTupleParam { strs, err in
    guard var (str1, _) = strs else { return }
    str1 = ""
    print(str1, {strs!})
  }

  // OUT-OF-LINE-VAR:      let strs = try await stringTupleParam()
  // OUT-OF-LINE-VAR-NEXT: var (str1, _) = strs
  // OUT-OF-LINE-VAR-NEXT: str1 = ""
  // OUT-OF-LINE-VAR-NEXT: print(str1, {strs})

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=OUT-OF-LINE-CASE %s
  stringTupleParam { strs, err in
    guard case (let str1, var str2)? = strs else { return }
    print(str1, str2, strs!)
  }

  // OUT-OF-LINE-CASE:      let strs = try await stringTupleParam()
  // OUT-OF-LINE-CASE-NEXT: var (str1, str2) = strs
  // OUT-OF-LINE-CASE-NEXT: print(str1, str2, strs)

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=FALLBACK %s
  stringTupleParam { strs, err in
    guard let (str1, str2) = strs, str1 == "hi" else { fatalError() }
    print(str1, str2, err)
  }

  // FALLBACK:      var strs: (String, String)? = nil
  // FALLBACK-NEXT: var err: (any Error)? = nil
  // FALLBACK-NEXT: do {
  // FALLBACK-NEXT:   strs = try await stringTupleParam()
  // FALLBACK-NEXT: } catch {
  // FALLBACK-NEXT:   err = error
  // FALLBACK-NEXT: }
  // FALLBACK-EMPTY:
  // FALLBACK-NEXT: guard let (str1, str2) = strs, str1 == "hi" else { fatalError() }
  // FALLBACK-NEXT: print(str1, str2, err)

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=GUARD-AND-UNHANDLED %s
  stringTupleParam { strs, err in
    guard let (str1, str2) = strs else { fatalError() }
    print(str1, str2)
    if .random(), err == nil {
      print("yay")
    } else {
      print("nay")
    }
  }

  // GUARD-AND-UNHANDLED:      do {
  // GUARD-AND-UNHANDLED-NEXT:   let (str1, str2) = try await stringTupleParam()
  // GUARD-AND-UNHANDLED-NEXT:   print(str1, str2)
  // GUARD-AND-UNHANDLED-NEXT:   if .random(), <#err#> == nil {
  // GUARD-AND-UNHANDLED-NEXT:     print("yay")
  // GUARD-AND-UNHANDLED-NEXT:   } else {
  // GUARD-AND-UNHANDLED-NEXT:     print("nay")
  // GUARD-AND-UNHANDLED-NEXT:   }
  // GUARD-AND-UNHANDLED-NEXT: } catch let err {
  // GUARD-AND-UNHANDLED-NEXT:   fatalError()
  // GUARD-AND-UNHANDLED-NEXT: }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=MIXED-BINDINGS %s
  stringTupleParam { strs, err in
    guard let x = strs else { return }
    guard var (str1, str2) = strs else { return }
    guard var y = strs else { return }
    guard let z = strs else { return }
    y = ("hello", "there")
    print(x, y, z, str1, str2)
  }

  // Make sure that we
  // 1. Coalesce the z binding, as it is a let
  // 2. Preserve the y binding, as it is a var
  // 3. Print the multi-var binding out of line
  //
  // MIXED-BINDINGS:      let x = try await stringTupleParam()
  // MIXED-BINDINGS-NEXT: var (str1, str2) = x
  // MIXED-BINDINGS-NEXT: var y = x
  // MIXED-BINDINGS-NEXT: y = ("hello", "there")
  // MIXED-BINDINGS-NEXT: print(x, y, x, str1, str2)

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=MIXED-BINDINGS2 %s
  stringTupleParam { strs, err in
    guard var (str1, str2) = strs else { return }
    str1 = "hi"
    guard var x = strs else { return }
    x = ("hello", "there")
    print(x, str1, str2)
  }

  // MIXED-BINDINGS2:      var x = try await stringTupleParam()
  // MIXED-BINDINGS2-NEXT: var (str1, str2) = x
  // MIXED-BINDINGS2-NEXT: str1 = "hi"
  // MIXED-BINDINGS2-NEXT: x = ("hello", "there")
  // MIXED-BINDINGS2-NEXT: print(x, str1, str2)

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=MIXED-BINDINGS3 %s
  stringTupleParam { strs, err in
    guard let (str1, str2) = strs else { return }
    guard let x = strs else { return }
    print(x, str1, str2)
  }

  // MIXED-BINDINGS3:      let x = try await stringTupleParam()
  // MIXED-BINDINGS3-NEXT: let (str1, str2) = x
  // MIXED-BINDINGS3-NEXT: print(x, str1, str2)

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=ALIAS-BINDINGS %s
  stringTupleParam { strs, err in
    guard let x = strs else { return }
    guard let y = strs else { return }
    guard let z = strs else { return }
    print(x, y, z)
  }

  // ALIAS-BINDINGS:      let x = try await stringTupleParam()
  // ALIAS-BINDINGS-NEXT: print(x, x, x)

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=ALIAS-BINDINGS2 %s
  stringTupleParam { strs, err in
    guard var x = strs else { return }
    guard var y = strs else { return }
    guard let z = strs else { return }
    print(x, y, z)
  }

  // ALIAS-BINDINGS2:      let z = try await stringTupleParam()
  // ALIAS-BINDINGS2-NEXT: var x = z
  // ALIAS-BINDINGS2-NEXT: var y = z
  // ALIAS-BINDINGS2-NEXT: print(x, y, z)

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=ALIAS-BINDINGS3 %s
  stringTupleParam { strs, err in
    guard var x = strs else { return }
    guard var y = strs else { return }
    guard var z = strs else { return }
    print(x, y, z)
  }

  // ALIAS-BINDINGS3:      var x = try await stringTupleParam()
  // ALIAS-BINDINGS3-NEXT: var y = x
  // ALIAS-BINDINGS3-NEXT: var z = x
  // ALIAS-BINDINGS3-NEXT: print(x, y, z)

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=ALIAS-BINDINGS4 %s
  stringTupleParam { strs, err in
    guard var x = strs else { return }
    guard let y = strs else { return }
    guard let z = strs else { return }
    print(x, y, z)
  }

  // ALIAS-BINDINGS4:      let y = try await stringTupleParam()
  // ALIAS-BINDINGS4-NEXT: var x = y
  // ALIAS-BINDINGS4-NEXT: print(x, y, y)

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=ALIAS-BINDINGS5 %s
  stringTupleParam { strs, err in
    guard var x = strs else { return }
    print(x)
  }

  // ALIAS-BINDINGS5:      var x = try await stringTupleParam()
  // ALIAS-BINDINGS5-NEXT: print(x)

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=STRING-TUPLE-RESULT %s
  stringTupleResult { res in
    switch res {
    case .success((let x, let y)):
      print(x, y)
    case .failure:
      print("oh no")
    }
  }
  // STRING-TUPLE-RESULT:      do {
  // STRING-TUPLE-RESULT-NEXT:   let (x, y) = try await stringTupleResult()
  // STRING-TUPLE-RESULT-NEXT:   print(x, y)
  // STRING-TUPLE-RESULT-NEXT: } catch {
  // STRING-TUPLE-RESULT-NEXT:   print("oh no")
  // STRING-TUPLE-RESULT-NEXT: }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=STRING-TUPLE-RESULT %s
  stringTupleResult { res in
    switch res {
    case let .success((x, y)):
      print(x, y)
    case .failure:
      print("oh no")
    }
  }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=MIXED-TUPLE-RESULT %s
  mixedTupleResult { res in
    if case .failure(let err) = res {
      print("oh no")
    }
    if case .success(((let x, let y), let z)) = res {
      print("a", x, y, z)
    }
    switch res {
    case .success(let ((x, _), z)):
      print("b", x, z)
    case .failure:
      print("oh no again")
    }
  }

  // MIXED-TUPLE-RESULT:      do {
  // MIXED-TUPLE-RESULT-NEXT:   let res = try await mixedTupleResult()
  // MIXED-TUPLE-RESULT-NEXT:   let ((x, y), z) = res
  // MIXED-TUPLE-RESULT-NEXT:   let ((x1, _), z1) = res
  // MIXED-TUPLE-RESULT-NEXT:   print("a", x, y, z)
  // MIXED-TUPLE-RESULT-NEXT:   print("b", x1, z1)
  // MIXED-TUPLE-RESULT-NEXT: } catch let err {
  // MIXED-TUPLE-RESULT-NEXT:   print("oh no")
  // MIXED-TUPLE-RESULT-NEXT:   print("oh no again")
  // MIXED-TUPLE-RESULT-NEXT: }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=MIXED-TUPLE-RESULT2 %s
  mixedTupleResult { res in
    switch res {
    case .success(((let x, let _), let z)):
      print(x, z)
    case .failure(let err):
      print("oh no \(err)")
    }
  }

  // MIXED-TUPLE-RESULT2:      do {
  // MIXED-TUPLE-RESULT2-NEXT:   let ((x, _), z) = try await mixedTupleResult()
  // MIXED-TUPLE-RESULT2-NEXT:   print(x, z)
  // MIXED-TUPLE-RESULT2-NEXT: } catch let err {
  // MIXED-TUPLE-RESULT2-NEXT:   print("oh no \(err)")
  // MIXED-TUPLE-RESULT2-NEXT: }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=MIXED-TUPLE-RESULT3 %s
  mixedTupleResult { res in
    if let ((_, y), z) = try? res.get() {
      print(y, z)
    } else {
      print("boo")
    }
  }

  // MIXED-TUPLE-RESULT3:      do {
  // MIXED-TUPLE-RESULT3-NEXT:   let ((_, y), z) = try await mixedTupleResult()
  // MIXED-TUPLE-RESULT3-NEXT:   print(y, z)
  // MIXED-TUPLE-RESULT3-NEXT: } catch {
  // MIXED-TUPLE-RESULT3-NEXT:   print("boo")
  // MIXED-TUPLE-RESULT3-NEXT: }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=MULTIPLE-TUPLE-PARAM %s
  multipleTupleParam { strs, ints, err in
    guard let (str1, str2) = strs, let (int1, int2) = ints else {
      print("ohno")
      return
    }
    print(str1, str2, int1, int2)
  }
  // MULTIPLE-TUPLE-PARAM:      do {
  // MULTIPLE-TUPLE-PARAM-NEXT:   let ((str1, str2), (int1, int2)) = try await multipleTupleParam()
  // MULTIPLE-TUPLE-PARAM-NEXT:   print(str1, str2, int1, int2)
  // MULTIPLE-TUPLE-PARAM-NEXT: } catch let err {
  // MULTIPLE-TUPLE-PARAM-NEXT:   print("ohno")
  // MULTIPLE-TUPLE-PARAM-NEXT: }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=MULTIPLE-TUPLE-PARAM2 %s
  multipleTupleParam { strs, ints, err in
    guard let (str1, str2) = strs, var (int1, int2) = ints else {
      print("ohno")
      return
    }
    print(str1, str2, int1, int2)
  }
  // MULTIPLE-TUPLE-PARAM2:      do {
  // MULTIPLE-TUPLE-PARAM2-NEXT:   var ((str1, str2), (int1, int2)) = try await multipleTupleParam()
  // MULTIPLE-TUPLE-PARAM2-NEXT:   print(str1, str2, int1, int2)
  // MULTIPLE-TUPLE-PARAM2-NEXT: } catch let err {
  // MULTIPLE-TUPLE-PARAM2-NEXT:   print("ohno")
  // MULTIPLE-TUPLE-PARAM2-NEXT: }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=MULTIPLE-TUPLE-PARAM3 %s
  multipleTupleParam { strs, ints, err in
    guard let (str1, str2) = strs, let (int1, int2) = ints else {
      print("ohno")
      return
    }
    print(strs!)
    print(str1, str2, int1, int2)
  }
  // MULTIPLE-TUPLE-PARAM3:      do {
  // MULTIPLE-TUPLE-PARAM3-NEXT:   let (strs, (int1, int2)) = try await multipleTupleParam()
  // MULTIPLE-TUPLE-PARAM3-NEXT:   let (str1, str2) = strs
  // MULTIPLE-TUPLE-PARAM3-NEXT:   print(strs)
  // MULTIPLE-TUPLE-PARAM3-NEXT:   print(str1, str2, int1, int2)
  // MULTIPLE-TUPLE-PARAM3-NEXT: } catch let err {
  // MULTIPLE-TUPLE-PARAM3-NEXT:   print("ohno")
  // MULTIPLE-TUPLE-PARAM3-NEXT: }
}

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=NAME-COLLISION %s
func testNameCollision(_ completion: @escaping () -> Void) {
  let a = ""
  stringTupleParam { strs, err in
    guard let (a, b) = strs else { return }
    print(a, b)
  }
  let b = ""
  stringTupleParam { strs, err in
    guard let (a, b) = strs else { return }
    print(a, b, strs!)
  }
  print(a, b)
  completion()
}

// TODO: `throws` isn't added to the function declaration
// NAME-COLLISION:      func testNameCollision() async {
// NAME-COLLISION-NEXT:   let a = ""
// NAME-COLLISION-NEXT:   let (a1, b) = try await stringTupleParam()
// NAME-COLLISION-NEXT:   print(a1, b)
// NAME-COLLISION-NEXT:   let b1 = ""
// NAME-COLLISION-NEXT:   let strs = try await stringTupleParam()
// NAME-COLLISION-NEXT:   let (a2, b2) = strs
// NAME-COLLISION-NEXT:   print(a2, b2, strs)
// NAME-COLLISION-NEXT:   print(a, b1)
// NAME-COLLISION-NEXT:   return
// NAME-COLLISION-NEXT: }

// RUN: %refactor -convert-to-async -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=NAME-COLLISION2 %s
func testNameCollision2(_ completion: @escaping () -> Void) {
  mixedTupleResult { res in
    guard case let .success((x, y), z) = res else { return }
    stringTupleParam { strs, err in
      if let (x, y) = strs {
        print("a", x, y)
      }
    }
    print("b", x, y, z)
  }
}

// TODO: `throws` isn't added to the function declaration
// NAME-COLLISION2:      func testNameCollision2() async {
// NAME-COLLISION2-NEXT:   let ((x, y), z) = try await mixedTupleResult()
// NAME-COLLISION2-NEXT:   let (x1, y1) = try await stringTupleParam()
// NAME-COLLISION2-NEXT:   print("a", x1, y1)
// NAME-COLLISION2-NEXT:   print("b", x, y, z)
// NAME-COLLISION2-NEXT: }

// Cannot use refactor-check-compiles, as cannot pattern match with placeholders.
// RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):1 | %FileCheck -check-prefix=TEST-UNHANDLED %s
anyCompletion { val, err in
  if let x = val {
    print("a")
  }
  if let _ = val {
    print("b")
  }
  if case let x? = val {
    print("c")
  }
  if case let _? = val {
    print("d")
  }
  if case E.e? = val {
    print("e")
  }
  if case (let x as String)? = val {
    print("f")
  }
  if case let "" as String = val {
    print("g")
  }
}

// TEST-UNHANDLED:      let x = try await anyCompletion()
// TEST-UNHANDLED-NEXT: print("a")
// TEST-UNHANDLED-NEXT: print("b")
// TEST-UNHANDLED-NEXT: print("c")
// TEST-UNHANDLED-NEXT: print("d")
// TEST-UNHANDLED-NEXT: if case E.e? = <#x#> {
// TEST-UNHANDLED-NEXT:   print("e")
// TEST-UNHANDLED-NEXT: }
// TEST-UNHANDLED-NEXT: if case (let x as String)? = <#x#> {
// TEST-UNHANDLED-NEXT:   print("f")
// TEST-UNHANDLED-NEXT: }
// TEST-UNHANDLED-NEXT: if case let "" as String = <#x#> {
// TEST-UNHANDLED-NEXT:   print("g")
// TEST-UNHANDLED-NEXT: }

// RUN: not %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):1
anyResultCompletion { res in
  switch res {
  case .success(let unwrapped?):
    print(unwrapped)
  case .success(let x):
    print(x)
  case .failure:
    print("oh no")
  }
}

// RUN: not %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):1
anyResultCompletion { res in
  switch res {
  case .success(let str as String):
    print(str)
  case .success(let x):
    print(x)
  case .failure:
    print("oh no")
  }
}

// RUN: not %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):1
anyResultCompletion { res in
  switch res {
  case .success(E.e?):
    print("ee")
  case .success(let x):
    print(x)
  case .failure:
    print("oh no")
  }
}

// RUN: not %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):1
anyResultCompletion { res in
  switch res {
  case .success("" as String):
    print("empty string")
  case .success(let x):
    print(x)
  case .failure:
    print("oh no")
  }
}

// FIXME: This should ideally be turned into a 'catch let e as E'.
// RUN: not %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):1
anyResultCompletion { res in
  switch res {
  case .success(let a):
    print(a)
  case .failure(let e as E):
    print("oh no e")
  case .failure:
    print("oh no")
  }
}

// FIXME: This should ideally be turned into a 'catch E.e'.
// RUN: not %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):1
anyResultCompletion { res in
  switch res {
  case .success(let a):
    print(a)
  case .failure(E.e):
    print("oh no ee")
  case .failure:
    print("oh no")
  }
}

// Make sure we handle a capture list okay.
class C {
  // RUN: %refactor-check-compiles -convert-to-async -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=CAPTURE %s
  func foo() {
    let _ = { [weak self] in
      print(self!)
    }
  }
}
// CAPTURE:      func foo() async {
// CAPTURE-NEXT:   let _ = { [weak self] in
// CAPTURE-NEXT:     print(self!)
// CAPTURE-NEXT:   }
// CAPTURE-NEXT: }
