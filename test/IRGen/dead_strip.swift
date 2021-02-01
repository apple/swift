// Checks that applying -dead_strip linker flag actually removes unused classes and functions from static libraries.

// REQUIRES: OS=macosx

// RUN: echo "-target x86_64-apple-macos11.0 -swift-version 5 -parse-as-library -working-directory %t" >> %t-commonflags
// RUN: echo "-Xfrontend -disable-objc-interop" >> %t-commonflags
// RUN: echo "-Xfrontend -disable-reflection-metadata -Xfrontend -disable-reflection-names" >> %t-commonflags

// RUN: %empty-directory(%t)
// RUN: %target-swiftc_driver @%t-commonflags -emit-library -static -DMODULE -emit-module %s -module-name MyModule
// RUN: %target-swiftc_driver @%t-commonflags %s -I%t -L%t -lMyModule -Xlinker -dead_strip -module-name main -o %t/main
// RUN: %llvm-nm --defined-only %t/main | %FileCheck %s

// RUN: %empty-directory(%t)
// RUN: %target-swiftc_driver @%t-commonflags -emit-library -static -DMODULE -emit-module %s -module-name MyModule -Xfrontend -emit-dead-strippable-symbols
// RUN: %target-swiftc_driver @%t-commonflags %s -I%t -L%t -lMyModule -Xlinker -dead_strip -module-name main -o %t/main
// RUN: %llvm-nm --defined-only %t/main | %FileCheck %s -check-prefix CHECK-DEADSTRIPPABLE

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
func main() {
	print("Hello!")
	fooFromModule()
}

#endif

// CHECK: _$s8MyModule07barFromB0yyF
// CHECK: _$s8MyModule07fooFromB0yyF
// CHECK: _$s8MyModule0A5ClassCAA0A8ProtocolAAMc
// CHECK: _$s8MyModule0A5ClassCAA0A8ProtocolAAWP
// CHECK: _$s8MyModule0A5ClassCACycfC
// CHECK: _$s8MyModule0A5ClassCACycfCTq
// CHECK: _$s8MyModule0A5ClassCACycfc
// CHECK: _$s8MyModule0A5ClassCMa
// CHECK: _$s8MyModule0A5ClassCMf
// CHECK: _$s8MyModule0A5ClassCMn
// CHECK: _$s8MyModule0A5ClassCN
// CHECK: _$s8MyModule0A5ClassCfD
// CHECK: _$s8MyModule0A5ClassCfd
// CHECK: _$s8MyModule0A8ProtocolMp
// CHECK: _$s8MyModuleMXM

// CHECK-DEADSTRIPPABLE-NOT: _$s8MyModule07barFromB0yyF
// CHECK-DEADSTRIPPABLE:     _$s8MyModule07fooFromB0yyF
// CHECK-DEADSTRIPPABLE-NOT: _$s8MyModule0A5ClassCAA0A8ProtocolAAMc
// CHECK-DEADSTRIPPABLE-NOT: _$s8MyModule0A5ClassCAA0A8ProtocolAAWP
// CHECK-DEADSTRIPPABLE-NOT: _$s8MyModule0A5ClassCACycfC
// CHECK-DEADSTRIPPABLE-NOT: _$s8MyModule0A5ClassCACycfCTq
// CHECK-DEADSTRIPPABLE-NOT: _$s8MyModule0A5ClassCACycfc
// CHECK-DEADSTRIPPABLE-NOT: _$s8MyModule0A5ClassCMa
// CHECK-DEADSTRIPPABLE-NOT: _$s8MyModule0A5ClassCMf
// CHECK-DEADSTRIPPABLE-NOT: _$s8MyModule0A5ClassCMn
// CHECK-DEADSTRIPPABLE-NOT: _$s8MyModule0A5ClassCN
// CHECK-DEADSTRIPPABLE-NOT: _$s8MyModule0A5ClassCfD
// CHECK-DEADSTRIPPABLE-NOT: _$s8MyModule0A5ClassCfd
// CHECK-DEADSTRIPPABLE-NOT: _$s8MyModule0A8ProtocolMp
// CHECK-DEADSTRIPPABLE-NOT: _$s8MyModuleMXM
