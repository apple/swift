//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2017 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
// RUN: %target-run-simple-swift
// REQUIRES: executable_test
// REQUIRES: objc_interop
// UNSUPPORTED: back_deployment_runtime

import Foundation
import CoreGraphics

#if FOUNDATION_XCTEST
import XCTest
class TestCodableSuper : XCTestCase { }
#else
import StdlibUnittest
class TestCodableSuper { }
#endif

// MARK: - Helper Functions
@available(macOS 10.11, iOS 9.0, watchOS 2.0, tvOS 9.0, *)
func makePersonNameComponents(namePrefix: String? = nil,
                              givenName: String? = nil,
                              middleName: String? = nil,
                              familyName: String? = nil,
                              nameSuffix: String? = nil,
                              nickname: String? = nil) -> PersonNameComponents {
    var result = PersonNameComponents()
    result.namePrefix = namePrefix
    result.givenName = givenName
    result.middleName = middleName
    result.familyName = familyName
    result.nameSuffix = nameSuffix
    result.nickname = nickname
    return result
}

func debugDescription<T>(_ value: T) -> String {
    if let debugDescribable = value as? CustomDebugStringConvertible {
        return debugDescribable.debugDescription
    } else if let describable = value as? CustomStringConvertible {
        return describable.description
    } else {
        return "\(value)"
    }
}

func performEncodeAndDecode<T : Codable>(of value: T, encode: (T) throws -> Data, decode: (T.Type, Data) throws -> T, lineNumber: Int) -> T {

    let data: Data
    do {
        data = try encode(value)
    } catch {
        fatalError("\(#file):\(lineNumber): Unable to encode \(T.self) <\(debugDescription(value))>: \(error)")
    }

    do {
        return try decode(T.self, data)
    } catch {
        fatalError("\(#file):\(lineNumber): Unable to decode \(T.self) <\(debugDescription(value))>: \(error)")
    }
}

func expectRoundTripEquality<T : Codable>(of value: T, encode: (T) throws -> Data, decode: (T.Type, Data) throws -> T, lineNumber: Int) where T : Equatable {

    let decoded = performEncodeAndDecode(of: value, encode: encode, decode: decode, lineNumber: lineNumber)

    expectEqual(value, decoded, "\(#file):\(lineNumber): Decoded \(T.self) <\(debugDescription(decoded))> not equal to original <\(debugDescription(value))>")
}

func expectRoundTripEqualityThroughJSON<T : Codable>(for value: T, expectedJSON: String? = nil, lineNumber: Int) where T : Equatable {
    let inf = "INF", negInf = "-INF", nan = "NaN"
    let encode = { (_ value: T) throws -> Data in
        let encoder = JSONEncoder()
        encoder.nonConformingFloatEncodingStrategy = .convertToString(positiveInfinity: inf,
                                                                      negativeInfinity: negInf,
                                                                      nan: nan)
        if #available(macOS 10.13, iOS 11.0, watchOS 4.0, tvOS 11.0, *) {
            encoder.outputFormatting = .sortedKeys
        }
        let encoded = try encoder.encode(value)

        if let expectedJSON = expectedJSON {
            let actualJSON = String(decoding: encoded, as: UTF8.self)
            expectEqual(expectedJSON, actualJSON, line: UInt(lineNumber))
        }
        return encoded
    }

    let decode = { (_ type: T.Type, _ data: Data) throws -> T in
        let decoder = JSONDecoder()
        decoder.nonConformingFloatDecodingStrategy = .convertFromString(positiveInfinity: inf,
                                                                        negativeInfinity: negInf,
                                                                        nan: nan)
        return try decoder.decode(type, from: data)
    }

    expectRoundTripEquality(of: value, encode: encode, decode: decode, lineNumber: lineNumber)
}

func expectRoundTripEqualityThroughPlist<T : Codable>(for value: T, lineNumber: Int) where T : Equatable {
    let encode = { (_ value: T) throws -> Data in
        return try PropertyListEncoder().encode(value)
    }

    let decode = { (_ type: T.Type,_ data: Data) throws -> T in
        return try PropertyListDecoder().decode(type, from: data)
    }

    expectRoundTripEquality(of: value, encode: encode, decode: decode, lineNumber: lineNumber)
}

// MARK: - Helper Types
// A wrapper around a UUID that will allow it to be encoded at the top level of an encoder.
struct UUIDCodingWrapper : Codable, Equatable, Hashable, CodingKeyRepresentable {
    let value: UUID

    init(_ value: UUID) {
        self.value = value
    }

    init?<T: CodingKey>(codingKey: T) {
        guard let uuid = UUID(uuidString: codingKey.stringValue) else { return nil }
        self.value = uuid
    }

    var codingKey: CodingKey {
        GenericCodingKey(stringValue: value.uuidString)
    }

    static func ==(_ lhs: UUIDCodingWrapper, _ rhs: UUIDCodingWrapper) -> Bool {
        return lhs.value == rhs.value
    }
}

// MARK: - Tests
class TestCodable : TestCodableSuper {
    // MARK: - AffineTransform
#if os(macOS)
    lazy var affineTransformValues: [Int : AffineTransform] = [
        #line : AffineTransform.identity,
        #line : AffineTransform(),
        #line : AffineTransform(translationByX: 2.0, byY: 2.0),
        #line : AffineTransform(scale: 2.0),
        #line : AffineTransform(rotationByDegrees: .pi / 2),

        #line : AffineTransform(m11: 1.0, m12: 2.5, m21: 66.2, m22: 40.2, tX: -5.5, tY: 3.7),
        #line : AffineTransform(m11: -55.66, m12: 22.7, m21: 1.5, m22: 0.0, tX: -22, tY: -33),
        #line : AffineTransform(m11: 4.5, m12: 1.1, m21: 0.025, m22: 0.077, tX: -0.55, tY: 33.2),
        #line : AffineTransform(m11: 7.0, m12: -2.3, m21: 6.7, m22: 0.25, tX: 0.556, tY: 0.99),
        #line : AffineTransform(m11: 0.498, m12: -0.284, m21: -0.742, m22: 0.3248, tX: 12, tY: 44)
    ]

    func test_AffineTransform_JSON() {
        for (testLine, transform) in affineTransformValues {
            expectRoundTripEqualityThroughJSON(for: transform, lineNumber: testLine)
        }
    }

    func test_AffineTransform_Plist() {
        for (testLine, transform) in affineTransformValues {
            expectRoundTripEqualityThroughPlist(for: transform, lineNumber: testLine)
        }
    }
#endif

    // MARK: - Calendar
    lazy var calendarValues: [Int : Calendar] = [
        #line : Calendar(identifier: .gregorian),
        #line : Calendar(identifier: .buddhist),
        #line : Calendar(identifier: .chinese),
        #line : Calendar(identifier: .coptic),
        #line : Calendar(identifier: .ethiopicAmeteMihret),
        #line : Calendar(identifier: .ethiopicAmeteAlem),
        #line : Calendar(identifier: .hebrew),
        #line : Calendar(identifier: .iso8601),
        #line : Calendar(identifier: .indian),
        #line : Calendar(identifier: .islamic),
        #line : Calendar(identifier: .islamicCivil),
        #line : Calendar(identifier: .japanese),
        #line : Calendar(identifier: .persian),
        #line : Calendar(identifier: .republicOfChina),
    ]

    func test_Calendar_JSON() {
        for (testLine, calendar) in calendarValues {
            expectRoundTripEqualityThroughJSON(for: calendar, lineNumber: testLine)
        }
    }

    func test_Calendar_Plist() {
        for (testLine, calendar) in calendarValues {
            expectRoundTripEqualityThroughPlist(for: calendar, lineNumber: testLine)
        }
    }

    // MARK: - CharacterSet
    lazy var characterSetValues: [Int : CharacterSet] = [
        #line : CharacterSet.controlCharacters,
        #line : CharacterSet.whitespaces,
        #line : CharacterSet.whitespacesAndNewlines,
        #line : CharacterSet.decimalDigits,
        #line : CharacterSet.letters,
        #line : CharacterSet.lowercaseLetters,
        #line : CharacterSet.uppercaseLetters,
        #line : CharacterSet.nonBaseCharacters,
        #line : CharacterSet.alphanumerics,
        #line : CharacterSet.decomposables,
        #line : CharacterSet.illegalCharacters,
        #line : CharacterSet.punctuationCharacters,
        #line : CharacterSet.capitalizedLetters,
        #line : CharacterSet.symbols,
        #line : CharacterSet.newlines
    ]

    func test_CharacterSet_JSON() {
        for (testLine, characterSet) in characterSetValues {
            expectRoundTripEqualityThroughJSON(for: characterSet, lineNumber: testLine)
        }
    }

    func test_CharacterSet_Plist() {
        for (testLine, characterSet) in characterSetValues {
            expectRoundTripEqualityThroughPlist(for: characterSet, lineNumber: testLine)
        }
    }

    // MARK: - CGAffineTransform
    lazy var cg_affineTransformValues: [Int : CGAffineTransform] = {
        var values = [
            #line : CGAffineTransform.identity,
            #line : CGAffineTransform(),
            #line : CGAffineTransform(translationX: 2.0, y: 2.0),
            #line : CGAffineTransform(scaleX: 2.0, y: 2.0),
            #line : CGAffineTransform(a: 1.0, b: 2.5, c: 66.2, d: 40.2, tx: -5.5, ty: 3.7),
            #line : CGAffineTransform(a: -55.66, b: 22.7, c: 1.5, d: 0.0, tx: -22, ty: -33),
            #line : CGAffineTransform(a: 4.5, b: 1.1, c: 0.025, d: 0.077, tx: -0.55, ty: 33.2),
            #line : CGAffineTransform(a: 7.0, b: -2.3, c: 6.7, d: 0.25, tx: 0.556, ty: 0.99),
            #line : CGAffineTransform(a: 0.498, b: -0.284, c: -0.742, d: 0.3248, tx: 12, ty: 44)
        ]

        if #available(macOS 10.13, iOS 11.0, watchOS 4.0, tvOS 11.0, *) {
            values[#line] = CGAffineTransform(rotationAngle: .pi / 2)
        }

        return values
    }()

    func test_CGAffineTransform_JSON() {
        for (testLine, transform) in cg_affineTransformValues {
            expectRoundTripEqualityThroughJSON(for: transform, lineNumber: testLine)
        }
    }

    func test_CGAffineTransform_Plist() {
        for (testLine, transform) in cg_affineTransformValues {
            expectRoundTripEqualityThroughPlist(for: transform, lineNumber: testLine)
        }
    }

    // MARK: - CGPoint
    lazy var cg_pointValues: [Int : CGPoint] = {
        var values = [
            #line : CGPoint.zero,
            #line : CGPoint(x: 10, y: 20)
        ]

        if #available(macOS 10.13, iOS 11.0, watchOS 4.0, tvOS 11.0, *) {
            // Limit on magnitude in JSON. See rdar://problem/12717407
            values[#line] = CGPoint(x: CGFloat.greatestFiniteMagnitude,
                                    y: CGFloat.greatestFiniteMagnitude)
        }

        return values
    }()

    func test_CGPoint_JSON() {
        for (testLine, point) in cg_pointValues {
            expectRoundTripEqualityThroughJSON(for: point, lineNumber: testLine)
        }
    }

    func test_CGPoint_Plist() {
        for (testLine, point) in cg_pointValues {
            expectRoundTripEqualityThroughPlist(for: point, lineNumber: testLine)
        }
    }

    // MARK: - CGSize
    lazy var cg_sizeValues: [Int : CGSize] = {
        var values = [
            #line : CGSize.zero,
            #line : CGSize(width: 30, height: 40)
        ]

        if #available(macOS 10.13, iOS 11.0, watchOS 4.0, tvOS 11.0, *) {
            // Limit on magnitude in JSON. See rdar://problem/12717407
            values[#line] = CGSize(width: CGFloat.greatestFiniteMagnitude,
                                   height: CGFloat.greatestFiniteMagnitude)
        }

        return values
    }()

    func test_CGSize_JSON() {
        for (testLine, size) in cg_sizeValues {
            expectRoundTripEqualityThroughJSON(for: size, lineNumber: testLine)
        }
    }

    func test_CGSize_Plist() {
        for (testLine, size) in cg_sizeValues {
            expectRoundTripEqualityThroughPlist(for: size, lineNumber: testLine)
        }
    }

    // MARK: - CGRect
    lazy var cg_rectValues: [Int : CGRect] = {
        var values = [
            #line : CGRect.zero,
            #line : CGRect.null,
            #line : CGRect(x: 10, y: 20, width: 30, height: 40)
        ]

        if #available(macOS 10.13, iOS 11.0, watchOS 4.0, tvOS 11.0, *) {
            // Limit on magnitude in JSON. See rdar://problem/12717407
            values[#line] = CGRect.infinite
        }

        return values
    }()

    func test_CGRect_JSON() {
        for (testLine, rect) in cg_rectValues {
            expectRoundTripEqualityThroughJSON(for: rect, lineNumber: testLine)
        }
    }

    func test_CGRect_Plist() {
        for (testLine, rect) in cg_rectValues {
            expectRoundTripEqualityThroughPlist(for: rect, lineNumber: testLine)
        }
    }

    // MARK: - CGVector
    lazy var cg_vectorValues: [Int : CGVector] = {
        var values = [
            #line : CGVector.zero,
            #line : CGVector(dx: 0.0, dy: -9.81)
        ]

        if #available(macOS 10.13, iOS 11.0, watchOS 4.0, tvOS 11.0, *) {
            // Limit on magnitude in JSON. See rdar://problem/12717407
            values[#line] = CGVector(dx: CGFloat.greatestFiniteMagnitude,
                                     dy: CGFloat.greatestFiniteMagnitude)
        }

        return values
    }()

    func test_CGVector_JSON() {
        for (testLine, vector) in cg_vectorValues {
            expectRoundTripEqualityThroughJSON(for: vector, lineNumber: testLine)
        }
    }

    func test_CGVector_Plist() {
        for (testLine, vector) in cg_vectorValues {
            expectRoundTripEqualityThroughPlist(for: vector, lineNumber: testLine)
        }
    }

    // MARK: - ClosedRange
    func test_ClosedRange_JSON() {
        let value = 0...Int.max
        let decoded = performEncodeAndDecode(of: value, encode: { try JSONEncoder().encode($0) }, decode: { try JSONDecoder().decode($0, from: $1)  }, lineNumber: #line)
        expectEqual(value.upperBound, decoded.upperBound, "\(#file):\(#line): Decoded ClosedRange upperBound <\(debugDescription(decoded))> not equal to original <\(debugDescription(value))>")
        expectEqual(value.lowerBound, decoded.lowerBound, "\(#file):\(#line): Decoded ClosedRange lowerBound <\(debugDescription(decoded))> not equal to original <\(debugDescription(value))>")
    }

    func test_ClosedRange_Plist() {
        let value = 0...Int.max
        let decoded = performEncodeAndDecode(of: value, encode: { try PropertyListEncoder().encode($0) }, decode: { try PropertyListDecoder().decode($0, from: $1)  }, lineNumber: #line)
        expectEqual(value.upperBound, decoded.upperBound, "\(#file):\(#line): Decoded ClosedRange upperBound <\(debugDescription(decoded))> not equal to original <\(debugDescription(value))>")
        expectEqual(value.lowerBound, decoded.lowerBound, "\(#file):\(#line): Decoded ClosedRange lowerBound <\(debugDescription(decoded))> not equal to original <\(debugDescription(value))>")
    }

    // MARK: - ContiguousArray
    lazy var contiguousArrayValues: [Int : ContiguousArray<String>] = [
        #line : [],
        #line : ["foo"],
        #line : ["foo", "bar"],
        #line : ["foo", "bar", "baz"],
    ]

    func test_ContiguousArray_JSON() {
        for (testLine, contiguousArray) in contiguousArrayValues {
            expectRoundTripEqualityThroughJSON(for: contiguousArray, lineNumber: testLine)
        }
    }

    func test_ContiguousArray_Plist() {
        for (testLine, contiguousArray) in contiguousArrayValues {
            expectRoundTripEqualityThroughPlist(for: contiguousArray, lineNumber: testLine)
        }
    }

    // MARK: - DateComponents
    lazy var dateComponents: Set<Calendar.Component> = [
        .era, .year, .month, .day, .hour, .minute, .second, .nanosecond,
        .weekday, .weekdayOrdinal, .quarter, .weekOfMonth, .weekOfYear,
        .yearForWeekOfYear, .timeZone, .calendar
    ]

    func test_DateComponents_JSON() {
        let calendar = Calendar(identifier: .gregorian)
        let components = calendar.dateComponents(dateComponents, from: Date())
        expectRoundTripEqualityThroughJSON(for: components, lineNumber: #line - 1)
    }

    func test_DateComponents_Plist() {
        let calendar = Calendar(identifier: .gregorian)
        let components = calendar.dateComponents(dateComponents, from: Date())
        expectRoundTripEqualityThroughPlist(for: components, lineNumber: #line - 1)
    }

    // MARK: - DateInterval
    @available(macOS 10.12, iOS 10.0, watchOS 3.0, tvOS 10.0, *)
    static let dateIntervalValues: [Int : DateInterval] = [
        #line : DateInterval(),
        #line : DateInterval(start: Date.distantPast, end: Date()),
        #line : DateInterval(start: Date(), end: Date.distantFuture),
        #line : DateInterval(start: Date.distantPast, end: Date.distantFuture)
    ]

    @available(macOS 10.12, iOS 10.0, watchOS 3.0, tvOS 10.0, *)
    func test_DateInterval_JSON() {
        for (testLine, interval) in Self.dateIntervalValues {
            expectRoundTripEqualityThroughJSON(for: interval, lineNumber: testLine)
        }
    }

    @available(macOS 10.12, iOS 10.0, watchOS 3.0, tvOS 10.0, *)
    func test_DateInterval_Plist() {
        for (testLine, interval) in Self.dateIntervalValues {
            expectRoundTripEqualityThroughPlist(for: interval, lineNumber: testLine)
        }
    }

    // MARK: - Decimal
    lazy var decimalValues: [Int : Decimal] = [
        #line : Decimal.leastFiniteMagnitude,
        #line : Decimal.greatestFiniteMagnitude,
        #line : Decimal.leastNormalMagnitude,
        #line : Decimal.leastNonzeroMagnitude,
        #line : Decimal(),

        // See 33996620 for re-enabling this test.
        // #line : Decimal.pi,
    ]

    func test_Decimal_JSON() {
        for (testLine, decimal) in decimalValues {
            // Decimal encodes as a number in JSON and cannot be encoded at the top level.
            expectRoundTripEqualityThroughJSON(for: TopLevelWrapper(decimal), lineNumber: testLine)
        }
    }

    func test_Decimal_Plist() {
        for (testLine, decimal) in decimalValues {
            expectRoundTripEqualityThroughPlist(for: decimal, lineNumber: testLine)
        }
    }

    @available(SwiftStdlib 5.6, *)
    func test_Dictionary_JSON() {
        enum X: String, Codable { case a, b }
        enum Y: String, Codable, CodingKeyRepresentable { case a, b }
        enum Z: Codable, CodingKeyRepresentable {
            case a
            case b
            init?<T: CodingKey>(codingKey: T) {
                switch codingKey.stringValue {
                case "α":
                    self = .a
                case "β":
                    self = .b
                default:
                    return nil
                }
            }

            var codingKey: CodingKey {
                GenericCodingKey(stringValue: encoded)
            }

            var encoded: String {
                switch self {
                case .a: return "α"
                case .b: return "β"
                }
            }
        }
        enum S: Character, Codable, CodingKeyRepresentable {
            case a = "a"
            case b = "b"

            init?<T: CodingKey>(codingKey: T) {
                guard codingKey.stringValue.count == 1 else { return nil }
                self.init(rawValue: codingKey.stringValue.first!)
            }

            var codingKey: CodingKey {
                GenericCodingKey(stringValue: "\(self.rawValue)")
            }
        }

        enum U: Int, Codable { case a = 0, b}
        enum V: Int, Codable, CodingKeyRepresentable { case a = 0, b }
        enum W: Codable, CodingKeyRepresentable {
            case a
            case b
            init?<T: CodingKey>(codingKey: T) {
                guard let intValue = codingKey.intValue else { return nil }
                switch intValue {
                case 42:
                    self = .a
                case 64:
                    self = .b
                default:
                    return nil
                }
            }
            var codingKey: CodingKey {
                GenericCodingKey(intValue: self.encoded)
            }
            var encoded: Int {
                switch self {
                case .a: return 42
                case .b: return 64
                }
            }
        }

        let uuid = UUID(uuidString: "E621E1F8-C36C-495A-93FC-0C247A3E6E5F")!
        let uuidWrapper = UUIDCodingWrapper(uuid)

        expectRoundTripEqualityThroughJSON(for: [X.a: true],             expectedJSON: #"["a",true]"#,                                    lineNumber: #line)
        expectRoundTripEqualityThroughJSON(for: [Y.a: true, Y.b: false], expectedJSON: #"{"a":true,"b":false}"#,                          lineNumber: #line)
        expectRoundTripEqualityThroughJSON(for: [Z.a: true, Z.b: false], expectedJSON: #"{"α":true,"β":false}"#,                          lineNumber: #line)
        expectRoundTripEqualityThroughJSON(for: [S.a: true, S.b: false], expectedJSON: #"{"a":true,"b":false}"#,                          lineNumber: #line)
        expectRoundTripEqualityThroughJSON(for: [uuidWrapper: true],     expectedJSON: #"{"E621E1F8-C36C-495A-93FC-0C247A3E6E5F":true}"#, lineNumber: #line)
        expectRoundTripEqualityThroughJSON(for: [uuid: true],            expectedJSON: #"["E621E1F8-C36C-495A-93FC-0C247A3E6E5F",true]"#, lineNumber: #line)
        expectRoundTripEqualityThroughJSON(for: [U.a: true],             expectedJSON: #"[0,true]"#,                                      lineNumber: #line)
        expectRoundTripEqualityThroughJSON(for: [V.a: true, V.b: false], expectedJSON: #"{"0":true,"1":false}"#,                          lineNumber: #line)
        expectRoundTripEqualityThroughJSON(for: [W.a: true, W.b: false], expectedJSON: #"{"42":true,"64":false}"#,                        lineNumber: #line)
    }


    // MARK: - IndexPath
    lazy var indexPathValues: [Int : IndexPath] = [
        #line : IndexPath(), // empty
        #line : IndexPath(index: 0), // single
        #line : IndexPath(indexes: [1, 2]), // pair
        #line : IndexPath(indexes: [3, 4, 5, 6, 7, 8]), // array
    ]

    func test_IndexPath_JSON() {
        for (testLine, indexPath) in indexPathValues {
            expectRoundTripEqualityThroughJSON(for: indexPath, lineNumber: testLine)
        }
    }

    func test_IndexPath_Plist() {
        for (testLine, indexPath) in indexPathValues {
            expectRoundTripEqualityThroughPlist(for: indexPath, lineNumber: testLine)
        }
    }

    // MARK: - IndexSet
    lazy var indexSetValues: [Int : IndexSet] = [
        #line : IndexSet(),
        #line : IndexSet(integer: 42),
    ]
    lazy var indexSetMaxValues: [Int : IndexSet] = [
        #line : IndexSet(integersIn: 0 ..< Int.max)
    ]

    func test_IndexSet_JSON() {
        for (testLine, indexSet) in indexSetValues {
            expectRoundTripEqualityThroughJSON(for: indexSet, lineNumber: testLine)
        }
        if #available(macOS 10.10, iOS 8, *) {
            // Mac OS X 10.9 and iOS 7 weren't able to round-trip Int.max in JSON.
            for (testLine, indexSet) in indexSetMaxValues {
                expectRoundTripEqualityThroughJSON(for: indexSet, lineNumber: testLine)
            }
        }
    }

    func test_IndexSet_Plist() {
        for (testLine, indexSet) in indexSetValues {
            expectRoundTripEqualityThroughPlist(for: indexSet, lineNumber: testLine)
        }
        for (testLine, indexSet) in indexSetMaxValues {
            expectRoundTripEqualityThroughPlist(for: indexSet, lineNumber: testLine)
        }
    }

    // MARK: - Locale
    lazy var localeValues: [Int : Locale] = [
        #line : Locale(identifier: ""),
        #line : Locale(identifier: "en"),
        #line : Locale(identifier: "en_US"),
        #line : Locale(identifier: "en_US_POSIX"),
        #line : Locale(identifier: "uk"),
        #line : Locale(identifier: "fr_FR"),
        #line : Locale(identifier: "fr_BE"),
        #line : Locale(identifier: "zh-Hant-HK")
    ]

    func test_Locale_JSON() {
        for (testLine, locale) in localeValues {
            expectRoundTripEqualityThroughJSON(for: locale, lineNumber: testLine)
        }
    }

    func test_Locale_Plist() {
        for (testLine, locale) in localeValues {
            expectRoundTripEqualityThroughPlist(for: locale, lineNumber: testLine)
        }
    }

    // MARK: - Measurement
    @available(macOS 10.12, iOS 10.0, watchOS 3.0, tvOS 10.0, *)
    static let unitValues: [Int : Dimension] = [
        #line : UnitAcceleration.metersPerSecondSquared,
        #line : UnitMass.kilograms,
        #line : UnitLength.miles
    ]

    @available(macOS 10.12, iOS 10.0, watchOS 3.0, tvOS 10.0, *)
    func test_Measurement_JSON() {
        for (testLine, unit) in Self.unitValues {
            // FIXME: <rdar://problem/49026133>
            // Terminating due to uncaught exception NSInvalidArgumentException:
            // *** You must override baseUnit in your class NSDimension to define its base unit.
            expectCrashLater()
            expectRoundTripEqualityThroughJSON(for: Measurement(value: 42, unit: unit), lineNumber: testLine)
        }
    }

    @available(macOS 10.12, iOS 10.0, watchOS 3.0, tvOS 10.0, *)
    func test_Measurement_Plist() {
        for (testLine, unit) in Self.unitValues {
            // FIXME: <rdar://problem/49026133>
            // Terminating due to uncaught exception NSInvalidArgumentException:
            // *** You must override baseUnit in your class NSDimension to define its base unit.
            expectCrashLater()
            expectRoundTripEqualityThroughPlist(for: Measurement(value: 42, unit: unit), lineNumber: testLine)
        }
    }

    // MARK: - Never
    @available(SwiftStdlib 5.9, *)
    func test_Never() {
        struct Nope: Codable {
            var no: Never
        }
      
        do {
            let neverJSON = Data(#"{"no":"never"}"#.utf8)
            _ = try JSONDecoder().decode(Nope.self, from: neverJSON)
            fatalError("Incorrectly decoded `Never` instance.")
        } catch {}
    }

    // MARK: - NSRange
    lazy var nsrangeValues: [Int : NSRange] = [
        #line : NSRange(),
        #line : NSRange(location: 5, length: 20),
    ]
    lazy var nsrangeMaxValues: [Int : NSRange] = [
        #line : NSRange(location: 0, length: Int.max),
        #line : NSRange(location: NSNotFound, length: 0),
    ]

    func test_NSRange_JSON() {
        for (testLine, range) in nsrangeValues {
            expectRoundTripEqualityThroughJSON(for: range, lineNumber: testLine)
        }
        if #available(macOS 10.10, iOS 8, *) {
            // Mac OS X 10.9 and iOS 7 weren't able to round-trip Int.max in JSON.
            for (testLine, range) in nsrangeMaxValues {
                expectRoundTripEqualityThroughJSON(for: range, lineNumber: testLine)
            }
        }
    }

    func test_NSRange_Plist() {
        for (testLine, range) in nsrangeValues {
            expectRoundTripEqualityThroughPlist(for: range, lineNumber: testLine)
        }
        for (testLine, range) in nsrangeMaxValues {
            expectRoundTripEqualityThroughPlist(for: range, lineNumber: testLine)
        }
    }

    // MARK: - PartialRangeFrom
    func test_PartialRangeFrom_JSON() {
        let value = 0...
        let decoded = performEncodeAndDecode(of: value, encode: { try JSONEncoder().encode($0) }, decode: { try JSONDecoder().decode($0, from: $1)  }, lineNumber: #line)
        expectEqual(value.lowerBound, decoded.lowerBound, "\(#file):\(#line): Decoded PartialRangeFrom <\(debugDescription(decoded))> not equal to original <\(debugDescription(value))>")
    }

    func test_PartialRangeFrom_Plist() {
        let value = 0...
        let decoded = performEncodeAndDecode(of: value, encode: { try PropertyListEncoder().encode($0) }, decode: { try PropertyListDecoder().decode($0, from: $1)  }, lineNumber: #line)
        expectEqual(value.lowerBound, decoded.lowerBound, "\(#file):\(#line): Decoded PartialRangeFrom <\(debugDescription(decoded))> not equal to original <\(debugDescription(value))>")
    }

    // MARK: - PartialRangeThrough
    func test_PartialRangeThrough_JSON() {
        let value = ...Int.max
        let decoded = performEncodeAndDecode(of: value, encode: { try JSONEncoder().encode($0) }, decode: { try JSONDecoder().decode($0, from: $1)  }, lineNumber: #line)
        expectEqual(value.upperBound, decoded.upperBound, "\(#file):\(#line): Decoded PartialRangeThrough <\(debugDescription(decoded))> not equal to original <\(debugDescription(value))>")
    }

    func test_PartialRangeThrough_Plist() {
        let value = ...Int.max
        let decoded = performEncodeAndDecode(of: value, encode: { try PropertyListEncoder().encode($0) }, decode: { try PropertyListDecoder().decode($0, from: $1)  }, lineNumber: #line)
        expectEqual(value.upperBound, decoded.upperBound, "\(#file):\(#line): Decoded PartialRangeThrough <\(debugDescription(decoded))> not equal to original <\(debugDescription(value))>")
    }

    // MARK: - PartialRangeUpTo
    func test_PartialRangeUpTo_JSON() {
        let value = ..<Int.max
        let decoded = performEncodeAndDecode(of: value, encode: { try JSONEncoder().encode($0) }, decode: { try JSONDecoder().decode($0, from: $1)  }, lineNumber: #line)
        expectEqual(value.upperBound, decoded.upperBound, "\(#file):\(#line): Decoded PartialRangeUpTo <\(debugDescription(decoded))> not equal to original <\(debugDescription(value))>")
    }

    func test_PartialRangeUpTo_Plist() {
        let value = ..<Int.max
        let decoded = performEncodeAndDecode(of: value, encode: { try PropertyListEncoder().encode($0) }, decode: { try PropertyListDecoder().decode($0, from: $1)  }, lineNumber: #line)
        expectEqual(value.upperBound, decoded.upperBound, "\(#file):\(#line): Decoded PartialRangeUpTo <\(debugDescription(decoded))> not equal to original <\(debugDescription(value))>")
    }

    // MARK: - PersonNameComponents
    @available(macOS 10.11, iOS 9.0, watchOS 2.0, tvOS 9.0, *)
    static let personNameComponentsValues: [Int : PersonNameComponents] = [
        #line : makePersonNameComponents(givenName: "John", familyName: "Appleseed"),
        #line : makePersonNameComponents(givenName: "John", familyName: "Appleseed", nickname: "Johnny"),
        #line : makePersonNameComponents(namePrefix: "Dr.", givenName: "Jane", middleName: "A.", familyName: "Appleseed", nameSuffix: "Esq.", nickname: "Janie")
    ]

    @available(macOS 10.11, iOS 9.0, watchOS 2.0, tvOS 9.0, *)
    func test_PersonNameComponents_JSON() {
        for (testLine, components) in Self.personNameComponentsValues {
            expectRoundTripEqualityThroughJSON(for: components, lineNumber: testLine)
        }
    }

    @available(macOS 10.11, iOS 9.0, watchOS 2.0, tvOS 9.0, *)
    func test_PersonNameComponents_Plist() {
        for (testLine, components) in Self.personNameComponentsValues {
            expectRoundTripEqualityThroughPlist(for: components, lineNumber: testLine)
        }
    }

    // MARK: - Range
    func test_Range_JSON() {
        let value = 0..<Int.max
        let decoded = performEncodeAndDecode(of: value, encode: { try JSONEncoder().encode($0) }, decode: { try JSONDecoder().decode($0, from: $1)  }, lineNumber: #line)
        expectEqual(value.upperBound, decoded.upperBound, "\(#file):\(#line): Decoded Range upperBound <\(debugDescription(decoded))> not equal to original <\(debugDescription(value))>")
        expectEqual(value.lowerBound, decoded.lowerBound, "\(#file):\(#line): Decoded Range lowerBound<\(debugDescription(decoded))> not equal to original <\(debugDescription(value))>")
    }

    func test_Range_Plist() {
        let value = 0..<Int.max
        let decoded = performEncodeAndDecode(of: value, encode: { try PropertyListEncoder().encode($0) }, decode: { try PropertyListDecoder().decode($0, from: $1)  }, lineNumber: #line)
        expectEqual(value.upperBound, decoded.upperBound, "\(#file):\(#line): Decoded Range upperBound<\(debugDescription(decoded))> not equal to original <\(debugDescription(value))>")
        expectEqual(value.lowerBound, decoded.lowerBound, "\(#file):\(#line): Decoded Range lowerBound<\(debugDescription(decoded))> not equal to original <\(debugDescription(value))>")
    }

    // MARK: - TimeZone
    lazy var timeZoneValues: [Int : TimeZone] = [
        #line : TimeZone(identifier: "America/Los_Angeles")!,
        #line : TimeZone(identifier: "UTC")!,
        #line : TimeZone.current
    ]

    func test_TimeZone_JSON() {
        for (testLine, timeZone) in timeZoneValues {
            expectRoundTripEqualityThroughJSON(for: timeZone, lineNumber: testLine)
        }
    }

    func test_TimeZone_Plist() {
        for (testLine, timeZone) in timeZoneValues {
            expectRoundTripEqualityThroughPlist(for: timeZone, lineNumber: testLine)
        }
    }

    // MARK: - URL
    lazy var urlValues: [Int : URL] = {
        var values: [Int : URL] = [
            #line : URL(fileURLWithPath: NSTemporaryDirectory()),
            #line : URL(fileURLWithPath: "/"),
            #line : URL(string: "http://swift.org")!,
            #line : URL(string: "documentation", relativeTo: URL(string: "http://swift.org")!)!
        ]

        if #available(macOS 10.11, iOS 9.0, watchOS 2.0, tvOS 9.0, *) {
            values[#line] = URL(fileURLWithPath: "bin/sh", relativeTo: URL(fileURLWithPath: "/"))
        }

        return values
    }()

    func test_URL_JSON() {
        for (testLine, url) in urlValues {
            // URLs encode as single strings in JSON. They lose their baseURL this way.
            // For relative URLs, we don't expect them to be equal to the original.
            if url.baseURL == nil {
                // This is an absolute URL; we can expect equality.
                expectRoundTripEqualityThroughJSON(for: TopLevelWrapper(url), lineNumber: testLine)
            } else {
                // This is a relative URL. Make it absolute first.
                let absoluteURL = URL(string: url.absoluteString)!
                expectRoundTripEqualityThroughJSON(for: TopLevelWrapper(absoluteURL), lineNumber: testLine)
            }
        }
    }

    func test_URL_Plist() {
        for (testLine, url) in urlValues {
            expectRoundTripEqualityThroughPlist(for: url, lineNumber: testLine)
        }
    }

    // MARK: - URLComponents
    lazy var urlComponentsValues: [Int : URLComponents] = [
        #line : URLComponents(),

        #line : URLComponents(string: "http://swift.org")!,
        #line : URLComponents(string: "http://swift.org:80")!,
        #line : URLComponents(string: "https://www.mywebsite.org/api/v42/something.php#param1=hi&param2=hello")!,
        #line : URLComponents(string: "ftp://johnny:apples@myftpserver.org:4242/some/path")!,

        #line : URLComponents(url: URL(string: "http://swift.org")!, resolvingAgainstBaseURL: false)!,
        #line : URLComponents(url: URL(string: "http://swift.org:80")!, resolvingAgainstBaseURL: false)!,
        #line : URLComponents(url: URL(string: "https://www.mywebsite.org/api/v42/something.php#param1=hi&param2=hello")!, resolvingAgainstBaseURL: false)!,
        #line : URLComponents(url: URL(string: "ftp://johnny:apples@myftpserver.org:4242/some/path")!, resolvingAgainstBaseURL: false)!,
        #line : URLComponents(url: URL(fileURLWithPath: NSTemporaryDirectory()), resolvingAgainstBaseURL: false)!,
        #line : URLComponents(url: URL(fileURLWithPath: "/"), resolvingAgainstBaseURL: false)!,
        #line : URLComponents(url: URL(string: "documentation", relativeTo: URL(string: "http://swift.org")!)!, resolvingAgainstBaseURL: false)!,

        #line : URLComponents(url: URL(string: "http://swift.org")!, resolvingAgainstBaseURL: true)!,
        #line : URLComponents(url: URL(string: "http://swift.org:80")!, resolvingAgainstBaseURL: true)!,
        #line : URLComponents(url: URL(string: "https://www.mywebsite.org/api/v42/something.php#param1=hi&param2=hello")!, resolvingAgainstBaseURL: true)!,
        #line : URLComponents(url: URL(string: "ftp://johnny:apples@myftpserver.org:4242/some/path")!, resolvingAgainstBaseURL: true)!,
        #line : URLComponents(url: URL(fileURLWithPath: NSTemporaryDirectory()), resolvingAgainstBaseURL: true)!,
        #line : URLComponents(url: URL(fileURLWithPath: "/"), resolvingAgainstBaseURL: true)!,
        #line : URLComponents(url: URL(string: "documentation", relativeTo: URL(string: "http://swift.org")!)!, resolvingAgainstBaseURL: true)!,

        #line : {
            var components = URLComponents()
            components.scheme = "https"
            return components
        }(),

        #line : {
            var components = URLComponents()
            components.user = "johnny"
            return components
        }(),

        #line : {
            var components = URLComponents()
            components.password = "apples"
            return components
        }(),

        #line : {
            var components = URLComponents()
            components.host = "0.0.0.0"
            return components
        }(),

        #line : {
            var components = URLComponents()
            components.port = 8080
            return components
        }(),

        #line : {
            var components = URLComponents()
            components.path = ".."
            return components
        }(),

        #line : {
            var components = URLComponents()
            components.query = "param1=hi&param2=there"
            return components
        }(),

        #line : {
            var components = URLComponents()
            components.fragment = "anchor"
            return components
        }(),

        #line : {
            var components = URLComponents()
            components.scheme = "ftp"
            components.user = "johnny"
            components.password = "apples"
            components.host = "0.0.0.0"
            components.port = 4242
            components.path = "/some/file"
            components.query = "utf8=✅"
            components.fragment = "anchor"
            return components
        }()
    ]

    func test_URLComponents_JSON() {
        for (testLine, components) in urlComponentsValues {
            expectRoundTripEqualityThroughJSON(for: components, lineNumber: testLine)
        }
    }

    func test_URLComponents_Plist() {
        for (testLine, components) in urlComponentsValues {
            expectRoundTripEqualityThroughPlist(for: components, lineNumber: testLine)
        }
    }

    // MARK: - UUID
    lazy var uuidValues: [Int : UUID] = [
        #line : UUID(),
        #line : UUID(uuidString: "E621E1F8-C36C-495A-93FC-0C247A3E6E5F")!,
        #line : UUID(uuidString: "e621e1f8-c36c-495a-93fc-0c247a3e6e5f")!,
        #line : UUID(uuid: uuid_t(0xe6,0x21,0xe1,0xf8,0xc3,0x6c,0x49,0x5a,0x93,0xfc,0x0c,0x24,0x7a,0x3e,0x6e,0x5f))
    ]

    func test_UUID_JSON() {
        for (testLine, uuid) in uuidValues {
            // We have to wrap the UUID since we cannot have a top-level string.
            expectRoundTripEqualityThroughJSON(for: UUIDCodingWrapper(uuid), lineNumber: testLine)
        }
    }

    func test_UUID_Plist() {
        for (testLine, uuid) in uuidValues {
            // We have to wrap the UUID since we cannot have a top-level string.
            expectRoundTripEqualityThroughPlist(for: UUIDCodingWrapper(uuid), lineNumber: testLine)
        }
    }
}

// MARK: - Helper Types

struct GenericCodingKey: CodingKey {
    var stringValue: String
    var intValue: Int?

    init(stringValue: String) {
        self.stringValue = stringValue
    }

    init(intValue: Int) {
        self.stringValue = "\(intValue)"
        self.intValue = intValue
    }
}

struct TopLevelWrapper<T> : Codable, Equatable where T : Codable, T : Equatable {
    let value: T

    init(_ value: T) {
        self.value = value
    }

    static func ==(_ lhs: TopLevelWrapper<T>, _ rhs: TopLevelWrapper<T>) -> Bool {
        return lhs.value == rhs.value
    }
}

// MARK: - Tests

#if !FOUNDATION_XCTEST
var tests = [
    "test_Calendar_JSON" : TestCodable.test_Calendar_JSON,
    "test_Calendar_Plist" : TestCodable.test_Calendar_Plist,
    "test_CharacterSet_JSON" : TestCodable.test_CharacterSet_JSON,
    "test_CharacterSet_Plist" : TestCodable.test_CharacterSet_Plist,
    "test_CGAffineTransform_JSON" : TestCodable.test_CGAffineTransform_JSON,
    "test_CGAffineTransform_Plist" : TestCodable.test_CGAffineTransform_Plist,
    "test_CGPoint_JSON" : TestCodable.test_CGPoint_JSON,
    "test_CGPoint_Plist" : TestCodable.test_CGPoint_Plist,
    "test_CGSize_JSON" : TestCodable.test_CGSize_JSON,
    "test_CGSize_Plist" : TestCodable.test_CGSize_Plist,
    "test_CGRect_JSON" : TestCodable.test_CGRect_JSON,
    "test_CGRect_Plist" : TestCodable.test_CGRect_Plist,
    "test_CGVector_JSON" : TestCodable.test_CGVector_JSON,
    "test_CGVector_Plist" : TestCodable.test_CGVector_Plist,
    "test_ClosedRange_JSON" : TestCodable.test_ClosedRange_JSON,
    "test_ClosedRange_Plist" : TestCodable.test_ClosedRange_Plist,
    "test_ContiguousArray_JSON" : TestCodable.test_ContiguousArray_JSON,
    "test_ContiguousArray_Plist" : TestCodable.test_ContiguousArray_Plist,
    "test_DateComponents_JSON" : TestCodable.test_DateComponents_JSON,
    "test_DateComponents_Plist" : TestCodable.test_DateComponents_Plist,
    "test_Decimal_JSON" : TestCodable.test_Decimal_JSON,
    "test_Decimal_Plist" : TestCodable.test_Decimal_Plist,
    "test_IndexPath_JSON" : TestCodable.test_IndexPath_JSON,
    "test_IndexPath_Plist" : TestCodable.test_IndexPath_Plist,
    "test_IndexSet_JSON" : TestCodable.test_IndexSet_JSON,
    "test_IndexSet_Plist" : TestCodable.test_IndexSet_Plist,
    "test_Locale_JSON" : TestCodable.test_Locale_JSON,
    "test_Locale_Plist" : TestCodable.test_Locale_Plist,
    "test_NSRange_JSON" : TestCodable.test_NSRange_JSON,
    "test_NSRange_Plist" : TestCodable.test_NSRange_Plist,
    "test_PartialRangeFrom_JSON" : TestCodable.test_PartialRangeFrom_JSON,
    "test_PartialRangeFrom_Plist" : TestCodable.test_PartialRangeFrom_Plist,
    "test_PartialRangeThrough_JSON" : TestCodable.test_PartialRangeThrough_JSON,
    "test_PartialRangeThrough_Plist" : TestCodable.test_PartialRangeThrough_Plist,
    "test_PartialRangeUpTo_JSON" : TestCodable.test_PartialRangeUpTo_JSON,
    "test_PartialRangeUpTo_Plist" : TestCodable.test_PartialRangeUpTo_Plist,
    "test_Range_JSON" : TestCodable.test_Range_JSON,
    "test_Range_Plist" : TestCodable.test_Range_Plist,
    "test_TimeZone_JSON" : TestCodable.test_TimeZone_JSON,
    "test_TimeZone_Plist" : TestCodable.test_TimeZone_Plist,
    "test_URL_JSON" : TestCodable.test_URL_JSON,
    "test_URL_Plist" : TestCodable.test_URL_Plist,
    "test_UUID_JSON" : TestCodable.test_UUID_JSON,
    "test_UUID_Plist" : TestCodable.test_UUID_Plist,
]

#if os(macOS)
    tests["test_AffineTransform_JSON"] = TestCodable.test_AffineTransform_JSON
    tests["test_AffineTransform_Plist"] = TestCodable.test_AffineTransform_Plist
#endif

if #available(macOS 10.11, iOS 9.0, watchOS 2.0, tvOS 9.0, *) {
    tests["test_PersonNameComponents_JSON"] = TestCodable.test_PersonNameComponents_JSON
    tests["test_PersonNameComponents_Plist"] = TestCodable.test_PersonNameComponents_Plist
}

if #available(macOS 10.12, iOS 10.0, watchOS 3.0, tvOS 10.0, *) {
    tests["test_DateInterval_JSON"] = TestCodable.test_DateInterval_JSON
    tests["test_DateInterval_Plist"] = TestCodable.test_DateInterval_Plist
    tests["test_Measurement_JSON"] = TestCodable.test_Measurement_JSON
    tests["test_Measurement_Plist"] = TestCodable.test_Measurement_Plist
}

if #available(macOS 10.12, iOS 10.0, watchOS 3.0, tvOS 10.0, *) {
    tests["test_URLComponents_JSON"] = TestCodable.test_URLComponents_JSON
    tests["test_URLComponents_Plist"] = TestCodable.test_URLComponents_Plist
}

if #available(SwiftStdlib 5.6, *) {
    tests["test_Dictionary_JSON"] = TestCodable.test_Dictionary_JSON
}

if #available(SwiftStdlib 5.9, *) {
    tests["test_Never"] = TestCodable.test_Never
}

var CodableTests = TestSuite("TestCodable")
for (name, test) in tests {
    CodableTests.test(name) { test(TestCodable())() }
}

runAllTests()
#endif
