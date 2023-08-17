// RUN: %target-swift-frontend -emit-sil -verify %s

func foo<T>(a: Bool, t: T) {
  let x: T
  defer { print(x) }

  x = t
  return
}

func bar<T>(a: Bool, t: T) {
  let x: T // expected-note {{defined here}}
  defer { print(x) } //expected-error{{constant 'x' used before being initialized}}

  if a {
    x = t
    return
  }
}

func bas<T>(a: Bool, t: T) {
  let x: T 
  defer { print(x) }

  if a {
    x = t
    return
  }

  x = t
}
