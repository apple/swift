// RUN: %target-swift-frontend -print-ast %s | %FileCheck %s

// CHECK-LABEL: internal enum Simple : Hashable
enum Simple: Hashable {
  // CHECK:        case a
  case a
  // CHECK:        case b
  case b

  // CHECK:        @_implements(Equatable, ==(_:_:)) internal static func __derived_enum_equals(_ a: Simple, _ b: Simple) -> Bool {
  // CHECK-NEXT:     private var index_a: Int
  // CHECK-NEXT:     switch a {
  // CHECK-NEXT:     case .a:
  // CHECK-NEXT:       index_a = 0
  // CHECK-NEXT:     case .b:
  // CHECK-NEXT:       index_a = 1
  // CHECK-NEXT:     }
  // CHECK-NEXT:     private var index_b: Int
  // CHECK-NEXT:     switch b {
  // CHECK-NEXT:     case .a:
  // CHECK-NEXT:       index_b = 0
  // CHECK-NEXT:     case .b:
  // CHECK-NEXT:       index_b = 1
  // CHECK-NEXT:     }
  // CHECK-NEXT:     return index_a == index_b
  // CHECK-NEXT:   }

  // CHECK:        internal func hash(into hasher: inout Hasher) {
  // CHECK-NEXT:     private var discriminator: Int
  // CHECK-NEXT:     switch self {
  // CHECK-NEXT:     case .a:
  // CHECK-NEXT:       discriminator = 0
  // CHECK-NEXT:     case .b:
  // CHECK-NEXT:       discriminator = 1
  // CHECK-NEXT:     }
  // CHECK-NEXT:     hasher.combine(discriminator)
  // CHECK-NEXT:   }

  // CHECK:        internal var hashValue: Int {
  // CHECK-NEXT:     get {
  // CHECK-NEXT:       return _hashValue(for: self)
  // CHECK-NEXT:     }
  // CHECK-NEXT:   }
}

// CHECK-LABEL: internal enum HasAssociatedValues : Hashable
enum HasAssociatedValues: Hashable {
  // CHECK:        case a(Int)
  case a(Int)
  // CHECK:        case b(String)
  case b(String)
  // CHECK:        case c
  case c

  // CHECK:        internal func hash(into hasher: inout Hasher) {
  // CHECK-NEXT:     switch self {
  // CHECK-NEXT:     case .a(let a0):
  // CHECK-NEXT:       hasher.combine(0)
  // CHECK-NEXT:       hasher.combine(a0)
  // CHECK-NEXT:     case .b(let a0):
  // CHECK-NEXT:       hasher.combine(1)
  // CHECK-NEXT:       hasher.combine(a0)
  // CHECK-NEXT:     case .c:
  // CHECK-NEXT:       hasher.combine(2)
  // CHECK-NEXT:     }
  // CHECK-NEXT:   }

  // CHECK:        @_implements(Equatable, ==(_:_:)) internal static func __derived_enum_equals(_ a: HasAssociatedValues, _ b: HasAssociatedValues) -> Bool {
  // CHECK-NEXT:     switch (a, b) {
  // CHECK-NEXT:     case (.a(let l0), .a(let r0)):
  // CHECK-NEXT:       guard l0  r0 else {
  // CHECK-NEXT:         return false
  // CHECK-NEXT:       }
  // CHECK-NEXT:       return true
  // CHECK-NEXT:     case (.b(let l0), .b(let r0)):
  // CHECK-NEXT:       guard l0  r0 else {
  // CHECK-NEXT:         return false
  // CHECK-NEXT:       }
  // CHECK-NEXT:       return true
  // CHECK-NEXT:     case (.c, .c):
  // CHECK-NEXT:       return true
  // CHECK-NEXT:     default:
  // CHECK-NEXT:       return false
  // CHECK-NEXT:     }
  // CHECK-NEXT:   }

  // CHECK:        internal var hashValue: Int {
  // CHECK-NEXT:     get {
  // CHECK-NEXT:       return _hashValue(for: self)
  // CHECK-NEXT:     }
  // CHECK-NEXT:   }
}

// CHECK-LABEL: internal enum HasUnavailableElement : Hashable
enum HasUnavailableElement: Hashable {
  // CHECK:       case a
  case a
  // CHECK:       @available(*, unavailable)
  // CHECK-NEXT:  case b
  @available(*, unavailable)
  case b

  // CHECK:       @_implements(Equatable, ==(_:_:)) internal static func __derived_enum_equals(_ a: HasUnavailableElement, _ b: HasUnavailableElement) -> Bool {
  // CHECK-NEXT:    private var index_a: Int
  // CHECK-NEXT:    switch a {
  // CHECK-NEXT:    case .a:
  // CHECK-NEXT:      index_a = 0
  // CHECK-NEXT:    case .b:
  // CHECK-NEXT:      _diagnoseUnavailableCodeReached()
  // CHECK-NEXT:    }
  // CHECK-NEXT:    private var index_b: Int
  // CHECK-NEXT:    switch b {
  // CHECK-NEXT:    case .a:
  // CHECK-NEXT:      index_b = 0
  // CHECK-NEXT:    case .b:
  // CHECK-NEXT:      _diagnoseUnavailableCodeReached()
  // CHECK-NEXT:    }
  // CHECK-NEXT:    return index_a == index_b
  // CHECK-NEXT:  }

  // CHECK:       internal func hash(into hasher: inout Hasher) {
  // CHECK-NEXT:    private var discriminator: Int
  // CHECK-NEXT:    switch self {
  // CHECK-NEXT:    case .a:
  // CHECK-NEXT:      discriminator = 0
  // CHECK-NEXT:    case .b:
  // CHECK-NEXT:      _diagnoseUnavailableCodeReached()
  // CHECK-NEXT:    }
  // CHECK-NEXT:    hasher.combine(discriminator)
  // CHECK-NEXT:  }

  // CHECK:       internal var hashValue: Int {
  // CHECK-NEXT:    get {
  // CHECK-NEXT:      return _hashValue(for: self)
  // CHECK-NEXT:    }
  // CHECK-NEXT:  }
}

// CHECK-LABEL: internal enum HasAssociatedValuesAndUnavailableElement : Hashable
enum HasAssociatedValuesAndUnavailableElement: Hashable {
  // CHECK:        case a(Int)
  case a(Int)
  // CHECK:       @available(*, unavailable)
  // CHECK-NEXT:  case b(String)
  @available(*, unavailable)
  case b(String)

  // CHECK:       internal func hash(into hasher: inout Hasher) {
  // CHECK-NEXT:    switch self {
  // CHECK-NEXT:    case .a(let a0):
  // CHECK-NEXT:      hasher.combine(0)
  // CHECK-NEXT:      hasher.combine(a0)
  // CHECK-NEXT:    case .b:
  // CHECK-NEXT:      _diagnoseUnavailableCodeReached()
  // CHECK-NEXT:    }
  // CHECK-NEXT:  }

  // CHECK:       @_implements(Equatable, ==(_:_:)) internal static func __derived_enum_equals(_ a: HasAssociatedValuesAndUnavailableElement, _ b: HasAssociatedValuesAndUnavailableElement) -> Bool {
  // CHECK-NEXT:    switch (a, b) {
  // CHECK-NEXT:    case (.a(let l0), .a(let r0)):
  // CHECK-NEXT:      guard l0  r0 else {
  // CHECK-NEXT:        return false
  // CHECK-NEXT:      }
  // CHECK-NEXT:      return true
  // CHECK-NEXT:    case (.b, .b):
  // CHECK-NEXT:      _diagnoseUnavailableCodeReached()
  // CHECK-NEXT:    default:
  // CHECK-NEXT:      return false
  // CHECK-NEXT:    }
  // CHECK-NEXT:  }

  // CHECK:       internal var hashValue: Int {
  // CHECK-NEXT:    get {
  // CHECK-NEXT:      return _hashValue(for: self)
  // CHECK-NEXT:    }
  // CHECK-NEXT:  }
}

// CHECK-LABEL: internal enum UnavailableEnum : Hashable
@available(*, unavailable)
enum UnavailableEnum: Hashable {
  // CHECK:        case a
  case a
  // CHECK:        case b
  case b

  // CHECK:        @_implements(Equatable, ==(_:_:)) internal static func __derived_enum_equals(_ a: UnavailableEnum, _ b: UnavailableEnum) -> Bool {
  // CHECK-NEXT:     private var index_a: Int
  // CHECK-NEXT:     switch a {
  // CHECK-NEXT:     case .a:
  // CHECK-NEXT:       index_a = 0
  // CHECK-NEXT:     case .b:
  // CHECK-NEXT:       index_a = 1
  // CHECK-NEXT:     }
  // CHECK-NEXT:     private var index_b: Int
  // CHECK-NEXT:     switch b {
  // CHECK-NEXT:     case .a:
  // CHECK-NEXT:       index_b = 0
  // CHECK-NEXT:     case .b:
  // CHECK-NEXT:       index_b = 1
  // CHECK-NEXT:     }
  // CHECK-NEXT:     return index_a == index_b
  // CHECK-NEXT:   }

  // CHECK:        internal func hash(into hasher: inout Hasher) {
  // CHECK-NEXT:     private var discriminator: Int
  // CHECK-NEXT:     switch self {
  // CHECK-NEXT:     case .a:
  // CHECK-NEXT:       discriminator = 0
  // CHECK-NEXT:     case .b:
  // CHECK-NEXT:       discriminator = 1
  // CHECK-NEXT:     }
  // CHECK-NEXT:     hasher.combine(discriminator)
  // CHECK-NEXT:   }

  // CHECK:        internal var hashValue: Int {
  // CHECK-NEXT:     get {
  // CHECK-NEXT:       return _hashValue(for: self)
  // CHECK-NEXT:     }
  // CHECK-NEXT:   }

}
