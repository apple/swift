// --------------------------------
// A Swift × Haskell stdlib flavour
// --------------------------------

/// MARK: GHC.Show

public protocol Show: ~Copyable {
  borrowing func show() -> String
}

extension CustomStringConvertible {
  public func show() -> String { return description }
}
extension Int: Show {}

public func print(_ s: borrowing some Show & ~Copyable) {
  print(s.show())
}

/// MARK: Data.Eq

public protocol Eq: ~Copyable {
  static func ==(_ a: borrowing Self, _ b: borrowing Self) -> Bool
  static func /=(_ a: borrowing Self, _ b: borrowing Self) -> Bool
}

public extension Eq where Self: ~Copyable {
  static func /=(_ a: borrowing Self, _ b: borrowing Self) -> Bool {
    return !(a == b)
  }
}

/// MARK: Result
public enum Either<Success: ~Copyable, Failure: Error>: ~Copyable {
  case success(Success)
  case failure(Failure)
}

extension Either: Copyable where Success: Copyable {}

extension Either where Failure == Swift.Error {
  public init(catching body: () throws -> Success) {
    do {
      self = .success(try body())
    } catch {
      self = .failure(error)
    }
  }
}

/// MARK: Iteration

public protocol Generator: ~Copyable {
  associatedtype Element: ~Copyable
  func next() -> Maybe<Element>
}

/// Eager assertion function, to avoid autoclosures.
public func check(_ result: Bool, _ string: String? = nil,
                  _ file: String = #file, _ line: Int = #line) {
  if result { return }
  var msg = "assertion failure (\(file):\(line))"
  if let extra = string {
    msg += ":\t" + extra
  }
  fatalError(msg)
}

// MARK: Tuples
public enum Pair<L: ~Copyable, R: ~Copyable>: ~Copyable {
  case pair(L, R)
}
extension Pair: Copyable where L: Copyable, R: Copyable {}

/// MARK: Data.Maybe
public enum Maybe<Wrapped: ~Copyable>: ~Copyable {
  case just(Wrapped)
  case nothing
}
extension Maybe: Copyable where Wrapped: Copyable {}

extension Maybe: Show where Wrapped: Show & ~Copyable {
  public borrowing func show() -> String {
    switch self {
      case let .just(elm):
        return elm.show()
      case .nothing:
        return "<nothing>"
    }
  }
}

extension Maybe: Eq where Wrapped: Eq, Wrapped: ~Copyable {
  public static func ==(_ a: borrowing Self, _ b: borrowing Self) -> Bool {
    switch a {
      case let .just(a1):
        switch b {
          case let .just(b1):
            return a1 == b1
          case .nothing:
            return false
        }
      case .nothing:
        switch b {
          case .just:
            return false
          case .nothing:
            return true
        }
    }
  }
}

public func isJust<A: ~Copyable>(_ m: borrowing Maybe<A>) -> Bool {
  switch m {
    case .just:
      return true
    case .nothing:
      return false
  }
}

@inlinable
public func isNothing<A: ~Copyable>(_ m: borrowing Maybe<A>) -> Bool {
  return !isJust(m)
}

public struct UnownedRef<Instance: AnyObject> {
  @usableFromInline
  internal unowned(unsafe) var _value: Instance
}

/// Provides underlying support so that you can create recursive enums, because
/// noncopyable enums do not yet support indirect cases.
public struct Box<Wrapped: ~Copyable>: ~Copyable {
  private let _pointer: UnsafeMutablePointer<Wrapped>

  init(_ wrapped: consuming Wrapped) {
    _pointer = .allocate(capacity: 1)
    _pointer.initialize(to: wrapped)
  }

  deinit {
    _pointer.deinitialize(count: 1)
    _pointer.deallocate()
  }

  consuming func take() -> Wrapped {
    let wrapped = _pointer.move()
    _pointer.deallocate()
    discard self
    return wrapped
  }

  var borrow: Wrapped {
    _read { yield _pointer.pointee }
  }

  consuming func map(_ transform: (consuming Wrapped) -> Wrapped) -> Self {
    _pointer.initialize(to: transform(_pointer.move()))
    return self
  }
}


/// MARK: Data.List
///
/// A singly-linked list
public enum List<Element: ~Copyable>: ~Copyable {
  case cons(Element, Box<List<Element>>)
  case empty

  public init(_ head: consuming Element,
              _ tail: consuming List<Element>) {
    self = .cons(head, Box(tail))
  }

  public init() { self = .empty }
}

/// Pure Iteration
extension List where Element: ~Copyable {
  /// Performs forward iteration through the list, accumulating a result value.
  /// Returns f(xn,...,f(x2, f(x1, init))...), or `init` if the list is empty.
  public borrowing func foldl<Out>(
                        init initial: consuming Out,
                        _ f: (borrowing Element, consuming Out) -> Out) -> Out
                        where Out: ~Copyable {
    func loop(_ acc: consuming Out, _ lst: borrowing Self) -> Out {
      switch lst {
        case .empty:
            return acc
        case let .cons(elm, tail):
          return loop(f(elm, acc), tail.borrow)
      }
    }
    return loop(initial, self)
  }

  /// Performs reverse iteration through the list, accumulating a result value.
  /// Returns f(x1, f(x2,...,f(xn, init)...)) or `init` if the list is empty.
  public borrowing func foldr<Out>(
                        init initial: consuming Out,
                        _ f: (borrowing Element, consuming Out) -> Out) -> Out
                        where Out: ~Copyable {
  switch self {
    case .empty:
      return initial
    case let .cons(elm, tail):
      return f(elm, tail.borrow.foldr(init: initial, f))
    }
  }

  // Forward iteration without accumulating a result.
  public borrowing func forEach(_ f: (borrowing Element) -> Void) -> Void {
    switch self {
    case .empty: return
    case let .cons(elm, tail):
      f(elm)
      return tail.borrow.forEach(f)
    }
  }
}

/// Initialization
extension List where Element: ~Copyable {
  // Generates a list of elements [f(0), f(1), ..., f(n-1)] from left to right.
  // For n < 0, the empty list is created.
  public init(length n: Int, _ f: (Int) -> Element) {
    guard n > 0 else {
      self = .empty
      return
    }

    let cur = n-1
    let elm = f(cur)
    self = List(elm, List(length: cur, f))
  }
}

/// Basic utilities
extension List where Element: ~Copyable {
  /// Is this list empty?
  ///
  /// Complexity: O(1)
  public var isEmpty: Bool {
    borrowing get {
      switch self {
      case .empty: true
      case .cons(_, _): false
      }
    }
  }

  /// How many elements are in this list?
  ///
  /// Complexity: O(n)
  public borrowing func length() -> Int {
    return foldl(init: 0) { $1 + 1 }
  }

  /// Pop the first element off the list, if present.
  ///
  /// Complexity: O(1)
  public consuming func pop() -> Optional<Pair<Element, List<Element>>> {
    switch consume self {
      case .empty: .none
      case let .cons(elm, tail): .pair(elm, tail.take())
    }
  }

  /// Push an element onto the front of the list.
  ///
  /// Complexity: O(1)
  public consuming func push(_ newHead: consuming Element) -> List<Element> {
    return List(newHead, self)
  }

  /// Produces a new list that is the reverse of this list.
  ///
  /// Complexity: O(n)
  public consuming func reverse() -> List<Element> {
    var new = List<Element>()
    while case let .pair(head, tail) = pop() {
      new = new.push(head)
      self = tail
    }
    return new
  }
}

extension List: Show where Element: Show & ~Copyable {
  public borrowing func show() -> String {
    return "[" + foldl(init: "]", { $0.show() + ", " + $1 })
  }
}
