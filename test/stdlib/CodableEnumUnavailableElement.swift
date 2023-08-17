// RUN: %target-run-simple-swift
// RUN: %target-run-simple-swift(-Xfrontend -unavailable-decl-optimization=complete)
// REQUIRES: executable_test
// REQUIRES: objc_interop

import StdlibUnittest
import Foundation

var CodableEnumUnavailableElementTestSuite = TestSuite("CodableEnumUnavailableElement")

@available(*, unavailable)
struct UnavailableStruct: Codable {}

enum EnumWithUnavailableCase: Codable {
  case a
  @available(*, unavailable)
  case b(UnavailableStruct)
}

func decodeEnum(from json: String) throws -> EnumWithUnavailableCase {
  let data = json.data(using: .utf8)!
  return try JSONDecoder().decode(EnumWithUnavailableCase.self, from: data)
}

CodableEnumUnavailableElementTestSuite.test("decode_available_case") {
  do {
    let decoded = try decodeEnum(from: #"{"a":{}}"#)
    switch decoded {
    case .a: break
    default: assertionFailure("Unexpected value \(decoded)")
    }
  } catch {
    expectUnreachable("Unexpected error \(error)")
  }
}

CodableEnumUnavailableElementTestSuite.test("decode_unavailable_case") {
  do {
    let decoded = try decodeEnum(from: #"{"b":{"_0":{}}}"#)
    expectUnreachable("Unexpectedly decoded \(decoded)")
  } catch DecodingError.dataCorrupted {
    // Expected error
  } catch {
    expectUnreachable("Unexpected error \(error)")
  }
}

runAllTests()
