import Namespaces

#if RESILIENT

extension Namespace.SimpleTypealias {
  public static func test() -> Int { 42 }
}

#else

extension Namespace.Parent {
  public static func test() -> Int { 42 }
}

extension Namespace.Parent.Child {
  public static func test() -> Int { 52 }
}

extension Namespace.NestedNamespace.NestedStruct {
  public func test() -> Int { 62 }
}

#endif
