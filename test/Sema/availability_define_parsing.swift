// RUN: not %target-swift-frontend -typecheck %s \
// RUN:   -define-availability "_brokenParse:a b c d" \
// RUN:   -define-availability ":a b c d" \
// RUN:   -define-availability "_justAName" \
// RUN:   -define-availability "_brokenPlatforms:spaceOS 10.11" \
// RUN:   -define-availability "_refuseWildcard:iOS 13.0, *, macOS 11.0" \
// RUN:   -define-availability "_duplicateVersion 1.0:iOS 13.0" \
// RUN:   -define-availability "_duplicateVersion 1.0:iOS 13.0" \
// RUN:   2>&1 | %FileCheck %s

// Force reading the argument macros.
@available(_brokenPlatforms)
public func brokenPlaforms() {}

// CHECK: -define-availability argument:1:16: error: expected version number
// CHECK-NEXT: _brokenParse:a b c d

// CHECK: -define-availability argument:1:1: error: expected an identifier to begin an availability macro definition
// CHECK-NEXT: :a b c d

// CHECK: -define-availability argument:1:11: error: expected ':' after '_justAName' in availability macro definition
// CHECK-NEXT: _justAName

// CHECK: -define-availability argument:1:31: warning: unrecognized platform name 'spaceOS'
// CHECK-NEXT: _brokenPlatforms:spaceOS 10.11

// CHECK: -define-availability argument:1:27: error: future platforms identified by '*' cannot be used in an availability macro
// CHECK-NEXT: _refuseWildcard

// CHECK: duplicate definition of availability macro '_duplicateVersion' for version '1.0'
// CHECK-NEXT: _duplicateVersion
