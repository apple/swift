// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -static -o %t %S/sil_witness_tables_external_conformance.swift
// RUN: %target-swift-frontend %use_no_opaque_pointers -I %t -primary-file %s -emit-ir | %FileCheck %s
// RUN: %target-swift-frontend -I %t -primary-file %s -emit-ir

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

// CHECK: [[EXTERNAL_CONFORMER_EXTERNAL_P_WITNESS_TABLE:@"\$s39sil_witness_tables_external_conformance17ExternalConformerVAA0F1PAAWP"]] = external{{( dllimport)?}} global i8*, align 8
// CHECK: [[CONFORMER_Q_WITNESS_TABLE:@"\$s18sil_witness_tables9ConformerVAA1QAAWP"]] = hidden constant [3 x i8*] [
// CHECK:   i8* bitcast ([5 x i8*]* [[CONFORMER_P_WITNESS_TABLE:@"\$s18sil_witness_tables9ConformerVAA1PAAWP"]] to i8*),
// CHECK:   i8* bitcast (void (%T18sil_witness_tables9ConformerV*, %swift.type*, i8**)* @"$s18sil_witness_tables9ConformerVAA1QA2aDP7qMethod{{[_0-9a-zA-Z]*}}FTW" to i8*)
// CHECK: ]
// CHECK: [[CONFORMER_P_WITNESS_TABLE]] = hidden global [5 x i8*] [
// CHECK:   @"associated conformance 18sil_witness_tables9ConformerVAA1PAA5AssocAaDP_AA1A"
// CHECK:   "symbolic{{.*}}18sil_witness_tables14AssocConformerV"
// CHECK:   i8* bitcast (void (%swift.type*, %swift.type*, i8**)* @"$s18sil_witness_tables9ConformerVAA1PA2aDP12staticMethod{{[_0-9a-zA-Z]*}}FZTW" to i8*),
// CHECK:   i8* bitcast (void (%T18sil_witness_tables9ConformerV*, %swift.type*, i8**)* @"$s18sil_witness_tables9ConformerVAA1PA2aDP14instanceMethod{{[_0-9a-zA-Z]*}}FTW" to i8*)
// CHECK: ]
// CHECK: [[CONFORMER2_P_WITNESS_TABLE:@"\$s18sil_witness_tables10Conformer2VAA1PAAWP"]] = hidden global [5 x i8*]

struct Conformer2: Q {
  typealias Assoc = AssocConformer

  static func staticMethod() {}
  func instanceMethod() {}
  func qMethod() {}
}

// CHECK-LABEL: define hidden swiftcc void @"$s18sil_witness_tables7erasure1cAA2QQ_pAA9ConformerV_tF"(%T18sil_witness_tables2QQP* noalias nocapture sret({{.*}}) %0)
// CHECK:         [[WITNESS_TABLE_ADDR:%.*]] = getelementptr inbounds %T18sil_witness_tables2QQP, %T18sil_witness_tables2QQP* %0, i32 0, i32 2
// CHECK-NEXT:    store i8** getelementptr inbounds ([2 x i8*], [2 x i8*]* [[CONFORMER_QQ_WITNESS_TABLE:@"\$s.*WP"]], i32 0, i32 0), i8*** [[WITNESS_TABLE_ADDR]], align 8
func erasure(c: Conformer) -> QQ {
  return c
}

// CHECK-LABEL: define hidden swiftcc void @"$s18sil_witness_tables15externalErasure1c0a1_b1_c1_D12_conformance9ExternalP_pAD0G9ConformerV_tF"(%T39sil_witness_tables_external_conformance9ExternalPP* noalias nocapture sret({{.*}}) %0)
// CHECK:         [[WITNESS_TABLE_ADDR:%.*]] = getelementptr inbounds %T39sil_witness_tables_external_conformance9ExternalPP, %T39sil_witness_tables_external_conformance9ExternalPP* %0, i32 0, i32 2
// CHECK-NEXT:    store i8** [[EXTERNAL_CONFORMER_EXTERNAL_P_WITNESS_TABLE]], i8*** %2, align 8
func externalErasure(c: ExternalConformer) -> ExternalP {
  return c
}

// FIXME: why do these have different linkages?

// CHECK-LABEL: define hidden swiftcc %swift.metadata_response @"$s18sil_witness_tables14AssocConformerVMa"(i64 %0)
// CHECK:         ret %swift.metadata_response { %swift.type* bitcast (i64* getelementptr inbounds {{.*}} @"$s18sil_witness_tables14AssocConformerVMf", i32 0, i32 2) to %swift.type*), i64 0 }
