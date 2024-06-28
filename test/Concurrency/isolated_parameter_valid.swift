// RUN: %target-swift-frontend -disable-availability-checking -swift-version 6 %s -emit-sil -o /dev/null -verify

public func doNotCross(
  isolation: isolated (any Actor)? = #isolation,
  _ block: () async -> Void
) async {
  await block()
}

actor MyActor {
  func doStuff() {}

  func test() async {
    await doNotCross {
      doStuff()
    }
  }
}
