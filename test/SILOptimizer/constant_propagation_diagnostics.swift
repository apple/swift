// RUN: %target-swift-frontend -emit-sil -sdk %S/../SILGen/Inputs %s -o /dev/null -verify

// <rdar://problem/18213320> enum with raw values that are too big are not diagnosed
enum EnumWithTooLargeElements : UInt8 {
  case negativeOne = -1     // expected-error 2 {{negative integer '-1' overflows when stored into unsigned type 'UInt8'}}
  case one = 1
  case two = 2
  case three
  case twoHundredFiftyFour = 254
  case twoHundredFiftyFive
  case twoHundredFiftySix    // expected-error 2 {{integer literal '256' overflows when stored into 'UInt8'}}
  case tooFarByFar = 123456  // expected-error 2 {{integer literal '123456' overflows when stored into 'UInt8'}}
}
