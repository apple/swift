// RUN: %target-swift-frontend -typecheck %s -swift-version 3
// RUN: %empty-directory(%t)
// RUN: mkdir -p %t && %target-swift-frontend -typecheck -update-code -primary-file %s -emit-migrated-file-path %t/double_fixit_ok.result -swift-version 3
// RUN: diff -u %s.expected %t/double_fixit_ok.result
// RUN: %target-swift-frontend -typecheck %s.expected -swift-version 4

@available(swift, obsoleted: 4,  renamed: "Thing.constant___renamed")
let ThingConstantGotRenamed = 1

@available(swift, obsoleted: 4,  renamed: "Thing.constant_renamed")
let ThingConstantWasRenamed = 1

struct Thing {
  static let constant___renamed = 1
  static let constant_renamed = 1
  func foo(_ c: Int) {}
}

class MyClass {
  func foo() {
    let _: Thing = {
      let t = Thing()
      t.foo(ThingConstantGotRenamed)
      t.foo(ThingConstantWasRenamed)
      return t
    }()
  }
}


