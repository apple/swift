// RUN: %target-swift-frontend -module-name objc_retainAutoreleasedReturnValue -import-objc-header %S/Inputs/StaticInline.h %s -emit-ir | %FileCheck %s
// RUN: %target-swift-frontend -module-name objc_retainAutoreleasedReturnValue -O -import-objc-header %S/Inputs/StaticInline.h %s -emit-ir -disable-llvm-optzns | %FileCheck %s --check-prefix=OPT

// REQUIRES: objc_interop
// REQUIRES: CPU=x86_64
// REQUIRES: OS=macosx
import Foundation

@inline(never)
public func useClosure(_ dict: NSDictionary, _ action : (NSDictionary) -> ()) {
  action(dict)
}

@inline(never)
public func test(_ dict: NSDictionary) {
  useClosure(dict, { $0.objectEnumerator()} )
}

//  Don't tail call objc_retainAutoreleasedReturnValue as this would block the
//  autorelease return value optimization.

// callq  0x01ec08 ; symbol stub for: objc_msgSend
// movq   %rax, %rdi
// popq   %rbp  ;<== Blocks the handshake from objc_autoreleaseReturnValue
// jmp    0x01ec20 ; symbol stub for: objc_retainAutoreleasedReturnValue

// CHECK-LABEL: define {{.*}}swiftcc void @"$s34objc_retainAutoreleasedReturnValue4testyySo12NSDictionaryCFyADXEfU_"(ptr %0)
// CHECK: entry:
// CHECK:   call {{.*}}@objc_msgSend
// CHECK:   notail call ptr @llvm.objc.retainAutoreleasedReturnValue
// CHECK:   ret void

// OPT-LABEL: define {{.*}}swiftcc void @"$s34objc_retainAutoreleasedReturnValue10useClosureyySo12NSDictionaryC_yADXEtF09$s34objc_bcd16Value4testyySo12H10CFyADXEfU_Tf1nc_n"(ptr %0)
// OPT: entry:
// OPT:   [[R:%.*]] = call ptr @objc_msgSend({{.*}}) [ "clang.arc.attachedcall"(ptr @llvm.objc.retainAutoreleasedReturnValue) 
// OPT:   call void (...) @llvm.objc.clang.arc.noop.use(ptr [[R]])
// OPT:   ret void
