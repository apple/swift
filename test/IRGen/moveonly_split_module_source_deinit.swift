// RUN: %empty-directory(%t)
// RUN: %target-swiftc_driver -emit-module -module-name server -emit-module-path %t/server.swiftmodule %s %S/Inputs/moveonly_split_module_source_input.swift
// RUN: %target-swift-frontend -module-name server -primary-file %s %S/Inputs/moveonly_split_module_source_input.swift -emit-ir -emit-module-path %t/server.swiftmodule | %FileCheck %s -check-prefix=REFERRING_MODULE
// RUN: %target-swift-frontend -module-name server  %s -primary-file %S/Inputs/moveonly_split_module_source_input.swift -emit-ir -emit-module-path %t/server.swiftmodule | %FileCheck %s -check-prefix=DEFINING_MODULE

// UNSUPPORTED: CPU=arm64e

// Make sure we call the deinit through the value witness table in the other module.

// REFERRING_MODULE-LABEL: define {{.*}}swiftcc void @"$s6serverAAV4mainyyKFZ"(ptr swiftself %0, ptr noalias nocapture swifterror dereferenceable({{(8|4)}}) %1) {{.*}}{
// REFERRING_MODULE: [[SERVER:%.*]] = alloca %T6server8MoveOnlyV
// REFERRING_MODULE: [[VALUE_WITNESS_TABLE:%.*]] = getelementptr inbounds ptr, ptr %"$s6server8MoveOnlyVN.valueWitnesses"
// REFERRING_MODULE: [[VALUE_WITNESS:%.*]] = load ptr, ptr [[VALUE_WITNESS_TABLE]]
// REFERRING_MODULE: call void [[VALUE_WITNESS]](ptr noalias [[SERVER]], ptr @"$s6server8MoveOnlyVN")

// Make sure that in the other module, we do call the deinit directly from the value witness.
// DEFINING_MODULE-LABEL: define internal void @"$s6server8MoveOnlyVwxx"(ptr noalias %object, ptr %MoveOnly) {{.*}} {
// DEFINING_MODULE: [[VAR:%.*]] = getelementptr inbounds {{%.*}}, ptr %object
// DEFINING_MODULE: [[LOADED_VAR:%.*]] = load ptr, ptr [[VAR]],
// DEFINING_MODULE: [[VAR2:%.*]] = getelementptr inbounds {{%.*}}, ptr %object
// DEFINING_MODULE: [[LOADED_VAR2:%.*]] = load ptr, ptr [[VAR2]],
// DEFINING_MODULE: call swiftcc void @"$s6server8MoveOnlyVfD"(ptr [[LOADED_VAR]], ptr [[LOADED_VAR2]])
@main
public struct server {
    public static func main() throws {
        let server = MoveOnly()
        _ = server
    }
}
