// RUN: %target-run-simple-swift(-parse-as-library -Onone -enable-experimental-feature Embedded -runtime-compatibility-version none -wmo -Xfrontend -disable-objc-interop -Xlinker -dead_strip -Xlinker %swift_obj_root/lib/swift/embedded/%target-cpu-apple-macos/libswiftUnicodeDataTables.a) | %FileCheck %s
// RUN: %target-run-simple-swift(-parse-as-library -O     -enable-experimental-feature Embedded -runtime-compatibility-version none -wmo -Xfrontend -disable-objc-interop -Xlinker -dead_strip -Xlinker %swift_obj_root/lib/swift/embedded/%target-cpu-apple-macos/libswiftUnicodeDataTables.a) | %FileCheck %s
// RUN: %target-run-simple-swift(-parse-as-library -Osize -enable-experimental-feature Embedded -runtime-compatibility-version none -wmo -Xfrontend -disable-objc-interop -Xlinker -dead_strip -Xlinker %swift_obj_root/lib/swift/embedded/%target-cpu-apple-macos/libswiftUnicodeDataTables.a) | %FileCheck %s

// REQUIRES: swift_in_compiler
// REQUIRES: executable_test
// REQUIRES: optimized_stdlib
// REQUIRES: OS=macosx

@main
struct Main {
  static func main() {
    let chars: [Character] = ["a", "b", "c"]
    let s = Substring.init(chars)
    print(s)
    print("OK!")
  }
}

// CHECK: abc
// CHECK: OK!
