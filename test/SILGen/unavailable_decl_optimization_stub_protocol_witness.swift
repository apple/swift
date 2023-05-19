// RUN: %target-swift-emit-silgen -module-name Test -parse-as-library %s -verify -unavailable-decl-optimization=stub | %FileCheck %s --check-prefixes=CHECK

public struct S {}

public protocol P {
  static func requirement(_ s: S) -> Self
}

public enum EnumWithProtocolWitness {
  @available(*, unavailable)
  case requirement(_ s: S)
}

@available(*, unavailable)
extension EnumWithProtocolWitness: P {}

// protocol witness for static P.requirement(_:) in conformance EnumWithProtocolWitness
//
// CHECK-LABEL: sil{{.*}}@$s4Test23EnumWithProtocolWitnessOAA1PA2aDP11requirementyxAA1SVFZTW
// CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN:ss31_diagnoseUnavailableCodeReacheds5NeverOy(FTwb|F)]] : $@convention(thin) () -> Never
// CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
// CHECK:         {{%.*}} = function_ref @$s4Test23EnumWithProtocolWitnessO11requirementyAcA1SVcACmF
// CHECK:       } // end sil function '$s4Test23EnumWithProtocolWitnessOAA1PA2aDP11requirementyxAA1SVFZTW'
//
// EnumWithProtocolWitness.requirement(_:)
//
// CHECK-LABEL: sil{{.*}}@$s4Test23EnumWithProtocolWitnessO11requirementyAcA1SVcACmF
// CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN]] : $@convention(thin) () -> Never
// CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
// CHECK:         enum $EnumWithProtocolWitness, #EnumWithProtocolWitness.requirement!enumelt
// CHECK:       } // end sil function '$s4Test23EnumWithProtocolWitnessO11requirementyAcA1SVcACmF'
