// RUN: %target-swift-frontend -emit-ir -primary-file %s -module-name test_v7k | %FileCheck %s
// RUN: %target-swift-frontend -S -primary-file %s -module-name test_v7k | %FileCheck -check-prefix=V7K %s

// REQUIRES: CPU=armv7k
// REQUIRES: OS=watchos

// CHECK-LABEL: define hidden swiftcc float @"$s8test_v7k9addFloats{{.*}}"(float %0, float %1)
// CHECK: fadd float %0, %1
// CHECK: ret float
// V7K-LABEL: _$s8test_v7k9addFloats{{.*}}
// V7K: vadd.f32 s0, s0, s1
func addFloats(x: Float, y : Float) -> Float {
  return x+y
}

// CHECK-LABEL: define hidden swiftcc double @"$s8test_v7k10addDoubles{{.*}}"(double %0, double %1, double %2)
// CHECK: fadd double %0, %1
// CHECK: fadd double
// CHECK: ret double
// V7K-LABEL: _$s8test_v7k10addDoubles
// V7K: vadd.f64 [[R:d[0-9]+]], d0, d1
// V7K: vadd.f64 d0, [[R]], d2
func addDoubles(x: Double, y: Double, z: Double) -> Double {
  return x+y+z
}

// CHECK-LABEL: define hidden swiftcc float @"$s8test_v7k6addFDF{{.*}}"(float %0, double %1, float %2)
// CHECK: fmul float
// CHECK: ret float
// V7K-LABEL: _$s8test_v7k6addFDF
// V7K: vmul.f32 s0, s0, s1
// z is back-filled to s1
func addFDF(x: Float, y: Double, z: Float) -> Float {
  return x*z
}

// CHECK-LABEL: define hidden swiftcc double @"$s8test_v7k8addStack{{.*}}"(double %0, double %1, double %2, double %3, double %4, double %5, double %6, float %7, double %8)
// CHECK: fadd double
// CHECK: ret double
// V7K-LABEL: _$s8test_v7k8addStack
// V7K: sub     sp, #72
// V7K: vldr d16, [sp, #72]
// V7K: vadd.f64 d0, d6, d16
// a is assigned to d6, c is passed via stack
func addStack(d0: Double, d1: Double, d2: Double, d3: Double, d4: Double,
              d5: Double, a: Double, b: Float, c: Double) -> Double {
  return a+c
}

// CHECK-LABEL: define hidden swiftcc float @"$s8test_v7k9addStack{{.*}}"(double %0, double %1, double %2, double %3, double %4, double %5, double %6, float %7, double %8, float %9)
// CHECK: fadd float
// V7K-LABEL: _$s8test_v7k9addStack
// V7K: sub     sp, #80
// V7K: vldr s0, [sp, #88]
// V7K: vadd.f32 s0, s14, s0
// a is assigned to s14, b is via stack, c is via stack since it can't be back-filled to s15
func addStack2(d0: Double, d1: Double, d2: Double, d3: Double, d4: Double, 
               d5: Double, d6: Double, a: Float, b: Double, c: Float) -> Float {
  return a+c
}

// Passing various enums:
// CHECK-LABEL: define hidden swiftcc void @"$s8test_v7k0A5Empty{{.*}}"()
// V7K-LABEL: _$s8test_v7k0A5Empty
enum Empty {}
func testEmpty(x: Empty) -> () {
}

// CHECK-LABEL: define hidden swiftcc i32 @"$s8test_v7k0A6Single{{.*}}"()
// CHECK: ret i32 1
// V7K-LABEL: _$s8test_v7k0A6Single
// V7K: movs    r0, #1
enum SingleCase { case X }
func testSingle(x: SingleCase) -> Int32{
  switch x {
  case SingleCase.X:
    return 1
  }
}

// CHECK-LABEL: define hidden swiftcc double @"$s8test_v7k0A4Data{{.*}}"(i32 %0, double %1)
// CHECK: ret double
// V7K-LABEL: _$s8test_v7k0A4Data
// V7K: vstr    d0, [sp, #16]
// V7K: vldr    d16, [sp, #16]
// V7K: vstr    d16, [sp, #8]
// V7K: vldr    d0, [sp, #8]
// V7K: bx      lr
enum DataCase { case Y(Int, Double) }
func testData(x: DataCase) -> Double {
  switch x {
  case let .Y(i, d):
    return d
  }
}

// CHECK-LABEL: define hidden swiftcc i32 @"$s8test_v7k0A6Clike2{{.*}}"(i8 %0)
// CHECK: [[ID:%[0-9]+]] = phi i32 [ 2, {{.*}} ], [ 1, {{.*}} ]
// CHECK: ret i32 [[ID]]
// V7K-LABEL: _$s8test_v7k0A6Clike2
// V7K: tst.w r0, #1
// V7K: movs r0, #1
// V7K: movs r0, #2
enum CLike2 {
  case A
  case B
}
func testClike2(x: CLike2) -> Int {
  switch x {
  case CLike2.A:
    return 1
  case CLike2.B:
    return 2
  }
}

// CHECK-LABEL: define hidden swiftcc i32 @"$s8test_v7k0A6Clike8{{.*}}"(i32 %0, i8 %1)
// CHECK: [[ID:%[0-9]+]] = phi i32 [ -1, {{.*}} ], [ 1, {{.*}} ]
// CHECK: ret i32 [[ID]]
// V7K-LABEL: _$s8test_v7k0A6Clike8
// V7K: sxtb r0, r1
// V7K: cbnz r0
// V7K: movs r0, #1
// V7K: mvn r0, #0
enum CLike8 {
  case A
  case B
  case C
  case D
  case E
  case F
  case G
  case H
}
func testClike8(t: Int, x: CLike8) -> Int {
  switch x {
  case CLike8.A:
    return 1
  default:
    return -1
  }
}

// layout of the enum: the tag bit is set for the no-data cases, which are then
// assigned values in the data area of the enum in declaration order
// CHECK-LABEL: define hidden swiftcc double @"$s8test_v7k0A7SingleP{{.*}}"(i32 %0, i32 %1, i8 %2)
// CHECK: br i1
// CHECK: switch i64 [[ID:%[0-9]+]]
// CHECK: [[FIRST:%[0-9]+]] = zext i32 %0 to i64
// CHECK: [[F1:%[0-9]+]] = and i64 [[FIRST]], 4294967295
// CHECK: [[SECOND:%[0-9]+]] = zext i32 %1 to i64
// CHECK: [[TEMP:%[0-9]+]] = shl i64 [[SECOND]], 32
// CHECK: [[T1:%[0-9]+]] = and i64 [[TEMP]], -4294967296
// CHECK: [[RESULT:%[0-9]+]] = or i64 [[F1]], [[T1]]
// CHECK: bitcast i64 [[RESULT]] to double
// CHECK: phi double [ 0.000000e+00, {{.*}} ]
// V7K-LABEL: _$s8test_v7k0A7SingleP
// V7K: tst.w     r2, #1
// V7K: vldr    d0, [{{.*}}]
enum SinglePayload {
  case Paragraph
  case Char(Double)
  case Chapter
}
func testSingleP(x: SinglePayload) -> Double {
  switch x {
  case let .Char(d):
    return d
  default:
    return 0.0
  }
}

// CHECK-LABEL: define hidden swiftcc double @"$s8test_v7k0A6MultiP{{.*}}"(i32 %0, i32 %1, i8 %2)
// CHECK: [[FIRST:%[0-9]+]] = zext i32 %0 to i64
// CHECK: [[F1:%[0-9]+]] = and i64 [[FIRST]], 4294967295
// CHECK: [[SECOND:%[0-9]+]] = zext i32 %1 to i64
// CHECK: [[TEMP:%[0-9]+]] = shl i64 [[SECOND]], 32
// CHECK: [[T1:%[0-9]+]] = and i64 [[TEMP]], -4294967296
// CHECK: [[RESULT:%[0-9]+]] = or i64 [[F1]], [[T1]]
// CHECK: bitcast i64 [[RESULT]] to double
// CHECK: sitofp i32 {{.*}} to double 
// CHECK: phi double [ 0.000000e+00, {{.*}} ]
// CHECK: ret double
// V7K-LABEL: _$s8test_v7k0A6MultiP
// V7K:        ldr     r0, [sp, #24]
// V7K:        ldr     r1, [sp, #28]
// V7K:        vmov    d16, r0, r1
// V7K:        vstr    d16, [sp, #8]
// V7K:        vldr    d16, [sp, #8]
// V7K:        vstr    d16, [sp]
// V7K:        vldr    d0, [sp]
// V7K:        add     sp, #{{[0-9]+}}
// V7K:        bx      lr
// Backend will assign r0, r1 and r2 for input parameters and d0 for return values.
class Bignum {}
enum MultiPayload {
  case X(Int)
  case Y(Double)
  case Z(Bignum)
}
func testMultiP(x: MultiPayload) -> Double {
  switch x {
  case let .X(i):
    return Double(i)
  case let .Y(d):
    return d
  default:
    return 0.0
  }
}

// CHECK-LABEL: define hidden swiftcc float @"$s8test_v7k0A3Opt{{.*}}"(i32 %0, i8 %1)
// CHECK: entry:
// CHECK: [[TR:%.*]] = trunc i8 %1
// CHECK: br i1 [[TR]], {{.*}}, label %[[PAYLOADLABEL:.*]]
// CHECK: [[PAYLOADLABEL]]:
// CHECK: [[ID:%[0-9]+]] = bitcast i32 %0 to float
// CHECK: ret float
// V7K-LABEL: _$s8test_v7k0A3Opt
// V7K:         tst.w     r1, #1
// V7K:         vmov    s0, r0
// V7K:         vstr    s0, [sp, [[SLOT:#[0-9]+]]
// V7K:         vldr    s0, [sp, [[SLOT]]
// V7K:         pop     {{{.*}}, pc}
func testOpt(x: Float?) -> Float {
  return x!
}

// Returning tuple: (Int, Int)
// CHECK-LABEL: define hidden swiftcc { i32, i32 } @"$s8test_v7k6minMax{{.*}}"(i32 %0, i32 %1)
// V7K-LABEL: _$s8test_v7k6minMax
// V7K: ldr r0
// V7K: ldr r1
func minMax(x : Int, y : Int) -> (min: Int, max: Int) {
    var currentMin = x
    var currentMax = y
    if y < x {
      currentMin = y
      currentMax = x
    }
    return (currentMin, currentMax)
}

// Returning struct: Double x 4; Int8, Double, Double;
struct MyRect {
  var x : Double
  var y : Double
  var w : Double
  var h : Double
}
struct MyPoint {
var x: Double
var y: Double
}

struct MySize {
var w: Double
var h: Double
}
struct MyRect2 {
  var t: Int8
  var p : MyPoint
  init() {
    t = 1
    p = MyPoint(x : 0.0, y: 0.0)
  }
}
struct MyRect4 {
  var t: Int8
  var p : MyPoint
  var s: MySize
  init() {
   t = 1
   p = MyPoint(x : 0.0, y: 0.0)
   s = MySize(w: 1.0, h: 2.0)
  }
}
// CHECK-LABEL: define hidden swiftcc { double, double, double, double } @"$s8test_v7k0A4Ret2{{.*}}"(double %0, i32 %1)
// V7K-LABEL: _$s8test_v7k0A4Ret2
// double in d0, i32 in r0, return in d0,...,d3
// V7K: vmov [[ID:s[0-9]+]], r0
// V7K: vcvt.f64.s32 d0, [[ID]]
// V7K: bl
// V7K: vldr    d2, [sp, #8]
func testRet2(w : Double, i : Int) -> MyRect {
  var r = MyRect(x : Double(i), y : 2.0, w : 3.0, h : 4.0)
  r.w = w
  return r
}
// CHECK-LABEL: define hidden swiftcc { i8, double, double } @"$s8test_v7k0A4Ret3{{.*}}"()
// V7K-LABEL: _$s8test_v7k0A4Ret3
func testRet3() -> MyRect2 {
  var r = MyRect2()
  return r
}

// Returning tuple?: (Int x 6)?
// CHECK-LABEL: define hidden swiftcc void @"$s8test_v7k7minMax2{{.*}}"(ptr noalias nocapture sret({{.*}}) %0, i32 %1, i32 %2)
// V7K-LABEL: _$s8test_v7k7minMax2
// We will indirectly return an optional with the address in r0, input parameters will be in r1 and r2
// V7K: str r0, [sp, [[IDX:#[0-9]+]]]
// V7K: cmp r1, r2
// V7K: ldr [[R0_RELOAD:r[0-9]+]], [sp, [[IDX]]]
// V7K: str {{.*}}, [{{.*}}[[R0_RELOAD]]]
// V7K: str.w {{.*}}, [{{.*}}[[R0_RELOAD]], #4]
// V7K: str.w {{.*}}, [{{.*}}[[R0_RELOAD]], #8]
// V7K: str.w {{.*}}, [{{.*}}[[R0_RELOAD]], #12]
// V7K: str {{.*}}, [{{.*}}[[R0_RELOAD]], #16]
// V7K: str {{.*}}, [{{.*}}[[R0_RELOAD]], #20]
// V7K: and {{.*}}, {{.*}}, #1
// V7K: strb {{.*}}, [{{.*}}[[R0_RELOAD]], #24]
func minMax2(x : Int, y : Int) -> (min: Int, max: Int, min2: Int, max2: Int, min3: Int, max3: Int)? {
    if x == y {
      return nil
    }
    var currentMin = x
    var currentMax = y
    if y < x {
      currentMin = y
      currentMax = x
    }
    return (currentMin, currentMax, currentMin, currentMax, currentMin, currentMax)
}

// Returning struct?: {Int x 6}?
// CHECK-LABEL: define hidden swiftcc void @"$s8test_v7k7minMax3{{.*}}"(ptr noalias nocapture sret({{.*}}) %0, i32 %1, i32 %2)
// V7K-LABEL: _$s8test_v7k7minMax3
struct Ret {
  var min:Int
  var max:Int
  var min2, max2 : Int
  var min3, max3 : Int
}
func minMax3(x : Int, y : Int) -> Ret? {
    if x == y {
      return nil
    }
    var currentMin = x
    var currentMax = y
    if y < x {
      currentMin = y
      currentMax = x
    }
    var r = Ret(min:currentMin, max:currentMax, min2:currentMin, max2:currentMax, min3:currentMin, max3:currentMax)
    return r
}
