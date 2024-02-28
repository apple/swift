// This test checks that embedded Swift doesn't mark public functions/symbols as llvm.used / llvm.compiler.used

// RUN: %target-swift-emit-ir %s -parse-stdlib -enable-experimental-feature Embedded -target arm64e-apple-none-elf -wmo | %FileCheck %s --check-prefix CHECK-ELF
// RUN: %target-swift-emit-ir %s -parse-stdlib -enable-experimental-feature Embedded -target arm64e-apple-none-macho -wmo | %FileCheck %s --check-prefix CHECK-MACHO

// REQUIRES: swift_in_compiler

struct Bool {}

protocol Player {
  func play()
  var canPlay: Bool { get }
}

struct Concrete : Player {
  func play() { }
  var canPlay: Bool { Bool() }
}

func start(p: some Player) {
  p.play()
}

public func main() {
  start(p: Concrete())
}

// CHECK-ELF: @_swift1_autolink_entries =
// CHECK-ELF: @llvm.used = appending global [1 x ptr] [ptr @_swift1_autolink_entries], section "llvm.metadata"
// CHECK-ELF-NOT: @llvm.used

// CHECK-MACHO-NOT: @llvm.compiler.used
// CHECK-MACHO-NOT: @llvm.used
