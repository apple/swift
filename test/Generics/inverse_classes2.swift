// RUN: %target-typecheck-verify-swift -disable-availability-checking

@_moveOnly // expected-error {{'@_moveOnly' attribute is only valid on structs or enums}}
class KlassLegacy {}

class KlassModern: ~Copyable {} // expected-error {{classes cannot be '~Copyable'}}

actor FamousPerson: ~Copyable {} // expected-error{{actors cannot be '~Copyable'}}

class Konditional<T: ~Copyable> {}

func checks<T: ~Copyable, C>(
          _ a: KlassLegacy,
          _ b: KlassModern,
          _ c: Konditional<T>,
          _ d: Konditional<C>) {}
