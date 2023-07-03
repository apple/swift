// RUN: %empty-directory(%t)
// RUN: %target-build-swift %S/Inputs/Inlining.swift -parse-as-library -g -o %t/Inlining
// RUN: %target-build-swift %s -parse-as-library -g -o %t/DwarfReader
// RUN: %target-run %t/DwarfReader %t/Inlining | %FileCheck %s

// REQUIRES: OS=linux-gnu
// REQUIRES: backtracing

@_spi(DwarfTest) import _Backtracing

@main
struct DwarfReader {
  static func main() {
    if CommandLine.argc != 2 {
      print("usage: DwarfReader <path-to-binary>")
      return
    }

    // CHECK: {{.*}}/Inlining is a {{32|64}}-bit ELF image
    // CHECK: Units:
    // CHECK: Call Sites:

    if !testDwarfReaderFor(path: CommandLine.arguments[1]) {
      exit(1)
    }
  }
}
