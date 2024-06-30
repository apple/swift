// RUN: %empty-directory(%t)

// RUN: %target-typecheck-verify-swift -show-diagnostics-after-fatal -package-name MyPackage -package-manifest %S/Inputs/package-deps/Package.swift -package-target-name MyTest -package-available-modules %S/Inputs/package-deps/available_modules.json -verify-additional-file %S/Inputs/package-deps/Package.swift

import Testing
// expected-error@-1{{no such module 'Testing'}}
