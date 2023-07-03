// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -static -o %t %S/sil_witness_tables_external_conformance.swift
// RUN: %target-swift-frontend -I %t -primary-file %s -emit-ir | %FileCheck %s

// REQUIRES: CPU=x86_64

import sil_witness_tables_external_conformance

// FIXME: This should be a SIL test, but we can't parse sil_witness_tables
// yet.

protocol A {}

protocol P {
  associatedtype Assoc: A

  static func staticMethod()
  func instanceMethod()
}

protocol Q : P {
  func qMethod()
}

protocol QQ {
  func qMethod()
}

struct AssocConformer: A {}

struct Conformer: Q, QQ {
  typealias Assoc = AssocConformer

  static func staticMethod() {}
  func instanceMethod() {}
  func qMethod() {}
}

// CHECK: [[EXTERNAL_CONFORMER_EXTERNAL_P_WITNESS_TABLE:@"\$s39sil_witness_tables_external_conformance17ExternalConformerVAA0F1PAAWP"]] = external{{( dllimport)?}} global ptr, align 8
// CHECK: [[CONFORMER_Q_WITNESS_TABLE:@"\$s18sil_witness_tables9ConformerVAA1QAAWP"]] = hidden constant [3 x ptr] [
// CHECK:   ptr [[CONFORMER_P_WITNESS_TABLE:@"\$s18sil_witness_tables9ConformerVAA1PAAWP"]],
// CHECK:   ptr @"$s18sil_witness_tables9ConformerVAA1QA2aDP7qMethod{{[_0-9a-zA-Z]*}}FTW"
// CHECK: ]
// CHECK: [[CONFORMER_P_WITNESS_TABLE]] = hidden global [5 x ptr] [
// CHECK:   @"associated conformance 18sil_witness_tables9ConformerVAA1PAA5AssocAaDP_AA1A"
// CHECK:   "symbolic{{.*}}18sil_witness_tables14AssocConformerV"
// CHECK:   ptr @"$s18sil_witness_tables9ConformerVAA1PA2aDP12staticMethod{{[_0-9a-zA-Z]*}}FZTW",
// CHECK:   ptr @"$s18sil_witness_tables9ConformerVAA1PA2aDP14instanceMethod{{[_0-9a-zA-Z]*}}FTW"
// CHECK: ]
// CHECK: [[CONFORMER2_P_WITNESS_TABLE:@"\$s18sil_witness_tables10Conformer2VAA1PAAWP"]] = hidden global [5 x ptr]

struct Conformer2: Q {
  typealias Assoc = AssocConformer

  static func staticMethod() {}
  func instanceMethod() {}
  func qMethod() {}
}

// CHECK-LABEL: define hidden swiftcc void @"$s18sil_witness_tables7erasure1cAA2QQ_pAA9ConformerV_tF"(ptr noalias nocapture sret({{.*}}) %0)
// CHECK:         [[WITNESS_TABLE_ADDR:%.*]] = getelementptr inbounds %T18sil_witness_tables2QQP, ptr %0, i32 0, i32 2
// CHECK-NEXT:    store ptr [[CONFORMER_QQ_WITNESS_TABLE:@"\$s.*WP"]], ptr [[WITNESS_TABLE_ADDR]], align 8
func erasure(c: Conformer) -> QQ {
  return c
}

// CHECK-LABEL: define hidden swiftcc void @"$s18sil_witness_tables15externalErasure1c0a1_b1_c1_D12_conformance9ExternalP_pAD0G9ConformerV_tF"(ptr noalias nocapture sret({{.*}}) %0)
// CHECK:         [[WITNESS_TABLE_ADDR:%.*]] = getelementptr inbounds %T39sil_witness_tables_external_conformance9ExternalPP, ptr %0, i32 0, i32 2
// CHECK-NEXT:    store ptr [[EXTERNAL_CONFORMER_EXTERNAL_P_WITNESS_TABLE]], ptr %2, align 8
func externalErasure(c: ExternalConformer) -> ExternalP {
  return c
}

// FIXME: why do these have different linkages?

// CHECK-LABEL: define hidden swiftcc %swift.metadata_response @"$s18sil_witness_tables14AssocConformerVMa"(i64 %0)
// CHECK:         ret %swift.metadata_response { ptr getelementptr inbounds {{.*}} @"$s18sil_witness_tables14AssocConformerVMf", i32 0, i32 2), i64 0 }
