// RUN: %target-swift-frontend -typecheck %s -swift-version 3
// RUN: %target-swift-frontend -typecheck -update-code -primary-file %s -emit-migrated-file-path %t.result -swift-version 3
// RUN: diff -u %s.expected %t.result
// RUN: %target-swift-frontend -typecheck %s.expected -swift-version 4

func foo(f: (Void) -> ()) {}
