// RUN: %target-typecheck-verify-swift %clang-importer-sdk
// RUN: %target-swift-emit-silgen(mock-sdk: %clang-importer-sdk) -swift-version 5 -verify %s | %FileCheck %s

// REQUIRES: objc_interop

import Foundation
import CoreGraphics

func test_to_cgfloat(_: CGFloat) {}
func test_from_cgfloat(_: Double) {}

func test_returns_double(_: CGFloat) -> Double {
  42.0
}

func test_returns_cgfloat(_: Double) -> CGFloat {
  42.0
}

let d: Double    = 0.0
let cgf: CGFloat = 0.0

// CHECK: test_various_situations_converting_to_cgfloat()
func test_various_situations_converting_to_cgfloat() {
  // CHECK: function_ref @$s12CoreGraphics7CGFloatVyACSdcfC : $@convention(method) (Double, @thin CGFloat.Type) -> CGFloat
  let _: CGFloat = d

  // CHECK: function_ref @$s12CoreGraphics7CGFloatVyACSdcfC : $@convention(method) (Double, @thin CGFloat.Type) -> CGFloat
  // CHECK: function_ref @$s34implicit_double_cgfloat_conversion08test_to_C0yy12CoreGraphics7CGFloatVF : $@convention(thin) (CGFloat) -> ()
  test_to_cgfloat(d)

  // CHECK: function_ref @$sSd1poiyS2d_SdtFZ : $@convention(method) (Double, Double, @thin Double.Type) -> Double
  // CHECK: function_ref @$s12CoreGraphics7CGFloatVyACSdcfC : $@convention(method) (Double, @thin CGFloat.Type) -> CGFloat
  test_to_cgfloat(d + d) // One implicit conversion, `+` is (Double, Double) -> Double

  // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC : $@convention(method) (CGFloat, @thin Double.Type) -> Double
  // CHECK: function_ref @$sSd1poiyS2d_SdtFZ : $@convention(method) (Double, Double, @thin Double.Type) -> Double
  // CHECK: function_ref @$s12CoreGraphics7CGFloatVyACSdcfC : $@convention(method) (Double, @thin CGFloat.Type) -> CGFloat
  test_to_cgfloat(d + cgf) // Two conversions but `+` on Double is still preferred.

  // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC : $@convention(method) (CGFloat, @thin Double.Type) -> Double
  // CHECK: function_ref @$sSd1poiyS2d_SdtFZ : $@convention(method) (Double, Double, @thin Double.Type) -> Double
  // CHECK: function_ref @$sSd1soiyS2d_SdtFZ : $@convention(method) (Double, Double, @thin Double.Type) -> Double
  // CHECK: function_ref @$s12CoreGraphics7CGFloatVyACSdcfC : $@convention(method) (Double, @thin CGFloat.Type) -> CGFloat
  test_to_cgfloat(d + cgf - d) // One conversion (`cgf` -> Double) inside and one outside, both operators preferred on Double.

  // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC : $@convention(method) (CGFloat, @thin Double.Type) -> Double
  // CHECK: function_ref @$sSd1poiyS2d_SdtFZ : $@convention(method) (Double, Double, @thin Double.Type) -> Double
  // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC : $@convention(method) (CGFloat, @thin Double.Type) -> Double
  // CHECK: function_ref @$sSd1soiyS2d_SdtFZ : $@convention(method) (Double, Double, @thin Double.Type) -> Double
  // CHECK: function_ref @$s12CoreGraphics7CGFloatVyACSdcfC : $@convention(method) (Double, @thin CGFloat.Type) -> CGFloat
  test_to_cgfloat(d + cgf - cgf) // Double is always preferred over CGFloat, so three conversion here.

  // CHECK: function_ref @$s12CoreGraphics7CGFloatVyACSdcfC : $@convention(method) (Double, @thin CGFloat.Type) -> CGFloat
  // CHECK: function_ref @$s34implicit_double_cgfloat_conversion013test_returns_B0ySd12CoreGraphics7CGFloatVF : $@convention(thin) (CGFloat) -> Double
  // CHECK: function_ref @$s12CoreGraphics7CGFloatVyACSdcfC : $@convention(method) (Double, @thin CGFloat.Type) -> CGFloat
  // CHECK: function_ref @$s34implicit_double_cgfloat_conversion08test_to_C0yy12CoreGraphics7CGFloatVF : $@convention(thin) (CGFloat) -> ()
  test_to_cgfloat(test_returns_double(d)) // Two conversions

  // Overloads with CGFloat are preferred if that allows to avoid any implicit conversions.
  func test_loading_tuple_elements(values: inout (CGFloat, CGFloat)) {
    struct S {
      init(x: Double, y: Double) {}
      init(x: CGFloat, y: CGFloat) {}
    }

    // CHECK: function_ref @$s34implicit_double_cgfloat_conversion038test_various_situations_converting_to_C0yyF0E23_loading_tuple_elementsL_6valuesy12CoreGraphics7CGFloatV_AGtz_tF1SL_V1x1yAiG_AGtcfC : $@convention(method) (CGFloat, CGFloat, @thin S.Type) -> S
    _ = S(x: 0.0, y: values.0) // Ok
  }
}

// CHECK: test_various_situations_converting_to_double()
func test_various_situations_converting_to_double() {
  // function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC : $@convention(method) (CGFloat, @thin Double.Type) -> Double
  let _: Double = cgf

  // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC : $@convention(method) (CGFloat, @thin Double.Type) -> Double
  // CHECK: function_ref @$s34implicit_double_cgfloat_conversion010test_from_C0yySdF : $@convention(thin) (Double) -> ()
  test_from_cgfloat(cgf)
  // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC : $@convention(method) (CGFloat, @thin Double.Type) -> Double
  // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC : $@convention(method) (CGFloat, @thin Double.Type) -> Double
  // CHECK: function_ref @$sSd1poiyS2d_SdtFZ : $@convention(method) (Double, Double, @thin Double.Type) -> Double
  test_from_cgfloat(cgf + cgf) // Two conversions for `cgf` and `+` is on `Double`
  // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC : $@convention(method) (CGFloat, @thin Double.Type) -> Double
  // CHECK: function_ref @$sSd1poiyS2d_SdtFZ : $@convention(method) (Double, Double, @thin Double.Type) -> Double
  test_from_cgfloat(d + cgf) // One conversion `cgf` to Double and `+` is on `Double`
  // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC : $@convention(method) (CGFloat, @thin Double.Type) -> Double
  // CHECK: function_ref @$sSd1poiyS2d_SdtFZ : $@convention(method) (Double, Double, @thin Double.Type) -> Double
  // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC : $@convention(method) (CGFloat, @thin Double.Type) -> Double
  // CHECK: function_ref @$sSd1soiyS2d_SdtFZ : $@convention(method) (Double, Double, @thin Double.Type) -> Double
  test_from_cgfloat(cgf + d - cgf) // Two conversions of `cgf` and both `+` and `-` are on Double
  // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC : $@convention(method) (CGFloat, @thin Double.Type) -> Double
  // CHECK: function_ref @$sSd1poiyS2d_SdtFZ : $@convention(method) (Double, Double, @thin Double.Type) -> Double
  // CHECK: function_ref @$sSd1soiyS2d_SdtFZ : $@convention(method) (Double, Double, @thin Double.Type) -> Double
  // CHECK: function_ref @$s34implicit_double_cgfloat_conversion010test_from_C0yySdF : $@convention(thin) (Double) -> ()
  test_from_cgfloat(cgf + d - d) // One conversion and both operators are on Double

  // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC : $@convention(method) (CGFloat, @thin Double.Type) -> Double
  // CHECK: function_ref @$s34implicit_double_cgfloat_conversion013test_returns_C0y12CoreGraphics7CGFloatVSdF : $@convention(thin) (Double) -> CGFloat
  // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC : $@convention(method) (CGFloat, @thin Double.Type) -> Double
  // CHECK: function_ref @$s34implicit_double_cgfloat_conversion010test_from_C0yySdF : $@convention(thin) (Double) -> ()
  test_from_cgfloat(test_returns_cgfloat(cgf)) // Two conversions - argument and result.
}

func test_conversions_with_optionals(v: CGFloat?) {
  // CHECK: function_ref @$s34implicit_double_cgfloat_conversion31test_conversions_with_optionals1vy12CoreGraphics7CGFloatVSg_tFAFyYaKXEfu_ : $@convention(thin) @async () -> (CGFloat, @error Error)
  // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC : $@convention(method) (CGFloat, @thin Double.Type) -> Double
  let _: Double = (v ?? 0)
}

func test_static_members_are_contextually_convertible() {
  struct S {
    static var testProp: CGFloat { 42 }
    static func testFunc() -> CGFloat { 42 }
  }

  func test_prop(s: S) -> Double {
    // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC : $@convention(method) (CGFloat, @thin Double.Type) -> Double
    return S.testProp // Ok
  }

  func test_method(s: S) -> Double {
    // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC : $@convention(method) (CGFloat, @thin Double.Type) -> Double
    return S.testFunc() // Ok
  }
}

func test_narrowing_is_delayed(x: Double, y: CGFloat) {
  func test(_: CGFloat) {}

  func overloaded(_: Double, _: Double) -> Double {}
  func overloaded(_: CGFloat, _: CGFloat) -> CGFloat {}

  // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC
  // CHECK: function_ref @$sSd1doiyS2d_SdtFZ
  // CHECK: function_ref @$s12CoreGraphics7CGFloatVyACSdcfC
  let _: CGFloat = x / y // CGFloat.init(x / Double.init(y))
  // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC
  // CHECK: function_ref @$sSd1doiyS2d_SdtFZ
  // CHECK: function_ref @$sSd22_builtinIntegerLiteralSdBI_tcfC
  // CHECK: function_ref @$sSd1poiyS2d_SdtFZ
  // CHECK: function_ref @$s12CoreGraphics7CGFloatVyACSdcfC
  let _: CGFloat = x / y + 1 // CGFloat.init(x / Double(y) + 1 as Double)
  // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC
  // CHECK: function_ref @$s34implicit_double_cgfloat_conversion25test_narrowing_is_delayed1x1yySd_12CoreGraphics7CGFloatVtF10overloadedL_yS2d_SdtF
  // CHECK: function_ref @$s12CoreGraphics7CGFloatVyACSdcf
  let _: CGFloat = overloaded(x, y) // Prefers `overloaded(Double, Double) -> Double`
  // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC
  // CHECK: function_ref @$s34implicit_double_cgfloat_conversion25test_narrowing_is_delayed1x1yySd_12CoreGraphics7CGFloatVtF10overloadedL_yS2d_SdtF
  // CHECK: @$s34implicit_double_cgfloat_conversion25test_narrowing_is_delayed1x1yySd_12CoreGraphics7CGFloatVtF10overloadedL_yS2d_SdtF
  // CHECK: function_ref @$s12CoreGraphics7CGFloatVyACSdcf
  let _: CGFloat = overloaded(x, overloaded(x, y)) // Prefers `overloaded(Double, Double) -> Double` in both occurances.

  // Calls should behave exactly the same as contextual conversions.

  // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC
  // CHECK: function_ref @$sSd1doiyS2d_SdtFZ
  // CHECK: function_ref @$s12CoreGraphics7CGFloatVyACSdcfC
  test(x / y)
  // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC
  // CHECK: function_ref @$sSd1doiyS2d_SdtFZ
  // CHECK: function_ref @$sSd22_builtinIntegerLiteralSdBI_tcfC
  // CHECK: function_ref @$sSd1poiyS2d_SdtFZ
  // CHECK: function_ref @$s12CoreGraphics7CGFloatVyACSdcfC
  test(x / y + 1)
  // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC
  // CHECK: function_ref @$s34implicit_double_cgfloat_conversion25test_narrowing_is_delayed1x1yySd_12CoreGraphics7CGFloatVtF10overloadedL_yS2d_SdtF
  // CHECK: function_ref @$s12CoreGraphics7CGFloatVyACSdcf
  test(overloaded(x, y))
  // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC
  // CHECK: function_ref @$s34implicit_double_cgfloat_conversion25test_narrowing_is_delayed1x1yySd_12CoreGraphics7CGFloatVtF10overloadedL_yS2d_SdtF
  // CHECK: @$s34implicit_double_cgfloat_conversion25test_narrowing_is_delayed1x1yySd_12CoreGraphics7CGFloatVtF10overloadedL_yS2d_SdtF
  // CHECK: function_ref @$s12CoreGraphics7CGFloatVyACSdcf
  test(overloaded(x, overloaded(x, y)))
}

extension CGFloat {
  static func /(_: CGFloat, _: CGFloat) -> CGFloat { fatalError() }

  static prefix func -(_: Self) -> Self { fatalError() }
}

// Make sure that solution with no Double/CGFloat conversions is preferred
func test_no_ambiguity_with_unary_operators(width: CGFloat, height: CGFloat) {
  struct R {
    init(x: CGFloat, y: CGFloat, width: CGFloat, height: CGFloat) {}
    init(x: Double,  y: Double,  width: Double,  height: Double) {}
    init(x: Int,     y: Int,     width: Int,     height: Int) {}
  }

  // CHECK: function_ref @$s12CoreGraphics7CGFloatV34implicit_double_cgfloat_conversionE1doiyA2C_ACtFZ
  // CHECK: function_ref @$s34implicit_double_cgfloat_conversion38test_no_ambiguity_with_unary_operators5width6heighty12CoreGraphics7CGFloatV_AGtF1RL_V1x1yAcdiG_A3GtcfC
  _ = R(x: width / 4, y: -height / 2, width: width, height: height)
}

func test_conversions_with_optional_promotion(d: Double, cgf: CGFloat) {
  func test_double(_: Double??) {}
  func test_cgfloat(_: CGFloat??) {}

  // CHECK: function_ref @$sSd12CoreGraphicsEySdAA7CGFloatVcfC
  // CHECK-NEXT: apply
  // CHECK-NEXT: enum $Optional<Double>, #Optional.some!enumelt
  // CHECK-NEXT: enum $Optional<Optional<Double>>, #Optional.some!enumelt
  test_double(cgf)

  // CHECK: function_ref @$s12CoreGraphics7CGFloatVyACSdcfC
  // CHECK-NEXT: apply
  // CHECK-NEXT: enum $Optional<CGFloat>, #Optional.some!enumelt
  // CHECK-NEXT: enum $Optional<Optional<CGFloat>>, #Optional.some!enumelt
  test_cgfloat(d)
}
