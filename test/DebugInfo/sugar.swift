// RUN: %target-swift-frontend %s -emit-ir -g -o - | %FileCheck %s

// Apart from typealiases, sugared types are just mangled as their desugared type.
//
// This test makes sure that we don't mess up type substitutions when mangling
// types containing sugared types by virtue of all manglings getting round-tripped
// through the remangler.

let o: (Int?, Optional<Int>) = (nil, nil)
let a: ([[Int]], [Array<Int>], Array<[Int]>, Array<Array<Int>>) = ([], [], [], [])
let d: ([Int : Float], Dictionary<Int, Float>) = ([:], [:])
let p: (((((Int)))), Int) = (0, 0)

// CHECK: !DICompositeType(tag: DW_TAG_structure_type, name: "$sSiXSq_SiSgtD", {{.*}})
// CHECK: !DICompositeType(tag: DW_TAG_structure_type, name: "$sSiXSaXSa_SaySiGXSaSaySiXSaGSayAAGtD", {{.*}})
// CHECK: !DICompositeType(tag: DW_TAG_structure_type, name: "$sSiSfXSD_SDySiSfGtD", {{.*}})
// CHECK: !DICompositeType(tag: DW_TAG_structure_type, name: "$sSi_SitD", {{.*}})
