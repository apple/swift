// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -enable-experimental-feature MoveOnlyEnumDeinits -Xllvm -sil-disable-pass=simplification -g -emit-module  -module-name OtherModule %S/Inputs/moveonly_deinit.swift -emit-module-path %t/OtherModule.swiftmodule
// RUN: %target-swift-frontend -enable-experimental-feature MoveOnlyEnumDeinits -Xllvm -sil-disable-pass=simplification -g -I %t %s -emit-silgen
// RUN: %target-sil-opt -enable-experimental-feature MoveOnlyEnumDeinits %t/OtherModule.swiftmodule | %FileCheck -check-prefix=CHECK-SERIALIZED %s

// Make sure we can deserialize deinits of both enums and structs.

import OtherModule

// CHECK-SERIALIZED: sil_moveonlydeinit [serialized] MoveOnlyStruct {
// CHECK-SERIALIZED: sil_moveonlydeinit [serialized] MoveOnlyEnum {
let s = MoveOnlyStruct(desc: 5)
let e = MoveOnlyEnum.lhs(5)
