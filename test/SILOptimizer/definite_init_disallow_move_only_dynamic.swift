// RUN: %target-swift-frontend -emit-sil -verify %s

enum E: Error { case err }

struct NC: ~Copyable {
  let x = 0

  deinit { print("deinit") }
}

func chk(_ cond: Bool) throws {
  let y: NC // expected-error{{not supported}} expected-warning{{never used}}
  if cond {
    y = NC()
  }
  throw E.err
}

try? chk(true)
