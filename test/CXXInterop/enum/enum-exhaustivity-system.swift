// RUN: %target-swift-frontend -typecheck %s -Xcc -isystem -Xcc %S/Inputs -verify -enable-nonfrozen-enum-exhaustivity-diagnostics

import EnumExhaustivity

func test(_ value: RegularEnum, _ exhaustiveValue: ExhaustiveEnum) {
  switch value { // expected-warning {{switch covers known cases, but 'RegularEnum' may have additional unknown values, possibly added in future versions}} expected-note {{handle unknown values using "@unknown default"}}
  case .A: break
  case .B: break
  }

  switch exhaustiveValue { // always okay
  case .A: break
  case .B: break
  }
}

func testAttributes(
  _ rete: RegularEnumTurnedExhaustive,
  _ arete: AnotherRegularEnumTurnedExhaustive,
  _ retetb: RegularEnumTurnedExhaustiveThenBackViaAPINotes,
  _ fdte: ForwardDeclaredTurnedExhaustive,
  _ fdo: ForwardDeclaredOnly
) {
  switch rete {
  case .A, .B: break
  }

  switch arete {
  case .A, .B: break
  }

  switch retetb { // expected-warning {{switch covers known cases, but 'RegularEnumTurnedExhaustiveThenBackViaAPINotes' may have additional unknown values, possibly added in future versions}} expected-note {{handle unknown values using "@unknown default"}}
  case .A, .B: break
  }

  switch fdte {
  case .A, .B: break
  }

  switch fdo {
  case .A, .B: break
  }
}

func testUnavailableCases(_ value: UnavailableCases) {
  switch value { // okay
  case .A: break
  case .B: break
  }
}
