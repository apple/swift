// RUN: %target-swift-frontend -print-ast %s 2>&1 | %FileCheck %s

protocol Archivable {
  func archive(version: String)
}
// CHECK: internal protocol Archivable {
// CHECK:   func archive(version: String)
// CHECK: }

func archive(_ a: Archivable) {
  a.archive(version: "1")
}
// CHECK: internal func archive(_ a: Archivable) {
// CHECK:   a.archive(version: "1")
// CHECK: }

func cast(_ a: Any) {
  let conditional = a as? Archivable
  let forced = a as! Archivable
}
// CHECK: internal func cast(_ a: Any) {
// CHECK:   @_hasInitialValue private let conditional: (any Archivable)? = a as? any Archivable
// CHECK:   @_hasInitialValue private let forced: any Archivable = a as! any Archivable
// CHECK: }
