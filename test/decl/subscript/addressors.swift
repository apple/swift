// RUN: %target-typecheck-verify-swift -parse-stdlib

import Swift

func someValidAddress<T>() -> UnsafeMutablePointer<T> { fatalError() }
func someValidAddress<T>() -> UnsafePointer<T> { fatalError() }

struct ValidImmutable {
  var base: UnsafePointer<Int>

  subscript(index: Int) -> Int {
    unsafeAddress {
      return base
    }
  }
}

struct ValidBoth {
  var base: UnsafeMutablePointer<Int>

  subscript(index: Int) -> Int {
    unsafeAddress {
      return UnsafePointer(base)
    }
    unsafeMutableAddress {
      return base
    }
  }
}

struct OnlyMutable {
  var base: UnsafeMutablePointer<Int>

  subscript(index: Int) -> Int {
    unsafeMutableAddress { // expected-error {{subscript with a mutable addressor must also have a getter, addressor, or 'read' accessor}}
      return base
    }
  }
}

struct Repeated {
  var base: UnsafeMutablePointer<Int>

  subscript(index: Int) -> Int {
    unsafeAddress { // expected-note {{previous definition}}
      return UnsafePointer(base)
    }
    unsafeAddress { // expected-error {{subscript already has an addressor}}
      return base
    }
  }
}

struct RepeatedMutable {
  var base: UnsafeMutablePointer<Int>

  subscript(index: Int) -> Int {
    unsafeAddress {
      return UnsafePointer(base)
    }
    unsafeMutableAddress { // expected-note {{previous definition}}
      return base
    }
    unsafeMutableAddress { // expected-error {{subscript already has a mutable addressor}}
      return base
    }
  }
}

struct AddressorAndGet {
  var base: UnsafePointer<Int>

  subscript(index: Int) -> Int {
    unsafeAddress { // expected-error {{subscript cannot provide both an addressor and a getter}}
      return base
    }
    get { // expected-note {{getter defined here}}
      return base.get() // expected-error {{value of type 'UnsafePointer<Int>' has no member 'get'}}
    }
  }
}

struct AddressorAndSet {
  var base: UnsafePointer<Int>

  subscript(index: Int) -> Int {
    unsafeAddress {
      return base
    }
    set {
    }
  }
}

struct MutableAddressorAndGet {
  var base: UnsafeMutablePointer<Int>

  subscript(index: Int) -> Int {
    unsafeMutableAddress {
      return base
    }
    get {
      return base.pointee
    }
  }
}

protocol HasImmutableSubscript {
  subscript(index: Int) -> Int { get }
}
protocol HasMutableSubscript {
  subscript(index: Int) -> Int { get set } // expected-note {{protocol requires}}
}

struct DisobedientImmutableAddressor: HasMutableSubscript { // expected-error {{does not conform}}
  subscript(index: Int) -> Int { // expected-note {{candidate is not settable}}
    unsafeAddress { return someValidAddress() }
  }
}

struct ObedientImmutableAddressor: HasImmutableSubscript {
  subscript(index: Int) -> Int {
    unsafeAddress { return someValidAddress() }
  }
}

struct ObedientMutableAddressor: HasMutableSubscript {
  subscript(index: Int) -> Int {
    unsafeAddress { return someValidAddress() }
    unsafeMutableAddress { return someValidAddress() }
  }
}

protocol HasMutatingImmutableSubscript {
  subscript(index: Int) -> Int { mutating get }
}
protocol HasMutatingMutableSubscript {
  subscript(index: Int) -> Int { mutating get set } // expected-note {{protocol requires}}
}

// We allow mutating accessor requirements to be implemented by non-mutating accessors.

struct DisobedientImmutableAddressor2: HasMutatingMutableSubscript { // expected-error {{does not conform}}
  subscript(index: Int) -> Int { // expected-note {{candidate is not settable}}
    unsafeAddress { return someValidAddress() }
  }
}

struct ObedientImmutableAddressor2: HasMutatingImmutableSubscript {
  subscript(index: Int) -> Int {
    unsafeAddress { return someValidAddress() }
  }
}

struct ObedientMutableAddressor2: HasMutatingMutableSubscript {
  subscript(index: Int) -> Int {
    unsafeAddress { return someValidAddress() }
    unsafeMutableAddress { return someValidAddress() }
  }
}

// Non-mutating accessor requirements cannot be implemented by mutating accessors.

protocol HasNonMutatingMutableSubscript {
  subscript(index: Int) -> Int { get nonmutating set } // expected-note {{protocol requires}}
}

struct DisobedientNonMutatingMutableAddressor: HasNonMutatingMutableSubscript { // expected-error {{does not conform}}
  subscript(index: Int) -> Int {
    unsafeAddress { return someValidAddress() }
    unsafeMutableAddress { return someValidAddress() } // expected-note {{candidate is marked 'mutating' but protocol does not allow it}}
  }
}

struct ObedientNonMutatingMutableAddressor: HasNonMutatingMutableSubscript {
  subscript(index: Int) -> Int {
    unsafeAddress { return someValidAddress() }
    nonmutating unsafeMutableAddress { return someValidAddress() }
  }
}

struct RedundantAddressors {
  var owner : Builtin.NativeObject
  subscript(index: Int) -> Int {
    unsafeAddress { return someValidAddress() } // expected-note {{previous definition of addressor here}}
    unsafeAddress { return someValidAddress() } // expected-error {{subscript already has an addressor}}
  }
}

struct RedundantMutableAddressors {
  var owner : Builtin.NativeObject
  subscript(index: Int) -> Int {
    unsafeAddress { return someValidAddress() }
    unsafeMutableAddress { return someValidAddress() } // expected-note {{previous definition of mutable addressor here}}
    unsafeMutableAddress { return someValidAddress() } // expected-error {{subscript already has a mutable addressor}}
  }
}
