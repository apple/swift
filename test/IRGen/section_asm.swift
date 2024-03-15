// RUN: %target-swift-frontend -enable-experimental-feature SymbolLinkageMarkers -primary-file %S/section.swift -S -parse-as-library | %FileCheck --check-prefix CHECK%target-os-binfmt-elf %s

// REQUIRES: swift_in_compiler
// UNSUPPORTED: CPU=wasm32

// CHECK: .section{{.*}}__TEXT,__mysection
// CHECK-NOT: .section
// CHECK: $s7section3fooyyF:

// CHECK: .section{{.*}}__TEXT,__mysection
// CHECK-NOT: .section
// CHECK: $s7section8MyStructV3fooyyF:

// CHECK: .section{{.*}}__DATA,__mysection
// CHECK-NOT: .section
// CHECK: $s7section2g0Sivp:
// CHECK-NOT: .section
// CHECK: $s7section2g1Si_Sitvp:
// CHECK-NOT: .section
// CHECK: $s7section2g2Sbvp:
// CHECK-NOT: .section
// CHECK: $s7section2g3Sbvp:
// CHECK-NOT: .section
// CHECK: $s7section2g4SpySiGSgvp:
// CHECK-NOT: .section
// CHECK: $s7section2g5SpySiGSgvp:
// CHECK-NOT: .section
// CHECK: $s7section8MyStructV7static0SivpZ:

// CHECKELF: .section{{.*}}"__TEXT,__mysection","ax"
// CHECKELF-NOT: .section
// CHECKELF: $s7section3fooyyF:

// CHECKELF: .section{{.*}}"__TEXT,__mysection","ax"
// CHECKELF-NOT: .section
// CHECKELF: $s7section8MyStructV3fooyyF:

// CHECKELF: .section{{.*}}"__DATA,__mysection","aw"
// CHECKELF-NOT: .section
// CHECKELF: $s7section2g0Sivp:
// CHECKELF-NOT: .section
// CHECKELF: $s7section2g1Si_Sitvp:
// CHECKELF-NOT: .section
// CHECKELF: $s7section2g2Sbvp:
// CHECKELF: .section{{.*}}"__DATA,__mysection","awR"
// CHECKELF: $s7section2g3Sbvp:
// CHECKELF: .section{{.*}}"__DATA,__mysection","aw"
// CHECKELF: $s7section2g4SpySiGSgvp:
// CHECKELF-NOT: .section
// CHECKELF: $s7section2g5SpySiGSgvp:
// CHECKELF-NOT: .section
// CHECKELF: $s7section8MyStructV7static0SivpZ:
