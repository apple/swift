// RUN: %empty-directory(%t) && %target-swift-frontend -c -swift-version 4 -update-code -primary-file %s -emit-migrated-file-path %t/migrated_null_migration.swift -emit-remap-file-path %t/null_migration.remap -o /dev/null
// RUN: %diff -u %s %t/migrated_null_migration.swift

// This file tests that, if all migration passes are no-op,
// there are no changes to the file.

protocol P {
  func foo()
}

class C : P {
  func foo() {}
}

struct S : P {
  func foo() {}
}

enum E : P {
  func foo() {}
}

