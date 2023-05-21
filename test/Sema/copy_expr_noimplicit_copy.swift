// RUN: %target-typecheck-verify-swift -disable-availability-checking -enable-experimental-feature NoImplicitCopy

class Klass {}

func consumeKlass(_ x: __owned Klass) {}

func testNoImplicitCopyWorks() {
  @_noImplicitCopy let x = Klass()
  let _ = copy x
  consumeKlass(x)
}
