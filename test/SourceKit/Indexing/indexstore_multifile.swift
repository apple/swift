// RUN: %empty-directory(%t)
// RUN: %sourcekitd-test -req=index-to-store %s -index-store-path %t/idx -index-unit-output-path %t/indexstore_multifile.o -- -c -module-name indexstoremodule -o args_output_path.o %s %S/Inputs/indexstore_multifile_other.swift
// RUN: c-index-test core -print-unit %t/idx | %FileCheck  --dump-input=always --dump-input-filter=all %s

struct Foo {
  let bar: Int
  let counter: Int
  let other: OtherStruct

  init(bar: Int, counter: Int = 0, other: OtherStruct) {
    self.bar = bar
    self.counter = counter
    self.other = other
  }
}

// CHECK: indexstore_multifile.o-{{.*}}
// CHECK: module-name: indexstoremodule
// CHECK: main-path: SOURCE_DIR{{/|\\}}test{{/|\\}}SourceKit{{/|\\}}Indexing{{/|\\}}indexstore_multifile.swift
// CHECK: out-file: BUILD_DIR{{.*}}indexstore_multifile.o
// CHECK: is-debug: 1
// CHECK: Unit | system | Swift | {{.*}}{{/|\\}}Swift.swiftmodule
// CHECK: Record | user | SOURCE_DIR{{/|\\}}test{{/|\\}}SourceKit{{/|\\}}Indexing{{/|\\}}indexstore_multifile.swift | indexstore_multifile.swift-{{.*}}
