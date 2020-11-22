// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// RUN: %target-run-simple-swift
// REQUIRES: executable_test
// REQUIRES: objc_interop

import Foundation

#if FOUNDATION_XCTEST
    import XCTest
    class TestMeasurementSuper : XCTestCase { }
#else
    import StdlibUnittest
    class TestMeasurementSuper { }
#endif

// We define our own units here so that we can have closer control over checking the behavior of just struct Measurement and not the rest of Foundation
@available(OSX 10.12, iOS 10.0, watchOS 3.0, tvOS 10.0, *)
class MyDimensionalUnit : Dimension {
    class var unitA : MyDimensionalUnit {
        return MyDimensionalUnit(symbol: "a", converter: UnitConverterLinear(coefficient: 1))
    }
    class var unitKiloA : MyDimensionalUnit {
        return MyDimensionalUnit(symbol: "ka", converter: UnitConverterLinear(coefficient: 1_000))
    }
    class var unitMegaA : MyDimensionalUnit {
        return MyDimensionalUnit(symbol: "Ma", converter: UnitConverterLinear(coefficient: 1_000_000))
    }
    override class func baseUnit() -> Self {
        return MyDimensionalUnit.unitA as! Self
    }
}

@available(OSX 10.12, iOS 10.0, watchOS 3.0, tvOS 10.0, *)
class CustomUnit : Unit {
    override init(symbol: String) {
        super.init(symbol: symbol)
    }
    
    required init?(coder aDecoder: NSCoder) {
        super.init(coder: aDecoder)
    }
    
    public static let bugs = CustomUnit(symbol: "bug")
    public static let features = CustomUnit(symbol: "feature")
}

@available(OSX 10.12, iOS 10.0, watchOS 3.0, tvOS 10.0, *)
class TestMeasurement : TestMeasurementSuper {
    
    func testBasicConstruction() {
        let m1 = Measurement(value: 3, unit: MyDimensionalUnit.unitA)
        let m2 = Measurement(value: 3, unit: MyDimensionalUnit.unitA)
        
        let m3 = m1 + m2
        
        expectEqual(6, m3.value)
        expectEqual(m1, m2)
        
        let m10 = Measurement(value: 2, unit: CustomUnit.bugs)
        let m11 = Measurement(value: 2, unit: CustomUnit.bugs)
        let m12 = Measurement(value: 3, unit: CustomUnit.bugs)
        
        expectEqual(m10, m11)
        expectNotEqual(m10, m12)
        
        // This test has 2 + 2 + 3 bugs
        expectEqual((m10 + m11 + m12).value, 7)
    }
    
    func testConversion() {
        let m1 = Measurement(value: 1000, unit: MyDimensionalUnit.unitA)
        let kiloM1 = Measurement(value: 1, unit: MyDimensionalUnit.unitKiloA)

        let result = m1.converted(to: MyDimensionalUnit.unitKiloA)
        expectEqual(kiloM1, result)
        
        let sameResult = m1.converted(to: MyDimensionalUnit.unitA)
        expectEqual(sameResult, m1)
        
        // This correctly fails to build
        
        // let m2 = Measurement(value: 1, unit: CustomUnit.bugs)
        // m2.converted(to: MyDimensionalUnit.unitKiloA)
    }
    
    func testOperators() {
        // Which is bigger: 1 ka or 1 Ma?
        let oneKiloA = Measurement(value: 1, unit: MyDimensionalUnit.unitKiloA)
        let oneMegaA = Measurement(value: 1, unit: MyDimensionalUnit.unitMegaA)
        
        expectTrue(oneKiloA < oneMegaA)
        expectFalse(oneKiloA > oneMegaA)
        expectTrue(oneKiloA * 2000 > oneMegaA)
        expectTrue(oneMegaA / 1_000_000 < oneKiloA)
        expectTrue(2000 * oneKiloA > oneMegaA)
        expectTrue(2 / oneMegaA > oneMegaA)
        expectEqual(2 / (oneMegaA * 2), oneMegaA)
        expectTrue(oneMegaA <= oneKiloA * 1000)
        expectTrue(oneMegaA - oneKiloA <= oneKiloA * 1000)
        expectTrue(oneMegaA >= oneKiloA * 1000)
        expectTrue(oneMegaA >= ((oneKiloA * 1000) - oneKiloA))
        
        // Dynamically different dimensions
        expectEqual(Measurement(value: 1_001_000, unit: MyDimensionalUnit.unitA), oneMegaA + oneKiloA)
        
        var bugCount = Measurement(value: 1, unit: CustomUnit.bugs)
        expectEqual(bugCount.value, 1)
        bugCount = bugCount + Measurement(value: 4, unit: CustomUnit.bugs)
        expectEqual(bugCount.value, 5)
    }
    
    func testUnits() {
        expectEqual(MyDimensionalUnit.unitA, MyDimensionalUnit.unitA)
        expectTrue(MyDimensionalUnit.unitA == MyDimensionalUnit.unitA)
    }
    
    func testMeasurementFormatter() {
        let formatter = MeasurementFormatter()
        let measurement = Measurement(value: 100, unit: UnitLength.kilometers)
        let result = formatter.string(from: measurement)
        
        // Just make sure we get a result at all here
        expectFalse(result.isEmpty)
    }

    func testEquality() {
        let fiveKM = Measurement(value: 5, unit: UnitLength.kilometers)
        let fiveSeconds = Measurement(value: 5, unit: UnitDuration.seconds)
        let fiveThousandM = Measurement(value: 5000, unit: UnitLength.meters)

        expectTrue(fiveKM == fiveThousandM)
        expectEqual(fiveKM, fiveThousandM)
        expectFalse(fiveKM == fiveSeconds)
    }

    func testComparison() {
        let fiveKM = Measurement(value: 5, unit: UnitLength.kilometers)
        let fiveThousandM = Measurement(value: 5000, unit: UnitLength.meters)
        let sixKM = Measurement(value: 6, unit: UnitLength.kilometers)
        let sevenThousandM = Measurement(value: 7000, unit: UnitLength.meters)

        expectTrue(fiveKM < sixKM)
        expectTrue(fiveKM < sevenThousandM)
        expectTrue(fiveKM <= fiveThousandM)
    }

    func testHashing() {
        guard #available(macOS 10.15, iOS 13, watchOS 6, tvOS 13, *) else { return }
        let lengths: [[Measurement<UnitLength>]] = [
            [
                Measurement(value: 5, unit: UnitLength.kilometers),
                Measurement(value: 5000, unit: UnitLength.meters),
                Measurement(value: 5000, unit: UnitLength.meters),
            ],
            [
                Measurement(value: 1, unit: UnitLength.kilometers),
                Measurement(value: 1000, unit: UnitLength.meters),
            ],
            [
                Measurement(value: 1, unit: UnitLength.meters),
                Measurement(value: 1000, unit: UnitLength.millimeters),
            ],
        ]
        checkHashableGroups(lengths)

        let durations: [[Measurement<UnitDuration>]] = [
            [
                Measurement(value: 3600, unit: UnitDuration.seconds),
                Measurement(value: 60, unit: UnitDuration.minutes),
                Measurement(value: 1, unit: UnitDuration.hours),
            ],
            [
                Measurement(value: 1800, unit: UnitDuration.seconds),
                Measurement(value: 30, unit: UnitDuration.minutes),
                Measurement(value: 0.5, unit: UnitDuration.hours),
            ]
        ]
        checkHashableGroups(durations)

        let custom: [Measurement<CustomUnit>] = [
            Measurement(value: 1, unit: CustomUnit.bugs),
            Measurement(value: 2, unit: CustomUnit.bugs),
            Measurement(value: 3, unit: CustomUnit.bugs),
            Measurement(value: 4, unit: CustomUnit.bugs),
            Measurement(value: 1, unit: CustomUnit.features),
            Measurement(value: 2, unit: CustomUnit.features),
            Measurement(value: 3, unit: CustomUnit.features),
            Measurement(value: 4, unit: CustomUnit.features),
        ]
        checkHashable(custom, equalityOracle: { $0 == $1 })
    }

    func test_AnyHashableContainingMeasurement() {
        let values: [Measurement<UnitLength>] = [
          Measurement(value: 100, unit: UnitLength.meters),
          Measurement(value: 100, unit: UnitLength.kilometers),
          Measurement(value: 100, unit: UnitLength.kilometers),
        ]
        let anyHashables = values.map(AnyHashable.init)
        expectEqual(Measurement<UnitLength>.self, type(of: anyHashables[0].base))
        expectEqual(Measurement<UnitLength>.self, type(of: anyHashables[1].base))
        expectEqual(Measurement<UnitLength>.self, type(of: anyHashables[2].base))
        expectNotEqual(anyHashables[0], anyHashables[1])
        expectEqual(anyHashables[1], anyHashables[2])
    }

    func test_AnyHashableCreatedFromNSMeasurement() {
        let values: [NSMeasurement] = [
            NSMeasurement(doubleValue: 100, unit: UnitLength.meters),
            NSMeasurement(doubleValue: 100, unit: UnitLength.kilometers),
            NSMeasurement(doubleValue: 100, unit: UnitLength.kilometers),
        ]
        let anyHashables = values.map(AnyHashable.init)
        expectEqual(Measurement<Unit>.self, type(of: anyHashables[0].base))
        expectEqual(Measurement<Unit>.self, type(of: anyHashables[1].base))
        expectEqual(Measurement<Unit>.self, type(of: anyHashables[2].base))
        expectNotEqual(anyHashables[0], anyHashables[1])
        expectEqual(anyHashables[1], anyHashables[2])
    }
}

#if !FOUNDATION_XCTEST
if #available(OSX 10.12, iOS 10.0, tvOS 10.0, watchOS 3.0, *) {
    let MeasurementTests = TestSuite("TestMeasurement")
    MeasurementTests.test("testBasicConstruction") { TestMeasurement().testBasicConstruction() }
    MeasurementTests.test("testConversion") { TestMeasurement().testConversion() }
    MeasurementTests.test("testOperators") { TestMeasurement().testOperators() }
    MeasurementTests.test("testUnits") { TestMeasurement().testUnits() }
    MeasurementTests.test("testMeasurementFormatter") { TestMeasurement().testMeasurementFormatter() }
    MeasurementTests.test("testEquality") { TestMeasurement().testEquality() }
    MeasurementTests.test("testComparison") { TestMeasurement().testComparison() }
    MeasurementTests.test("testHashing") { TestMeasurement().testHashing() }
    MeasurementTests.test("test_AnyHashableContainingMeasurement") { TestMeasurement().test_AnyHashableContainingMeasurement() }
  MeasurementTests.test("test_AnyHashableCreatedFromNSMeasurement") { TestMeasurement().test_AnyHashableCreatedFromNSMeasurement() }
    runAllTests()
}
#endif
