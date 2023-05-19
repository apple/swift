// RUN: %empty-directory(%t)

// REQUIRES: concurrency

func manyWithError() async throws -> (String, Int) { ("", 0) }
func manyWithError(_ completion: @escaping (String?, Int?, Error?) -> Void) { }

func mixed() async -> (String, Int) { ("", 0) }
func mixed(_ completion: @escaping (String?, Int) -> Void) { }

func mixedError() async throws -> (String, Int) { ("", 0) }
func mixedError(_ completion: @escaping (String?, Int, Error?) -> Void) { }

func testParamsMulti() async throws {
  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=MANYBOUND %s
  manyWithError { res1, res2, err in
    print("before")
    if let bad = err {
      print("got error \(bad)")
      return
    }
    if let str = res1, let i = res2 {
      print("got result \(str)")
      print("got result \(i)")
    }
    print("after")
  }
  // MANYBOUND: do {
  // MANYBOUND-NEXT: let (str, i) = try await manyWithError()
  // MANYBOUND-NEXT: print("before")
  // MANYBOUND-NEXT: print("got result \(str)")
  // MANYBOUND-NEXT: print("got result \(i)")
  // MANYBOUND-NEXT: print("after")
  // MANYBOUND-NEXT: } catch let bad {
  // MANYBOUND-NEXT: print("got error \(bad)")
  // MANYBOUND-NEXT: }

  // FIXME: This case is a little tricky: Being in the else block of 'if let str = res1'
  // should allow us to place 'if let i = res2' in the failure block. However, this
  // is a success condition, so we still place it in the success block. Really what
  // we need to do here is check to see if manyWithError has an existing async
  // alternative that still returns optional success values, and allow success
  // classification in that case. Otherwise, we'd probably be better off leaving
  // the condition unhandled, as it's not clear what the user is doing.
  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=MANYUNBOUND-ERR %s
  manyWithError { res1, res2, err in
    print("before")
    if let str = res1 {
      print("got result \(str)")
    } else if let i = res2 {
      print("got result \(i)")
    } else {
      print("got error \(err!)")
    }
    print("after")
  }
  // MANYUNBOUND-ERR: do {
  // MANYUNBOUND-ERR-NEXT: let (str, i) = try await manyWithError()
  // MANYUNBOUND-ERR-NEXT: print("before")
  // MANYUNBOUND-ERR-NEXT: print("got result \(str)")
  // MANYUNBOUND-ERR-NEXT: print("got result \(i)")
  // MANYUNBOUND-ERR-NEXT: print("after")
  // MANYUNBOUND-ERR-NEXT: } catch let err {
  // MANYUNBOUND-ERR-NEXT: print("got error \(err)")
  // MANYUNBOUND-ERR-NEXT: }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=MANYBOUND %s
  manyWithError { res1, res2, err in
    print("before")
    if let bad = err {
      print("got error \(bad)")
      return
    }
    if let str = res1 {
      print("got result \(str)")
    }
    if let i = res2 {
      print("got result \(i)")
    }
    print("after")
  }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=MIXED-COND %s
  manyWithError { res1, res2, err in
    print("before")
    if res1 != nil && res2 == nil {
      print("got result \(res1!)")
    }
    print("after")
  }
  //  MIXED-COND: convert_params_multi.swift
  //  MIXED-COND-NEXT: let (res1, res2) = try await manyWithError()
  //  MIXED-COND-NEXT: print("before")
  //  MIXED-COND-NEXT: if <#res1#> != nil && <#res2#> == nil {
  //  MIXED-COND-NEXT: print("got result \(res1)")
  //  MIXED-COND-NEXT: }
  //  MIXED-COND-NEXT: print("after")

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=MIXED-CONDELSE %s
  manyWithError { res1, res2, err in
    print("before")
    if res1 != nil && res2 == nil {
      print("got result \(res1!)")
    } else {
      print("bad")
    }
    print("after")
  }
  // MIXED-CONDELSE: var res1: String? = nil
  // MIXED-CONDELSE-NEXT: var res2: Int? = nil
  // MIXED-CONDELSE-NEXT: var err: (any Error)? = nil
  // MIXED-CONDELSE-NEXT: do {
  // MIXED-CONDELSE-NEXT: (res1, res2) = try await manyWithError()
  // MIXED-CONDELSE-NEXT: } catch {
  // MIXED-CONDELSE-NEXT: err = error
  // MIXED-CONDELSE-NEXT: }
  // MIXED-CONDELSE-EMPTY:
  // MIXED-CONDELSE-NEXT: print("before")
  // MIXED-CONDELSE-NEXT: if res1 != nil && res2 == nil {
  // MIXED-CONDELSE-NEXT: print("got result \(res1!)")
  // MIXED-CONDELSE-NEXT: } else {
  // MIXED-CONDELSE-NEXT: print("bad")
  // MIXED-CONDELSE-NEXT: }
  // MIXED-CONDELSE-NEXT: print("after")

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=MANYUNBOUND-ERR %s
  manyWithError { res1, res2, err in
    print("before")
    guard let str = res1, let i = res2 else {
      print("got error \(err!)")
      return
    }
    print("got result \(str)")
    print("got result \(i)")
    print("after")
  }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=MANYUNBOUND %s
  manyWithError { res1, res2, err in
    print("before")
    guard res1 != nil && res2 != nil && err == nil else {
      print("got error \(err!)")
      return
    }
    print("got result \(res1!)")
    print("got result \(res2!)")
    print("after")
  }
  // MANYUNBOUND: do {
  // MANYUNBOUND-NEXT: let (res1, res2) = try await manyWithError()
  // MANYUNBOUND-NEXT: print("before")
  // MANYUNBOUND-NEXT: print("got result \(res1)")
  // MANYUNBOUND-NEXT: print("got result \(res2)")
  // MANYUNBOUND-NEXT: print("after")
  // MANYUNBOUND-NEXT: } catch let err {
  // MANYUNBOUND-NEXT: print("got error \(err)")
  // MANYUNBOUND-NEXT: }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=MANYUNBOUND %s
  manyWithError { res1, res2, err in
    print("before")
    guard res1 != nil else {
      print("got error \(err!)")
      return
    }
    print("got result \(res1!)")
    print("got result \(res2!)")
    print("after")
  }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=MANYUNBOUND %s
  manyWithError { res1, res2, err in
    print("before")
    guard err == nil else {
      print("got error \(err!)")
      return
    }
    print("got result \(res1!)")
    print("got result \(res2!)")
    print("after")
  }

  // Cannot use refactor-check-compiles, as cannot use non-optional 'str' in
  // optional binding.
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=MIXED %s
  mixed { str, num in
    print("before")
    if let res = str {
      print("got result \(res)")
    }
    print("\(num)")
    print("after")
  }
  // MIXED: convert_params_multi.swift
  // MIXED-NEXT: let (str, num) = await mixed()
  // MIXED-NEXT: print("before")
  // MIXED-NEXT: if let res = str {
  // MIXED-NEXT: print("got result \(res)")
  // MIXED-NEXT: }
  // MIXED-NEXT: print("\(num)")
  // MIXED-NEXT: print("after")
  // MIXED-NOT: }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 | %FileCheck -check-prefix=MIXED-ERROR %s
  mixedError { str, num, err in
    print("before")
    if let res = str {
      print("got result \(res)")
    } else {
      print("got \(err!)")
    }
    print("\(num)")
    print("after")
  }
  // MIXED-ERROR: convert_params_multi.swift
  // MIXED-ERROR-NEXT: do {
  // MIXED-ERROR-NEXT: let (res, num) = try await mixedError()
  // MIXED-ERROR-NEXT: print("before")
  // MIXED-ERROR-NEXT: print("got result \(res)")
  // MIXED-ERROR-NEXT: print("\(num)")
  // MIXED-ERROR-NEXT: print("after")
  // MIXED-ERROR-NEXT: } catch let err {
  // MIXED-ERROR-NEXT: print("got \(err)")
  // MIXED-ERROR-NEXT: }
  // MIXED-ERROR-NOT: }
}
