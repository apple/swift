// RUN: %target-swift-emit-silgen -module-name parameterized -disable-availability-checking %s | %FileCheck %s

protocol P<A> {
  associatedtype A
}

// The mangling for generalized existentials is buggy; we decide whether to
// qualify the primary associated type requirement with a protocol or not
// by looking at the first generic parameter of the outer generic context.

struct S1 {
  // CHECK-LABEL: sil hidden [ossa] @$s13parameterized2S1V1fAA1P_pSi1AAaEPRts_XPyF : $@convention(method) (S1) -> @out any P<Int> {
  func f() -> any P<Int> {}
}

struct S2<T> {
  // CHECK-LABEL: sil hidden [ossa] @$s13parameterized2S2V1fAA1P_pSi1ARts_XPyF : $@convention(method) <T> (S2<T>) -> @out any P<Int> {
  func f() -> any P<Int> {}
}

struct S3<each T> {
  // CHECK-LABEL: sil hidden [ossa] @$s13parameterized2S3V1fAA1P_pSi1AAaEPRts_XPyF : $@convention(method) <each T> (S3<repeat each T>) -> @out any P<Int> {
  func f() -> any P<Int> {}
}
