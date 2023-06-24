// RUN: %empty-directory(%t)
// RUN: %target-build-swift -parse-as-library -lto=llvm-thin %s -emit-bc -o %t/a.o %lto_flags
// RUN: %use_just_built_liblto ar -r -s %t/archive.a %t/a.o
// RUN: %use_just_built_liblto %target-clang %t/archive.a -isysroot %sdk -L%swift-lib-dir/swift/%target-sdk-name -flto -o %t/main
// RUN: %target-run %t/main | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: OS=macosx
// REQUIRES: no_asan

@_cdecl("main")
func main() -> Int {
	print("Hello!")
	return 0
}

// CHECK: Hello!
