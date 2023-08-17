func testAmbiguousFunctionReference() {
  func foo(a: Int) {}
  func foo(a: String) {}

  // RUN: %sourcekitd-test -req=cursor -pos=%(line + 1):7 %s -- %s | %FileCheck %s --check-prefix LOCAL_FUNC
  _ = foo

  // RUN: %sourcekitd-test -req=cursor -pos=%(line + 1):7 %s -- %s | %FileCheck %s --check-prefix LOCAL_FUNC
  _ = foo(a: UInt(1))

  // LOCAL_FUNC: source.lang.swift.ref.function.free
  // LOCAL_FUNC: <Declaration>func foo(a: <Type usr="s:Si">Int</Type>)</Declaration>
  // LOCAL_FUNC: SECONDARY SYMBOLS BEGIN
  // LOCAL_FUNC: source.lang.swift.ref.function.free
  // LOCAL_FUNC: <Declaration>func foo(a: <Type usr="s:SS">String</Type>)</Declaration>
  // LOCAL_FUNC: SECONDARY SYMBOLS END
}



struct TestDeduplicateResults {
  // The constraints system produces multiple solutions here for the argument type but
  // all reference the same declaration. Check that we de-duplicate them and that we
  // don’t report any secondary sybmols.
  static func staticFunc(_ duration: Int) {}

  func test() {
    // RUN: %sourcekitd-test -req=cursor -pos=%(line + 1):10 %s -- %s | %FileCheck %s --check-prefix STATIC_FUNC
    Self.staticFunc(1 * 1e9)
  }

  // STATIC_FUNC: source.lang.swift.ref.function.method.static
  // STATIC_FUNC: <Declaration>static func staticFunc(_ duration: <Type usr="s:Si">Int</Type>)</Declaration>
  // STATIC_FUNC: SECONDARY SYMBOLS BEGIN
  // STATIC_FUNC-NEXT: SECONDARY SYMBOLS END
}
