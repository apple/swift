// RUN: %empty-directory(%t)

// RUN: %target-build-swift -emit-executable %s -g -o %t/constrained_existentials -emit-module -Xfrontend -disable-availability-checking
// RUN: sed -ne '/\/\/ *DEMANGLE: /s/\/\/ *DEMANGLE: *//p' < %s > %t/input
// RUN: %lldb-moduleimport-test %t/constrained_existentials -type-from-mangled=%t/input | %FileCheck %s --match-full-lines

func blackHole(_: Any...) {}

protocol BaseProto<A, B> {
  associatedtype A
  associatedtype B
}

protocol DerivedProto<A, B>: BaseProto {}

protocol OtherProto {}

struct S<A, B>: DerivedProto, OtherProto {}

// We should lift the artificial ban on compositions involving constrained protocol types
typealias BaseProtoIntStringAndOtherProto = BaseProto<Int, String> & OtherProto

do {
  let e0: any BaseProto<Int, String> = S<Int, String>()
  let e1: any BaseProto<Int, String>.Type = S<Int, String>.self
  let e2: (any BaseProto<Int, String>).Type = (any BaseProto<Int, String>).self

  blackHole(e0, e1, e2)
}

// DEMANGLE: $s24constrained_existentials9BaseProto_pSi1AAaBPRts_SS1BADRtsXPD
// DEMANGLE: $s24constrained_existentials9BaseProto_pSi1AAaBPRts_SS1BADRtsXPXmTD
// DEMANGLE: $s24constrained_existentials9BaseProto_pSi1AAaBPRts_SS1BADRtsXPXMtD

// CHECK: any BaseProto<Int, String>
// CHECK: @thick any BaseProto<Int, String>.Type
// CHECK: @thin (any BaseProto<Int, String>).Type

do {
  let e0: any DerivedProto<Int, String> = S<Int, String>()
  let e1: any DerivedProto<Int, String>.Type = S<Int, String>.self
  let e2: (any DerivedProto<Int, String>).Type = (any DerivedProto<Int, String>).self

  blackHole(e0, e1, e2)
}
