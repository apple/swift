// Currently fails because a <NULL> is output
// REQUIRES: rdar113765916

// Check interface produced for the standard library.
//
// REQUIRES: nonexecutable_test
// REQUIRES: swift_swift_parser

//
// RUN: %target-swift-frontend -typecheck %s
// RUN: %target-swift-ide-test -print-module -module-to-print=Swift -source-filename %s -print-interface > %t.txt
// RUN: %FileCheck -check-prefix=CHECK-ARGC %s < %t.txt
// RUN: %FileCheck %s < %t.txt
// RUN: %FileCheck -check-prefix=CHECK-SUGAR %s < %t.txt
// RUN: %FileCheck -check-prefix=CHECK-MUTATING-ATTR %s < %t.txt
// RUN: %FileCheck -check-prefix=NO-FIXMES %s < %t.txt
// RUN: %FileCheck -check-prefix=CHECK-ARGC %s < %t.txt

// RUN: %target-swift-ide-test -print-module -module-to-print=Swift -source-filename %s -print-interface-doc > %t-doc.txt
// RUN: %FileCheck %s < %t-doc.txt

// RUN: %target-swift-ide-test -print-module -module-to-print=Swift -source-filename %s -print-interface -skip-underscored-stdlib-protocols > %t-prot.txt

// CHECK-ARGC: static var argc: Int32 { get }

// Builtin macro references
// CHECK: Builtin.ExternalMacro

// CHECK-NOT: @rethrows
// CHECK-NOT: {{^}}import
// CHECK-NOT: _Double
// CHECK-NOT: _StringBuffer
// CHECK-NOT: _LegacyStringCore
// CHECK-NOT: _SwiftRawStringStorage
// CHECK-NOT: _SwiftStringStorage
// CHECK-NOT: _StringGuts
// CHECK-NOT: _ArrayBody
// DONT_CHECK-NOT: {{([^I]|$)([^n]|$)([^d]|$)([^e]|$)([^x]|$)([^a]|$)([^b]|$)([^l]|$)([^e]|$)}}
// CHECK-NOT: buffer: _ArrayBuffer
// CHECK-NOT: func ~>
// CHECK-NOT: _builtin
// CHECK-NOT: Builtin.
// CHECK-NOT: extension {{.*}}?
// CHECK-NOT: extension {{.*}}!
// CHECK-NOT: addressWithOwner
// CHECK-NOT: mutableAddressWithOwner
// CHECK-NOT: _ExpressibleByColorLiteral
// CHECK-NOT: _ExpressibleByFileReferenceLiteral
// CHECK-NOT: _ExpressibleByImageLiteral

// CHECK-SUGAR: extension Array :
// CHECK-SUGAR: extension Optional :

// CHECK-MUTATING-ATTR: mutating func

// NO-FIXMES-NOT: FIXME
// RUN: %target-swift-ide-test -print-module-groups -module-to-print=Swift -source-filename %s -print-interface > %t-group.txt
// RUN: %FileCheck -check-prefix=CHECK-GROUPS1 %s < %t-group.txt
// CHECK-GROUPS1: Module groups begin:
// CHECK-GROUPS1-DAG: Pointer
// CHECK-GROUPS1-DAG: C
// CHECK-GROUPS1-DAG: Protocols
// CHECK-GROUPS1-DAG: Optional
// CHECK-GROUPS1-DAG: Collection/Lazy Views
// CHECK-GROUPS1-DAG: Math
// CHECK-GROUPS1-DAG: Reflection
// CHECK-GROUPS1-DAG: Misc
// CHECK-GROUPS1-DAG: Collection
// CHECK-GROUPS1-DAG: Bool
// CHECK-GROUPS1-DAG: Assert
// CHECK-GROUPS1-DAG: String
// CHECK-GROUPS1-DAG: Collection/Array
// CHECK-GROUPS1-DAG: Collection/Type-erased
// CHECK-GROUPS1-NOT: <NULL>
// CHECK-GROUPS1: Module groups end.

/// Check that we can still print the interface of the stdlib with
/// deserialization safety enabled.
// RUN: %target-swift-ide-test -print-module-groups -module-to-print=Swift -source-filename %s -print-interface -enable-deserialization-safety > %t-group.txt
// RUN: %FileCheck -check-prefix=CHECK-GROUPS1 %s < %t-group.txt
