// RUN: %empty-directory(%t)
// RUN: %target-swiftc_driver -emit-module -module-name server -emit-module-path %t/server.swiftmodule %s %S/Inputs/moveonly_split_module_source_input.swift
// RUN: %target-swift-frontend -module-name server -primary-file %s %S/Inputs/moveonly_split_module_source_input.swift -emit-ir -emit-module-path %t/server.swiftmodule | %FileCheck %s -check-prefix=REFERRING_MODULE
// RUN: %target-swift-frontend -module-name server  %s -primary-file %S/Inputs/moveonly_split_module_source_input.swift -emit-ir -emit-module-path %t/server.swiftmodule | %FileCheck %s -check-prefix=DEFINING_MODULE

// Make sure we call the deinit through the value witness table in the other module.

// REFERRING_MODULE-LABEL: define {{.*}}swiftcc void @"$s6serverAAV4mainyyKFZ"(%swift.refcounted* swiftself %0, %swift.error** noalias nocapture swifterror dereferenceable({{4|8}}) %1) {{.*}}{
// REFERRING_MODULE: [[SERVER:%.*]] = alloca %T6server8MoveOnlyV
// REFERRING_MODULE: [[VALUE_WITNESS_TABLE:%.*]] = getelementptr inbounds i8*, i8** %"$s6server8MoveOnlyVN.valueWitnesses"
// REFERRING_MODULE: [[VALUE_WITNESS:%.*]] = load i8*, i8** [[VALUE_WITNESS_TABLE]]
// REFERRING_MODULE: [[DESTROY:%.*]] = bitcast i8* [[VALUE_WITNESS]]
// REFERRING_MODULE: [[CAST_SERVER:%.*]] = bitcast %T6server8MoveOnlyV* [[SERVER]]
// REFERRING_MODULE: call void [[DESTROY]]({{%.*}} [[CAST_SERVER]], {{%.*}} @"$s6server8MoveOnlyVN")
// REFERRING_MODULE: [[CAST_SERVER:%.*]] = bitcast %T6server8MoveOnlyV* [[SERVER]]
// REFERRING_MODULE: call void @llvm.lifetime.end.p0i8({{.*}}, i8* [[CAST_SERVER]])
// REFERRING_MODULE-NEXT: ret void

// Make sure that in the other module, we do call the deinit directly from the value witness.
// DEFINING_MODULE-LABEL: define internal void @"$s6server8MoveOnlyVwxx"(%swift.opaque* noalias %object, %swift.type* %MoveOnly) {{.*}} {
// DEFINING_MODULE: [[SELF:%.*]] = bitcast %swift.opaque* [[ARG:%.*]] to %T6server8MoveOnlyV*
// DEFINING_MODULE: [[VAR:%.*]] = getelementptr inbounds {{%.*}}, {{%.*}}* [[SELF]]
// DEFINING_MODULE: [[LOADED_VAR:%.*]] = load {{%.*}}*, {{%.*}}** [[VAR]],
// DEFINING_MODULE: [[VAR2:%.*]] = getelementptr inbounds {{%.*}}, {{%.*}}* [[SELF]]
// DEFINING_MODULE: [[LOADED_VAR2:%.*]] = load {{%.*}}*, {{%.*}}** [[VAR2]],
// DEFINING_MODULE: call swiftcc void @"$s6server8MoveOnlyVfD"({{%.*}}* [[LOADED_VAR]], {{%.*}}* [[LOADED_VAR2]])
@main
public struct server {
    public static func main() throws {
        let server = MoveOnly()
        _ = server
    }
}
