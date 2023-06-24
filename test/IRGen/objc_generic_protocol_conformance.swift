// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -emit-silgen -primary-file %s -enable-objc-interop -import-objc-header %S/Inputs/objc_generic_protocol_conformance.h | %FileCheck --check-prefix=SIL %s
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -emit-ir -primary-file %s -enable-objc-interop -import-objc-header %S/Inputs/objc_generic_protocol_conformance.h | %FileCheck --check-prefix=IR %s

protocol P {
  func foo()
}

extension Foo: P {}

// SIL-LABEL: sil private [transparent] [thunk] [ossa] @$sSo3FooCyxG33objc_generic_protocol_conformance1PA2dEP3fooyyFTW {{.*}} @pseudogeneric
// IR-LABEL: define internal swiftcc void @"$sSo3FooCyxG33objc_generic_protocol_conformance1PA2dEP3fooyyFTW"(ptr noalias nocapture swiftself dereferenceable({{4|8}}) %0, ptr{{( %Self)?}}, ptr{{( %SelfWitnessTable)?}})
