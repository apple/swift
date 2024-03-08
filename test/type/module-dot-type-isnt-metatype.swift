// RUN: %target-typecheck-verify-swift -module-name M

struct Type {
  let name: String
}
func test(type: M.Type) {
  _ = type.name
}
