// RUN: %target-typecheck-verify-swift -enable-objc-interop -disable-objc-attr-requires-foundation-module

class Foo {
  func bar(_: bar) {} // expected-error{{cannot find type 'bar' in scope}}
}

class C {
	var triangle : triangle  // expected-error{{cannot find type 'triangle' in scope}}

	init() {}
}

typealias t = t // expected-error {{type alias 't' references itself}} expected-note {{while resolving type 't'}} expected-note {{through reference here}}
// expected-error@-1 {{circular reference}}

extension Foo {
  convenience init() {} // expected-error{{invalid redeclaration of synthesized 'init()'}}
}

class InitClass {
  init(arg: Bool) {} // expected-note{{add '@objc' to make this declaration overridable}}
  @objc init(baz: Int) {} // expected-note{{overridden declaration is here}}
  @objc dynamic init(bar: Int) {}
}
class InitSubclass: InitClass {}
// expected-note@-1{{implicit initializer 'init(baz:)' declared here}}
// expected-note@-2{{implicit initializer 'init(bar:)' declared here}}
extension InitSubclass {
  convenience init(arg: Bool) {} // expected-error{{non-@objc initializer 'init(arg:)' declared in 'InitClass' cannot be overridden from extension}}
  convenience override init(baz: Int) {}
  // expected-error@-1 {{initializer 'init(baz:)' with Objective-C selector 'initWithBaz:' conflicts with implicit initializer 'init(baz:)' with the same Objective-C selector}}
  // expected-error@-2 {{cannot override a non-dynamic class declaration from an extension}}
  convenience override init(bar: Int) {}
  // expected-error@-1 {{initializer 'init(bar:)' with Objective-C selector 'initWithBar:' conflicts with implicit initializer 'init(bar:)' with the same Objective-C selector}}
}

struct InitStruct {
  let foo: Int
}
extension InitStruct {
  init(foo: Int) {} // expected-error{{invalid redeclaration of synthesized memberwise 'init(foo:)'}}
}

// <rdar://problem/17564699> QoI: Structs should get convenience initializers
struct MyStruct {
  init(k: Int) {
  }
  convenience init() {  // expected-error {{initializers in structs are not marked with 'convenience'}} {{3-15=}}
    self.init(k: 1)
  }
}
