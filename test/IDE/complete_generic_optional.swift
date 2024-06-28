// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=FOO_OPTIONAL_1 | %FileCheck %s -check-prefix=FOO_OPTIONAL_1

struct Bar {
}

struct Foo<T> {
  func myFunction(_ foobar: T) {
  }
}

// https://github.com/apple/swift/issues/43259
// Code completion does not instantiate generic arguments of a type wrapped
// in an optional
let x: Foo<Bar>? = Foo<Bar>()
x.#^FOO_OPTIONAL_1^#
// FOO_OPTIONAL_1: Begin completions, 8 items
// FOO_OPTIONAL_1-DAG: Decl[InstanceMethod]/CurrNominal/Erase[1]: ?.myFunction({#(foobar): Bar#})[#Void#]; name=myFunction(:)
// FOO_OPTIONAL_1-DAG: Keyword[self]/CurrNominal: self[#Foo<Bar>?#]; name=self
