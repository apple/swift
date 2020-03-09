// RUN: %empty-directory(%t)
// RUN: %target-build-swift -parse-stdlib %s -module-name main -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out
// REQUIRES: executable_test

import Swift
import StdlibUnittest

let DemangleToMetadataTests = TestSuite("DemangleToMetadata")


DemangleToMetadataTests.test("malformed mangled names") {
  expectNil(_typeByName("blah"))
}

DemangleToMetadataTests.test("tuple types") {
  expectEqual(type(of: ()), _typeByName("yt")!)
  expectEqual(type(of: ((), ())), _typeByName("yt_ytt")!)
  expectEqual(type(of: ((), b: ())), _typeByName("yt_yt1bt")!)
  expectEqual(type(of: (a: (), ())), _typeByName("yt1a_ytt")!)
  expectEqual(type(of: (a: (), b: ())), _typeByName("yt1a_yt1bt")!)

  // Initial creation of metadata via demangling a type name.
  expectNotNil(_typeByName("yt1a_yt3bcdt"))
}

func f0() { }
var f0_thin: @convention(thin) () -> Void = f0
var f0_c: @convention(c) () -> Void = f0

#if _runtime(_ObjC)
var f0_block: @convention(block) () -> Void = f0
#endif

func f0_throws() throws { }

func f1(x: ()) { }
func f2(x: (), y: ()) { }

func f1_variadic(x: ()...) { }
func f1_inout(x: inout ()) { }
func f1_shared(x: __shared AnyObject) { }
func f1_owned(x: __owned AnyObject) { }

func f2_variadic_inout(x: ()..., y: inout ()) { }

func f1_escaping(_: @escaping (Int) -> Float) { }
func f1_autoclosure(_: @autoclosure () -> Float) { }
func f1_escaping_autoclosure(_: @autoclosure @escaping () -> Float) { }

DemangleToMetadataTests.test("function types") {
  // Conventions
  expectEqual(type(of: f0), _typeByName("yyc")!)
  expectEqual(type(of: f0_thin), _typeByName("yyXf")!)
  expectEqual(type(of: f0_c), _typeByName("yyXC")!)
#if _runtime(_ObjC)
  expectEqual(type(of: f0_block), _typeByName("yyXB")!)
#endif

  // Throwing functions
  expectEqual(type(of: f0_throws), _typeByName("yyKc")!)

  // More parameters.
  expectEqual(type(of: f1), _typeByName("yyyt_tc")!)
  expectEqual(type(of: f2), _typeByName("yyyt_yttc")!)

  // Variadic parameters.
  expectEqual(type(of: f1_variadic), _typeByName("yyytd_tc")!)

  // Inout parameters.
  expectEqual(type(of: f1_inout), _typeByName("yyytzc")!)

  // Ownership parameters.
  expectEqual(type(of: f1_shared), _typeByName("yyyXlhc")!)
  expectEqual(type(of: f1_owned), _typeByName("yyyXlnc")!)

  // Mix-and-match.
  expectEqual(type(of: f2_variadic_inout), _typeByName("yyytd_ytztc")!)

  // A function type that hasn't been built before.
  expectEqual("(Int, Float, Double, String, Character, UInt, Bool) -> ()",
    String(describing: _typeByName("yySi_SfSdSSs9CharacterVSuSbtc")!))

  // Escaping
  expectEqual(type(of: f1_escaping), _typeByName("ySfSicc")!)

  // Autoclosure
  expectEqual(type(of: f1_autoclosure), _typeByName("ySfyXKc")!)
  expectEqual(type(of: f1_escaping_autoclosure), _typeByName("ySfyXAc")!)
}

DemangleToMetadataTests.test("metatype types") {
  expectEqual(type(of: type(of: ())), _typeByName("ytm")!)
  expectEqual(type(of: type(of: f0)), _typeByName("yycm")!)
}

func f2_any_anyobject(_: Any, _: AnyObject) { }

class C { }

protocol P1 { }
protocol P2 { }
protocol P3 { }

func f1_composition(_: P1 & P2) { }
func f1_composition_anyobject(_: AnyObject & P1) { }
func f1_composition_superclass(_: C & P1 & P2) { }

DemangleToMetadataTests.test("existential types") {
  // Any, AnyObject
  expectEqual(type(of: f2_any_anyobject), _typeByName("yyyp_yXltc")!)

  // References to protocols.
  expectEqual(type(of: f1_composition), _typeByName("yy4main2P1_4main2P2pc")!)

  // Reference to protocol with AnyObject.
  expectEqual(type(of: f1_composition_anyobject), _typeByName("yy4main2P1_Xlc")!)

  // References to superclass.
  expectEqual(type(of: f1_composition_superclass), _typeByName("yy4main2P1_4main2P2AA1CCXcc")!)

  // Demangle an existential type that hasn't been seen before.
  expectEqual("P1 & P2 & P3", String(describing: _typeByName("4main2P1_4main2P24main2P3p")!))
}

DemangleToMetadataTests.test("existential metatype types") {
  // Any
  expectEqual(type(of: Any.self), _typeByName("ypm")!)

  // AnyObject
  expectEqual(type(of: AnyObject.self), _typeByName("yXlm")!)

  // References to metatype of protocols.
  expectEqual(type(of: (P1 & P2).self), _typeByName("4main2P1_4main2P2pm")!)

  // References to metatype involving protocols and superclass.
  expectEqual(type(of: (C & P1 & P2).self), _typeByName("4main2P1_4main2P2AA1CCXcm")!)
}

struct S {
  struct Nested { }
}

enum E { case e }

DemangleToMetadataTests.test("nominal types") {
  // Simple Struct
  expectEqual(type(of: S()), _typeByName("4main1SV")!)

  // Simple Enum
  expectEqual(type(of: E.e), _typeByName("4main1EO")!)

  // Simple Class
  expectEqual(type(of: C()), _typeByName("4main1CC")!)

  // Swift standard library types
  expectEqual(type(of: Int()), _typeByName("Si")!)
  expectEqual(type(of: Int16()), _typeByName("s5Int16V")!)

  // Nested struct
  expectEqual(type(of: S.Nested()), _typeByName("4main1SV6NestedV")!)

  // Class referenced by "ModuleName.ClassName" syntax.
  expectEqual(type(of: C()), _typeByName("main.C")!)
}

protocol P4 {
  associatedtype Assoc1
  associatedtype Assoc2
}

extension S: P4 {
  typealias Assoc1 = Int
  typealias Assoc2 = String
}

enum EG<T, U> { case a }

class CG3<T, U, V> { }


DemangleToMetadataTests.test("simple generic specializations") {
  expectEqual([Int].self, _typeByName("SaySiG")!)
  expectEqual(EG<Int, String>.self, _typeByName("4main2EGOySiSSG")!)
  expectEqual(CG3<Int, Double, String>.self, _typeByName("4main3CG3CySiSdSSG")!)
}

extension EG {
  struct NestedSG<V> { }
}

extension C {
  enum Nested<T, U> {
    case a

    struct Innermore {
      struct Innermost<V> { }
    }
  }
}

class CG2<T, U> {
  class Inner<V> {
    struct Innermost<W1, W2, W3, W4> { }
  }
}

DemangleToMetadataTests.test("nested generic specializations") {
  expectEqual(EG<Int, String>.NestedSG<Double>.self,
    _typeByName("4main2EGO8NestedSGVySiSS_SdG")!)
  expectEqual(C.Nested<Int, String>.Innermore.Innermost<Double>.self,
    _typeByName("4main1CC6NestedO9InnermoreV9InnermostVy_SiSS__SdG")!)
  expectEqual(CG2<Int, String>.Inner<Double>.self,
    _typeByName("4main3CG2C5InnerCySiSS_SdG")!)
  expectEqual(
    CG2<Int, String>.Inner<Double>.Innermost<Int8, Int16, Int32, Int64>.self,
    _typeByName("4main3CG2C5InnerC9InnermostVySiSS_Sd_s4Int8Vs5Int16Vs5Int32Vs5Int64VG")!)
}

DemangleToMetadataTests.test("demangle built-in types") {
  expectEqual(Builtin.Int8.self,     _typeByName("Bi8_")!)
  expectEqual(Builtin.Int16.self,    _typeByName("Bi16_")!)
  expectEqual(Builtin.Int32.self,    _typeByName("Bi32_")!)
  expectEqual(Builtin.Int64.self,    _typeByName("Bi64_")!)
  expectEqual(Builtin.Int128.self,   _typeByName("Bi128_")!)
  expectEqual(Builtin.Int256.self,   _typeByName("Bi256_")!)
  expectEqual(Builtin.Int512.self,   _typeByName("Bi512_")!)

  expectEqual(Builtin.NativeObject.self, _typeByName("Bo")!)
  expectEqual(Builtin.BridgeObject.self, _typeByName("Bb")!)
  expectEqual(Builtin.UnsafeValueBuffer.self, _typeByName("BB")!)

  expectEqual(Builtin.FPIEEE32.self, _typeByName("Bf32_")!)
  expectEqual(Builtin.FPIEEE64.self, _typeByName("Bf64_")!)

  expectEqual(Builtin.Vec4xFPIEEE32.self, _typeByName("Bf32_Bv4_")!)
}

class CG4<T: P1, U: P2> {
  struct InnerGeneric<V: P3> { }
}

struct ConformsToP1: P1 { }
struct ConformsToP2: P2 { }
struct ConformsToP3: P3 { }

struct ContextualWhere1<T> {
  class Nested1 where T: P1 { }
  struct Nested2 where T == Int { }
}

DemangleToMetadataTests.test("protocol conformance requirements") {
  expectEqual(CG4<ConformsToP1, ConformsToP2>.self,
    _typeByName("4main3CG4CyAA12ConformsToP1VAA12ConformsToP2VG")!)
  expectEqual(CG4<ConformsToP1, ConformsToP2>.InnerGeneric<ConformsToP3>.self,
    _typeByName("4main3CG4C12InnerGenericVyAA12ConformsToP1VAA12ConformsToP2V_AA12ConformsToP3VG")!)
  expectEqual(ContextualWhere1<ConformsToP1>.Nested1.self,
    _typeByName("4main16ContextualWhere1V7Nested1CyAA12ConformsToP1V_G")!)

  // Failure cases: failed conformance requirements.
  expectNil(_typeByName("4main3CG4CyAA12ConformsToP1VAA12ConformsToP1VG"))
  expectNil(_typeByName("4main3CG4CyAA12ConformsToP2VAA12ConformsToP2VG"))
  expectNil(_typeByName("4main3CG4C12InnerGenericVyAA12ConformsToP1VAA12ConformsToP2V_AA12ConformsToP2VG"))
}

struct SG5<T: P4> where T.Assoc1: P1, T.Assoc2: P2 { }

struct ConformsToP4a : P4 {
  typealias Assoc1 = ConformsToP1
  typealias Assoc2 = ConformsToP2
}

struct ConformsToP4b : P4 {
  typealias Assoc1 = ConformsToP1
  typealias Assoc2 = ConformsToP1
}

struct ConformsToP4c : P4 {
  typealias Assoc1 = ConformsToP2
  typealias Assoc2 = ConformsToP2
}

struct ContextualWhere2<U: P4> {
  struct Nested1 where U.Assoc1: P1, U.Assoc2: P2 { }
  enum Nested2 where U.Assoc1 == U.Assoc2 { }
}

DemangleToMetadataTests.test("associated type conformance requirements") {
  expectEqual(SG5<ConformsToP4a>.self,
    _typeByName("4main3SG5VyAA13ConformsToP4aVG")!)
  expectEqual(ContextualWhere2<ConformsToP4a>.Nested1.self,
    _typeByName("4main16ContextualWhere2V7Nested1VyAA13ConformsToP4aV_G")!)

  // Failure cases: failed conformance requirements.
  expectNil(_typeByName("4main3SG5VyAA13ConformsToP4bVG"))
  expectNil(_typeByName("4main3SG5VyAA13ConformsToP4cVG"))
  expectNil(_typeByName("4main3SG5VyAA12ConformsToP1cVG"))
}

struct SG6<T: P4> where T.Assoc1 == T.Assoc2 { }
struct SG7<T: P4> where T.Assoc1 == Int { }
struct SG8<T: P4> where T.Assoc1 == [T.Assoc2] { }

struct ConformsToP4d : P4 {
  typealias Assoc1 = [ConformsToP2]
  typealias Assoc2 = ConformsToP2
}

DemangleToMetadataTests.test("same-type requirements") {
  // Concrete type.
  expectEqual(SG7<S>.self,
    _typeByName("4main3SG7VyAA1SVG")!)
  expectEqual(ContextualWhere1<Int>.Nested2.self,
    _typeByName("4main16ContextualWhere1V7Nested2VySi_G")!)

  // Other associated type.
  expectEqual(SG6<ConformsToP4b>.self,
    _typeByName("4main3SG6VyAA13ConformsToP4bVG")!)
  expectEqual(SG6<ConformsToP4c>.self,
    _typeByName("4main3SG6VyAA13ConformsToP4cVG")!)
  expectEqual(ContextualWhere2<ConformsToP4b>.Nested2.self,
    _typeByName("4main16ContextualWhere2V7Nested2OyAA13ConformsToP4bV_G")!)

  // Structural type.
  expectEqual(SG8<ConformsToP4d>.self,
    _typeByName("4main3SG8VyAA13ConformsToP4dVG")!)

  // Failure cases: types don't match.
  expectNil(_typeByName("4main3SG7VyAA13ConformsToP4aVG"))
  expectNil(_typeByName("4main3SG6VyAA13ConformsToP4aVG"))
  expectNil(_typeByName("4main3SG8VyAA13ConformsToP4cVG"))
}

struct SG9<T: AnyObject> { }

DemangleToMetadataTests.test("AnyObject requirements") {
  expectEqual(SG9<C>.self,
    _typeByName("4main3SG9VyAA1CCG")!)

  // Failure cases: failed AnyObject constraint.
  expectNil(_typeByName("4main3SG9VyAA1SVG"))
}

struct SG10<T: C> { }

class C2 : C { }
class C3 { }

DemangleToMetadataTests.test("superclass requirements") {
  expectEqual(SG10<C>.self,
    _typeByName("4main4SG10VyAA1CCG")!)
  expectEqual(SG10<C2>.self,
    _typeByName("4main4SG10VyAA2C2CG")!)

  // Failure cases: not a subclass.
  expectNil(_typeByName("4main4SG10VyAA2C3CG"))
}

//
// Extensions of external types, and constrained extensions
//

struct SG11<T> {}

extension Dictionary {
  struct Inner<V: P1> {}
}

extension SG11 where T: P1 {
  struct InnerTConformsToP1<U: P2> { }
}

extension SG11.InnerTConformsToP1 where U: P3 {
  struct InnermostUConformsToP3<V: P4> { }
}

struct ConformsToP2AndP3: P2, P3 { }

DemangleToMetadataTests.test("Nested types in extensions") {
  expectEqual(
    Dictionary<String, Int>.Inner<ConformsToP1>.self,
    _typeByName("s10DictionaryV4mainE5InnerVySSSi_AC12ConformsToP1VG")!)
  expectEqual(
    SG11<ConformsToP1>.InnerTConformsToP1<ConformsToP2>.self,
    _typeByName("4main4SG11VA2A2P1RzlE016InnerTConformsToC0VyAA08ConformsfC0V_AA0gF2P2VG")!)
  expectEqual(
    SG11<ConformsToP1>.InnerTConformsToP1<ConformsToP2AndP3>
                      .InnermostUConformsToP3<ConformsToP4a>.self,
    _typeByName("4main4SG11VA2A2P1RzlE016InnerTConformsToC0VA2A2P3Rd__rlE018InnermostUConformsfG0VyAA08ConformsfC0V_AA0jf5P2AndG0V_AA0jF3P4aVG")!)

  // Failure case: Dictionary's outer `Key: Hashable` constraint not sastified
  expectNil(_typeByName("s10DictionaryV4mainE5InnerVyAC12ConformsToP1VSi_AC12ConformsToP1VG"))
  // Failure case: Dictionary's inner `V: P1` constraint not satisfied
  expectNil(_typeByName("s10DictionaryV4mainE5InnerVySSSi_AC12ConformsToP2VG"))

  // Failure case: SG11's outer `T: P1` constraint not satisfied
  expectNil(_typeByName("4main4SG11VA2A2P1RzlE016InnerTConformsToC0VyAA08ConformsF2P2V_AHGMa"))
  // Failure case: SG11's inner `U: P2` constraint not satisfied
  expectNil(_typeByName("4main4SG11VA2A2P1RzlE016InnerTConformsToC0VyAA08ConformsfC0V_AHGMa"))

  // TODO: Failure case: InnermostUConformsToP3's 'U: P3' constraint not satisfied
  
}

//
// Nested types in same-type-constrained extensions
//

struct SG12<T: P1, U: P2> {}

struct ConformsToP1AndP2 : P1, P2 { }

extension SG12 where U == T {
  struct InnerTEqualsU<V: P3> { }
}

extension SG12 where T == ConformsToP1 {
  struct InnerTEqualsConformsToP1<V: P3> { }
}

extension SG12 where U == ConformsToP2 {
  struct InnerUEqualsConformsToP2<V: P3> { }
}

DemangleToMetadataTests.test("Nested types in same-type-constrained extensions") {
  expectEqual(
    SG12<ConformsToP1AndP2, ConformsToP1AndP2>.InnerTEqualsU<ConformsToP3>.self,
    _typeByName("4main4SG12VA2A2P2Rzq_RszrlE13InnerTEqualsUVyAA015ConformsToP1AndC0VAH_AA0fG2P3VG")!)
  expectEqual(
    SG12<ConformsToP1, ConformsToP2>.InnerTEqualsConformsToP1<ConformsToP3>.self,
    _typeByName("4main4SG12VA2A12ConformsToP1VRszrlE012InnerTEqualscdE0VyAeA0cD2P2V_AA0cD2P3VG")!)
  expectEqual(
    SG12<ConformsToP1, ConformsToP2>.InnerUEqualsConformsToP2<ConformsToP3>.self,
    _typeByName("4main4SG12VA2A12ConformsToP2VRs_rlE012InnerUEqualscdE0VyAA0cD2P1VAE_AA0cD2P3VG")!)

  // TODO: Cases where mangled name doesn't match constraints
  // T != U in InnerTEqualsU
  // V !: P3 in InnerTEqualsU
  // T != ConformsToP1 in InnerTEqualsConformsToP1
  // V !: P3 in InnerTEqualsConformsToP1
}

runAllTests()

