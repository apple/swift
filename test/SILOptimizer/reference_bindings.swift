// RUN: %target-swift-frontend -verify -sil-verify-all -enable-experimental-feature ReferenceBindings -emit-sil %s

func varBindingTest() {
    var x = "1"
    x = "1"
    inout x1 = x // expected-error {{overlapping accesses to 'x', but modification requires exclusive access; consider copying to a local variable}}
    let _ = x1
    inout x2 = x // expected-note {{conflicting access is here}}
    let _ = x2
}

func varBindingTest2() {
    var x = "1"
    x = "1"
    inout x1 = x // expected-error {{overlapping accesses to 'x', but modification requires exclusive access; consider copying to a local variable}}
    let _ = x1
    x = "2" // expected-note {{conflicting access is here}}
}

func varBindingTest3() {
    var x = "1"
    x = "1"
    do {
      inout x1 = x
      let _ = x1
    }
    x = "2"
}
