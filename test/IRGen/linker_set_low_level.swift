// RUN: %target-swift-frontend -enable-experimental-feature SymbolLinkageMarkers %s -emit-ir -parse-as-library | %FileCheck %s

// REQUIRES: swift_in_compiler

@_used
@_section("__TEXT,__mysection")
let my_global1: Int = 42

@_used
@_section("__TEXT,__mysection")
let my_global2: Int = 46

@_silgen_name(raw: "section$start$__TEXT$__mysection")
var mysection_start: Int

@_silgen_name(raw: "section$end$__TEXT$__mysection")
var mysection_end: Int

// CHECK: @"$s20linker_set_low_level10my_global1Sivp" = {{.*}}constant %TSi <{ {{(i64|i32)}} 42 }>, section "__TEXT,__mysection
// CHECK: @"$s20linker_set_low_level10my_global2Sivp" = {{.*}}constant %TSi <{ {{(i64|i32)}} 46 }>, section "__TEXT,__mysection
// CHECK: @"\01section$start$__TEXT$__mysection" = {{.*}}global %TSi
// CHECK: @"\01section$end$__TEXT$__mysection" = {{.*}}global %TSi
