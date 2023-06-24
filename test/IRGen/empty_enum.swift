// RUN: %target-swift-frontend %use_no_opaque_pointers -primary-file %s -emit-ir | %FileCheck %s
// RUN: %target-swift-frontend -primary-file %s -emit-ir

// CHECK: @"$s10empty_enum6JamaisOMf" =
//   CHECK-SAME: {{@"\$sytWV"|i8\*\* getelementptr inbounds \(%swift.enum_vwtable, %swift.enum_vwtable\* @"\$s10empty_enum6JamaisOWV", i32 0, i32 0\)}}

enum Jamais {}
