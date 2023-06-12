// RUN: %empty-directory(%t)

// Package name should not be empty
// RUN: not %target-swift-frontend -typecheck %s -package-name "" 2>&1 | %FileCheck %s -check-prefix CHECK-EMPTY
// CHECK-EMPTY: error: package-name is empty
// CHECK-EMPTY: error: 'log' has a package access level but no -package-name was specified: {{.*}}.swift

// If package access level is used but no package-name is passed, it should error
// RUN: not %target-swift-frontend -typecheck %s 2>&1 | %FileCheck %s -check-prefix CHECK-MISSING
// CHECK-MISSING: error: 'log' has a package access level but no -package-name was specified: {{.*}}.swift

// Package name can be same as the module name
// RUN: %target-swift-frontend -module-name Logging -package-name Logging %s -emit-module -emit-module-path %t/Logging.swiftmodule
// RUN: test -f %t/Logging.swiftmodule

// Package name can be a standard library name
// RUN: %target-swift-frontend -module-name Logging -package-name Swift %s -emit-module -emit-module-path %t/Logging.swiftmodule
// RUN: test -f %t/Logging.swiftmodule

// Package name can have any unicode characters

// RUN: %target-swift-frontend %s -typecheck -verify -package-name " "
// RUN: %target-swift-frontend %s -typecheck -verify -package-name "swift-util.log"
// RUN: %target-swift-frontend %s -typecheck -verify -package-name "swift$util.log"
// RUN: %target-swift-frontend %s -typecheck -verify -package-name "swift\$util.log"
// RUN: %target-swift-frontend %s -typecheck -verify -package-name "swift*util.log"

// RUN: %target-swift-frontend %s -typecheck -verify -package-name "-swift*util.log"
// RUN: %target-swift-frontend %s -typecheck -verify -package-name ".swift*util-log"
// RUN: %target-swift-frontend %s -typecheck -verify -package-name "\#swift#utillog"
// RUN: %target-swift-frontend %s -typecheck -verify -package-name "swift^util\&lo\(g+@"

// RUN: %target-swift-frontend %s -typecheck -verify -package-name "swift-util$tools*log"
// RUN: %target-swift-frontend %s -typecheck -verify -package-name "swift/utils/tools/log.git"

// RUN: %target-swift-frontend %s -typecheck -verify -package-name "foo bar baz git"
// RUN: %target-swift-frontend %s -typecheck -verify -package-name "My-Logging%Pkg"

// RUN: %target-swift-frontend %s -typecheck -verify -package-name Προϊόν

// RUN: %target-swift-frontend %s -typecheck -verify -package-name “\n”
// RUN: %target-swift-frontend %s -typecheck -verify -package-name “\\n”

// RUN: %target-swift-frontend %s -typecheck -verify -package-name "a\\nb"
// RUN: %target-swift-frontend %s -typecheck -verify -package-name "a\nde-f.g ~!@#$%^&<>?/|:"

package func log() {}
