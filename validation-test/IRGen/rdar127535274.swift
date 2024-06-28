// RUN: %target-build-swift -g -Xfrontend -enable-experimental-feature -Xfrontend LayoutStringValueWitnesses -Xfrontend -enable-experimental-feature -Xfrontend LayoutStringValueWitnessesInstantiation -Xfrontend -enable-layout-string-value-witnesses -Xfrontend -enable-layout-string-value-witnesses-instantiation -Xfrontend -enable-library-evolution -c -parse-as-library -emit-ir %s | %FileCheck %s

// CHECK: define internal ptr @"$s13rdar1275352744TestVMi"
// CHECK:  [[METADATA:%.*]] = call ptr @swift_allocateGenericValueMetadataWithLayoutString
// CHECK:  call void @swift_generic_instantiateLayoutString(ptr @"type_layout_string l13rdar1275352744TestVyxG", ptr [[METADATA]])
// CHECK:  ret ptr [[METADATA]]
// CHECK: }
public struct Test<T> {
    let x: [T]
    let y: [T]
}
