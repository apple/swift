
public protocol Observable {}

public protocol Observer<Subject> {
  associatedtype Subject: Observable
}

public struct ObservationRegistrar<Subject: Observable> {
  public init() {}

  public func addObserver(_ observer: some Observer<Subject>) {}

  public func removeObserver(_ observer: some Observer<Subject>) {}

  public func beginAccess<Value>(_ keyPath: KeyPath<Subject, Value>) {}

  public func beginAccess() {}

  public func endAccess() {}

  public func register<Value>(observable: Subject, willSet: KeyPath<Subject, Value>, to: Value) {}

  public func register<Value>(observable: Subject, didSet: KeyPath<Subject, Value>) {}
}

@attached(
  member,
  names: named(_registrar), named(addObserver), named(removeObserver), named(withTransaction), named(Storage), named(_storage)
)
@attached(memberAttribute)
@attached(extension, conformances: Observable)
public macro Observable() = #externalMacro(module: "MacroDefinition", type: "ObservableMacro")

@attached(accessor)
public macro ObservableProperty() = #externalMacro(module: "MacroDefinition", type: "ObservablePropertyMacro")

@attached(peer, names: overloaded)
public macro addCompletionHandler() = #externalMacro(module: "MacroDefinition", type: "AddCompletionHandler")

@attached(peer, names: suffixed(Builder))
public macro AddClassReferencingSelf() = #externalMacro(module: "MacroDefinition", type: "AddClassReferencingSelfMacro")

@attached(peer, names: named(value))
public macro declareVarValuePeer() = #externalMacro(module: "MacroDefinition", type: "VarValueMacro")
