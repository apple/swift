// RUN: %empty-directory(%t)

// RUN: %target-build-swift -emit-executable %s -g -o %t/constrained_existentials -emit-module -Xfrontend -disable-availability-checking
// RUN: sed -ne '/\/\/ *DEMANGLE: /s/\/\/ *DEMANGLE: *//p' < %s > %t/input
// RUN: %lldb-moduleimport-test %t/constrained_existentials -type-from-mangled=%t/input | %FileCheck %s --match-full-lines

func blackHole(_: Any...) {}
func blackHole_noncopyable(_: consuming any ~Copyable) {}

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

protocol NCProto: ~Copyable {}
struct NC: ~Copyable {}
struct GenNC<T: ~Copyable>: ~Copyable, NCProto {}

do {
  let e0: any NCProto & ~Copyable = GenNC<NC>()
  let e1: any NCProto & ~Copyable = GenNC<String>()

  // FIXME: breaks the MoveChecker (rdar://129885532)
//   blackHole_noncopyable(consume e0)
//   blackHole_noncopyable(consume e1)
}
