// REQUIRES: swift_swift_parser, executable_test

// RUN: %target-run-simple-swift(-Xfrontend -plugin-path -Xfrontend %swift-host-lib-dir/plugins -emit-tbd -emit-tbd-path %t.tbd)

import Swift

@attached(member, names: named(RawValue), named(rawValue), named(`init`), arbitrary)
@attached(conformance)
public macro OptionSet<RawType>() =
  #externalMacro(module: "SwiftMacros", type: "OptionSetMacro")

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

let options = ShippingOptions.express
assert(options.contains(.nextDay))
assert(options.contains(.secondDay))
assert(!options.contains(.standard))

