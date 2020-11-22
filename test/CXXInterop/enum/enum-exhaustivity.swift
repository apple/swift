// RUN: %target-swift-frontend -typecheck %s -I %S/Inputs -verify -warnings-as-errors -enable-nonfrozen-enum-exhaustivity-diagnostics

// RUN: %target-swift-ide-test -source-filename %s -print-module -module-to-print EnumExhaustivity -I %S/Inputs | %FileCheck %s

// CHECK-LABEL: {{^}}enum RegularEnum : {{.+}} {
// CHECK:      case A
// CHECK-NEXT: case B
// CHECK-NEXT: {{^}$}}

// CHECK-LABEL: {{^}}@frozen enum ExhaustiveEnum : {{.+}} {
// CHECK:      case A
// CHECK-NEXT: case B
// CHECK-NEXT: {{^}$}}


import EnumExhaustivity

func test(_ value: RegularEnum, _ exhaustiveValue: ExhaustiveEnum) {
  switch value { // expected-error {{switch covers known cases, but 'RegularEnum' may have additional unknown values}} expected-note {{handle unknown values using "@unknown default"}}
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

  switch retetb { // expected-error {{switch covers known cases, but 'RegularEnumTurnedExhaustiveThenBackViaAPINotes' may have additional unknown values}} expected-note {{handle unknown values using "@unknown default"}}
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
