
// Normal test.
// RUN: %empty-directory(%t/normal)
// RUN: %target-swiftc_driver -emit-module -module-name server -emit-module-path %t/normal/server.swiftmodule %s %S/Inputs/moveonly_split_module_source_input.swift
// RUN: %target-swiftc_driver  -emit-executable -module-name server -emit-module-path %t/normal/server.swiftmodule %s %S/Inputs/moveonly_split_module_source_input.swift -o %t/normal/server
// RUN: %target-codesign %t/normal/server
// RUN: %target-run %t/normal/server | %FileCheck %s

// Large test.
// RUN: %empty-directory(%t/large)
// RUN: %target-swiftc_driver -emit-module -module-name server -emit-module-path %t/large/server.swiftmodule %s %S/Inputs/moveonly_split_module_source_input.swift -DMAKE_LARGE
// RUN: %target-swiftc_driver  -emit-executable -module-name server -emit-module-path %t/large/server.swiftmodule %s %S/Inputs/moveonly_split_module_source_input.swift -o %t/large/server
// RUN: %target-codesign %t/large/server
// RUN: %target-run %t/large/server | %FileCheck %s

// REQUIRES: executable_test

@main
public struct server {
    public static func main() throws {
        let server = MoveOnly() // CHECK: ==> I am in the deinit!
    }
}
