// RUN: %empty-directory(%t)
// RUN: cp %s %t/input.swift
// RUN: %target-swift-frontend -c -swift-version 4 -update-code -primary-file %t/input.swift -emit-migrated-file-path %t/always_remove_old_remap_file.result -emit-remap-file-path %t/always_remove_old_remap_file.remap -o /dev/null
// RUN: ls %t/always_remove_old_remap_file.remap

// Simulate leaving behind code that can't build in Swift 4:
// RUN: echo asdfads >> %t/input.swift

// Migrate again. This should delete the old remap file.
// RUN: not %target-swift-frontend -swift-version 4 -c -update-code -primary-file %t/input.swift -emit-migrated-file-path %t/always_remove_old_remap_file.result -emit-remap-file-path %t/always_remove_old_remap_file.remap -o /dev/null

// RUN: not ls %t/always_remove_old_remap_file.remap

func foo() {}
