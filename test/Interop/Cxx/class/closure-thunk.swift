// RUN: %target-swiftxx-frontend -I %S/Inputs -emit-silgen %s | %FileCheck --dump-input=always %s

import NonTrivialClasses

// CHECK: file-check-string

public func f() {
  cfunc({NonTrivial in})
}

public func test1() {
  cfuncARCWeak({ARCWeak in})
}
