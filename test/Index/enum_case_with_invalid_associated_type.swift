enum MyEnum {
// RUN: %target-swift-ide-test -print-indexed-symbols -source-filename %s | %FileCheck %s
  case test(artifactID: String, hostTriple: Triple)
// CHECK: enumerator/Swift | test(artifactID:hostTriple:)
// CHECK: param/Swift | artifactID
// CHECK: param/Swift | hostTriple
}
