// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-build-swift -module-name=Lib -package-name Pkg -I%t -emit-silgen %t/Lib.swift -o %t/Lib-nonres.sil
// RUN: %FileCheck %s --check-prefix=CHECK-NONRES < %t/Lib-nonres.sil
// RUN: %target-build-swift -module-name=Lib -package-name Pkg -I%t -emit-silgen -O -wmo %t/Lib.swift | %FileCheck %s --check-prefix=CHECK-NONRES
// RUN: %target-build-swift -module-name=Lib -package-name Pkg -I%t -emit-silgen %t/Lib.swift -enable-library-evolution -o %t/Lib-res.sil
// RUN: %FileCheck %s < %t/Lib-res.sil
// RUN: %target-build-swift -module-name=Lib -package-name Pkg -I%t -emit-silgen %t/Lib.swift -enable-library-evolution -O -wmo | %FileCheck %s
// RUN: %target-build-swift -module-name=Lib -package-name Pkg -I%t -emit-silgen %t/Lib.swift -enable-library-evolution -Xfrontend -experimental-allow-non-resilient-access -Xfrontend -experimental-package-cmo -O -wmo | %FileCheck %s

// RUN: %target-build-swift -module-name=Lib -package-name Pkg -I%t -emit-module %t/Lib.swift -enable-library-evolution -Xfrontend -experimental-allow-non-resilient-access -Xfrontend -experimental-package-cmo -O -wmo -o %t/Lib.swiftmodule
// RUN: %target-build-swift -module-name=Client -package-name Pkg -I%t -emit-silgen %t/Client.swift -I %t | %FileCheck %s --check-prefix=CHECK-CLIENT

//--- Client.swift
import Lib
public func client() {
  /// Should not be calling S.x.unsafeMutableAddressor when accessing resilient global var;
  /// instead, should be calling the opaque getter.
  // CHECK-CLIENT-NOT: s3Lib1SV1xSSvau
  // CHECK-CLIENT: function_ref @$s3Lib1SV1xSSvgZ
  print(S.x)
}

//--- Lib.swift
public struct S {
  public static var x = "hello world"
}

// one-time initialization token for x
// CHECK-NONRES: sil_global private @$s3Lib1SV1x_Wz : $Builtin.Word
// CHECK: sil_global private @$s3Lib1SV1x_Wz : $Builtin.Word

// static S.x
// CHECK-NONRES: sil_global @$s3Lib1SV1xSSvpZ : $String
// CHECK: sil_global private @$s3Lib1SV1xSSvpZ : $String

// one-time initialization function for x
// CHECK-NONRES: sil private [global_init_once_fn] [ossa] @$s3Lib1SV1x_WZ : $@convention(c) (Builtin.RawPointer) -> () {
// CHECK: sil private [global_init_once_fn] [ossa] @$s3Lib1SV1x_WZ : $@convention(c) (Builtin.RawPointer) -> () {

// S.x.unsafeMutableAddressor
// CHECK-NONRES: sil [global_init] [ossa] @$s3Lib1SV1xSSvau : $@convention(thin) () -> Builtin.RawPointer {
// CHECK: sil hidden [global_init] [ossa] @$s3Lib1SV1xSSvau : $@convention(thin) () -> Builtin.RawPointer {
// CHECK: global_addr @$s3Lib1SV1x_Wz
// CHECK: address_to_pointer
// function_ref one-time initialization function for x
// CHECK: function_ref @$s3Lib1SV1x_WZ
// CHECK: global_addr @$s3Lib1SV1xSSvpZ
// CHECK: address_to_pointer

// static S.x.getter
// CHECK-NONRES: sil [transparent] [serialized] [ossa] @$s3Lib1SV1xSSvgZ : $@convention(method) (@thin S.Type) -> @owned String {
// CHECK: sil [ossa] @$s3Lib1SV1xSSvgZ : $@convention(method) (@thin S.Type) -> @owned String {
