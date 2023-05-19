// RUN: %target-swift-frontend -emit-ir %s -g | %FileCheck %s

// CHECK: !{{[0-9]+}} = !DICompositeType(tag: DW_TAG_structure_type, name: "$sxxQp_QSiIgp_D", {{.*}})
public func f<each Input>(builder: (repeat each Input) -> ()) {}
