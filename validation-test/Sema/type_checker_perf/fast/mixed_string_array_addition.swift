// RUN: %target-typecheck-verify-swift -swift-version 5 -solver-expression-time-threshold=1

func method(_ arg: String, body: () -> [String]) {}

func test(str: String, properties: [String]) {
  method(str + "" + str + "") {
    properties.map { param in
      "" + param + "" + param + "" + param + ""
    } + [""]
  }
}
