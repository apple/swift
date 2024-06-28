/// Check semantic verification cross-import overlays with non-public imports.

// RUN: %empty-directory(%t)
// RUN: cp -r %S/Inputs/lib-templates/* %t/
// RUN: split-file --leading-lines %s %t

//--- SomeUnrelatedModule.swift
// RUN: %target-swift-emit-module-interface(%t/lib/swift/SomeUnrelatedModule.swiftinterface) %t/SomeUnrelatedModule.swift -module-name SomeUnrelatedModule

//--- BothPublic.swift
// RUN: %target-swift-emit-module-interface(%t.swiftinterface) %t/BothPublic.swift -enable-cross-import-overlays -I %t/lib/swift -module-name ClientLibrary -verify

public import DeclaringLibrary
public import BystandingLibrary
public import SomeUnrelatedModule // expected-warning {{public import of 'SomeUnrelatedModule' was not used in public declarations or inlinable code}}

public func fn(_: OverlayLibraryTy) {}


//--- BothHidden.swift
// RUN: %target-swift-emit-module-interface(%t.swiftinterface) %t/BothHidden.swift -enable-cross-import-overlays -I %t/lib/swift -module-name ClientLibrary -verify

internal import DeclaringLibrary
internal import BystandingLibrary

public func fn(_: OverlayLibraryTy) {}
// expected-error @-1 {{function cannot be declared public because its parameter uses an internal type}}
// expected-note @-2 {{struct 'OverlayLibraryTy' is imported by this file as 'internal' from '_OverlayLibrary'}}


//--- FirstHidden.swift
// RUN: %target-swift-emit-module-interface(%t.swiftinterface) %t/FirstHidden.swift -enable-cross-import-overlays -I %t/lib/swift -module-name ClientLibrary -verify

internal import DeclaringLibrary
public import BystandingLibrary

public func fn(_: OverlayLibraryTy) {}
// expected-error @-1 {{function cannot be declared public because its parameter uses an internal type}}
// expected-note @-2 {{struct 'OverlayLibraryTy' is imported by this file as 'internal' from '_OverlayLibrary'}}


//--- SecondHidden.swift
// RUN: %target-swift-emit-module-interface(%t.swiftinterface) %t/SecondHidden.swift -enable-cross-import-overlays -I %t/lib/swift -module-name ClientLibrary -verify

public import DeclaringLibrary
internal import BystandingLibrary

public func fn(_: OverlayLibraryTy) {}
// expected-error @-1 {{function cannot be declared public because its parameter uses an internal type}}
// expected-note @-2 {{struct 'OverlayLibraryTy' is imported by this file as 'internal' from '_OverlayLibrary'}}


//--- PrivateVsInternal.swift
// RUN: %target-swift-emit-module-interface(%t.swiftinterface) %t/PrivateVsInternal.swift -enable-cross-import-overlays -I %t/lib/swift -module-name ClientLibrary -verify

private import DeclaringLibrary
internal import BystandingLibrary

internal func fn(_: OverlayLibraryTy) {}
// expected-error @-1 {{function cannot be declared internal because its parameter uses a private type}}
// expected-note @-2 {{struct 'OverlayLibraryTy' is imported by this file as 'private' from '_OverlayLibrary'}}


//--- InternalVsPrivate.swift
// RUN: %target-swift-emit-module-interface(%t.swiftinterface) %t/InternalVsPrivate.swift -enable-cross-import-overlays -I %t/lib/swift -module-name ClientLibrary -verify

internal import DeclaringLibrary
private import BystandingLibrary

internal func fn(_: OverlayLibraryTy) {}
// expected-error @-1 {{function cannot be declared internal because its parameter uses a private type}}
// expected-note @-2 {{struct 'OverlayLibraryTy' is imported by this file as 'private' from '_OverlayLibrary'}}


//--- UnusedOverlay.swift
// RUN: %target-swift-emit-module-interface(%t.swiftinterface) %t/UnusedOverlay.swift -enable-cross-import-overlays -I %t/lib/swift -module-name ClientLibrary -verify

public import DeclaringLibrary // expected-warning {{public import of 'DeclaringLibrary' was not used in public declarations or inlinable code}}
public import BystandingLibrary // expected-warning {{public import of 'BystandingLibrary' was not used in public declarations or inlinable code}}
public import SomeUnrelatedModule // expected-warning {{public import of 'SomeUnrelatedModule' was not used in public declarations or inlinable code}}
