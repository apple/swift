// RUN: %target-typecheck-verify-swift -parse-as-library
// REQUIRES: OS=macosx

// Allow referencing unavailable API in situations where the caller is marked unavailable in the same circumstances.

@available(OSX, unavailable)
@discardableResult
func osx() -> Int { return 0 } // expected-note * {{'osx()' has been explicitly marked unavailable here}}

@available(OSXApplicationExtension, unavailable)
func osx_extension() {}

@available(OSX, unavailable)
func osx_pair() -> (Int, Int) { return (0, 0) } // expected-note * {{'osx_pair()' has been explicitly marked unavailable here}}

func call_osx_extension() {
    osx_extension() // OK; osx_extension is only unavailable if -application-extension is passed.
}
func call_osx() {
    osx() // expected-error {{'osx()' is unavailable}}
}

@available(OSX, unavailable)
func osx_call_osx_extension() {
    osx_extension() // OK; osx_extension is only unavailable if -application-extension is passed.
}

@available(OSX, unavailable)
func osx_call_osx() {
    osx() // OK; same
}

@available(OSXApplicationExtension, unavailable)
func osx_extension_call_osx_extension() {
    osx_extension()
}

@available(OSXApplicationExtension, unavailable)
func osx_extension_call_osx() {
    osx() // expected-error {{'osx()' is unavailable}}
}

@available(OSX, unavailable)
var osx_init_osx = osx() // OK

@available(OSXApplicationExtension, unavailable)
var osx_extension_init_osx = osx() // expected-error {{'osx()' is unavailable}}

@available(OSX, unavailable)
var osx_inner_init_osx = { let _ = osx() } // OK

@available(OSXApplicationExtension, unavailable)
var osx_extension_inner_init_osx = { let _ = osx() } // expected-error {{'osx()' is unavailable}}

struct Outer {
  @available(OSX, unavailable)
  var osx_init_osx = osx() // OK

  @available(OSX, unavailable)
  lazy var osx_lazy_osx = osx() // OK

  @available(OSXApplicationExtension, unavailable)
  var osx_extension_init_osx = osx() // expected-error {{'osx()' is unavailable}}

  @available(OSXApplicationExtension, unavailable)
  var osx_extension_lazy_osx = osx() // expected-error {{'osx()' is unavailable}}

  @available(OSX, unavailable)
  var osx_init_multi1_osx = osx(), osx_init_multi2_osx = osx() // OK

  @available(OSXApplicationExtension, unavailable)
  var osx_extension_init_multi1_osx = osx(), osx_extension_init_multi2_osx = osx() // expected-error 2 {{'osx()' is unavailable}}

  @available(OSX, unavailable)
  var (osx_init_deconstruct1_osx, osx_init_deconstruct2_osx) = osx_pair() // OK

  @available(OSXApplicationExtension, unavailable)
  var (osx_extension_init_deconstruct1_osx, osx_extension_init_deconstruct2_osx) = osx_pair() // expected-error {{'osx_pair()' is unavailable}}

  @available(OSX, unavailable)
  var (_, osx_init_deconstruct2_only_osx) = osx_pair() // OK
  
  @available(OSXApplicationExtension, unavailable)
  var (_, osx_extension_init_deconstruct2_only_osx) = osx_pair() // expected-error {{'osx_pair()' is unavailable}}

  @available(OSX, unavailable)
  var (osx_init_deconstruct1_only_osx, _) = osx_pair() // OK
  
  @available(OSXApplicationExtension, unavailable)
  var (osx_extension_init_deconstruct1_only_osx, _) = osx_pair() // expected-error {{'osx_pair()' is unavailable}}

  @available(OSX, unavailable)
  var osx_inner_init_osx = { let _ = osx() } // OK
  
  @available(OSXApplicationExtension, unavailable)
  var osx_extension_inner_init_osx = { let _ = osx() } // expected-error {{'osx()' is unavailable}}
}

extension Outer {
  @available(OSX, unavailable)
  static var also_osx_init_osx = osx() // OK
  
  @available(OSXApplicationExtension, unavailable)
  static var also_osx_extension_init_osx = osx() // expected-error {{'osx()' is unavailable}}
}

@available(OSX, unavailable)
extension Outer {
  static var outer_osx_init_osx = osx() // OK
}

@available(OSX, unavailable)
struct NotOnOSX {
  var osx_init_osx = osx() // OK
  lazy var osx_lazy_osx = osx() // OK
  var osx_init_multi1_osx = osx(), osx_init_multi2_osx = osx() // OK
  var (osx_init_deconstruct1_osx, osx_init_deconstruct2_osx) = osx_pair() // OK
  var (_, osx_init_deconstruct2_only_osx) = osx_pair() // OK
  var (osx_init_deconstruct1_only_osx, _) = osx_pair() // OK
}
