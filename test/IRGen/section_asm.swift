// RUN: %target-swift-frontend -enable-experimental-feature SymbolLinkageMarkers -primary-file %S/section.swift -S -parse-as-library | %FileCheck %s
// REQUIRES: swift_in_compiler
// UNSUPPORTED: CPU=wasm32

// CHECK: .section{{.*}}"__TEXT,__mysection","ax"
// CHECK-NOT: .section
// CHECK: $s7section3fooyyF:

// CHECK: .section{{.*}}"__TEXT,__mysection","ax"
// CHECK-NOT: .section
// CHECK: $s7section8MyStructV3fooyyF:

// CHECK: .section{{.*}}"__DATA,__mysection","aw"
// CHECK-NOT: .section
// CHECK: $s7section2g0Sivp:
// CHECK-NOT: .section
// CHECK: $s7section2g1Si_Sitvp:
// CHECK-NOT: .section
// CHECK: $s7section2g2Sbvp:
// CHECK: .section{{.*}}"__DATA,__mysection","awR"
// CHECK: $s7section2g3Sbvp:
// CHECK: .section{{.*}}"__DATA,__mysection","aw"
// CHECK: $s7section2g4SpySiGSgvp:
// CHECK-NOT: .section
// CHECK: $s7section2g5SpySiGSgvp:
// CHECK-NOT: .section
// CHECK: $s7section8MyStructV7static0SivpZ:
