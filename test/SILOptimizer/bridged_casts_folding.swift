// RUN: %target-swift-frontend -O -emit-sil %s | %FileCheck %s

// REQUIRES: objc_interop

// Check that casts between bridged types are replaced by more 
// efficient code sequences.
// 
// In particular, checked_cast_* and unconditional_checked_* instructions,
// which are pretty expensive at run-time (e.g. because they use
// runtime _dynamicCast calls and check conformances at run-time),
// should be replaced by invocations of specialized bridging functions,
// which make use of statically known compile-time conformances and 
// do not perform any conformance checks at run-time.

import Foundation

public func forcedCast<NS, T>(_ ns: NS) -> T {
  return ns as! T
}

public func condCast<NS, T>(_ ns: NS) -> T? {
  return ns as? T
}

// Check optimizations of casts from NSString to String

var nsString: NSString = "string"

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding29testForcedCastNStoSwiftStringSSyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @$sSS10FoundationE26_forceBridgeFromObjectiveC_6resultySo8NSStringC_SSSgztFZ : $@convention(method) (@guaranteed NSString, @inout Optional<String>, @thin String.Type) -> ()
// CHECK: return
@inline(never)
public func testForcedCastNStoSwiftString() -> String {
  var o: String = forcedCast(nsString)
  return o
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding27testCondCastNStoSwiftStringSSSgyF
// CHECK-NOT: checked_cast
// CHECK: function_ref @$sSS10FoundationE34_conditionallyBridgeFromObjectiveC_6resultSbSo8NSStringC_SSSgztFZ : $@convention(method) (@guaranteed NSString, @inout Optional<String>, @thin String.Type) -> Bool
// CHECK: return
@inline(never)
public func testCondCastNStoSwiftString() -> String? {
  var o: String? = condCast(nsString)
  return o
}


// Check optimizations of casts from NSNumber to Int

var nsIntNumber = NSNumber(value: 1)

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding32testForcedCastNSNumberToSwiftIntSiyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @$sSi10FoundationE26_forceBridgeFromObjectiveC_6resultySo8NSNumberC_SiSgztFZ : $@convention(method) (@guaranteed NSNumber, @inout Optional<Int>, @thin Int.Type) -> ()
// CHECK: return
@inline(never)
public func testForcedCastNSNumberToSwiftInt() -> Int {
  var o: Int = forcedCast(nsIntNumber)
  return o
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding30testCondCastNSNumberToSwiftIntSiSgyF
// CHECK-NOT: checked_cast
// CHECK: function_ref @$sSi10FoundationE34_conditionallyBridgeFromObjectiveC_6resultSbSo8NSNumberC_SiSgztFZ : $@convention(method) (@guaranteed NSNumber, @inout Optional<Int>, @thin Int.Type) -> Bool
// CHECK: return
@inline(never)
public func testCondCastNSNumberToSwiftInt() -> Int? {
  var o: Int? = condCast(nsIntNumber)
  return o
}

// Check optimizations of casts from NSNumber to Double

var nsDoubleNumber = NSNumber(value: 1.234)

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding35testForcedCastNSNumberToSwiftDoubleSdyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @$sSd10FoundationE26_forceBridgeFromObjectiveC_6resultySo8NSNumberC_SdSgztFZ : $@convention(method) (@guaranteed NSNumber, @inout Optional<Double>, @thin Double.Type) -> ()
// CHECK: return
@inline(never)
public func testForcedCastNSNumberToSwiftDouble() -> Double {
  var o: Double = forcedCast(nsDoubleNumber)
  return o
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding33testCondCastNSNumberToSwiftDoubleSdSgyF
// CHECK-NOT: checked_cast
// CHECK: function_ref @$sSd10FoundationE34_conditionallyBridgeFromObjectiveC_6resultSbSo8NSNumberC_SdSgztFZ : $@convention(method) (@guaranteed NSNumber, @inout Optional<Double>, @thin Double.Type) -> Bool
// CHECK: return
@inline(never)
public func testCondCastNSNumberToSwiftDouble() -> Double? {
  var o: Double? = condCast(nsDoubleNumber)
  return o
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding38testForcedCastNSIntNumberToSwiftDoubleSdyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @$sSd10FoundationE26_forceBridgeFromObjectiveC_6resultySo8NSNumberC_SdSgztFZ : $@convention(method) (@guaranteed NSNumber, @inout Optional<Double>, @thin Double.Type) -> ()
// CHECK: return
@inline(never)
public func testForcedCastNSIntNumberToSwiftDouble() -> Double {
  var o: Double = forcedCast(nsIntNumber)
  return o
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding36testCondCastNSIntNumberToSwiftDoubleSdSgyF
// CHECK-NOT: checked_cast
// CHECK: function_ref @$sSd10FoundationE34_conditionallyBridgeFromObjectiveC_6resultSbSo8NSNumberC_SdSgztFZ : $@convention(method) (@guaranteed NSNumber, @inout Optional<Double>, @thin Double.Type) -> Bool
// CHECK: return
@inline(never)
public func testCondCastNSIntNumberToSwiftDouble() -> Double? {
  var o: Double? = condCast(nsIntNumber)
  return o
}



// Check optimization of casts from NSArray to Swift Array

var nsArrInt: NSArray = [1, 2, 3, 4]
var nsArrDouble: NSArray = [1.1, 2.2, 3.3, 4.4]
var nsArrString: NSArray = ["One", "Two", "Three", "Four"]

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding31testForcedCastNStoSwiftArrayIntSaySiGyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @$sSa10FoundationE26_forceBridgeFromObjectiveC_6resultySo7NSArrayC_SayxGSgztFZ : $@convention(method) <τ_0_0> (@guaranteed NSArray, @inout Optional<Array<τ_0_0>>, @thin Array<τ_0_0>.Type) -> ()
// CHECK: return
@inline(never)
public func testForcedCastNStoSwiftArrayInt() -> [Int] {
  var arr: [Int] = forcedCast(nsArrInt)
  return arr
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding29testCondCastNStoSwiftArrayIntSaySiGSgyF
// CHECK-NOT: checked_cast
// CHECK: function_ref @$sSa10FoundationE34_conditionallyBridgeFromObjectiveC_6resultSbSo7NSArrayC_SayxGSgztFZ : $@convention(method) <τ_0_0> (@guaranteed NSArray, @inout Optional<Array<τ_0_0>>, @thin Array<τ_0_0>.Type) -> Bool
// CHECK: return
@inline(never)
public func testCondCastNStoSwiftArrayInt() -> [Int]? {
  var arrOpt: [Int]? = condCast(nsArrInt)
  return arrOpt
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding34testForcedCastNStoSwiftArrayDoubleSaySdGyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @$sSa10FoundationE26_forceBridgeFromObjectiveC_6resultySo7NSArrayC_SayxGSgztFZ : $@convention(method) <τ_0_0> (@guaranteed NSArray, @inout Optional<Array<τ_0_0>>, @thin Array<τ_0_0>.Type) -> ()
// CHECK: return
@inline(never)
public func testForcedCastNStoSwiftArrayDouble() -> [Double] {
  var arr: [Double] = forcedCast(nsArrDouble)
  return arr
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding32testCondCastNStoSwiftArrayDoubleSaySdGSgyF
// CHECK-NOT: checked_cast
// CHECK: function_ref @$sSa10FoundationE34_conditionallyBridgeFromObjectiveC_6resultSbSo7NSArrayC_SayxGSgztFZ : $@convention(method) <τ_0_0> (@guaranteed NSArray, @inout Optional<Array<τ_0_0>>, @thin Array<τ_0_0>.Type) -> Bool
// CHECK: return
@inline(never)
public func testCondCastNStoSwiftArrayDouble() -> [Double]? {
  var arrOpt: [Double]? = condCast(nsArrDouble)
  return arrOpt
}


// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding34testForcedCastNStoSwiftArrayStringSaySSGyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @$sSa10FoundationE26_forceBridgeFromObjectiveC_6resultySo7NSArrayC_SayxGSgztFZ : $@convention(method) <τ_0_0> (@guaranteed NSArray, @inout Optional<Array<τ_0_0>>, @thin Array<τ_0_0>.Type) -> ()
// CHECK: return
@inline(never)
public func testForcedCastNStoSwiftArrayString() -> [String] {
  var arr: [String] = forcedCast(nsArrString)
  return arr
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding32testCondCastNStoSwiftArrayStringSaySSGSgyF
// CHECK-NOT: checked_cast
// CHECK: function_ref @$sSa10FoundationE34_conditionallyBridgeFromObjectiveC_6resultSbSo7NSArrayC_SayxGSgztFZ : $@convention(method) <τ_0_0> (@guaranteed NSArray, @inout Optional<Array<τ_0_0>>, @thin Array<τ_0_0>.Type) -> Bool
// CHECK: return
@inline(never)
public func testCondCastNStoSwiftArrayString() -> [String]? {
  var arrOpt: [String]? = condCast(nsArrString)
  return arrOpt
}



// Check optimization of casts from NSDictionary to Swift Dictionary

var nsDictInt: NSDictionary = [1:1, 2:2, 3:3, 4:4]
var nsDictDouble: NSDictionary = [1.1 : 1.1, 2.2 : 2.2, 3.3 : 3.3, 4.4 : 4.4]
var nsDictString: NSDictionary = ["One":"One", "Two":"Two", "Three":"Three", "Four":"Four"]

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding30testForcedCastNStoSwiftDictIntSDyS2iGyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @$sSD10FoundationE26_forceBridgeFromObjectiveC_6resultySo12NSDictionaryC_SDyxq_GSgztFZ : $@convention(method) <τ_0_0, τ_0_1 where τ_0_0 : Hashable> (@guaranteed NSDictionary, @inout Optional<Dictionary<τ_0_0, τ_0_1>>, @thin Dictionary<τ_0_0, τ_0_1>.Type) -> ()
// CHECK: return
@inline(never)
public func testForcedCastNStoSwiftDictInt() -> [Int: Int] {
  var dict: [Int: Int] = forcedCast(nsDictInt)
  return dict
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding28testCondCastNStoSwiftDictIntSDyS2iGSgyF
// CHECK-NOT: checked_cast
// CHECK: function_ref @$sSD10FoundationE34_conditionallyBridgeFromObjectiveC_6resultSbSo12NSDictionaryC_SDyxq_GSgztFZ : $@convention(method) <τ_0_0, τ_0_1 where τ_0_0 : Hashable> (@guaranteed NSDictionary, @inout Optional<Dictionary<τ_0_0, τ_0_1>>, @thin Dictionary<τ_0_0, τ_0_1>.Type) -> Bool
// CHECK: return
@inline(never)
public func testCondCastNStoSwiftDictInt() -> [Int: Int]? {
  var dictOpt: [Int: Int]? = condCast(nsDictInt)
  return dictOpt
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding33testForcedCastNStoSwiftDictDoubleSDyS2dGyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @$sSD10FoundationE26_forceBridgeFromObjectiveC_6resultySo12NSDictionaryC_SDyxq_GSgztFZ : $@convention(method) <τ_0_0, τ_0_1 where τ_0_0 : Hashable> (@guaranteed NSDictionary, @inout Optional<Dictionary<τ_0_0, τ_0_1>>, @thin Dictionary<τ_0_0, τ_0_1>.Type) -> ()
// CHECK: return
@inline(never)
public func testForcedCastNStoSwiftDictDouble() -> [Double: Double] {
  var dict: [Double: Double] = forcedCast(nsDictDouble)
  return dict
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding31testCondCastNStoSwiftDictDoubleSDyS2dGSgyF
// CHECK-NOT: checked_cast
// CHECK: function_ref @$sSD10FoundationE34_conditionallyBridgeFromObjectiveC_6resultSbSo12NSDictionaryC_SDyxq_GSgztFZ : $@convention(method) <τ_0_0, τ_0_1 where τ_0_0 : Hashable> (@guaranteed NSDictionary, @inout Optional<Dictionary<τ_0_0, τ_0_1>>, @thin Dictionary<τ_0_0, τ_0_1>.Type) -> Bool
// CHECK: return
@inline(never)
public func testCondCastNStoSwiftDictDouble() -> [Double: Double]? {
  var dictOpt: [Double: Double]? = condCast(nsDictDouble)
  return dictOpt
}


// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding33testForcedCastNStoSwiftDictStringSDyS2SGyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @$sSD10FoundationE26_forceBridgeFromObjectiveC_6resultySo12NSDictionaryC_SDyxq_GSgztFZ : $@convention(method) <τ_0_0, τ_0_1 where τ_0_0 : Hashable> (@guaranteed NSDictionary, @inout Optional<Dictionary<τ_0_0, τ_0_1>>, @thin Dictionary<τ_0_0, τ_0_1>.Type) -> ()
// CHECK: return
@inline(never)
public func testForcedCastNStoSwiftDictString() -> [String: String] {
  var dict: [String: String] = forcedCast(nsDictString)
  return dict
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding31testCondCastNStoSwiftDictStringSDyS2SGSgyF
// CHECK-NOT: checked_cast
// CHECK: function_ref @$sSD10FoundationE34_conditionallyBridgeFromObjectiveC_6resultSbSo12NSDictionaryC_SDyxq_GSgztFZ : $@convention(method) <τ_0_0, τ_0_1 where τ_0_0 : Hashable> (@guaranteed NSDictionary, @inout Optional<Dictionary<τ_0_0, τ_0_1>>, @thin Dictionary<τ_0_0, τ_0_1>.Type) -> Bool
// CHECK: return
@inline(never)
public func testCondCastNStoSwiftDictString() -> [String: String]? {
  var dictOpt: [String: String]? = condCast(nsDictString)
  return dictOpt
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding40testForcedCastNSDictStringtoSwiftDictIntSDyS2iGyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @$sSD10FoundationE26_forceBridgeFromObjectiveC_6resultySo12NSDictionaryC_SDyxq_GSgztFZ : $@convention(method) <τ_0_0, τ_0_1 where τ_0_0 : Hashable> (@guaranteed NSDictionary, @inout Optional<Dictionary<τ_0_0, τ_0_1>>, @thin Dictionary<τ_0_0, τ_0_1>.Type) -> ()
// CHECK: return
@inline(never)
public func testForcedCastNSDictStringtoSwiftDictInt() -> [Int: Int] {
  var dictOpt: [Int: Int] = forcedCast(nsDictString)
  return dictOpt
}


// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding38testCondCastNSDictStringtoSwiftDictIntSDyS2iGSgyF
// CHECK-NOT: checked_cast
// CHECK: function_ref @$sSD10FoundationE34_conditionallyBridgeFromObjectiveC_6resultSbSo12NSDictionaryC_SDyxq_GSgztFZ : $@convention(method) <τ_0_0, τ_0_1 where τ_0_0 : Hashable> (@guaranteed NSDictionary, @inout Optional<Dictionary<τ_0_0, τ_0_1>>, @thin Dictionary<τ_0_0, τ_0_1>.Type) -> Bool
// CHECK: return
@inline(never)
public func testCondCastNSDictStringtoSwiftDictInt() -> [Int: Int]? {
  var dictOpt: [Int: Int]? = condCast(nsDictString)
  return dictOpt
}


// Check optimization of casts from NSSet to Swift Set

var nsSetInt: NSSet = [1, 2, 3, 4]
var nsSetDouble: NSSet = [1.1, 2.2, 3.3, 4.4]
var nsSetString: NSSet = ["One", "Two", "Three", "Four"]

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding29testForcedCastNStoSwiftSetIntShySiGyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @$sSh10FoundationE26_forceBridgeFromObjectiveC_6resultySo5NSSetC_ShyxGSgztFZ : $@convention(method) <τ_0_0 where τ_0_0 : Hashable> (@guaranteed NSSet, @inout Optional<Set<τ_0_0>>, @thin Set<τ_0_0>.Type) -> ()
// CHECK: return
@inline(never)
public func testForcedCastNStoSwiftSetInt() -> Set<Int> {
  var set: Set<Int> = forcedCast(nsSetInt)
  return set
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding27testCondCastNStoSwiftSetIntShySiGSgyF
// CHECK-NOT: checked_cast
// CHECK: function_ref @$sSh10FoundationE34_conditionallyBridgeFromObjectiveC_6resultSbSo5NSSetC_ShyxGSgztFZ : $@convention(method) <τ_0_0 where τ_0_0 : Hashable> (@guaranteed NSSet, @inout Optional<Set<τ_0_0>>, @thin Set<τ_0_0>.Type) -> Bool
// CHECK: return
@inline(never)
public func testCondCastNStoSwiftSetInt() -> Set<Int>? {
  var setOpt: Set<Int>? = condCast(nsSetInt)
  return setOpt
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding32testForcedCastNStoSwiftSetDoubleShySdGyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @$sSh10FoundationE26_forceBridgeFromObjectiveC_6resultySo5NSSetC_ShyxGSgztFZ : $@convention(method) <τ_0_0 where τ_0_0 : Hashable> (@guaranteed NSSet, @inout Optional<Set<τ_0_0>>, @thin Set<τ_0_0>.Type) -> ()
// CHECK: return
@inline(never)
public func testForcedCastNStoSwiftSetDouble() -> Set<Double> {
  var set: Set<Double> = forcedCast(nsSetDouble)
  return set
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding30testCondCastNStoSwiftSetDoubleShySdGSgyF
// CHECK-NOT: checked_cast
// CHECK: function_ref @$sSh10FoundationE34_conditionallyBridgeFromObjectiveC_6resultSbSo5NSSetC_ShyxGSgztFZ : $@convention(method) <τ_0_0 where τ_0_0 : Hashable> (@guaranteed NSSet, @inout Optional<Set<τ_0_0>>, @thin Set<τ_0_0>.Type) -> Bool
// CHECK: return
@inline(never)
public func testCondCastNStoSwiftSetDouble() -> Set<Double>? {
  var setOpt: Set<Double>? = condCast(nsSetDouble)
  return setOpt
}


// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding32testForcedCastNStoSwiftSetStringShySSGyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @$sSh10FoundationE26_forceBridgeFromObjectiveC_6resultySo5NSSetC_ShyxGSgztFZ : $@convention(method) <τ_0_0 where τ_0_0 : Hashable> (@guaranteed NSSet, @inout Optional<Set<τ_0_0>>, @thin Set<τ_0_0>.Type) -> ()
// CHECK: return
@inline(never)
public func testForcedCastNStoSwiftSetString() -> Set<String> {
  var set: Set<String> = forcedCast(nsSetString)
  return set
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding30testCondCastNStoSwiftSetStringShySSGSgyF
// CHECK-NOT: checked_cast
// CHECK: function_ref @$sSh10FoundationE34_conditionallyBridgeFromObjectiveC_6resultSbSo5NSSetC_ShyxGSgztFZ : $@convention(method) <τ_0_0 where τ_0_0 : Hashable> (@guaranteed NSSet, @inout Optional<Set<τ_0_0>>, @thin Set<τ_0_0>.Type) -> Bool
// CHECK: return
@inline(never)
public func testCondCastNStoSwiftSetString() -> Set<String>? {
  var setOpt: Set<String>? = condCast(nsSetString)
  return setOpt
}


// Check optimizations of casts from String to NSString

var swiftString: String = "string"

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding29testForcedCastSwiftToNSStringSo0I0CyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testForcedCastSwiftToNSString() -> NSString {
  var o: NSString = forcedCast(swiftString)
  return o
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding27testCondCastSwiftToNSStringSo0I0CSgyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testCondCastSwiftToNSString() -> NSString? {
  var o: NSString? = condCast(swiftString)
  return o
}


// Check optimizations of casts from Int to NSNumber

var swiftIntNumber: Int = 1

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding32testForcedCastSwiftIntToNSNumberSo0J0CyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testForcedCastSwiftIntToNSNumber() -> NSNumber {
  var o: NSNumber = forcedCast(swiftIntNumber)
  return o
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding30testCondCastSwiftIntToNSNumberSo0J0CSgyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testCondCastSwiftIntToNSNumber() -> NSNumber? {
  var o: NSNumber? = condCast(swiftIntNumber)
  return o
}

// Check optimizations of casts from Double to NSNumber

var swiftDoubleNumber: Double = 1.234

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding35testForcedCastSwiftDoubleToNSNumberSo0J0CyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testForcedCastSwiftDoubleToNSNumber() -> NSNumber {
  var o: NSNumber = forcedCast(swiftDoubleNumber)
  return o
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding33testCondCastSwiftDoubleToNSNumberSo0J0CSgyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testCondCastSwiftDoubleToNSNumber() -> NSNumber? {
  var o: NSNumber? = condCast(swiftDoubleNumber)
  return o
}


// Check optimization of casts from Swift Array to NSArray

var arrInt: [Int] = [1, 2, 3, 4]
var arrDouble: [Double] = [1.1, 2.2, 3.3, 4.4]
var arrString: [String] = ["One", "Two", "Three", "Four"]

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding31testForcedCastSwiftToNSArrayIntSo0I0CyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testForcedCastSwiftToNSArrayInt() -> NSArray {
  var arr: NSArray = forcedCast(arrInt)
  return arr
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding29testCondCastSwiftToNSArrayIntSo0I0CSgyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testCondCastSwiftToNSArrayInt() -> NSArray? {
  var arrOpt: NSArray? = condCast(arrInt)
  return arrOpt
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding34testForcedCastSwiftToNSArrayDoubleSo0I0CyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testForcedCastSwiftToNSArrayDouble() -> NSArray {
  var arr: NSArray = forcedCast(arrDouble)
  return arr
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding32testCondCastSwiftToNSArrayDoubleSo0I0CSgyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testCondCastSwiftToNSArrayDouble() -> NSArray? {
  var arrOpt: NSArray? = condCast(arrDouble)
  return arrOpt
}


// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding34testForcedCastSwiftToNSArrayStringSo0I0CyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testForcedCastSwiftToNSArrayString() -> NSArray {
  var arr: NSArray = forcedCast(arrString)
  return arr
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding32testCondCastSwiftToNSArrayStringSo0I0CSgyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testCondCastSwiftToNSArrayString() -> NSArray? {
  var arrOpt: NSArray? = condCast(arrString)
  return arrOpt
}


// Check optimization of casts from Swift Dict to NSDict

var dictInt: [Int: Int] = [1:1, 2:2, 3:3, 4:4]
var dictDouble: [Double: Double] = [1.1 : 1.1, 2.2 : 2.2, 3.3 : 3.3, 4.4 : 4.4]
var dictString: [String: String] = ["One":"One", "Two":"Two", "Three":"Three", "Four":"Four"]

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding30testForcedCastSwiftToNSDictIntSo12NSDictionaryCyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testForcedCastSwiftToNSDictInt() -> NSDictionary {
  var dict: NSDictionary = forcedCast(dictInt)
  return dict
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding28testCondCastSwiftToNSDictIntSo12NSDictionaryCSgyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testCondCastSwiftToNSDictInt() -> NSDictionary? {
  var dictOpt: NSDictionary? = condCast(dictInt)
  return dictOpt
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding33testForcedCastSwiftToNSDictDoubleSo12NSDictionaryCyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testForcedCastSwiftToNSDictDouble() -> NSDictionary {
  var dict: NSDictionary = forcedCast(dictDouble)
  return dict
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding31testCondCastSwiftToNSDictDoubleSo12NSDictionaryCSgyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testCondCastSwiftToNSDictDouble() -> NSDictionary? {
  var dictOpt: NSDictionary? = condCast(dictDouble)
  return dictOpt
}


// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding33testForcedCastSwiftToNSDictStringSo12NSDictionaryCyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testForcedCastSwiftToNSDictString() -> NSDictionary {
  var dict: NSDictionary = forcedCast(dictString)
  return dict
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding31testCondCastSwiftToNSDictStringSo12NSDictionaryCSgyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testCondCastSwiftToNSDictString() -> NSDictionary? {
  var dictOpt: NSDictionary? = condCast(dictString)
  return dictOpt
}


// Check optimization of casts from Swift Set to NSSet

var setInt: Set<Int> = [1, 2, 3, 4]
var setDouble: Set<Double> = [1.1, 2.2, 3.3, 4.4]
var setString: Set<String> = ["One", "Two", "Three", "Four"]

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding29testForcedCastSwiftToNSSetIntSo0I0CyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testForcedCastSwiftToNSSetInt() -> NSSet {
  var set: NSSet = forcedCast(setInt)
  return set
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding27testCondCastSwiftToNSSetIntSo0I0CSgyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testCondCastSwiftToNSSetInt() -> NSSet? {
  var setOpt: NSSet? = condCast(setInt)
  return setOpt
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding32testForcedCastSwiftToNSSetDoubleSo0I0CyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testForcedCastSwiftToNSSetDouble() -> NSSet {
  var set: NSSet = forcedCast(setDouble)
  return set
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding30testCondCastSwiftToNSSetDoubleSo0I0CSgyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testCondCastSwiftToNSSetDouble() -> NSSet? {
  var setOpt: NSSet? = condCast(setDouble)
  return setOpt
}


// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding32testForcedCastSwiftToNSSetStringSo0I0CyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testForcedCastSwiftToNSSetString() -> NSSet {
  var set: NSSet = forcedCast(setString)
  return set
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding30testCondCastSwiftToNSSetStringSo0I0CSgyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: return
@inline(never)
public func testCondCastSwiftToNSSetString() -> NSSet? {
  var setOpt: NSSet? = condCast(setString)
  return setOpt
}

// Casts involving generics cannot be optimized.

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding25testForcedCastFromGeneric{{[_0-9a-zA-Z]*}}F
// CHECK: unconditional_checked
// CHECK: return
@inline(never)
public func testForcedCastFromGeneric<T>(_ x: T) -> NSString {
  var set: NSString = x as! NSString
  return set
}

// CHECK-LABEL: sil [noinline] {{.*}}@$s21bridged_casts_folding23testForcedCastToGeneric{{[_0-9a-zA-Z]*}}F
// CHECK: unconditional_checked
// CHECK: return
@inline(never)
public func testForcedCastToGeneric<T>(_ x: T) -> T {
  var set: T = nsString as! T
  return set
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding23testCondCastFromGeneric{{[_0-9a-zA-Z]*}}F
// CHECK: checked_cast_addr_br
// CHECK: return
@inline(never)
public func testCondCastFromGeneric<T>(_ x: T) -> NSString? {
  var setOpt: NSString? = x as? NSString
  return setOpt
}

// CHECK-LABEL: sil [noinline] {{.*}}@$s21bridged_casts_folding21testCondCastToGeneric{{[_0-9a-zA-Z]*}}F
// CHECK: checked_cast_addr_br
// CHECK: return
@inline(never)
public func testCondCastToGeneric<T>(_ x: T) -> T? {
  var setOpt: T? = nsString as? T
  return setOpt
}


// Run-time tests

func testit() {
  //// ObjC -> Swift
  
  // Arrays
  print("NS to Swift arrays: Start")
  print(testForcedCastNStoSwiftArrayInt())
  print(testCondCastNStoSwiftArrayInt())
  
  print(testForcedCastNStoSwiftArrayDouble())
  print(testCondCastNStoSwiftArrayDouble())
  
  print(testForcedCastNStoSwiftArrayString())
  print(testCondCastNStoSwiftArrayString())
  print("NS to Swift arrays: End")
  
  // Dicts
  print("NS to Swift dictionaries: Start")
  print(testForcedCastNStoSwiftDictInt())
  print(testCondCastNStoSwiftDictInt())
  
  print(testForcedCastNStoSwiftDictDouble())
  print(testCondCastNStoSwiftDictDouble())
  
  print(testForcedCastNStoSwiftDictString())
  print(testCondCastNStoSwiftDictString())
  print(testCondCastNSDictStringtoSwiftDictInt())
  // This line should crash at run-time
  //print(testForcedCastNSDictStringtoSwiftDictInt())
  print("NS to Swift dictionaries: End")
  
  // Sets
  print("NS to Swift sets: Start")
  print(testForcedCastNStoSwiftSetInt())
  print(testCondCastNStoSwiftSetInt())
  
  print(testForcedCastNStoSwiftSetDouble())
  print(testCondCastNStoSwiftSetDouble())
  
  print(testForcedCastNStoSwiftSetString())
  print(testCondCastNStoSwiftSetString())
  print("NS to Swift sets: End")
  
  
  // Basic types
  
  print("NS to Swift basic types: Start")
  print(testForcedCastNSNumberToSwiftInt())
  print(testCondCastNSNumberToSwiftInt())
  
  print(testForcedCastNSNumberToSwiftDouble())
  print(testCondCastNSNumberToSwiftDouble())
  
  print(testForcedCastNSIntNumberToSwiftDouble())
  print(testCondCastNSIntNumberToSwiftDouble())
  
  print(testForcedCastNStoSwiftString())
  print(testCondCastNStoSwiftString())
  print("NS to Swift basic types: End")
  
  //// Swift -> ObjC
  
  // Basic types
  
  print("Swift to NS basic types: Start")
  print(testForcedCastSwiftIntToNSNumber())
  print(testCondCastSwiftIntToNSNumber())
  
  print(testForcedCastSwiftDoubleToNSNumber())
  print(testCondCastSwiftDoubleToNSNumber())
  
  print(testForcedCastSwiftToNSString())
  print(testCondCastSwiftToNSString())
  print("Swift to NS basic types: End")
  
  // Arrays
  print("Swift to NS arrays: Start")
  
  print(testForcedCastSwiftToNSArrayInt())
  print(testCondCastSwiftToNSArrayInt())
  
  print(testForcedCastSwiftToNSArrayDouble())
  print(testCondCastSwiftToNSArrayDouble())
  
  print(testForcedCastSwiftToNSArrayString())
  print(testCondCastSwiftToNSArrayString())
  
  print("Swift to NS arrays: End")
  
  
  // Dicts
  print("Swift to NS dictionaries: Start")
  
  print(testForcedCastSwiftToNSDictInt())
  print(testCondCastSwiftToNSDictInt())
  
  print(testForcedCastSwiftToNSDictDouble())
  print(testCondCastSwiftToNSDictDouble())
  
  print(testForcedCastSwiftToNSDictString())
  print(testCondCastSwiftToNSDictString())
  
  print("Swift to NS dictionaries: End")
  
  // Sets
  print("Swift to NS sets: Start")
  
  print(testForcedCastSwiftToNSSetInt())
  print(testCondCastSwiftToNSSetInt())
  
  print(testForcedCastSwiftToNSSetDouble())
  print(testCondCastSwiftToNSSetDouble())
  
  print(testForcedCastSwiftToNSSetString())
  print(testCondCastSwiftToNSSetString())
  
  print("Swift to NS sets: End")
}

testit()

// Check optimizations of casts from String to CFString

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding29testForcedCastSwiftToCFStringSo0I3RefayF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: unchecked_ref_cast{{.*}}: $NSString to $CFString
// CHECK: end{{.*}}$s21bridged_casts_folding29testForcedCastSwiftToCFStringSo0I3RefayF
@inline(never)
public func testForcedCastSwiftToCFString() -> CFString {
  let o: CFString = forcedCast(swiftString)
  return o
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding27testCondCastSwiftToCFStringSo0I3RefaSgyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: unchecked_ref_cast{{.*}}: $NSString to $CFString
// CHECK: end{{.*}}$s21bridged_casts_folding27testCondCastSwiftToCFStringSo0I3RefaSgyF
@inline(never)
public func testCondCastSwiftToCFString() -> CFString? {
  let o: CFString? = condCast(swiftString)
  return o
}

// Check optimizations of casts from Int to CFNumber

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding32testForcedCastSwiftIntToCFNumberSo0J3RefayF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: unchecked_ref_cast{{.*}}: $NSNumber to $CFNumber
// CHECK: end{{.*}}$s21bridged_casts_folding32testForcedCastSwiftIntToCFNumberSo0J3RefayF
@inline(never)
public func testForcedCastSwiftIntToCFNumber() -> CFNumber {
  let o: CFNumber = forcedCast(swiftIntNumber)
  return o
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding30testCondCastSwiftIntToCFNumberSo0J3RefaSgyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: unchecked_ref_cast{{.*}}: $NSNumber to $CFNumber
// CHECK: end{{.*}}$s21bridged_casts_folding30testCondCastSwiftIntToCFNumberSo0J3RefaSgyF
@inline(never)
public func testCondCastSwiftIntToCFNumber() -> CFNumber? {
  let o: CFNumber? = condCast(swiftIntNumber)
  return o
}

// Check optimization of casts from Swift Array to CFArray

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding31testForcedCastSwiftToCFArrayIntSo0I3RefayF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: unchecked_ref_cast{{.*}}: $NSArray to $CFArray
// CHECK: end{{.*}}$s21bridged_casts_folding31testForcedCastSwiftToCFArrayIntSo0I3RefayF
@inline(never)
public func testForcedCastSwiftToCFArrayInt() -> CFArray {
  let arr: CFArray = forcedCast(arrInt)
  return arr
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding29testCondCastSwiftToCFArrayIntSo0I3RefaSgyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: unchecked_ref_cast{{.*}}: $NSArray to $CFArray
// CHECK: end{{.*}}$s21bridged_casts_folding29testCondCastSwiftToCFArrayIntSo0I3RefaSgyF
@inline(never)
public func testCondCastSwiftToCFArrayInt() -> CFArray? {
  let arrOpt: CFArray? = condCast(arrInt)
  return arrOpt
}

// Check optimization of casts from Swift Dict to CFDictionary

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding30testForcedCastSwiftToCFDictIntSo15CFDictionaryRefayF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: unchecked_ref_cast{{.*}}: $NSDictionary to $CFDictionary
// CHECK: end{{.*}}$s21bridged_casts_folding30testForcedCastSwiftToCFDictIntSo15CFDictionaryRefayF
@inline(never)
public func testForcedCastSwiftToCFDictInt() -> CFDictionary {
  let dict: CFDictionary = forcedCast(dictInt)
  return dict
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding28testCondCastSwiftToCFDictIntSo15CFDictionaryRefaSgyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: unchecked_ref_cast{{.*}}: $NSDictionary to $CFDictionary
// CHECK: end{{.*}}$s21bridged_casts_folding28testCondCastSwiftToCFDictIntSo15CFDictionaryRefaSgyF
@inline(never)
public func testCondCastSwiftToCFDictInt() -> CFDictionary? {
  let dictOpt: CFDictionary? = condCast(dictInt)
  return dictOpt
}

// Check optimization of casts from Swift Set to CFSet

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding29testForcedCastSwiftToCFSetIntSo0I3RefayF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: unchecked_ref_cast{{.*}}: $NSSet to $CFSet
// CHECK: end{{.*}}$s21bridged_casts_folding29testForcedCastSwiftToCFSetIntSo0I3RefayF
@inline(never)
public func testForcedCastSwiftToCFSetInt() -> CFSet {
  let set: CFSet = forcedCast(setInt)
  return set
}

// CHECK-LABEL: sil [noinline] @$s21bridged_casts_folding27testCondCastSwiftToCFSetIntSo0I3RefaSgyF
// CHECK-NOT: unconditional_checked
// CHECK: function_ref @{{.*}}_bridgeToObjectiveC
// CHECK: unchecked_ref_cast{{.*}}: $NSSet to $CFSet
// CHECK: end{{.*}}$s21bridged_casts_folding27testCondCastSwiftToCFSetIntSo0I3RefaSgyF
@inline(never)
public func testCondCastSwiftToCFSetInt() -> CFSet? {
  let setOpt: CFSet? = condCast(setInt)
  return setOpt
}

public class NSObjectSubclass : NSObject { }

var anyHashable: AnyHashable = 0

// CHECK-LABEL: $s21bridged_casts_folding29testUncondCastSwiftToSubclassAA08NSObjectI0CyF
// CHECK: [[GLOBAL:%[0-9]+]] = global_addr @$s21bridged_casts_folding11anyHashables03AnyE0Vv
// CHECK: [[TMP:%[0-9]+]] = alloc_stack $AnyHashable
// CHECK: [[ACCESS:%[0-9]+]] = begin_access [read] [static] [no_nested_conflict] [[GLOBAL]]
// CHECK: copy_addr [[ACCESS]] to [init] [[TMP]]
// CHECK: [[FUNC:%.*]] = function_ref @$ss11AnyHashableV10FoundationE19_bridgeToObjectiveCSo8NSObjectCyF
// CHECK-NEXT: apply [[FUNC]]([[TMP]])
// CHECK-NEXT: destroy_addr [[TMP]]
// CHECK-NEXT: unconditional_checked_cast {{%.*}} : $NSObject to NSObjectSubclass
// CHECK: } // end sil function '$s21bridged_casts_folding29testUncondCastSwiftToSubclassAA08NSObjectI0CyF'
@inline(never)
public func testUncondCastSwiftToSubclass() -> NSObjectSubclass {
  return anyHashable as! NSObjectSubclass
}

class MyThing: Hashable {
    let name: String
    
    init(name: String) {
        self.name = name
    }
    
    deinit {
        Swift.print("Deinit \(name)")
    }

    func hash(into hasher: inout Hasher) {}
    
    static func ==(lhs: MyThing, rhs: MyThing) -> Bool {
        return false
    }
}

// CHECK-LABEL: sil hidden [noinline] @$s21bridged_casts_folding26doSomethingWithAnyHashableyys0gH0VF : $@convention(thin) (@in_guaranteed AnyHashable) -> () {
// CHECK: %2 = alloc_stack $AnyHashable
// CHECK: copy_addr %0 to [init] %2 : $*AnyHashable
// CHECK: checked_cast_addr_br take_always AnyHashable in %2 : $*AnyHashable to MyThing
@inline(never)
func doSomethingWithAnyHashable(_ item: AnyHashable) {
  _ = item as? MyThing
}

@inline(never)
public func testMyThing() {
  let x = MyThing(name: "B")
  doSomethingWithAnyHashable(x)
}
