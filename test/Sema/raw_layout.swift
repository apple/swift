// RUN: %target-swift-frontend -enable-experimental-feature RawLayout -typecheck -verify %s

@_rawLayout(size: 4, alignment: 4) // expected-error{{type with @_rawLayout cannot be copied and must be declared ~Copyable}}
struct ImproperlyCopyable {}

@_rawLayout(size: 4, alignment: 3) // expected-error{{alignment value must be a power of two}}
struct InvalidAlignment: ~Copyable {}

@_rawLayout(size: 4, alignment: 4) // expected-error{{type with @_rawLayout cannot have stored properties}}
struct HasStoredProperty: ~Copyable { var x: Int }

@_rawLayout(size: 4, alignment: 4) // expected-error{{type with @_rawLayout cannot have stored properties}}
struct HasLazyStoredProperty: ~Copyable { lazy var x: Int = 42 }

@_rawLayout(size: 4, alignment: 4) // expected-error{{type with @_rawLayout cannot have stored properties}}
struct HasObservedStoredProperty: ~Copyable { var x: Int { didSet { } } }

@propertyWrapper
struct Wrapper<T> {
    var wrappedValue: T { fatalError() }
}

@_rawLayout(size: 4, alignment: 4) // expected-error{{type with @_rawLayout cannot have stored properties}}
struct HasWrappedStoredProperty: ~Copyable { @Wrapper var x: Int }

@_rawLayout(like: T) // expected-error{{cannot find type 'T' in scope}}
struct TypeDoesntExist<A>: ~Copyable {}

@_rawLayout(size: 4, alignment: 4) // expected-error{{@_rawLayout may only be used on 'struct'}}
enum EnumWithRawLayout: ~Copyable {}

@_rawLayout(size: 4, alignment: 4) // expected-error{{@_rawLayout may only be used on 'struct'}}
class ClassWithRawLayout {}

@_rawLayout(size: 4, alignment: 4)
struct Lock: ~Copyable {}

@_rawLayout(size: 4, alignment: 4) // expected-error{{@_rawLayout may only be used on 'struct'}}
typealias TypealiasWithRawLayout = Lock

struct Butt {
    @_rawLayout(size: 4, alignment: 4) // expected-error{{@_rawLayout may only be used on 'struct'}}
    var propertyWithStoredLayout: ()
}

@_rawLayout(size: 4, alignment: 4) @_alignment(16) // expected-error{{type with @_rawLayout cannot also have an @_alignment attribute}}
struct RawLayoutAndAlignment: ~Copyable {}
