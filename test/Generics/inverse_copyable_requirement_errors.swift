// RUN: %target-typecheck-verify-swift

// a concrete move-only type
struct MO: ~Copyable {
  var x: Int?
}

func totallyInvalid<T>(_ t: T) where MO: ~Copyable {}
// expected-error@-1{{type 'MO' in conformance requirement does not refer to a generic parameter or associated type}}

func packingHeat<each T>(_ t: repeat each T) {} // expected-note {{'where each T: Copyable' is implicit here}}
func packIt() {
  packingHeat(MO())  // expected-error {{global function 'packingHeat' requires that 'MO' conform to 'Copyable'}}
  packingHeat(10)
}

func packingUniqueHeat_1<each T: ~Copyable>(_ t: repeat each T) {}
// expected-error@-1{{cannot suppress '~Copyable' on type 'each T'}}
// expected-note@-2{{'where each T: Copyable' is implicit here}}
// expected-error@-3{{'each T' required to be 'Copyable' but is marked with '~Copyable'}}

func packingUniqueHeat_2<each T>(_ t: repeat each T)
   where repeat each T: ~Copyable {}
// expected-error@-1{{cannot suppress '~Copyable' on type 'each T'}}
// expected-note@-3{{'where each T: Copyable' is implicit here}}
// expected-error@-3{{'each T' required to be 'Copyable' but is marked with '~Copyable'}}

func packItUniquely() {
  packingUniqueHeat_1(MO())
  // expected-error@-1{{global function 'packingUniqueHeat_1' requires that 'MO' conform to 'Copyable'}}

  packingUniqueHeat_2(MO())
  // expected-error@-1{{global function 'packingUniqueHeat_2' requires that 'MO' conform to 'Copyable'}}

  packingUniqueHeat_1(10)
}
