// RUN: %empty-directory(%t)
// RUN: %use_just_built_liblto %target-build-swift -parse-as-library -working-directory %t -lto=llvm-thin -DMODULE -static -emit-library -emit-module %s -module-name MyModule
// RUN: %use_just_built_liblto %target-build-swift -parse-as-library -working-directory %t -lto=llvm-thin %s -I%t -L%t -lMyModule -module-name main -o %t/main %lto_flags
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: OS=macosx
// REQUIRES: no_asan

#if MODULE

public protocol MyProtocol {
}
public class MyClass: MyProtocol {
}

public func fooFromModule() { print("fooFromModule") }
public func barFromModule() { print("barFromModule") }

#else

import MyModule

@_cdecl("main")
func main() -> Int {
	print("Hello!")
	fooFromModule()
	return 0
}

#endif

// CHECK: Hello!
// CHECK: fooFromModule
