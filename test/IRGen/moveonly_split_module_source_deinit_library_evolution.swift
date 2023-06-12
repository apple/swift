// RUN: %empty-directory(%t)
// RUN: %target-build-swift-dylib(%t/%target-library-name(MoveOnlySplit)) -enable-library-evolution %S/Inputs/moveonly_split_module_source_input.swift -emit-module -emit-module-path %t/MoveOnlySplit.swiftmodule -module-name MoveOnlySplit -DTEST_LIBRARY_EVOLUTION
// RUN: %target-codesign %t/%target-library-name(MoveOnlySplit)

// RUN: %target-build-swift %s -lMoveOnlySplit -I %t -L %t -o %t/main %target-rpath(%t)
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main %t/%target-library-name(MoveOnlySplit) | %FileCheck -check-prefix=CHECK-LIBRARY-EVOLUTION %s

// REQUIRES: executable_test

import MoveOnlySplit

func main() {
    let server = MoveOnly() // CHECK-LIBRARY-EVOLUTION: ==> I am in the deinit resiliently!
}

main()
