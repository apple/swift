
// RUN: %target-swift-emit-silgen -verify -module-name nested_types_referencing_nested_functions %s | %FileCheck %s

do {
  func foo() { bar(2) }
  func bar<T>(_: T) { foo() }

  class Foo {
    // CHECK-LABEL: sil private [ossa] @$s025nested_types_referencing_A10_functions3FooL_CACycfc : $@convention(method) (@owned Foo) -> @owned Foo {
    init() {
      foo()
    }
    // CHECK-LABEL: sil private [ossa] @$s025nested_types_referencing_A10_functions3FooL_C3zimyyF : $@convention(method) (@guaranteed Foo) -> ()
    func zim() {
      foo()
    }
    // CHECK-LABEL: sil private [ossa] @$s025nested_types_referencing_A10_functions3FooL_C4zangyyxlF : $@convention(method) <T> (@in_guaranteed T, @guaranteed Foo) -> ()
    func zang<T>(_ x: T) {
      bar(x)
    }
    // CHECK-LABEL: sil private [ossa] @$s025nested_types_referencing_A10_functions3FooL_CfD : $@convention(method) (@owned Foo) -> ()
    deinit {
      foo()
    }
  }

  let x = Foo()
  x.zim()
  x.zang(1)
  _ = Foo.zim
  _ = Foo.zang as (Foo) -> (Int) -> ()
  _ = x.zim
  _ = x.zang as (Int) -> ()
}

// Invalid case
do {
  var x = 123 // expected-note {{captured value declared here}}

  func local() {
    // expected-error@-1 {{closure captures 'x' before it is declared}}
    _ = x // expected-note {{captured here}}
  }

  class Bar {
    func zang() {
      local()
    }
  }
}
