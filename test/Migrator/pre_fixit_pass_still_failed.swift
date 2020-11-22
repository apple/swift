// RUN: %empty-directory(%t) && not %target-swift-frontend -c -swift-version 4 -update-code -primary-file %s -emit-migrated-file-path %t/pre_fixit_pass.swift.result -o /dev/null

// We shouldn't be able to successfully use the pre-fix-it passes to
// fix this file before migration because there is a use of an
// cannot find 'bar' in scope.

func foo(s: String) {}
foo("Hello")
bar("Hello") // Unresolved failure
