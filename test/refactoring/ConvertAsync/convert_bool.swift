// REQUIRES: objc_interop
// REQUIRES: concurrency

// RUN: %empty-directory(%t)
// RUN: %build-clang-importer-objc-overlays

import Foundation
import ConvertBoolObjC

func boolWithErr() async throws -> Bool { true }
func boolWithErr(completion: @escaping (Bool, Error?) -> Void) {}

func multipleBoolWithErr() async throws -> (String, Bool, Bool) { ("", true, true) }
func multipleBoolWithErr(completion: @escaping (String?, Bool, Bool, Error?) -> Void) {}

func optionalBoolWithErr() async throws -> (String, Bool, Bool) { ("", true, true) }
func optionalBoolWithErr(completion: @escaping (String?, Bool?, Bool, Error?) -> Void) {}

func testConvertBool() async throws {
  // All 7 of the below should generate the same refactoring.

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 -I %S/Inputs -I %t -target %target-triple %clang-importer-sdk-nosource | %FileCheck -check-prefix=BOOL-WITH-ERR %s
  boolWithErr { b, err in
    if !b {
      fatalError("oh no \(err!)")
    }
    print("not err")
  }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 -I %S/Inputs -I %t -target %target-triple %clang-importer-sdk-nosource | %FileCheck -check-prefix=BOOL-WITH-ERR %s
  boolWithErr { b, err in
    if b {
      fatalError("oh no \(err!)")
    }
    print("not err")
  }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 -I %S/Inputs -I %t -target %target-triple %clang-importer-sdk-nosource | %FileCheck -check-prefix=BOOL-WITH-ERR %s
  boolWithErr { b, err in
    if !b && err != nil {
      fatalError("oh no \(err!)")
    }
    print("not err")
  }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 -I %S/Inputs -I %t -target %target-triple %clang-importer-sdk-nosource | %FileCheck -check-prefix=BOOL-WITH-ERR %s
  boolWithErr { b, err in
    if b && err != nil {
      fatalError("oh no \(err!)")
    }
    print("not err")
  }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 -I %S/Inputs -I %t -target %target-triple %clang-importer-sdk-nosource | %FileCheck -check-prefix=BOOL-WITH-ERR %s
  boolWithErr { b, err in
    if err != nil && b == false {
      fatalError("oh no \(err!)")
    }
    print("not err")
  }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 -I %S/Inputs -I %t -target %target-triple %clang-importer-sdk-nosource | %FileCheck -check-prefix=BOOL-WITH-ERR %s
  boolWithErr { b, err in
    if b == true && err == nil {
    } else {
      fatalError("oh no \(err!)")
    }
    print("not err")
  }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 -I %S/Inputs -I %t -target %target-triple %clang-importer-sdk-nosource | %FileCheck -check-prefix=BOOL-WITH-ERR %s
  boolWithErr { b, err in
    if !b && err == nil {
    } else {
      fatalError("oh no \(err!)")
    }
    print("not err")
  }

  // BOOL-WITH-ERR:      do {
  // BOOL-WITH-ERR-NEXT:   let b = try await boolWithErr()
  // BOOL-WITH-ERR-NEXT:   print("not err")
  // BOOL-WITH-ERR-NEXT: } catch let err {
  // BOOL-WITH-ERR-NEXT:   fatalError("oh no \(err)")
  // BOOL-WITH-ERR-NEXT: }

  // These 3 should both generate the same refactoring.

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 -I %S/Inputs -I %t -target %target-triple %clang-importer-sdk-nosource | %FileCheck -check-prefix=BOOL-WITH-ERR2 %s
  boolWithErr { success, err in
    if success == true && err == nil {
      print("hi")
    } else {
      fatalError("oh no \(err!)")
    }
    print("not err")
  }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 -I %S/Inputs -I %t -target %target-triple %clang-importer-sdk-nosource | %FileCheck -check-prefix=BOOL-WITH-ERR2 %s
  boolWithErr { success, err in
    if success && err == nil {
      print("hi")
    } else {
      fatalError("oh no \(err!)")
    }
    print("not err")
  }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 -I %S/Inputs -I %t -target %target-triple %clang-importer-sdk-nosource | %FileCheck -check-prefix=BOOL-WITH-ERR2 %s
  boolWithErr { success, err in
    if err == nil {
      print("hi")
    } else if !success {
      fatalError("oh no \(err!)")
    }
    print("not err")
  }

  // BOOL-WITH-ERR2:      do {
  // BOOL-WITH-ERR2-NEXT:   let success = try await boolWithErr()
  // BOOL-WITH-ERR2-NEXT:   print("hi")
  // BOOL-WITH-ERR2-NEXT:   print("not err")
  // BOOL-WITH-ERR2-NEXT: } catch let err {
  // BOOL-WITH-ERR2-NEXT:   fatalError("oh no \(err)")
  // BOOL-WITH-ERR2-NEXT: }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 -I %S/Inputs -I %t -target %target-triple %clang-importer-sdk-nosource | %FileCheck -check-prefix=BOOL-WITH-ERR3 %s
  boolWithErr { failure, err in
    if failure {
      print("a \(err!)")
    } else if .random() {
      print("b")
    } else {
      print("c")
    }
  }

  // BOOL-WITH-ERR3:      do {
  // BOOL-WITH-ERR3-NEXT:   let failure = try await boolWithErr()
  // BOOL-WITH-ERR3-NEXT:   if .random() {
  // BOOL-WITH-ERR3-NEXT:     print("b")
  // BOOL-WITH-ERR3-NEXT:   } else {
  // BOOL-WITH-ERR3-NEXT:     print("c")
  // BOOL-WITH-ERR3-NEXT:   }
  // BOOL-WITH-ERR3-NEXT: } catch let err {
  // BOOL-WITH-ERR3-NEXT:   print("a \(err)")
  // BOOL-WITH-ERR3-NEXT: }

  // Don't handle the below example as the force unwrap of err takes place under a different condition.
  // We cannot use refactor-check-compiles, as a placeholder cannot be force unwrapped.
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 -I %S/Inputs -I %t -target %target-triple %clang-importer-sdk-nosource | %FileCheck -check-prefix=BOOL-DONT-HANDLE %s
  boolWithErr { success, err in
    if !success {
      if err != nil {
        fatalError("oh no \(err!)")
      }
    }
    if !success {
      _ = err != nil ? fatalError("oh no \(err!)") : fatalError("some worries")
    }
    print("not err")
  }

  // BOOL-DONT-HANDLE:      let success = try await boolWithErr()
  // BOOL-DONT-HANDLE-NEXT: if !success {
  // BOOL-DONT-HANDLE-NEXT:   if <#err#> != nil {
  // BOOL-DONT-HANDLE-NEXT:     fatalError("oh no \(<#err#>!)")
  // BOOL-DONT-HANDLE-NEXT:   }
  // BOOL-DONT-HANDLE-NEXT: }
  // BOOL-DONT-HANDLE-NEXT: if !success {
  // BOOL-DONT-HANDLE-NEXT:   _ = <#err#> != nil ? fatalError("oh no \(<#err#>!)") : fatalError("some worries")
  // BOOL-DONT-HANDLE-NEXT: }
  // BOOL-DONT-HANDLE-NEXT: print("not err")

  // We cannot use refactor-check-compiles, as a placeholder cannot be force unwrapped.
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 -I %S/Inputs -I %t -target %target-triple %clang-importer-sdk-nosource | %FileCheck -check-prefix=BOOL-DONT-HANDLE2 %s
  boolWithErr { success, err in
    if !success {
      func doThings() {
        fatalError("oh no \(err!)")
      }
      doThings()
    }
    if !success {
      let doThings = {
        fatalError("oh no \(err!)")
      }
      doThings()
    }
    if !success {
      while err != nil {
        fatalError("oh no \(err!)")
      }
    }
    if !success {
      for _: Int in [] {
        fatalError("oh no \(err!)")
      }
    }
    print("not err")
  }

  // FIXME: The 'err' in doThings() should become a placeholder (rdar://78509286).
  // BOOL-DONT-HANDLE2:      let success = try await boolWithErr()
  // BOOL-DONT-HANDLE2-NEXT: if !success {
  // BOOL-DONT-HANDLE2-NEXT:   func doThings() {
  // BOOL-DONT-HANDLE2-NEXT:     fatalError("oh no \(err!)")
  // BOOL-DONT-HANDLE2-NEXT:   }
  // BOOL-DONT-HANDLE2-NEXT:   doThings()
  // BOOL-DONT-HANDLE2-NEXT: }
  // BOOL-DONT-HANDLE2-NEXT: if !success {
  // BOOL-DONT-HANDLE2-NEXT:   let doThings = {
  // BOOL-DONT-HANDLE2-NEXT:     fatalError("oh no \(<#err#>!)")
  // BOOL-DONT-HANDLE2-NEXT:   }
  // BOOL-DONT-HANDLE2-NEXT:   doThings()
  // BOOL-DONT-HANDLE2-NEXT: }
  // BOOL-DONT-HANDLE2-NEXT: if !success {
  // BOOL-DONT-HANDLE2-NEXT:   while <#err#> != nil {
  // BOOL-DONT-HANDLE2-NEXT:     fatalError("oh no \(<#err#>!)")
  // BOOL-DONT-HANDLE2-NEXT:   }
  // BOOL-DONT-HANDLE2-NEXT: }
  // BOOL-DONT-HANDLE2-NEXT: if !success {
  // BOOL-DONT-HANDLE2-NEXT:   for _: Int in [] {
  // BOOL-DONT-HANDLE2-NEXT:     fatalError("oh no \(<#err#>!)")
  // BOOL-DONT-HANDLE2-NEXT:   }
  // BOOL-DONT-HANDLE2-NEXT: }
  // BOOL-DONT-HANDLE2-NEXT: print("not err")

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 -I %S/Inputs -I %t -target %target-triple %clang-importer-sdk-nosource | %FileCheck -check-prefix=BOOL-DONT-HANDLE3 %s
  boolWithErr { success, err in
    if !success {
      fatalError("oh no maybe \(String(describing: err))")
    }
    print("not err")
  }

  // err is not force unwrapped, so don't handle.

  // BOOL-DONT-HANDLE3:      let success = try await boolWithErr()
  // BOOL-DONT-HANDLE3-NEXT: if !success {
  // BOOL-DONT-HANDLE3-NEXT:   fatalError("oh no maybe \(String(describing: <#err#>))")
  // BOOL-DONT-HANDLE3-NEXT: }
  // BOOL-DONT-HANDLE3-NEXT: print("not err")

  // We cannot use refactor-check-compiles, as a placeholder cannot be force unwrapped.
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 -I %S/Inputs -I %t -target %target-triple %clang-importer-sdk-nosource | %FileCheck -check-prefix=BOOL-DONT-HANDLE4 %s
  boolWithErr { failure, err in
    if failure {
      print("a")
    } else if .random() {
      print("b \(err!)")
    } else {
      print("c")
    }
  }

  // Don't handle the case where the err unwrap occurs in an unrelated else if
  // clause.

  // BOOL-DONT-HANDLE4:      let failure = try await boolWithErr()
  // BOOL-DONT-HANDLE4-NEXT: if failure {
  // BOOL-DONT-HANDLE4-NEXT:   print("a")
  // BOOL-DONT-HANDLE4-NEXT: } else if .random() {
  // BOOL-DONT-HANDLE4-NEXT:   print("b \(<#err#>!)")
  // BOOL-DONT-HANDLE4-NEXT: } else {
  // BOOL-DONT-HANDLE4-NEXT:   print("c")
  // BOOL-DONT-HANDLE4-NEXT: }

  // We cannot use refactor-check-compiles, as a placeholder cannot be force unwrapped.
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 -I %S/Inputs -I %t -target %target-triple %clang-importer-sdk-nosource | %FileCheck -check-prefix=BOOL-WITH-ERR-SILLY %s
  boolWithErr { success, err in
    if success == false && err == nil {
      print("ummm wat \(err!)")
      return
    }
    print("not err")
  }

  // BOOL-WITH-ERR-SILLY:      let success = try await boolWithErr()
  // BOOL-WITH-ERR-SILLY-NEXT: if success == false && <#err#> == nil {
  // BOOL-WITH-ERR-SILLY-NEXT:   print("ummm wat \(<#err#>!)")
  // BOOL-WITH-ERR-SILLY-NEXT:   <#return#>
  // BOOL-WITH-ERR-SILLY-NEXT: }
  // BOOL-WITH-ERR-SILLY-NEXT: print("not err")

  // We cannot use refactor-check-compiles, as a placeholder cannot be force unwrapped.
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 -I %S/Inputs -I %t -target %target-triple %clang-importer-sdk-nosource | %FileCheck -check-prefix=BOOL-WITH-ERR-SILLY2 %s
  boolWithErr { success, err in
    if success {
      print("ummm wat \(err!)")
    } else {
      print("ummm wat \(err!)")
    }
  }

  // The err unwrap is in both blocks, so it's not clear what to classify as.

  // BOOL-WITH-ERR-SILLY2:      let success = try await boolWithErr()
  // BOOL-WITH-ERR-SILLY2-NEXT: if success {
  // BOOL-WITH-ERR-SILLY2-NEXT:   print("ummm wat \(<#err#>!)")
  // BOOL-WITH-ERR-SILLY2-NEXT: } else {
  // BOOL-WITH-ERR-SILLY2-NEXT:   print("ummm wat \(<#err#>!)")
  // BOOL-WITH-ERR-SILLY2-NEXT: }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 -I %S/Inputs -I %t -target %target-triple %clang-importer-sdk-nosource | %FileCheck -check-prefix=MULTI-BOOL-WITH-ERR %s
  multipleBoolWithErr { str, b1, b2, err in
    if !b1 && !b2 {
      print("a \(err!)")
    }
    if b1, b2 {
      print("b \(err!)")
    }
    if !b1 {
      print("c \(err!)")
    }
    if !b2 {
      print("d \(err!)")
    }
  }

  // Don't handle the case where multiple flag checks are done in a single
  // condition, because it's not exactly clear what the user is doing. It's a
  // little unfortunate that we'll allow multiple flag checks in separate
  // conditions, but both of these cases seem somewhat uncommon, and there's no
  // real way to completely enforce a single flag param across e.g multiple calls
  // to the same function, so this is probably okay for now.

  // MULTI-BOOL-WITH-ERR:      do {
  // MULTI-BOOL-WITH-ERR-NEXT:   let (str, b1, b2) = try await multipleBoolWithErr()
  // MULTI-BOOL-WITH-ERR-NEXT: } catch let err {
  // MULTI-BOOL-WITH-ERR-NEXT:   if !<#b1#> && !<#b2#> {
  // MULTI-BOOL-WITH-ERR-NEXT:     print("a \(err)")
  // MULTI-BOOL-WITH-ERR-NEXT:   }
  // MULTI-BOOL-WITH-ERR-NEXT:   if <#b1#>, <#b2#> {
  // MULTI-BOOL-WITH-ERR-NEXT:     print("b \(err)")
  // MULTI-BOOL-WITH-ERR-NEXT:   }
  // MULTI-BOOL-WITH-ERR-NEXT:   print("c \(err)")
  // MULTI-BOOL-WITH-ERR-NEXT:   print("d \(err)")
  // MULTI-BOOL-WITH-ERR-NEXT: }

  // We cannot use refactor-check-compiles, as a placeholder cannot be force unwrapped.
  // RUN: %refactor -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 -I %S/Inputs -I %t -target %target-triple %clang-importer-sdk-nosource | %FileCheck -check-prefix=OPT-BOOL-WITH-ERR %s
  optionalBoolWithErr { str, optBool, b, err in
    if optBool != nil {
      print("a \(err!)")
    }
    if optBool == nil {
      print("b \(err!)")
    }
    if optBool == true {
      print("c \(err!)")
    }
    if ((optBool) == (false)) {
      print("d \(err!)")
    }
    if optBool == false {
      print("e \(String(describing: err))")
    }
    if optBool != true {
      print("f \(err!)")
    }
    if b {
      print("g \(err!)")
    }
  }

  // It's a little unfortunate that print("a \(<#err#>!)") gets classified in the success
  // block below, but it doesn't seem like a case that's likely to come up, as optBool
  // would then be inaccessible in the error block.

  // OPT-BOOL-WITH-ERR:      do {
  // OPT-BOOL-WITH-ERR-NEXT:   let (str, optBool, b) = try await optionalBoolWithErr()
  // OPT-BOOL-WITH-ERR-NEXT:   print("a \(<#err#>!)")
  // OPT-BOOL-WITH-ERR-NEXT:   if <#optBool#> == false {
  // OPT-BOOL-WITH-ERR-NEXT:     print("e \(String(describing: <#err#>))")
  // OPT-BOOL-WITH-ERR-NEXT:   }
  // OPT-BOOL-WITH-ERR-NEXT:   if <#optBool#> != true {
  // OPT-BOOL-WITH-ERR-NEXT:     print("f \(<#err#>!)")
  // OPT-BOOL-WITH-ERR-NEXT:   }
  // OPT-BOOL-WITH-ERR-NEXT: } catch let err {
  // OPT-BOOL-WITH-ERR-NEXT:   print("b \(err)")
  // OPT-BOOL-WITH-ERR-NEXT:   print("c \(err)")
  // OPT-BOOL-WITH-ERR-NEXT:   print("d \(err)")
  // OPT-BOOL-WITH-ERR-NEXT:   print("g \(err)")
  // OPT-BOOL-WITH-ERR-NEXT: }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 -I %S/Inputs -I %t -target %target-triple %clang-importer-sdk-nosource | %FileCheck -check-prefix=OBJC-BOOL-WITH-ERR %s
  ClassWithHandlerMethods.firstBoolFlagSuccess("") { str, success, unrelated, err in
    if !unrelated {
      print(err!)
    }
    if !success {
      print("oh no")
    }
    if !success {
      print(err!)
    }
    if success {
      print("woo")
    }
    if str != nil {
      print("also woo")
    }
  }

  // OBJC-BOOL-WITH-ERR:      do {
  // OBJC-BOOL-WITH-ERR-NEXT:   let (str, unrelated) = try await ClassWithHandlerMethods.firstBoolFlagSuccess("")
  // OBJC-BOOL-WITH-ERR-NEXT:   if !unrelated {
  // OBJC-BOOL-WITH-ERR-NEXT:     print(<#err#>!)
  // OBJC-BOOL-WITH-ERR-NEXT:   }
  // OBJC-BOOL-WITH-ERR-NEXT:   print("woo")
  // OBJC-BOOL-WITH-ERR-NEXT:   print("also woo")
  // OBJC-BOOL-WITH-ERR-NEXT: } catch let err {
  // OBJC-BOOL-WITH-ERR-NEXT:   print("oh no")
  // OBJC-BOOL-WITH-ERR-NEXT:   print(err)
  // OBJC-BOOL-WITH-ERR-NEXT: }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 -I %S/Inputs -I %t -target %target-triple %clang-importer-sdk-nosource | %FileCheck -check-prefix=OBJC-BOOL-WITH-ERR2 %s
  ClassWithHandlerMethods.secondBoolFlagFailure("") { str, unrelated, failure, err in
    if unrelated {
      print(err!)
    }
    if failure {
      print("oh no")
    }
    if failure {
      print(err!)
    }
    if !failure {
      print("woo")
    }
    if str != nil {
      print("also woo")
    }
    if failure && err == nil {
      print("wat")
    }
    if failure && err != nil {
      print("neat")
    }
    if failure, let _ = err {
      print("neato")
    }
  }

  // OBJC-BOOL-WITH-ERR2:      do {
  // OBJC-BOOL-WITH-ERR2-NEXT:   let (str, unrelated) = try await ClassWithHandlerMethods.secondBoolFlagFailure("")
  // OBJC-BOOL-WITH-ERR2-NEXT:   if unrelated {
  // OBJC-BOOL-WITH-ERR2-NEXT:     print(<#err#>!)
  // OBJC-BOOL-WITH-ERR2-NEXT:   }
  // OBJC-BOOL-WITH-ERR2-NEXT:   print("woo")
  // OBJC-BOOL-WITH-ERR2-NEXT:   print("also woo")
  // OBJC-BOOL-WITH-ERR2-NEXT:   if <#failure#> && <#err#> == nil {
  // OBJC-BOOL-WITH-ERR2-NEXT:     print("wat")
  // OBJC-BOOL-WITH-ERR2-NEXT:   }
  // OBJC-BOOL-WITH-ERR2-NEXT: } catch let err {
  // OBJC-BOOL-WITH-ERR2-NEXT:   print("oh no")
  // OBJC-BOOL-WITH-ERR2-NEXT:   print(err)
  // OBJC-BOOL-WITH-ERR2-NEXT:   print("neat")
  // OBJC-BOOL-WITH-ERR2-NEXT:   print("neato")
  // OBJC-BOOL-WITH-ERR2-NEXT: }

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 -I %S/Inputs -I %t -target %target-triple %clang-importer-sdk-nosource | %FileCheck -check-prefix=OBJC-BOOL-WITH-ERR-FALLBACK %s
  ClassWithHandlerMethods.firstBoolFlagSuccess("") { str, success, unrelated, err in
    guard success && success == .random() else { fatalError() }
    print("much success", unrelated, str)
  }
  // OBJC-BOOL-WITH-ERR-FALLBACK:      var str: String? = nil
  // OBJC-BOOL-WITH-ERR-FALLBACK-NEXT: let success: Bool
  // OBJC-BOOL-WITH-ERR-FALLBACK-NEXT: var unrelated: Bool? = nil
  // OBJC-BOOL-WITH-ERR-FALLBACK-NEXT: var err: (any Error)? = nil
  // OBJC-BOOL-WITH-ERR-FALLBACK-NEXT: do {
  // OBJC-BOOL-WITH-ERR-FALLBACK-NEXT:   (str, unrelated) = try await ClassWithHandlerMethods.firstBoolFlagSuccess("")
  // OBJC-BOOL-WITH-ERR-FALLBACK-NEXT:   success = true
  // OBJC-BOOL-WITH-ERR-FALLBACK-NEXT: } catch {
  // OBJC-BOOL-WITH-ERR-FALLBACK-NEXT:   err = error
  // OBJC-BOOL-WITH-ERR-FALLBACK-NEXT:   success = false
  // OBJC-BOOL-WITH-ERR-FALLBACK-NEXT: }
  // OBJC-BOOL-WITH-ERR-FALLBACK-EMPTY:
  // OBJC-BOOL-WITH-ERR-FALLBACK-NEXT: guard success && success == .random() else { fatalError() }
  // OBJC-BOOL-WITH-ERR-FALLBACK-NEXT: print("much success", unrelated, str)

  // RUN: %refactor-check-compiles -convert-call-to-async-alternative -dump-text -source-filename %s -pos=%(line+1):3 -I %S/Inputs -I %t -target %target-triple %clang-importer-sdk-nosource | %FileCheck -check-prefix=OBJC-BOOL-WITH-ERR-FALLBACK2 %s
  ClassWithHandlerMethods.secondBoolFlagFailure("") { str, unrelated, failure, err in
    guard !failure && failure == .random() else { fatalError() }
    print("much fails", unrelated, str)
  }
  // OBJC-BOOL-WITH-ERR-FALLBACK2:      var str: String? = nil
  // OBJC-BOOL-WITH-ERR-FALLBACK2-NEXT: var unrelated: Bool? = nil
  // OBJC-BOOL-WITH-ERR-FALLBACK2-NEXT: let failure: Bool
  // OBJC-BOOL-WITH-ERR-FALLBACK2-NEXT: var err: (any Error)? = nil
  // OBJC-BOOL-WITH-ERR-FALLBACK2-NEXT: do {
  // OBJC-BOOL-WITH-ERR-FALLBACK2-NEXT:   (str, unrelated) = try await ClassWithHandlerMethods.secondBoolFlagFailure("")
  // OBJC-BOOL-WITH-ERR-FALLBACK2-NEXT:   failure = false
  // OBJC-BOOL-WITH-ERR-FALLBACK2-NEXT: } catch {
  // OBJC-BOOL-WITH-ERR-FALLBACK2-NEXT:   err = error
  // OBJC-BOOL-WITH-ERR-FALLBACK2-NEXT:   failure = true
  // OBJC-BOOL-WITH-ERR-FALLBACK2-NEXT: }
  // OBJC-BOOL-WITH-ERR-FALLBACK2-EMPTY:
  // OBJC-BOOL-WITH-ERR-FALLBACK2-NEXT: guard !failure && failure == .random() else { fatalError() }
  // OBJC-BOOL-WITH-ERR-FALLBACK2-NEXT: print("much fails", unrelated, str)
}
