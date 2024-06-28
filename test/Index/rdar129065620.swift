// RUN: %empty-directory(%t)
// RUN: not %target-swift-frontend -typecheck %s -index-store-path %t

// rdar://129065620 - Make sure we don't crash when verifying the mangling.
enum Foo {
  case bar(id: UInt32)
  case bar(id: UInt32)
}

struct S {
  subscript(x: Invalid) -> Invalid {}
}
