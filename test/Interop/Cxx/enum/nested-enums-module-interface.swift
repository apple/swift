// RUN: %target-swift-ide-test -print-module -module-to-print=NestedEnums -I %S/Inputs -source-filename=x -enable-experimental-cxx-interop | %FileCheck %s

// CHECK: enum ns {
// CHECK:  struct EnumInNS : Equatable, RawRepresentable {
// CHECK:  }
// CHECK-NEXT:  static var kA: ns.EnumInNS { get }
// CHECK-NEXT:  static var kB: ns.EnumInNS { get }
// CHECK-NEXT:  enum ScopedEnumInNS : Int32 {
// CHECK:    case scopeA
// CHECK:    case scopeB
// CHECK-NEXT:  }
// CHECK-NEXT:  enum nestedNS {
// CHECK-NEXT:    struct EnumInNestedNS : Equatable, RawRepresentable {
// CHECK:    }
// CHECK-NEXT:    static var kNestedA: ns.nestedNS.EnumInNestedNS { get }
// CHECK-NEXT:    static var kNestedB: ns.nestedNS.EnumInNestedNS { get }
// CHECK-NEXT:  }
// CHECK-NEXT:}
