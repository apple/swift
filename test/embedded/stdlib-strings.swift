// RUN: %target-run-simple-swift(-enable-experimental-feature Embedded -parse-as-library -runtime-compatibility-version none -wmo) | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: optimized_stdlib
// REQUIRES: VENDOR=apple
// REQUIRES: OS=macosx

@main
struct Main {
  static func main() {
    let str = "Hello Hello This Is A Long String"
    print(str)
    print(str.uppercased())
    for word in str.split(separator: " ") {
      print(word.lowercased())
    }
  }
}

// CHECK: Hello Hello This Is A Long String
// CHECK: HELLO HELLO THIS IS A LONG STRING
// CHECK: hello
// CHECK: hello
// CHECK: this
// CHECK: is
// CHECK: a
// CHECK: long
// CHECK: string
