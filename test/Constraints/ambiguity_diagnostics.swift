// RUN: %target-typecheck-verify-swift -swift-version 5 -disable-availability-checking

protocol View {
}

extension View {
  func title<S>(_ title: S) -> some View where S : StringProtocol {
    EmptyView()
  }

  func title(_ titleKey: LocalizedString) -> some View {
    EmptyView()
  }
}

extension View {
  func background<T: ShapeStyle>(_: T) -> some View {
    EmptyView()
  }
}

struct EmptyView : View {}

struct Text : View {
  init(_: String) {}
}

protocol ShapeStyle {
}

struct AnyShapeStyle : ShapeStyle {}
struct AnyGradient : ShapeStyle {}

struct LocalizedString : ExpressibleByStringLiteral, ExpressibleByExtendedGraphemeClusterLiteral {
  init(extendedGraphemeClusterLiteral value: String) {}
  init(stringLiteral: String) {}
}

func test() {
  func __findValue(_: String, fallback: LocalizedString) -> LocalizedString { fatalError() }
  func __findValue<T: ExpressibleByStringLiteral>(_: String, fallback: T) -> T { fatalError() }
  func __findValue<T: ExpressibleByExtendedGraphemeClusterLiteral>(_: String, fallback: T) -> T { fatalError() }

  func ambiguitySource() -> AnyShapeStyle { fatalError() } // expected-note {{found this candidate}}
  func ambiguitySource() -> AnyGradient { fatalError() }   // expected-note {{found this candidate}}

  Text("Test")
    .title(__findValue("someKey", fallback: "<unknown>"))
    .background(ambiguitySource()) // expected-error {{ambiguous use of 'ambiguitySource()'}}
}
