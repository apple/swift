// RUN: %target-typecheck-verify-swift -swift-version 4

func testIUOToAny(_ t: AnyObject.Type!) {
  let _: Any = t
}
