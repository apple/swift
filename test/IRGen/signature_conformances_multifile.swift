// RUN: %target-swift-frontend -disable-generic-metadata-prespecialization -emit-ir -primary-file %s %S/Inputs/signature_conformances_other.swift | %FileCheck %s

// Make sure we correctly determine the witness table is dependent, even though
// it was defined in a different file.

// CHECK-LABEL: define hidden swiftcc void @"$s32signature_conformances_multifile5passQyyF"()
func passQ() {
  // CHECK: call swiftcc void @"$s32signature_conformances_multifile12AlsoConformsVACyxGycfC"(ptr @"$sSiN")
  // CHECK: %0 = call {{.*}} @"$s32signature_conformances_multifile12AlsoConformsVySiGMD"
  // CHECK: %1 = call ptr @"$s32signature_conformances_multifile12AlsoConformsVySiGACyxGAA1QAAWl"()
  // CHECK: call swiftcc void @"$s32signature_conformances_multifile6takesQyyxAA1QRzlF"(ptr noalias nocapture undef, ptr %0, ptr %1)
  takesQ(AlsoConforms<Int>())

  // CHECK: ret void
}

// CHECK-LABEL: define hidden swiftcc void @"$s32signature_conformances_multifile5passPyyF"()
func passP() {
  // CHECK: call swiftcc void @"$s32signature_conformances_multifile8ConformsVACyxq_GycfC"(ptr @"$sSiN", ptr @"$sSSN")
  // CHECK: %0 = call {{.*}} @"$s32signature_conformances_multifile8ConformsVySiSSGMD"
  // CHECK: %1 = call ptr @"$s32signature_conformances_multifile8ConformsVySiSSGACyxq_GAA1PAAWl"()
  // CHECK: call swiftcc void @"$s32signature_conformances_multifile6takesPyyxAA1PRzlF"(ptr noalias nocapture undef, ptr %0, ptr %1)
  takesP(Conforms<Int, String>())

  // CHECK: ret void
}
