// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -enable-objc-interop -typecheck -verify %s

// Most of these don't pass: rdar://110071334
// %target-swift-frontend(mock-sdk: %clang-importer-sdk) -enable-experimental-cxx-interop -enable-objc-interop -typecheck -verify %s

@_exported import macros

func circle_area(_ radius: CDouble) -> CDouble {
  return M_PI * radius * radius
}

func circle_area2(_ radius: CDouble) -> CDouble {
  return A_PI * radius * radius
}

func circle_area3(_ radius: CFloat) -> CFloat {
  return M_PIf * radius * radius
}

func convertGLBool(_ b: CInt) -> Bool {
  return b != GL_FALSE
}

func pixelFormat(_ alpha: Bool) -> CInt {
  if alpha {
    return GL_RGBA
  } else {
    return GL_RGB
  }
}

func boundsCheckU32(_ x: CUnsignedInt) -> Bool {
  return x >= 0 && x <= UINT32_MAX
}

func boundsCheckS64(_ x: CLongLong) -> Bool {
  return x <= INT64_MAX
}

func isEOF(_ c: CInt) -> Bool {
  return c == EOF
}

func subThree(_ x: CInt) -> CInt {
  return x + MINUS_THREE
}

// true/false are keywords, so they shouldn't conflict with the true/false in
// the C header.
func testTrueFalse() {
  var x : Bool = true
  var y : Bool = false

  _ = true // should not result in ambiguous use error
  _ = false

  _ = TRUE // expected-error {{cannot find 'TRUE' in scope}}
  _ = FALSE // expected-error {{cannot find 'FALSE' in scope}}

  _ = `true` // expected-error {{cannot find 'true' in scope}}
  _ = `false` // expected-error {{cannot find 'false' in scope}}
}

func testCStrings() -> Bool {
  var str: String = UTF8_STRING
  str = VERSION_STRING
  _ = str
}

func testObjCString() -> String {
  let str: String = OBJC_STRING
  return str
}

func testCFString() -> String {
  let str: String = CF_STRING
  return str
}

func testInvalidStringLiterals() {
  // <rdar://67840900> - assertion/crash from importing a macro with a string
  // literal containing invalid UTF-8 characters
  _ = INVALID_UTF8_STRING // expected-error {{cannot find 'INVALID_UTF8_STRING' in scope}}
}

func testInvalidIntegerLiterals() {
  var l1 = INVALID_INTEGER_LITERAL_1 // expected-error {{cannot find 'INVALID_INTEGER_LITERAL_1' in scope}}
  // FIXME: <rdar://problem/16445608> Swift should set up a DiagnosticConsumer for Clang
  // var l2 = INVALID_INTEGER_LITERAL_2 // FIXME {{cannot find 'INVALID_INTEGER_LITERAL_2' in scope}}
}

func testUsesMacroFromOtherModule() {
  let m1 = USES_MACRO_FROM_OTHER_MODULE_1
  let m2 = macros.USES_MACRO_FROM_OTHER_MODULE_1
  let m3 = USES_MACRO_FROM_OTHER_MODULE_2 // expected-error {{cannot find 'USES_MACRO_FROM_OTHER_MODULE_2' in scope}}
  let m4 = macros.USES_MACRO_FROM_OTHER_MODULE_2 // expected-error {{module 'macros' has no member named 'USES_MACRO_FROM_OTHER_MODULE_2'}}
}

func testSuppressed() {
  let m1 = NS_BLOCKS_AVAILABLE // expected-error {{cannot find 'NS_BLOCKS_AVAILABLE' in scope}}
  let m2 = CF_USE_OSBYTEORDER_H // expected-error {{cannot find 'CF_USE_OSBYTEORDER_H' in scope}}
}

func testNil() {
  var localNil: ()
  localNil = NULL_VIA_NAME    // expected-error {{'NULL_VIA_NAME' is unavailable: use 'nil' instead of this imported macro}}
  localNil = NULL_VIA_VALUE    // expected-error {{'NULL_VIA_VALUE' is unavailable: use 'nil' instead of this imported macro}}
  localNil = NULL_AS_NIL       // expected-error {{'NULL_AS_NIL' is unavailable: use 'nil' instead of this imported macro}}
  localNil = NULL_AS_CLASS_NIL // expected-error {{'NULL_AS_CLASS_NIL' is unavailable: use 'nil' instead of this imported macro}}

  localNil = Nil // expected-error {{cannot find 'Nil' in scope}}
}

func testBitwiseOps() {
  _ = DISPATCH_TIME_FOREVER as CUnsignedLongLong
  _ = (BIT_SHIFT_1 | BIT_SHIFT_2) as CInt
  _ = BIT_SHIFT_3 as CLongLong
  _ = BIT_SHIFT_4 as CUnsignedInt

  _ = RSHIFT_ONE as CUnsignedInt
  _ = RSHIFT_INVALID // expected-error {{cannot find 'RSHIFT_INVALID' in scope}}

  _ = XOR_HIGH as CUnsignedLongLong

  var attributes = 0 as CInt
  attributes |= ATTR_BOLD
  attributes |= ATTR_ITALIC
  attributes |= ATTR_UNDERLINE
  attributes |= ATTR_INVALID // expected-error {{cannot find 'ATTR_INVALID' in scope}}
}

func testIntegerArithmetic() {
  _ = ADD_ZERO as CInt
  _ = ADD_ONE as CInt
  _ = ADD_TWO as CInt
  _ = ADD_MINUS_TWO as CInt
  _ = ADD_MIXED_WIDTH as CLongLong
  _ = ADD_MIXED_SIGN as CLongLong
  _ = ADD_UNDERFLOW as CUnsignedInt
  _ = ADD_OVERFLOW as CUnsignedInt

  _ = SUB_ONE as CInt
  _ = SUB_ZERO as CInt
  _ = SUB_MINUS_ONE as CInt
  _ = SUB_MIXED_WIDTH as CLongLong
  _ = SUB_MIXED_SIGN as CUnsignedInt
  _ = SUB_UNDERFLOW as CUnsignedInt
  _ = SUB_OVERFLOW as CUnsignedInt

  _ = MULT_POS as CInt
  _ = MULT_NEG as CInt
  _ = MULT_MIXED_TYPES as CLongLong

  _ = DIVIDE_INTEGRAL as CInt
  _ = DIVIDE_NONINTEGRAL as CInt
  _ = DIVIDE_MIXED_TYPES as CLongLong
  _ = DIVIDE_INVALID // expected-error {{cannot find 'DIVIDE_INVALID' in scope}}
}

func testIntegerComparisons() {
  if EQUAL_FALSE, EQUAL_TRUE, EQUAL_TRUE_MIXED_TYPES,
     GT_FALSE, GT_TRUE, GTE_FALSE, GTE_TRUE,
     LT_FALSE, LT_TRUE, LTE_FALSE, LTE_TRUE {
    fatalError("You hit the jackpot!")
  }
}

func testLogicalComparisons() {
  if L_AND_TRUE, L_AND_FALSE, L_AND_TRUE_B, L_AND_FALSE_B,
     L_OR_TRUE,  L_OR_FALSE,  L_OR_TRUE_B,  L_OR_FALSE_B {
    fatalError("Yet again!")
  }
}

func testRecursion() {
  _ = RECURSION // expected-error {{cannot find 'RECURSION' in scope}}
  _ = REF_TO_RECURSION // expected-error {{cannot find 'REF_TO_RECURSION' in scope}}
  _ = RECURSION_IN_EXPR // expected-error {{cannot find 'RECURSION_IN_EXPR' in scope}}
  _ = RECURSION_IN_EXPR2 // expected-error {{cannot find 'RECURSION_IN_EXPR2' in scope}}
  _ = RECURSION_IN_EXPR3 // expected-error {{cannot find 'RECURSION_IN_EXPR3' in scope}}
}

func testNulls() {
  let _: Int = UNAVAILABLE_ONE // expected-error {{cannot find 'UNAVAILABLE_ONE' in scope}}
  let _: Int = DEPRECATED_ONE // expected-error {{cannot find 'DEPRECATED_ONE' in scope}}
  let _: Int = OKAY_TYPED_ONE // expected-error {{cannot convert value of type 'okay_t' (aka 'UInt32') to specified type 'Int'}}
}

func testHeaderGuard() {
  _ = IS_HEADER_GUARD // expected-error {{cannot find 'IS_HEADER_GUARD' in scope}}
  _ = LOOKS_LIKE_HEADER_GUARD_BUT_IS_USEFUL_CONSTANT
}
