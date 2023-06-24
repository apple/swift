// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module-path=%t/RAD.swiftmodule -module-name=RAD -enable-experimental-feature RuntimeDiscoverableAttrs %S/Inputs/runtime_attrs.swift
// RUN: %target-swift-frontend -primary-file %s -emit-ir -I %t -swift-version 5 -enable-experimental-feature RuntimeDiscoverableAttrs | %IRGenFileCheck %s

// REQUIRES: asserts
// REQUIRES: OS=macosx

// First, make sure that we emit:
// - accessible function records for each generator
// - runtime attribute records with correct number of trailing objects

// CHECK: @"$s18runtime_attributes4FlagVAA8globalFnyyFfaHF"
// CHECK: @"$s18runtime_attributes4FlagVAA1AV2v1SSvpfaHF"
// CHECK: @"$s18runtime_attributes4FlagVAA1AV4compSivpfaHF"
// CHECK: @"$s18runtime_attributes4FlagVAA1AV5test1SiyFZfaHF"
// CHECK: @"$s18runtime_attributes4FlagVAA1AV5test2yyFfaHF"
// CHECK: @"$s18runtime_attributes4FlagVAA1AV1xSaySiGSgvpfaHF"
// CHECK: @"$s18runtime_attributes13OnlyPropsTestVAA1AV1xSaySiGSgvpfaHF"
// CHECK: @"$s18runtime_attributes4FlagVAA1AV5InnerC4testSaySiGSgvpfaHF"
// CHECK: @"$s18runtime_attributes13OnlyPropsTestVAA1AV5InnerC4testSaySiGSgvpfaHF"
// CHECK: @"$s18runtime_attributes4FlagVAA1AV5InnerCfaHF"
// CHECK: @"$s18runtime_attributes4FlagVAA1AVfaHF"
// CHECK: @"$s18runtime_attributes4FlagVAA1AV5InnerC1BV03extD10StaticTestyyFZfaHF"
// CHECK: @"$s18runtime_attributes4FlagVAA1AV5InnerC1BV03extD4TestyyFZfaHF"
// CHECK: @"$s18runtime_attributes13OnlyPropsTestVAA1AV5InnerC1BV6storedSivpfaHF"
// CHECK: @"$s18runtime_attributes4FlagVAA1AV5InnerC1BV6storedSivpfaHF"
// CHECK: @"$s18runtime_attributes4FlagVAA1AV5InnerC1BVfaHF"
// CHECK: @"$s18runtime_attributes4FlagVAA1AV5InnerC13extStaticTestyyFZfaHF"
// CHECK: @"$s18runtime_attributes4FlagVAA1AV5InnerC7extTestyyFZfaHF"
// CHECK: @"$s3RAD6IgnoreV18runtime_attributes1AV5InnerC11extComputedSivpfaHF"
// CHECK: @"$s18runtime_attributes4FlagVAA1AV5InnerC11extComputedSivpfaHF"
// CHECK: @"$s18runtime_attributes13OnlyPropsTestVAA1AV5InnerC11extComputedSivpfaHF"
// CHECK: @"$s3RAD6IgnoreV18runtime_attributes16WithExternalAttrVfaHF"
// CHECK: @"$s3RAD13TestAmbiguityV18runtime_attributes4testyySiFfaHF"
// CHECK: @"$s3RAD13TestAmbiguityV18runtime_attributes4testyySSFfaHF"
// CHECK: @"$s3RAD13TestAmbiguityV18runtime_attributes0b2NoC0V10testStaticSiyFZfaHF"
// CHECK: @"$s3RAD13TestAmbiguityV18runtime_attributes0b2NoC0V10testStaticyyFZfaHF"
// CHECK: @"$s3RAD13TestAmbiguityV18runtime_attributes0b2NoC0V8testInstyySi_SStFfaHF"
// CHECK: @"$s3RAD13TestAmbiguityV18runtime_attributes0b2NoC0V8testInstyySi_SitFfaHF"
// CHECK: @"$s3RAD6IgnoreV18runtime_attributes14TestInference1VfaHF"
// CHECK: @"$s3RAD6IgnoreV18runtime_attributes14TestInference2CfaHF"
// CHECK: @"$s3RAD6IgnoreV18runtime_attributes14TestInference3OfaHF"
// CHECK: @"$s18runtime_attributes17AvailabilityTestsVAA08testWithC0yySiFfaHF"
// CHECK: @"$s18runtime_attributes17AvailabilityTestsVAA08TypeWithC0CfaHF"
// CHECK: @"$s18runtime_attributes17AvailabilityTestsVAA011MembersWithC0C8staticFnyyFZfaHF"
// CHECK: @"$s18runtime_attributes17AvailabilityTestsVAA011MembersWithC0C6instFnyyFfaHF"
// CHECK: @"$s18runtime_attributes17AvailabilityTestsVAA011MembersWithC0C4propSivpfaHF"
// CHECK: @"$s18runtime_attributes13FlagWithAvailVAA20attrIsHigherThanFuncyyFfaHF"
// CHECK: @"$s18runtime_attributes19FlagForInnerMethodsVAA9OuterTypeV0E0V15innerInstFnTestyyFfaHF"
// CHECK: @"$s18runtime_attributes19FlagForInnerMethodsVAA9OuterTypeV0E0V17innerStaticFnTestyyFZfaHF"
// CHECK: @"$s18runtime_attributes19FlagForInnerMethodsVAA9OuterTypeV0E0V07mutableE6FnTest1x_ySS_SiztFfaHF"
// CHECK: @"$s18runtime_attributes19FlagForInnerMethodsVAA9OuterTypeV15outerMutatingFnSiyFfaHF"
// CHECK: @"$s3RAD8EnumFlagO18runtime_attributes06globalB4TestSi_SaySSGtSgyFfaHF"
// CHECK: @"$s3RAD8EnumFlagO18runtime_attributes0B8TypeTestV1xSivpfaHF"
// CHECK: @"$s3RAD8EnumFlagO18runtime_attributes0B8TypeTestV8testInstyyFfaHF"
// CHECK: @"$s3RAD8EnumFlagO18runtime_attributes0B8TypeTestV10testStaticSiyFZfaHF"
// CHECK: @"$s3RAD8EnumFlagO18runtime_attributes0B8TypeTestVfaHF"

// CHECK: @"$s18runtime_attributes4FlagVHa" = internal constant
// CHECK-SAME: i32 0
// CHECK-SAME: @"$s18runtime_attributes4FlagVMn"
// CHECK-SAME: i32 16
// CHECK-SAME: @"$s18runtime_attributes4FlagVAA8globalFnyyFfaHF"
// CHECK-SAME: @"$s18runtime_attributes4FlagVAA1AV2v1SSvpfaHF"
// CHECK-SAME: @"$s18runtime_attributes4FlagVAA1AV4compSivpfaHF"
// CHECK-SAME: @"$s18runtime_attributes4FlagVAA1AV5test1SiyFZfaHF"
// CHECK-SAME: @"$s18runtime_attributes4FlagVAA1AV5test2yyFfaHF"
// CHECK-SAME: @"$s18runtime_attributes4FlagVAA1AV1xSaySiGSgvpfaHF"
// CHECK-SAME: @"$s18runtime_attributes4FlagVAA1AV5InnerC4testSaySiGSgvpfaHF"
// CHECK-SAME: @"$s18runtime_attributes4FlagVAA1AV5InnerCfaHF"
// CHECK-SAME: @"$s18runtime_attributes4FlagVAA1AVfaHF"
// CHECK-SAME: @"$s18runtime_attributes4FlagVAA1AV5InnerC1BV03extD10StaticTestyyFZfaHF"
// CHECK-SAME: @"$s18runtime_attributes4FlagVAA1AV5InnerC1BV03extD4TestyyFZfaHF"
// CHECK-SAME: @"$s18runtime_attributes4FlagVAA1AV5InnerC1BV6storedSivpfaHF"
// CHECK-SAME: @"$s18runtime_attributes4FlagVAA1AV5InnerC1BVfaHF"
// CHECK-SAME: @"$s18runtime_attributes4FlagVAA1AV5InnerC13extStaticTestyyFZfaHF"
// CHECK-SAME: @"$s18runtime_attributes4FlagVAA1AV5InnerC7extTestyyFZfaHF"
// CHECK-SAME: @"$s18runtime_attributes4FlagVAA1AV5InnerC11extComputedSivpfaHF"
// CHECK-SAME: section "__TEXT, __swift5_rattrs, regular"

// CHECK: @"$s18runtime_attributes13OnlyPropsTestVHa" = internal constant
// CHECK-SAME: i32 0
// CHECK-SAME: @"$s18runtime_attributes13OnlyPropsTestVMn"
// CHECK-SAME: i32 4
// CHECK-SAME: @"$s18runtime_attributes13OnlyPropsTestVAA1AV1xSaySiGSgvpfaHF"
// CHECK-SAME: @"$s18runtime_attributes13OnlyPropsTestVAA1AV5InnerC4testSaySiGSgvpfaHF"
// CHECK-SAME: @"$s18runtime_attributes13OnlyPropsTestVAA1AV5InnerC1BV6storedSivpfaHF"
// CHECK-SAME: @"$s18runtime_attributes13OnlyPropsTestVAA1AV5InnerC11extComputedSivpfaHF"
// CHECK-SAME: section "__TEXT, __swift5_rattrs, regular"

// CHECK: @"$s3RAD6IgnoreVHa" = internal constant
// CHECK-SAME: i32 0
// CHECK-SAME: ptr @"got.$s3RAD6IgnoreVMn"
// CHECK-SAME: i32 5
// CHECK-SAME: @"$s3RAD6IgnoreV18runtime_attributes1AV5InnerC11extComputedSivpfaHF"
// CHECK-SAME: @"$s3RAD6IgnoreV18runtime_attributes16WithExternalAttrVfaHF"
// CHECK-SAME: @"$s3RAD6IgnoreV18runtime_attributes14TestInference1VfaHF"
// CHECK-SAME: @"$s3RAD6IgnoreV18runtime_attributes14TestInference2CfaHF"
// CHECK-SAME: @"$s3RAD6IgnoreV18runtime_attributes14TestInference3OfaHF"
// CHECK-SAME: section "__TEXT, __swift5_rattrs, regular"

// CHECK: @"$s3RAD13TestAmbiguityVHa" = internal constant
// CHECK-SAME: i32 0
// CHECK-SAME: ptr @"got.$s3RAD13TestAmbiguityVMn"
// CHECK-SAME: i32 6
// CHECK-SAME: @"$s3RAD13TestAmbiguityV18runtime_attributes4testyySiFfaHF"
// CHECK-SAME: @"$s3RAD13TestAmbiguityV18runtime_attributes4testyySSFfaHF"
// CHECK-SAME: @"$s3RAD13TestAmbiguityV18runtime_attributes0b2NoC0V10testStaticSiyFZfaHF"
// CHECK-SAME: @"$s3RAD13TestAmbiguityV18runtime_attributes0b2NoC0V10testStaticyyFZfaHF"
// CHECK-SAME: @"$s3RAD13TestAmbiguityV18runtime_attributes0b2NoC0V8testInstyySi_SStFfaHF"
// CHECK-SAME: @"$s3RAD13TestAmbiguityV18runtime_attributes0b2NoC0V8testInstyySi_SitFfaHF"
// CHECK-SAME: section "__TEXT, __swift5_rattrs, regular"

// CHECK: @"$s18runtime_attributes17AvailabilityTestsVHa" = internal constant
// CHECK-SAME: i32 0
// CHECK-SAME: @"$s18runtime_attributes17AvailabilityTestsVMn"
// CHECK-SAME: i32 5
// CHECK-SAME: @"$s18runtime_attributes17AvailabilityTestsVAA08testWithC0yySiFfaHF
// CHECK-SAME: @"$s18runtime_attributes17AvailabilityTestsVAA08TypeWithC0CfaHF"
// CHECK-SAME: @"$s18runtime_attributes17AvailabilityTestsVAA011MembersWithC0C8staticFnyyFZfaHF"
// CHECK-SAME: @"$s18runtime_attributes17AvailabilityTestsVAA011MembersWithC0C6instFnyyFfaHF"
// CHECK-SAME: @"$s18runtime_attributes17AvailabilityTestsVAA011MembersWithC0C4propSivpfaHF"
// CHECK-SAME: section "__TEXT, __swift5_rattrs, regular"

// CHECK: @"$s18runtime_attributes13FlagWithAvailVHa" = internal constant
// CHECK-SAME: i32 0
// CHECK-SAME: @"$s18runtime_attributes13FlagWithAvailVMn"
// CHECK-SAME: i32 1
// CHECK-SAME: @"$s18runtime_attributes13FlagWithAvailVAA20attrIsHigherThanFuncyyFfaHF"
// CHECK-SAME: section "__TEXT, __swift5_rattrs, regular"

// CHECK: @"$s18runtime_attributes19FlagForInnerMethodsVHa" = internal constant
// CHECK-SAME: i32 0
// CHECK-SAME: @"$s18runtime_attributes19FlagForInnerMethodsVMn"
// CHECK-SAME: i32 4
// CHECK-SAME: @"$s18runtime_attributes19FlagForInnerMethodsVAA9OuterTypeV0E0V15innerInstFnTestyyFfaHF"
// CHECK-SAME: @"$s18runtime_attributes19FlagForInnerMethodsVAA9OuterTypeV0E0V17innerStaticFnTestyyFZfaHF"
// CHECK-SAME: @"$s18runtime_attributes19FlagForInnerMethodsVAA9OuterTypeV0E0V07mutableE6FnTest1x_ySS_SiztFfaHF"
// CHECK-SAME: @"$s18runtime_attributes19FlagForInnerMethodsVAA9OuterTypeV15outerMutatingFnSiyFfaHF"
// CHECK-SAME: section "__TEXT, __swift5_rattrs, regular"

// CHECK: @"$s3RAD8EnumFlagOHa" = internal constant
// CHECK-SAME: i32 0
// CHECK-SAME: ptr @"got.$s3RAD8EnumFlagOMn"
// CHECK-SAME: i32 5
// CHECK-SAME: @"$s3RAD8EnumFlagO18runtime_attributes06globalB4TestSi_SaySSGtSgyFfaHF"
// CHECK-SAME: @"$s3RAD8EnumFlagO18runtime_attributes0B8TypeTestV1xSivpfaHF"
// CHECK-SAME: @"$s3RAD8EnumFlagO18runtime_attributes0B8TypeTestV8testInstyyFfaHF"
// CHECK-SAME: @"$s3RAD8EnumFlagO18runtime_attributes0B8TypeTestV10testStaticSiyFZfaHF"
// CHECK-SAME: @"$s3RAD8EnumFlagO18runtime_attributes0B8TypeTestVfaHF"
// CHECK-SAME: section "__TEXT, __swift5_rattrs, regular"

import RAD

@runtimeMetadata
struct Flag<T> {
  init(attachedTo: T.Type, _ description: String = "") {}
  init<Args>(attachedTo: (Args) -> T, _ description: String = "") {}
  init<Base>(attachedTo: KeyPath<Base, T>, _ description: String = "") {}
}

@runtimeMetadata
struct OnlyPropsTest<B, V> {
  init(attachedTo: KeyPath<B, V>) {}
}

// - Check that all of the generator functions have been emitted

// CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes4FlagVAA8globalFnyyFfa"(ptr noalias nocapture sret(%T18runtime_attributes4FlagVyytGSg) %0)
@Flag("global") func globalFn() {}

@Flag
struct A {
  // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes4FlagVAA1AV2v1SSvpfa"(ptr noalias nocapture sret(%T18runtime_attributes4FlagVySSGSg) %0)
  @Flag("v1") var v1: String = ""

  // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes4FlagVAA1AV4compSivpfa"(ptr noalias nocapture sret(%T18runtime_attributes4FlagVySiGSg) %0)
  @Flag var comp: Int {
    get { 42 }
  }

  // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes4FlagVAA1AV5test1SiyFZfa"(ptr noalias nocapture sret(%T18runtime_attributes4FlagVySiGSg) %0)
  @Flag static func test1() -> Int { 42 }

  // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes4FlagVAA1AV5test2yyFfa"(ptr noalias nocapture sret(%T18runtime_attributes4FlagVyytGSg) %0)
  @Flag("test2") func test2() {} // Ok

  // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes4FlagVAA1AV1xSaySiGSgvpfa"(ptr noalias nocapture sret(%T18runtime_attributes4FlagVySaySiGSgGSg) %0)
  // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes13OnlyPropsTestVAA1AV1xSaySiGSgvpfa"(ptr noalias nocapture sret(%T18runtime_attributes13OnlyPropsTestVyAA1AVSaySiGSgGSg) %0)
  @OnlyPropsTest @Flag("x") var x: [Int]? = [] // Ok

  @Flag("Inner type") class Inner {
    // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes4FlagVAA1AV5InnerC4testSaySiGSgvpfa"(ptr noalias nocapture sret(%T18runtime_attributes4FlagVySaySiGSgGSg) %0)
    // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes13OnlyPropsTestVAA1AV5InnerC4testSaySiGSgvpfa"(ptr noalias nocapture sret(%T18runtime_attributes13OnlyPropsTestVyAA1AV5InnerCSaySiGSgGSg) %0)
    @OnlyPropsTest @Flag("test property") var test: [Int]? = nil // Ok
  } // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes4FlagVAA1AV5InnerCfa"(ptr noalias nocapture sret(%T18runtime_attributes4FlagVyAA1AV5InnerCGSg) %0)
}

// The generator for `struct A` is emitted last.
// CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes4FlagVAA1AVfa"(ptr noalias nocapture sret(%T18runtime_attributes4FlagVyAA1AVGSg) %0)

extension A.Inner {
  @Flag("B type") struct B {
    // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes4FlagVAA1AV5InnerC1BV03extD10StaticTestyyFZfa"(ptr noalias nocapture sret(%T18runtime_attributes4FlagVyytGSg) %0)
    @Flag static func extInnerStaticTest() {}
    // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes4FlagVAA1AV5InnerC1BV03extD4TestyyFZfa"(ptr noalias nocapture sret(%T18runtime_attributes4FlagVyytGSg) %0)
    @Flag static func extInnerTest() {}

    // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes13OnlyPropsTestVAA1AV5InnerC1BV6storedSivpfa"(ptr noalias nocapture sret(%T18runtime_attributes13OnlyPropsTestVyAA1AV5InnerC1BVSiGSg) %0)
    // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes4FlagVAA1AV5InnerC1BV6storedSivpfa"(ptr noalias nocapture sret(%T18runtime_attributes4FlagVySiGSg) %0)
    @Flag("stored prop") @OnlyPropsTest let stored: Int = 42
  } // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes4FlagVAA1AV5InnerC1BVfa"(ptr noalias nocapture sret(%T18runtime_attributes4FlagVyAA1AV5InnerC1BVGSg) %0

  // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes4FlagVAA1AV5InnerC13extStaticTestyyFZfa"(ptr noalias nocapture sret(%T18runtime_attributes4FlagVyytGSg) %0)
  @Flag static func extStaticTest() {}
  // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes4FlagVAA1AV5InnerC7extTestyyFZfa"(ptr noalias nocapture sret(%T18runtime_attributes4FlagVyytGSg) %0)
  @Flag static func extTest() {}

  // CHECK-LABEL: define hidden swiftcc void @"$s3RAD6IgnoreV18runtime_attributes1AV5InnerC11extComputedSivpfa"(ptr noalias nocapture sret(%T3RAD6IgnoreVys7KeyPathCy18runtime_attributes1AV5InnerCSiGGSg) %0)
  // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes4FlagVAA1AV5InnerC11extComputedSivpfa"(ptr noalias nocapture sret(%T18runtime_attributes4FlagVySiGSg) %0)
  // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes13OnlyPropsTestVAA1AV5InnerC11extComputedSivpfa"(ptr noalias nocapture sret(%T18runtime_attributes13OnlyPropsTestVyAA1AV5InnerCSiGSg) %0)
  @OnlyPropsTest @Flag @Ignore var extComputed: Int {
    get { 42 }
  }
}

// CHECK-LABEL: define hidden swiftcc void @"$s3RAD6IgnoreV18runtime_attributes16WithExternalAttrVfa"(ptr noalias nocapture sret(%T3RAD6IgnoreVy18runtime_attributes16WithExternalAttrVmGSg) %0)
@Ignore struct WithExternalAttr {}

// CHECK-LABEL: define hidden swiftcc void @"$s3RAD13TestAmbiguityV18runtime_attributes4testyySiFfa"(ptr noalias nocapture sret(%T3RAD13TestAmbiguityVSg) %0)
@TestAmbiguity public func test(_: Int) {}
// CHECK-LABEL: define hidden swiftcc void @"$s3RAD13TestAmbiguityV18runtime_attributes4testyySSFfa"(ptr noalias nocapture sret(%T3RAD13TestAmbiguityVSg) %0)
@TestAmbiguity public func test(_: String) {}

public struct TestNoAmbiguity {
  // CHECK-LABEL: define hidden swiftcc void @"$s3RAD13TestAmbiguityV18runtime_attributes0b2NoC0V10testStaticSiyFZfa"(ptr noalias nocapture sret(%T3RAD13TestAmbiguityVSg) %0)
  @TestAmbiguity static func testStatic() -> Int { 42 }
  // CHECK-LABEL: define hidden swiftcc void @"$s3RAD13TestAmbiguityV18runtime_attributes0b2NoC0V10testStaticyyFZfa"(ptr noalias nocapture sret(%T3RAD13TestAmbiguityVSg) %0)
  @TestAmbiguity static func testStatic() {}

  // CHECK-LABEL: define hidden swiftcc void @"$s3RAD13TestAmbiguityV18runtime_attributes0b2NoC0V8testInstyySi_SStFfa"(ptr noalias nocapture sret(%T3RAD13TestAmbiguityVSg) %0)
  @TestAmbiguity func testInst(_: Int, _: String) {}
  // CHECK-LABEL: define hidden swiftcc void @"$s3RAD13TestAmbiguityV18runtime_attributes0b2NoC0V8testInstyySi_SitFfa"(ptr noalias nocapture sret(%T3RAD13TestAmbiguityVSg) %0)
  @TestAmbiguity func testInst(_: Int, _: Int) {}
}

// CHECK-LABEL: define hidden swiftcc void @"$s3RAD6IgnoreV18runtime_attributes14TestInference1Vfa"(ptr noalias nocapture sret(%T3RAD6IgnoreVy18runtime_attributes14TestInference1VmGSg) %0)
public struct TestInference1 : Ignored {}
// CHECK-LABEL: define hidden swiftcc void @"$s3RAD6IgnoreV18runtime_attributes14TestInference2Cfa"(ptr noalias nocapture sret(%T3RAD6IgnoreVy18runtime_attributes14TestInference2CmGSg) %0)
public class  TestInference2 : Ignored {}
// CHECK-LABEL: define hidden swiftcc void @"$s3RAD6IgnoreV18runtime_attributes14TestInference3Ofa"(ptr noalias nocapture sret(%T3RAD6IgnoreVy18runtime_attributes14TestInference3OmGSg) %0)
public enum   TestInference3 : Ignored {}

@runtimeMetadata
struct AvailabilityTests {
  init(attachedTo: Any) {}
}

// CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes17AvailabilityTestsVAA08testWithC0yySiFfa"(ptr noalias nocapture sret(%T18runtime_attributes17AvailabilityTestsVSg) %0)
// CHECK: entry:
// CHECK: {{.*}} = call swiftcc i1 @"$ss26_stdlib_isOSVersionAtLeastyBi1_Bw_BwBwtF"(i64 42, i64 0, i64 0)
@available(macOS, introduced: 42.0)
@AvailabilityTests
func testWithAvailability(_: Int) {}

// CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes17AvailabilityTestsVAA08TypeWithC0Cfa"(ptr noalias nocapture sret(%T18runtime_attributes17AvailabilityTestsVSg) %0)
// CHECK: entry:
// CHECK: {{.*}} = call swiftcc i1 @"$ss26_stdlib_isOSVersionAtLeastyBi1_Bw_BwBwtF"(i64 100, i64 0, i64 0)
@available(macOS, introduced: 100.0)
@AvailabilityTests
class TypeWithAvailability {}

class MembersWithAvailability {
  // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes17AvailabilityTestsVAA011MembersWithC0C8staticFnyyFZfa"(ptr noalias nocapture sret(%T18runtime_attributes17AvailabilityTestsVSg) %0)
  // CHECK: entry:
  // CHECK: {{.*}} = call swiftcc i1 @"$ss26_stdlib_isOSVersionAtLeastyBi1_Bw_BwBwtF"(i64 201, i64 0, i64 0)
  @AvailabilityTests
  @available(macOS, introduced: 201.0)
  static func staticFn() {}

  // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes17AvailabilityTestsVAA011MembersWithC0C6instFnyyFfa"(ptr noalias nocapture sret(%T18runtime_attributes17AvailabilityTestsVSg) %0)
  // CHECK: entry:
  // CHECK: {{.*}} = call swiftcc i1 @"$ss26_stdlib_isOSVersionAtLeastyBi1_Bw_BwBwtF"(i64 202, i64 0, i64 0)
  @AvailabilityTests
  @available(macOS, introduced: 202.0)
  func instFn() {}

  // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes17AvailabilityTestsVAA011MembersWithC0C4propSivpfa"(ptr noalias nocapture sret(%T18runtime_attributes17AvailabilityTestsVSg) %0)
  // CHECK: entry:
  // CHECK: {{.*}} = call swiftcc i1 @"$ss26_stdlib_isOSVersionAtLeastyBi1_Bw_BwBwtF"(i64 203, i64 0, i64 0)
  @AvailabilityTests
  @available(macOS, introduced: 203.0)
  var prop: Int { get { 42 } }
}

@runtimeMetadata
@available(macOS, introduced: 110.0)
struct FlagWithAvail {
  init(attachedTo: Any) {}
}

// CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes13FlagWithAvailVAA20attrIsHigherThanFuncyyFfa"(ptr noalias nocapture sret(%T18runtime_attributes13FlagWithAvailVSg) %0)
// CHECK: entry:
// CHECK: {{.*}} = call swiftcc i1 @"$ss26_stdlib_isOSVersionAtLeastyBi1_Bw_BwBwtF"(i64 110, i64 0, i64 0)
@FlagWithAvail
@available(macOS, obsoleted: 100.0)
func attrIsHigherThanFunc() {}

func test_local_type_with_protocol_conformance() {
  // CHECK-NOT: define hidden swiftcc void @"$s18runtime_attributes41test_local_type_with_protocol_conformanceyyF9LocalTest{{.*}}"
  struct LocalTest : Ignored {} // Ok but attribute is not inferred.
}

@runtimeMetadata
struct FlagForInnerMethods<Result> {
  init(attachedTo: () -> Result) {}
  init<T>(attachedTo: (T) -> Result) {}
  init<T>(attachedTo: (T.Type) -> Result) {}
  init<T>(attachedTo: (inout T) -> Result) {}
  init<T>(attachedTo: (inout T, String, inout Int) -> Result) {}
}

struct OuterType {
  struct Inner {
    // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes19FlagForInnerMethodsVAA9OuterTypeV0E0V15innerInstFnTestyyFfa"(ptr noalias nocapture sret(%T18runtime_attributes19FlagForInnerMethodsVyytGSg) %0)
    @FlagForInnerMethods func innerInstFnTest() {}
    // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes19FlagForInnerMethodsVAA9OuterTypeV0E0V17innerStaticFnTestyyFZfa"(ptr noalias nocapture sret(%T18runtime_attributes19FlagForInnerMethodsVyytGSg) %0)
    @FlagForInnerMethods static func innerStaticFnTest() {}

    // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes19FlagForInnerMethodsVAA9OuterTypeV0E0V07mutableE6FnTest1x_ySS_SiztFfa"(ptr noalias nocapture sret(%T18runtime_attributes19FlagForInnerMethodsVyytGSg) %0)
    @FlagForInnerMethods mutating func mutableInnerFnTest(x: String, _ y: inout Int) {}
  }
}

extension OuterType {
  // CHECK-LABEL: define hidden swiftcc void @"$s18runtime_attributes19FlagForInnerMethodsVAA9OuterTypeV15outerMutatingFnSiyFfa"(ptr noalias nocapture sret(%T18runtime_attributes19FlagForInnerMethodsVySiGSg) %0)
  @FlagForInnerMethods mutating func outerMutatingFn() -> Int { 42 }
}

// CHECK-LABEL: define hidden swiftcc void @"$s3RAD8EnumFlagO18runtime_attributes06globalB4TestSi_SaySSGtSgyFfa"(ptr noalias nocapture sret(%T3RAD8EnumFlagOyytSi_SaySSGtSgGSg) %0)
@EnumFlag func globalEnumTest() -> (Int, [String])? {
  nil
}

@EnumFlag struct EnumTypeTest {
  // CHECK-LABEL: define hidden swiftcc void @"$s3RAD8EnumFlagO18runtime_attributes0B8TypeTestV1xSivpfa"(ptr noalias nocapture sret(%T3RAD8EnumFlagOy18runtime_attributes0B8TypeTestVSiGSg) %0)
  @EnumFlag var x: Int = 42
  // CHECK-LABEL: define hidden swiftcc void @"$s3RAD8EnumFlagO18runtime_attributes0B8TypeTestV8testInstyyFfa"(ptr noalias nocapture sret(%T3RAD8EnumFlagOy18runtime_attributes0B8TypeTestVytGSg) %0)
  @EnumFlag func testInst() {}
  // CHECK-LABEL: define hidden swiftcc void @"$s3RAD8EnumFlagO18runtime_attributes0B8TypeTestV10testStaticSiyFZfa"(ptr noalias nocapture sret(%T3RAD8EnumFlagOy18runtime_attributes0B8TypeTestVmSiGSg) %0)
  @EnumFlag static func testStatic() -> Int { 42 }
}
// CHECK-LABEL: define hidden swiftcc void @"$s3RAD8EnumFlagO18runtime_attributes0B8TypeTestVfa"(ptr noalias nocapture sret(%T3RAD8EnumFlagOy18runtime_attributes0B8TypeTestVytGSg) %0)
