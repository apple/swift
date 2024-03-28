// RUN: %target-typecheck-verify-swift \
// RUN:   -define-availability "_iOS13Aligned:macOS 10.15, iOS 13.0" \
// RUN:   -define-availability "_iOS14Aligned:macOS 11.0, iOS 14.0" \
// RUN:   -define-availability "_iOS14:iOS 14.0" \
// RUN:   -define-availability "_macOS11_0:macOS 11.0" \
// RUN:   -define-availability "_myProject 1.0:macOS 11.0" \
// RUN:   -define-availability "_myProject 2.5:macOS 12.5"

// RUN: %target-typecheck-verify-swift \
// RUN:   -enable-experimental-feature AvailabilityMacro='_iOS13Aligned:macOS 10.15, iOS 13.0' \
// RUN:   -enable-experimental-feature AvailabilityMacro="_iOS14Aligned:macOS 11.0, iOS 14.0" \
// RUN:   -enable-experimental-feature AvailabilityMacro='_iOS14:iOS 14.0' \
// RUN:   -enable-experimental-feature AvailabilityMacro="_macOS11_0:macOS 11.0" \
// RUN:   -enable-experimental-feature AvailabilityMacro='_myProject 1.0:macOS 11.0' \
// RUN:   -enable-experimental-feature AvailabilityMacro="_myProject 2.5:macOS 12.5"

// REQUIRES: OS=macosx

@available(_iOS13Aligned, *)
public func onMacOS10_15() {}

@available(_iOS14Aligned, *)
public func onMacOS11_0() {}

@available(_iOS14, _macOS11_0, tvOS 14.0, *)
public func composed() {}

@available(_iOS13Aligned, *)
@available(macOS, deprecated: 10.15)
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

@available(_unknownMacro, *) // expected-error {{expected declaration}}
// expected-error @-1 {{expected 'available' option such as 'unavailable', 'introduced', 'deprecated', 'obsoleted', 'message', or 'renamed'}}
public func unknownMacro() {}

@available(_iOS14) // expected-error {{must handle potential future platforms with '*'}}
public func noOtherOSes() {}

@available(_iOS13Aligned, *)
func client() {
  onMacOS10_15()
  onMacOS11_0() // expected-error {{is only available in macOS 11.0 or newer}}
  // expected-note @-1 {{add 'if #available' version check}}
  // expected-note @-2 {{change the @available attribute of the global function on macOS from 10.15 to 11.0}}
  onMacOSDeprecated()

  if #available(_iOS14Aligned, *) {
    onMacOS11_0()
  }

  if #unavailable(_iOS14Aligned) {
    onMacOS11_0() // expected-error {{is only available in macOS 11.0 or newer}}
    // expected-note @-1 {{add 'if #available' version check}}
    // expected-note @-2 {{change the @available attribute of the global function on macOS from 10.15 to 11.0}}
  } else {
    onMacOS11_0()
  }

  if #available(_unknownMacro, *) { } // expected-error {{expected version number}}
}

public func doIt(_ closure: () -> ()) {
  closure()
}

@inlinable
public func forbidMacrosInInlinableCode() {
  if #available(_iOS14Aligned, *) { } // expected-error {{availability macro cannot be used in an '@inlinable' function}}
  if #available(_iOS14, _macOS11_0, *) { } // expected-error {{availability macro cannot be used in an '@inlinable' function}}
  if #available(iOS 14.0, _macOS11_0, tvOS 14.0, *) { } // expected-error {{availability macro cannot be used in an '@inlinable' function}}
  if #unavailable(_iOS14Aligned) { } // expected-error {{availability macro cannot be used in an '@inlinable' function}}
  if #unavailable(_iOS14, _macOS11_0) { } // expected-error {{availability macro cannot be used in an '@inlinable' function}}
  if #unavailable(iOS 14.0, _macOS11_0, tvOS 14.0) { } // expected-error {{availability macro cannot be used in an '@inlinable' function}}
  doIt {
    if #available(_iOS14Aligned, *) { } // expected-error {{availability macro cannot be used in an '@inlinable' function}}
  }
}

@_alwaysEmitIntoClient
public func forbidMacrosInInlinableCode1() {
  if #available(_iOS14Aligned, *) { } // expected-error {{availability macro cannot be used in an '@_alwaysEmitIntoClient' function}}
  if #available(_iOS14, _macOS11_0, *) { } // expected-error {{availability macro cannot be used in an '@_alwaysEmitIntoClient' function}}
  if #available(iOS 14.0, _macOS11_0, tvOS 14.0, *) { } // expected-error {{availability macro cannot be used in an '@_alwaysEmitIntoClient' function}}
  if #unavailable(_iOS14Aligned) { } // expected-error {{availability macro cannot be used in an '@_alwaysEmitIntoClient' function}}
  if #unavailable(_iOS14, _macOS11_0) { } // expected-error {{availability macro cannot be used in an '@_alwaysEmitIntoClient' function}}
  if #unavailable(iOS 14.0, _macOS11_0, tvOS 14.0) { } // expected-error {{availability macro cannot be used in an '@_alwaysEmitIntoClient' function}}
  doIt {
    if #available(_iOS14Aligned, *) { } // expected-error {{availability macro cannot be used in an '@_alwaysEmitIntoClient' function}}
  }
}

@available(_iOS13Aligned, *)
@backDeployed(before: _iOS14Aligned)
public func forbidMacrosInInlinableCode2() {
  if #available(_iOS14Aligned, *) { } // expected-error {{availability macro cannot be used in a '@backDeployed' function}}
  if #available(_iOS14, _macOS11_0, *) { } // expected-error {{availability macro cannot be used in a '@backDeployed' function}}
  if #available(iOS 14.0, _macOS11_0, tvOS 14.0, *) { } // expected-error {{availability macro cannot be used in a '@backDeployed' function}}
  if #unavailable(_iOS14Aligned) { } // expected-error {{availability macro cannot be used in a '@backDeployed' function}}
  if #unavailable(_iOS14, _macOS11_0) { } // expected-error {{availability macro cannot be used in a '@backDeployed' function}}
  if #unavailable(iOS 14.0, _macOS11_0, tvOS 14.0) { } // expected-error {{availability macro cannot be used in a '@backDeployed' function}}
  doIt {
    if #available(_iOS14Aligned, *) { } // expected-error {{availability macro cannot be used in a '@backDeployed' function}}
  }
}
