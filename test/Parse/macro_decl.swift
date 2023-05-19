// RUN: %target-swift-frontend -parse -verify %s

func memberwiseInit() {} // dummy symbol
func stringify() {} // dummy symbol

extension String {
  static let bar = #stringify(4)

  #memberwiseInit

  #memberwiseInit { "abc" }

  #memberwiseInit(flavor: .chocolate)

  #memberwiseInit(flavor: .chocolate, haha: true)

  #memberwiseInit(flavor: .chocolate, haha: true) { "abc" }

  @available(macOS 999, *)
  public #memberwiseInit()

  static internal #memberwiseInit

  struct Foo {
    #memberwiseInit

    // expected-error @+1 {{expected a macro identifier for a pound literal declaration}}
    #-
  }

  // expected-error @+1 {{expected a macro identifier for a pound literal declaration}}
  #()
}

@RandomAttr #someFunc

public #someFunc

#someFunc

func test() {
  @discardableResult #someFunc

  dynamic #someFunc

  @CustomAttr
  isolated #someFunc
}
