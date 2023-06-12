// Normal test

// RUN: %empty-directory(%t/normal)
// RUN: %target-build-swift-dylib(%t/normal/%target-library-name(MoveOnlySplit)) %S/Inputs/moveonly_split_module_source_input.swift -emit-module -emit-module-path %t/normal/MoveOnlySplit.swiftmodule -module-name MoveOnlySplit -DTEST_LIBRARY_WITHOUT_LIBRARY_EVOLUTION
// RUN: %target-codesign %t/normal/%target-library-name(MoveOnlySplit)

// RUN: %target-build-swift %s -lMoveOnlySplit -I %t/normal -L %t/normal -o %t/normal/main %target-rpath(%t/normal)
// RUN: %target-codesign %t/normal/main
// RUN: %target-run %t/normal/main %t/normal/%target-library-name(MoveOnlySplit) | %FileCheck %s

// Normal large

// RUN: %empty-directory(%t/normal_large)
// RUN: %target-build-swift-dylib(%t/normal_large/%target-library-name(MoveOnlySplit)) %S/Inputs/moveonly_split_module_source_input.swift -emit-module -emit-module-path %t/normal_large/MoveOnlySplit.swiftmodule -module-name MoveOnlySplit -DTEST_LIBRARY_WITHOUT_LIBRARY_EVOLUTION -DMAKE_LARGE
// RUN: %target-codesign %t/normal_large/%target-library-name(MoveOnlySplit)

// RUN: %target-build-swift %s -lMoveOnlySplit -I %t/normal_large -L %t/normal_large -o %t/normal_large/main %target-rpath(%t/normal_large)
// RUN: %target-codesign %t/normal_large/main
// RUN: %target-run %t/normal_large/main %t/normal_large/%target-library-name(MoveOnlySplit) | %FileCheck %s

// Library evolution test

// RUN: %empty-directory(%t/library_evolution)
// RUN: %target-build-swift-dylib(%t/library_evolution/%target-library-name(MoveOnlySplit)) %S/Inputs/moveonly_split_module_source_input.swift -emit-module -emit-module-path %t/library_evolution/MoveOnlySplit.swiftmodule -module-name MoveOnlySplit -DTEST_LIBRARY_WITH_LIBRARY_EVOLUTION
// RUN: %target-codesign %t/library_evolution/%target-library-name(MoveOnlySplit)

// RUN: %target-build-swift %s -lMoveOnlySplit -I %t/library_evolution -L %t/library_evolution -o %t/library_evolution/main %target-rpath(%t/library_evolution)
// RUN: %target-codesign %t/library_evolution/main
// RUN: %target-run %t/library_evolution/main %t/library_evolution/%target-library-name(MoveOnlySplit) | %FileCheck -check-prefix=CHECK-LIBRARY-EVOLUTION %s

// Library evolution large

// RUN: %empty-directory(%t/library_evolution_large)
// RUN: %target-build-swift-dylib(%t/library_evolution_large/%target-library-name(MoveOnlySplit)) %S/Inputs/moveonly_split_module_source_input.swift -emit-module -emit-module-path %t/library_evolution_large/MoveOnlySplit.swiftmodule -module-name MoveOnlySplit -DTEST_LIBRARY_WITH_LIBRARY_EVOLUTION -DMAKE_LARGE
// RUN: %target-codesign %t/library_evolution_large/%target-library-name(MoveOnlySplit)

// RUN: %target-build-swift %s -lMoveOnlySplit -I %t/library_evolution_large -L %t/library_evolution_large -o %t/library_evolution_large/main %target-rpath(%t/library_evolution_large)
// RUN: %target-codesign %t/library_evolution_large/main
// RUN: %target-run %t/library_evolution_large/main %t/library_evolution_large/%target-library-name(MoveOnlySplit) | %FileCheck -check-prefix=CHECK-LIBRARY-EVOLUTION %s


// REQUIRES: executable_test

import MoveOnlySplit

func main() {
    // CHECK: ==> LIBRARY: I am in the deinit!
    // CHECK: ==> My name is: John!
    // CHECK-LIBRARY-EVOLUTION: ==> LIBRARY_EVOLUTION: I am in the deinit!
    // CHECK-LIBRARY-EVOLUTION: ==> My name is: John!
    let server = MoveOnly()
}

main()
