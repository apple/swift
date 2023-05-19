// RUN: %swift-frontend -dump-parse -verify %s > /dev/null

func discard<T>(_ t: T) {}

func a() {
  discard case // expected-error {{'case' label can only appear inside a 'switch' statement}}
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
}

func b() {
  discard 124 // expected-error {{consecutive statements on a line must be separated by ';'}}
}

func c() {
  discard where 5 == 0 // expected-error {{expected expression}}
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
}

func c() {
  // sticking a let here to ensure the parser is treating this like an expr.
  let _ = discard ++ something
}

func c() {
  let _ = discard self // expected-error {{consecutive statements on a line must be separated by ';'}}
}

func c() {
  discard Self
}

func c() {
  discard self
}

func c() {
  discard `self`
}

func c() {
  discard switch  // expected-error {{expected expression in 'switch' statement}}
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
}

func c() {
  discard SarahMarshall
}

func c() {
  discard x.y.z
}

func c() {
  discard discard self // expected-error {{consecutive statements on a line must be separated by ';'}}
}

class discard {
  func discard() {
    discard discard(self)

    let _ = discard()

    let _ = discard{
      discard   {
        discard itALL
      }
    }
  }

  var x: Int {
    get {
      discard self.x()
    }
  }
}
