
@OptionSet<UInt8>
struct ShippingOptions {
  private enum Options: Int {
    case nextDay
    case secondDay
    case priority
    case standard
  }

  static let express: ShippingOptions = [.nextDay, .secondDay]
  static let all: ShippingOptions = [.express, .priority, .standard]
}

func test(opts: ShippingOptions) {
  let _ = ShippingOptions.nextDay
}

@attached(member, names: named(RawValue), named(rawValue), named(`init`), arbitrary)
@attached(extension, conformances: OptionSet)
public macro OptionSet<RawType>() =
  #externalMacro(module: "SwiftMacros", type: "OptionSetMacro")

// REQUIRES: swift_swift_parser

// RUN: %empty-directory(%t)
// RUN: %sourcekitd-test \
// RUN:   -shell -- echo '## DIAGS ##' == \
// RUN:   -req=diags %s -- %s ==\
// RUN:   -shell -- echo '## CURSOR ##' == \
// RUN:   -req=cursor -pos=16:27 %s -- %s == \
// RUN:   -shell -- echo '## COMPLETE ##' == \
// RUN:   -req=complete -pos=16:27 %s -- %s == \
// RUN:   -shell -- echo '## COMPILE ##' == \
// RUN:   -req=compile -name test -- -c %s -module-name TestModule -o %t/out.o \
// RUN: | %FileCheck %s

// CHECK-LABEL: ## DIAGS ##
// CHECK: key.diagnostics: [
// CHECK-NEXT: ]

// CHECK-LABEL: ## CURSOR ##
// CHECK: source.lang.swift.ref.var.static

// CHECK-LABEL: ## COMPLETE ##
// CHECK: key.results: [
// CHECK:   key.description: "secondDay"

// CHECK-LABEL: ## COMPILE ##
// CHECK:      key.diagnostics: [
// CHECK-NEXT: ],
// CHECK:      key.value: 0
