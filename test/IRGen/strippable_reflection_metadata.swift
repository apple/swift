
// RUN: %target-swiftc_driver -Xfrontend -enable-opt-in-reflection-metadata -swift-version 5 -parse-as-library -working-directory %t -emit-library -static -DMODULE -emit-module %s -module-name MyModule
// RUN: %llvm-nm --defined-only %t/libMyModule.a -m | %FileCheck %s

@reflectable
public protocol RefProtocol {
}

public struct RefStruct: RefProtocol {
  let i: Int
}

public protocol NonRefProtocol {
}

public struct NonRefStruct: NonRefProtocol {
  let i: Int
}

// CHECK-DAG: (__TEXT,__swift5_fieldmd) non-external [no dead strip] _$s8MyModule11RefProtocol_pMF
// CHECK-DAG: (__TEXT,__swift5_fieldmd) non-external _$s8MyModule12NonRefStructVMF
// CHECK-DAG: (__TEXT,__swift5_fieldmd) non-external _$s8MyModule14NonRefProtocol_pMF
// CHECK-DAG: (__TEXT,__swift5_fieldmd) non-external [no dead strip] _$s8MyModule9RefStructVMF
