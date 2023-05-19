// RUN: %swift-frontend -dump-parse -verify %s > /dev/null

func _forget<T>(_ t: T) {}

func a() {
  _forget case // expected-error {{'case' label can only appear inside a 'switch' statement}}
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
}

func b() {
  _forget 124 // expected-error {{consecutive statements on a line must be separated by ';'}}
}

func c() {
  _forget where 5 == 0 // expected-error {{expected expression}}
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
}

func c() {
  // sticking a let here to ensure the parser is treating this like an expr.
  let _ = _forget ++ something
}

func c() {
  let _ = _forget self // expected-error {{consecutive statements on a line must be separated by ';'}}
}

func c() {
  _forget Self // expected-warning {{'_forget' keyword is deprecated and will be removed soon; use 'discard' instead}}{{3-10=discard}}
}

func c() {
  _forget self // expected-warning {{'_forget' keyword is deprecated and will be removed soon; use 'discard' instead}}{{3-10=discard}}
}

func c() {
  _forget `self` // expected-warning {{'_forget' keyword is deprecated and will be removed soon; use 'discard' instead}}{{3-10=discard}}
}

func c() {
  _forget switch  // expected-error {{expected expression in 'switch' statement}}
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
}

func c() {
  _forget SarahMarshall // expected-warning {{'_forget' keyword is deprecated and will be removed soon; use 'discard' instead}}{{3-10=discard}}
}

func c() {
  _forget x.y.z // expected-warning {{'_forget' keyword is deprecated and will be removed soon; use 'discard' instead}}{{3-10=discard}}
}

func c() {
  _forget _forget self // expected-error {{consecutive statements on a line must be separated by ';'}}
  // expected-warning@-1 {{'_forget' keyword is deprecated and will be removed soon; use 'discard' instead}}{{3-10=discard}}
}

class _forget {
  func _forget() {
    _forget _forget(self)
    // expected-warning@-1 {{'_forget' keyword is deprecated and will be removed soon; use 'discard' instead}}{{5-12=discard}}

    let _ = _forget()

    let _ = _forget{
      _forget   {
        _forget itALL
        // expected-warning@-1 {{'_forget' keyword is deprecated and will be removed soon; use 'discard' instead}}
      }
    }
  }

  var x: Int {
    get {
      _forget self.x() // expected-warning {{'_forget' keyword is deprecated and will be removed soon; use 'discard' instead}}
    }
  }
}
