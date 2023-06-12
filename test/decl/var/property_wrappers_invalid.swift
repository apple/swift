// RUN: %target-swift-frontend -typecheck %s -verify -verify-ignore-unknown

// FIXME: This should produce a diagnostic with a proper
// source location. Right now, we just get three useless errors:

// <unknown>:0: error: type of expression is ambiguous without a type annotation
// <unknown>:0: error: type of expression is ambiguous without a type annotation
// <unknown>:0: error: type of expression is ambiguous without a type annotation

// The actual problem is the type of the subscript declaration is wrong.

public class Store {
    @Published public var state: Any
    init() {
      self.state = 0
    }
}

@propertyWrapper public struct Published<Value> {
  public init(wrappedValue: Value) {}
  public var wrappedValue: Value {
    get { fatalError() }
    set {}
  }
  public static subscript(_enclosingInstance object: Any,
                          wrapped wrappedKeyPath: Any,
                          storage storageKeyPath: Any)
      -> Value {
    get { fatalError() }
    set {}
  }
  public struct Publisher {}
  public var projectedValue: Publisher {
    mutating get { fatalError() }
  }
}
