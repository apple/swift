// REQUIRES: swift_swift_parser

// RUN: %target-typecheck-verify-swift -swift-version 5 -parse-as-library -module-name MacrosTest

// We need this test because top-level freestanding macro expansions are parsed
// differently in library mode.

#undefinedMacro1
// expected-error@-1{{no macro named 'undefinedMacro1'}}

#undefinedMacro2 { definitelyNotDefined }
// expected-error@-1{{no macro named 'undefinedMacro2'}}
// expected-error@-2{{cannot find 'definitelyNotDefined' in scope}}
