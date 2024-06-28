// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -enable-experimental-feature Embedded -c -I%t -parse-as-library %t/MyModuleA.swift -o %t/MyModuleA.o -emit-module -emit-module-path %t/MyModuleA.swiftmodule -emit-empty-object-file
// RUN: %target-swift-frontend -enable-experimental-feature Embedded -c -I%t -parse-as-library %t/MyModuleB.swift -o %t/MyModuleB.o -emit-module -emit-module-path %t/MyModuleB.swiftmodule -emit-empty-object-file
// RUN: %target-swift-frontend -enable-experimental-feature Embedded -c -I%t -parse-as-library %t/MyModuleC.swift -o %t/MyModuleC.o -emit-module -emit-module-path %t/MyModuleC.swiftmodule -emit-empty-object-file
// RUN: %target-swift-frontend -enable-experimental-feature Embedded -c -I%t %t/Main.swift -o %t/Main.o
// RUN: %target-clang %t/Main.o %t/MyModuleA.o %t/MyModuleB.o %t/MyModuleC.o -o %t/a.out
// RUN: %target-run %t/a.out

// REQUIRES: swift_in_compiler
// REQUIRES: executable_test
// REQUIRES: OS=macosx || OS=linux-gnu

//--- MyModuleA.swift

public func a() {}

//--- MyModuleB.swift

import MyModuleA

public func b() {}

//--- MyModuleC.swift

import MyModuleB

public func c() {}

//--- Main.swift

import MyModuleA
import MyModuleB
import MyModuleC

a()
b()
c()
