// REQUIRES: OS=macosx
// RUN: %target-typecheck-verify-swift -parse-as-library -library-level api

@_spi_available(macOS 10.10, *)
@available(iOS 8.0, *)
public class MacOSSPIClass {} // expected-note {{type declared here}}

@_spi_available(iOS 8.0, *)
@available(macOS 10.10, *)
public class iOSSPIClass {}

@available(macOS 10.10, iOS 8.0, *)
public class MacOSDerived: MacOSSPIClass {} // expected-error {{cannot use class 'MacOSSPIClass' in a public or '@usableFromInline' conformance; it is SPI}}

@available(macOS 10.10, iOS 8.0, *)
public class iOSDerived: iOSSPIClass {}
