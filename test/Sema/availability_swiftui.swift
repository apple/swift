// RUN: %target-typecheck-verify-swift -verify -target %target-cpu-apple-macosx12.0 -module-name SwiftUI -library-level api
// RUN: not %target-typecheck-verify-swift -verify -target %target-cpu-apple-macosx12.0 -module-name Other -library-level api

// REQUIRES: OS=macosx

@available(macOS 11, *)
public class LessAvailable {}

@available(macOS 10.15, *)
@usableFromInline
class AnyColorBox: LessAvailable {} // Ok, exception specifically for AnyColorBox

@available(macOS 10.15, *)
@usableFromInline
class OtherClass: LessAvailable {} // expected-error {{'LessAvailable' is only available in macOS 11 or newer; clients of 'SwiftUI' may have a lower deployment target}}
// expected-note @-1 {{update @available attribute for macOS from '10.15' to '11' to meet the requirements of 'LessAvailable'}} {{13:18-23=11}}
