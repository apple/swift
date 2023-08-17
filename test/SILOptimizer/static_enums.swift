// RUN: %target-build-swift -parse-as-library -O %s -module-name=test -emit-sil | %FileCheck %s

// RUN: %empty-directory(%t) 
// RUN: %target-build-swift -parse-as-library -O -module-name=test %s -o %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s -check-prefix=CHECK-OUTPUT

// REQUIRES: executable_test,swift_stdlib_no_asserts,optimized_stdlib
// REQUIRES: swift_in_compiler


// CHECK-LABEL: sil_global @$s4test6optIntSiSgvp : $Optional<Int> = {
public var optInt: Int? = 27

// CHECK-LABEL: sil_global @$s4test9optIntNilSiSgvp : $Optional<Int> = {
public var optIntNil: Int? = nil

// CHECK-LABEL: sil_global @$s4test6optPtrSVSgvp : $Optional<UnsafeRawPointer> = {
public var optPtr: UnsafeRawPointer? = nil

public struct S {
  var a: Int?
  var b: Int8
}

public struct S2 {
  var s: S
  var c: Int8
}

// CHECK-LABEL: sil_global @$s4test1sAA1SVSgvp : $Optional<S> = {
public var s: S? = S(a: 27, b: 42)

// CHECK-LABEL: sil_global @$s4test4snilAA1SVSgvp : $Optional<S> = {
public var snil: S? = nil

// CHECK-LABEL: sil_global @$s4test5sanilAA1SVSgvp : $Optional<S> = {
public var sanil: S? = S(a: nil, b: 42)

// CHECK-LABEL: sil_global @$s4test2tsAA1SV_s4Int8Vtvp : $(S, Int8) = {
public var ts: (S, Int8) = (S(a: 27, b:42), 13)

// CHECK-LABEL: sil_global @$s4test2s2AA2S2Vvp : $S2 = {
public var s2: S2 = S2(s: S(a: 27, b: 42), c: 13)

public enum MP {
  case A(Int)
  case B(Int8)
  case C
  case D(Int?)
}

// CHECK-LABEL: sil_global @$s4test3mpaAA2MPOvp : $MP = {
public var mpa: MP = .A(27)
// CHECK-LABEL: sil_global @$s4test3mpbAA2MPOvp : $MP = {
public var mpb: MP = .B(42)
// CHECK-LABEL: sil_global @$s4test3mpcAA2MPOvp : $MP = {
public var mpc: MP = .C
// CHECK-LABEL: sil_global @$s4test3mpdAA2MPOvp : $MP = {
public var mpd: MP = .D(103)
// CHECK-LABEL: sil_global @$s4test6mpdnilAA2MPOvp : $MP = {
public var mpdnil: MP = .D(nil)
// CHECK-LABEL: sil_global @$s4test6optmpaAA2MPOSgvp : $Optional<MP> = {
public var optmpa: MP? = .A(27)
// CHECK-LABEL: sil_global @$s4test6optmpbAA2MPOSgvp : $Optional<MP> = {
public var optmpb: MP? = .B(42)
// CHECK-LABEL: sil_global @$s4test6optmpcAA2MPOSgvp : $Optional<MP> = {
public var optmpc: MP? = .C
// CHECK-LABEL: sil_global @$s4test6optmpdAA2MPOSgvp : $Optional<MP> = {
public var optmpd: MP? = .D(103)
// CHECK-LABEL: sil_global @$s4test9optmpdnilAA2MPOSgvp : $Optional<MP> = {
public var optmpdnil: MP? = .D(nil)
// CHECK-LABEL: sil_global @$s4test8optmpnilAA2MPOSgvp : $Optional<MP> = {
public var optmpnil: MP? = nil

// CHECK-LABEL: sil_global @$s4test3strSSSgvp : $Optional<String> = {
public var str: String? = "a long string exceeding the inline buffer"
// CHECK-LABEL: sil_global @$s4test6strnilSSSgvp : $Optional<String> = {
public var strnil: String? = nil
// CHECK-LABEL: sil_global @$s4test8shortstrSSSgvp : $Optional<String> = {
public var shortstr: String? = "short"
// CHECK-LABEL: sil_global @$s4test8emptystrSSSgvp : $Optional<String> = {
public var emptystr: String? = ""

public struct SpareBits {
    var o: UInt64 = 0
    var x: UInt8 = 0
    var y: UInt64 = 0
    var x_2: UInt8 = 0
    var y_2: UInt64 = 0
    var x_3: UInt8 = 0
    var y_3: UInt64 = 0
    var x_4: UInt8 = 0
    var y_4: UInt64 = 0
    var x_5: UInt8 = 0
    var y_5: UInt64 = 0
    var x_6: UInt8 = 0
    var y_6: UInt64 = 0
}


public enum Multipayload {
    case a
    case b(UnsafeRawPointer)
    case c(SpareBits)
    case e(String)
    case f
    case g
}

// CHECK-LABEL: sil_global hidden @$s4test4mpsbAA12MultipayloadOvp : $Multipayload = {
var mpsb = Multipayload.c(SpareBits(o: 1, x: 2, y: 3, x_2: 4, y_2: 5, x_3: 6, y_3: 7, x_4: 8, y_4: 9, x_5: 10, y_5: 11, x_6: 12, y_6: 13))
// CHECK-LABEL: sil_global hidden @$s4test4mpslAA12MultipayloadOvp : $Multipayload = {
var mpsl = Multipayload.e("a long string exceeding the inline buffer")
// CHECK-LABEL: sil_global hidden @$s4test4mpssAA12MultipayloadOvp : $Multipayload = {
var mpss = Multipayload.e("short")

public struct Inner {
  var x: UInt64
  var y: UInt8
}

public struct Outer {
  var i: Inner
  var z: UInt8
}

// CHECK-LABEL: sil_global hidden @$s4test5outerAA5OuterVSgvp : $Optional<Outer> = {
var outer: Outer? = Outer(i: Inner(x: 2, y: 3), z: 4)

// CHECK-LABEL: sil_global hidden @$s4test8optionalSiSgvp : $Optional<Int> = {
var optional: Int? = Optional(42)

struct StringGen {
  enum E {
    case none
    case str(String)
    case gen(() -> String)
  }

  var source: E
}

// CHECK-LABEL: sil_global hidden @$s4test3sg1AA9StringGenVvp : $StringGen = {
var sg1 = StringGen(source: .gen({ "gen" }))

// CHECK-LABEL: sil_global hidden @$s4test3sg2AA9StringGenVvp : $StringGen = {
var sg2 = StringGen(source: .none)

// CHECK-LABEL: sil_global hidden @$s4test3sg3AA9StringGenVvp : $StringGen = {
var sg3 = StringGen(source: .str("str"))

@inline(never)
func getStringGen(_ s: StringGen) -> String {
  switch s.source {
    case .gen(let f):
      return f()
    case .str(let s):
      return s
    case .none:
      return "none"
  }
}

public enum R {
    case success(Int)
    case failure(Error)
}

public let success: R = .success(27)

// CHECK-LABEL: sil_global hidden @$s4test10optSuccessAA1ROSgvp : $Optional<R> = {
var optSuccess: R? = success

public enum Color {
  case black
  case rgb(r: UInt8, g: UInt8, b: UInt8)
}

// CHECK-LABEL: sil_global hidden @$s4test8optBlackAA5ColorOSgvp : $Optional<Color> = {
var optBlack: Color? = Color.black
// CHECK-LABEL: sil_global hidden @$s4test9optSalmonAA5ColorOSgvp : $Optional<Color> = {
var optSalmon: Color? = Color.rgb(r: 0xfa, g: 0x80, b: 0x72)

// CHECK-LABEL: sil_global private @$s4test9createArrSaySiSgGyFTv_ : $_ContiguousArrayStorage<Optional<Int>> = {
@inline(never)
func createArr() -> [Int?] {
  return [ 27, 42, nil, 103 ]
}

@main
struct Main {
  static func main() {
    // CHECK-OUTPUT: optInt: Optional(27)
    print("optInt:", optInt as Any)
    // CHECK-OUTPUT: optIntNil: nil
    print("optIntNil:", optIntNil as Any)
    // CHECK-OUTPUT: optPtr: nil
    print("optPtr:", optPtr as Any)
    // CHECK-OUTPUT: s: Optional(test.S(a: Optional(27), b: 42))
    print("s:", s as Any)
    // CHECK-OUTPUT: snil: nil
    print("snil:", snil as Any)
    // CHECK-OUTPUT: sanil: Optional(test.S(a: nil, b: 42))
    print("sanil:", sanil as Any)
    // CHECK-OUTPUT: ts: (test.S(a: Optional(27), b: 42), 13)
    print("ts:", ts as Any)
    // CHECK-OUTPUT: s2: S2(s: test.S(a: Optional(27), b: 42), c: 13)
    print("s2:", s2 as Any)
    // CHECK-OUTPUT: mpa: A(27)
    print("mpa:", mpa as Any)
    // CHECK-OUTPUT: mpb: B(42)
    print("mpb:", mpb as Any)
    // CHECK-OUTPUT: mpc: C
    print("mpc:", mpc as Any)
    // CHECK-OUTPUT: mpd: D(Optional(103))
    print("mpd:", mpd as Any)
    // CHECK-OUTPUT: mpdnil: D(nil)
    print("mpdnil:", mpdnil as Any)
    // CHECK-OUTPUT: optmpa: Optional(test.MP.A(27))
    print("optmpa:", optmpa as Any)
    // CHECK-OUTPUT: optmpb: Optional(test.MP.B(42))
    print("optmpb:", optmpb as Any)
    // CHECK-OUTPUT: optmpc: Optional(test.MP.C)
    print("optmpc:", optmpc as Any)
    // CHECK-OUTPUT: optmpd: Optional(test.MP.D(Optional(103)))
    print("optmpd:", optmpd as Any)
    // CHECK-OUTPUT: optmpdnil: Optional(test.MP.D(nil))
    print("optmpdnil:", optmpdnil as Any)
    // CHECK-OUTPUT: optmpnil: nil
    print("optmpnil:", optmpnil as Any)
    // CHECK-OUTPUT: str: Optional("a long string exceeding the inline buffer")
    print("str:", str as Any)
    // CHECK-OUTPUT: strnil: nil
    print("strnil:", strnil as Any)
    // CHECK-OUTPUT: shortstr: Optional("short")
    print("shortstr:", shortstr as Any)
    // CHECK-OUTPUT: emptystr: Optional("")
    print("emptystr:", emptystr as Any)
    // CHECK-OUTPUT: mpsb: c(test.SpareBits(o: 1, x: 2, y: 3, x_2: 4, y_2: 5, x_3: 6, y_3: 7, x_4: 8, y_4: 9, x_5: 10, y_5: 11, x_6: 12, y_6: 13))
    print("mpsb:", mpsb)
    // CHECK-OUTPUT: mpsl: e("a long string exceeding the inline buffer")
    print("mpsl:", mpsl)
    // CHECK-OUTPUT: mpss: e("short")
    print("mpss:", mpss)
    // CHECK-OUTPUT: outer: Optional(test.Outer(i: test.Inner(x: 2, y: 3), z: 4))
    print("outer:", outer as Any)
    // CHECK-OUTPUT: optional: Optional(42)
    print("optional:", optional as Any)
    // CHECK-OUTPUT: createArr: [Optional(27), Optional(42), nil, Optional(103)]
    print("createArr:", createArr())
    // CHECK-OUTPUT: stringGen1: gen
    print("stringGen1: \(getStringGen(sg1))")
    // CHECK-OUTPUT: stringGen2: none
    print("stringGen2: \(getStringGen(sg2))")
    // CHECK-OUTPUT: stringGen3: str
    print("stringGen3: \(getStringGen(sg3))")
    // CHECK-OUTPUT: optSuccess: Optional(test.R.success(27))
    print("optSuccess:", optSuccess as Any)
    // CHECK-OUTPUT: optBlack: Optional(test.Color.black)
    print("optBlack:", optBlack as Any)
    // CHECK-OUTPUT: optSalmon: Optional(test.Color.rgb(r: 250, g: 128, b: 114))
    print("optSalmon:", optSalmon as Any)
  }
}


