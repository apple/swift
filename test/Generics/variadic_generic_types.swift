// RUN: %target-typecheck-verify-swift -disable-availability-checking

// Disallowed cases
struct MultiplePack<each T, each U> {} // expected-error {{generic type cannot declare more than one type pack}}
typealias MultiplePackAlias<each T, each U> = (repeat each T, repeat each U) // expected-error {{generic type cannot declare more than one type pack}}

// Temporary limitations
enum EnumWithPack<each T> { // expected-error {{enums cannot declare a type pack}}
  case cheddar
}

class ClassWithPack<each T> {}

struct OuterStruct<each T> {
  enum NestedEnum { // expected-error {{enums cannot declare a type pack}}
    case smokedGouda
  }

  class NestedClass {}
}

class BadInheritance1: ClassWithPack<Int> {} // expected-error {{cannot inherit from a generic class that declares a type pack}}
class BadInheritance2: OuterStruct<Int>.NestedClass {} // expected-error {{cannot inherit from a generic class that declares a type pack}}

// Type resolution of variadic type aliases
func bindAll() {
  struct Bind<each U> {}

  typealias Zero = Bind< > // OK
  typealias One = Bind<Int> // OK
  typealias Two = Bind<Int, String> // OK
  typealias Three = Bind<Int, String, Float> // OK
}

func bindPrefix() {
  struct Bind<Prefix, each U> {} // expected-note {{generic type 'Bind' declared here}}

  typealias Zero = Bind< > // expected-error {{generic type 'Bind' specialized with too few type parameters (got 0, but expected at least 1)}}
  typealias One = Bind<Int> // OK
  typealias Two = Bind<Int, String> // OK
  typealias Three = Bind<Int, String, Float> // OK
}

func bindSuffix() {
  struct Bind<each U, Suffix> {} // expected-note {{generic type 'Bind' declared here}}

  typealias Zero = Bind< > // expected-error {{generic type 'Bind' specialized with too few type parameters (got 0, but expected at least 1)}}
  typealias One = Bind<Int> // OK
  typealias Two = Bind<Int, String> // OK
  typealias Three = Bind<Int, String, Float> // OK
}

func bindPrefixAndSuffix() {
  struct Bind<Prefix, each U, Suffix> {} // expected-note 2{{generic type 'Bind' declared here}}

  typealias Zero = Bind< > // expected-error {{generic type 'Bind' specialized with too few type parameters (got 0, but expected at least 2)}}
  typealias One = Bind<Int> // expected-error {{generic type 'Bind' specialized with too few type parameters (got 1, but expected at least 2)}}
  typealias Two = Bind<Int, String> // OK
  typealias Three = Bind<Int, String, Float> // OK
  typealias Four = Bind<Int, String, Float, Bool> // OK
}

func bindAliasAll() {
  typealias Bind<each U> = (repeat each U)

  typealias Zero = Bind< > // OK
  typealias One = Bind<Int> // OK
  typealias Two = Bind<Int, String> // OK
  typealias Three = Bind<Int, String, Float> // OK
}

func bindAliasPrefix() {
  typealias Bind<Prefix, each U> = (Prefix, repeat each U) // expected-note {{generic type 'Bind' declared here}}

  typealias Zero = Bind< > // expected-error {{generic type 'Bind' specialized with too few type parameters (got 0, but expected at least 1)}}
  typealias One = Bind<Int> // OK
  typealias Two = Bind<Int, String> // OK
  typealias Three = Bind<Int, String, Float> // OK
}

func bindAliasSuffix() {
  typealias Bind<each U, Suffix> = (repeat each U, Suffix) // expected-note {{generic type 'Bind' declared here}}

  typealias Zero = Bind< > // expected-error {{generic type 'Bind' specialized with too few type parameters (got 0, but expected at least 1)}}
  typealias One = Bind<Int> // OK
  typealias Two = Bind<Int, String> // OK
  typealias Three = Bind<Int, String, Float> // OK
}

func bindAliasPrefixAndSuffix() {
  typealias Bind<Prefix, each U, Suffix> = (Prefix, repeat each U, Suffix) // expected-note 2{{generic type 'Bind' declared here}}

  typealias Zero = Bind< > // expected-error {{generic type 'Bind' specialized with too few type parameters (got 0, but expected at least 2)}}
  typealias One = Bind<Int> // expected-error {{generic type 'Bind' specialized with too few type parameters (got 1, but expected at least 2)}}
  typealias Two = Bind<Int, String> // OK
  typealias Three = Bind<Int, String, Float> // OK
  typealias Four = Bind<Int, String, Float, Bool> // OK
}
