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

public # someFunc // expected-error {{extraneous whitespace between '#' and macro name is not permitted}} {{9-10=}}

struct S {
  # someFunc // expected-error {{extraneous whitespace between '#' and macro name is not permitted}} {{4-5=}}
  #class // expected-error {{keyword 'class' cannot be used as an identifier here}} expected-note {{if this name is unavoidable, use backticks to escape it}} {{4-9=`class`}} 
  # struct Inner {} // expected-error {{expected a macro identifier for a pound literal declaration}} expected-error {{consecutive declarations on a line}}
}
