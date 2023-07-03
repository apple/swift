// REQUIRES: swift_swift_parser
// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t -plugin-path %swift-host-lib-dir/plugins

@OptionSet<UInt8>
struct ShippingOptions {
  private enum Options: Int {
    case nextDay
    case secondDay
    case priority
    case standard
  }
}

func foo() {
  ShippingOptions.#^MEMBER_STATIC^#
}

@attached(member, names: named(RawValue), named(rawValue), named(`init`), arbitrary)
@attached(conformance)
public macro OptionSet<RawType>() =
  #externalMacro(module: "SwiftMacros", type: "OptionSetMacro")

// MEMBER_STATIC: Keyword[self]/CurrNominal:          self[#ShippingOptions.Type#]; name=self
// MEMBER_STATIC: Decl[TypeAlias]/CurrNominal:        RawValue[#UInt8#]; name=RawValue
// MEMBER_STATIC: Decl[Constructor]/CurrNominal:      init({#rawValue: ShippingOptions.RawValue#})[#ShippingOptions#]; name=init(rawValue:)
// MEMBER_STATIC: Decl[StaticVar]/CurrNominal:        nextDay[#ShippingOptions#]; name=nextDay
// MEMBER_STATIC: Decl[StaticVar]/CurrNominal:        secondDay[#ShippingOptions#]; name=secondDay
// MEMBER_STATIC: Decl[StaticVar]/CurrNominal:        priority[#ShippingOptions#]; name=priority
// MEMBER_STATIC: Decl[StaticVar]/CurrNominal:        standard[#ShippingOptions#]; name=standard
// MEMBER_STATIC: Decl[TypeAlias]/CurrNominal:        ArrayLiteralElement[#ShippingOptions#]; name=ArrayLiteralElement
// MEMBER_STATIC: Decl[TypeAlias]/CurrNominal:        Element[#ShippingOptions#]; name=Element
// MEMBER_STATIC: Decl[Constructor]/Super/IsSystem:   init()[#ShippingOptions#]; name=init()
// MEMBER_STATIC: Decl[Constructor]/Super/IsSystem:   init({#(sequence): Sequence#})[#ShippingOptions#]; name=init(:)
