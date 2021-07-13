// RUN: %target-typecheck-verify-swift \
// RUN:   -define-availability "_iOS8Aligned:macOS 10.10, iOS 8.0" \
// RUN:   -define-availability "_iOS9Aligned:macOS 10.11, iOS 9.0" \
// RUN:   -define-availability "_iOS9:iOS 9.0" \
// RUN:   -define-availability "_macOS10_11:macOS 10.11" \
// RUN:   -define-availability "_myProject 1.0:macOS 10.11" \
// RUN:   -define-availability "_myProject 2.5:macOS 10.12"
// REQUIRES: OS=macosx

@available(_iOS8Aligned, *)
public func onMacOS10_10() {}

@available(_iOS9Aligned, *)
public func onMacOS10_11() {}

@available(_iOS9, _macOS10_11, tvOS 11.0, *)
public func composed() {}

@available(_iOS8Aligned, *)
@available(macOS, deprecated: 10.10)
public func onMacOSDeprecated() {}

@available(_myProject, *) // expected-error {{expected declaration}}
// expected-error @-1 {{reference to undefined version '0' for availability macro '_myProject'}}
public func onMyProject() {}

@available(_myProject 1.0, *)
public func onMyProjectV1() {}

@available(_myProject 2.5, *)
public func onMyProjectV2_5() {}

@available(_myProject 3.0, *)// expected-error {{expected declaration}}
// expected-error @-1 {{reference to undefined version '3.0' for availability macro '_myProject'}}
public func onMyProjectV3() {}

@available(_myProject 3_0, *)// expected-error {{expected declaration}}
// expected-error @-1 {{expected version number}}
public func brokenVersion() {}

@available(_unkownMacro, *) // expected-error {{expected declaration}}
// expected-error @-1 {{expected 'available' option such as 'unavailable', 'introduced', 'deprecated', 'obsoleted', 'message', or 'renamed'}}
public func unkownMacro() {}

@available(_iOS9) // expected-error {{must handle potential future platforms with '*'}}
public func noOtherOSes() {}

@available(_iOS8Aligned, *)
func client() {
  onMacOS10_10()
  onMacOS10_11() // expected-error {{is only available in macOS 10.11 or newer}}
  // expected-note @-1 {{add 'if #available' version check}}
  onMacOSDeprecated()

  if #available(_iOS9Aligned, *) {
    onMacOS10_11()
  }

  if #unavailable(_iOS9Aligned) {
    onMacOS10_11() // expected-error {{is only available in macOS 10.11 or newer}}
    // expected-note @-1 {{add 'if #available' version check}}
  } else {
    onMacOS10_11()
  }

  if #available(_unknownMacro, *) { } // expected-error {{expected version number}}
}

@inlinable
public func forbidMacrosInInlinableCode() {
  if #available(_iOS9Aligned, *) { } // expected-error {{availability macro cannot be used in inlinable global function}}
  if #available(_iOS9, _macOS10_11, *) { } // expected-error {{availability macro cannot be used in inlinable global function}}
  if #available(iOS 9.0, _macOS10_11, tvOS 9.0, *) { } // expected-error {{availability macro cannot be used in inlinable global function}}
  if #unavailable(_iOS9Aligned) { } // expected-error {{availability macro cannot be used in inlinable global function}}
  if #unavailable(_iOS9, _macOS10_11) { } // expected-error {{availability macro cannot be used in inlinable global function}}
  if #unavailable(iOS 9.0, _macOS10_11, tvOS 9.0) { } // expected-error {{availability macro cannot be used in inlinable global function}}
}

@_alwaysEmitIntoClient
public func forbidMacrosInInlinableCode1() {
  if #available(_iOS9Aligned, *) { } // expected-error {{availability macro cannot be used in inlinable global function}}
  if #available(_iOS9, _macOS10_11, *) { } // expected-error {{availability macro cannot be used in inlinable global function}}
  if #available(iOS 9.0, _macOS10_11, tvOS 9.0, *) { } // expected-error {{availability macro cannot be used in inlinable global function}}
  if #unavailable(_iOS9Aligned) { } // expected-error {{availability macro cannot be used in inlinable global function}}
  if #unavailable(_iOS9, _macOS10_11) { } // expected-error {{availability macro cannot be used in inlinable global function}}
  if #unavailable(iOS 9.0, _macOS10_11, tvOS 9.0) { } // expected-error {{availability macro cannot be used in inlinable global function}}
}
