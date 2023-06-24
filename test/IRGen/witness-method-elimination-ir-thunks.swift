// Tests that under -enable-llvm-wme, protocol witness table calls to protocols
// defined by other modules are using thunks (instead of direct wtable loads).

// RUN: %empty-directory(%t)
// RUN: %target-build-swift -Xfrontend -enable-llvm-wme -parse-as-library %s -DLIBRARY -module-name Library -emit-module -o %t/Library.swiftmodule
// RUN: %target-build-swift -Xfrontend -enable-llvm-wme -parse-as-library %s -DCLIENT -module-name Main -I%t -emit-ir -o - | %FileCheck %s

#if LIBRARY

public protocol MyLibraryProtocol {
  func library_req()
}

#endif

#if CLIENT

import Library

public protocol MyLocalProtocol {
  func local_req()
}

extension MyLocalProtocol {
  func func1() {
    // CHECK: define hidden swiftcc void @"$s4Main15MyLocalProtocolPAAE5func1yyF"
    self.local_req()
    // CHECK: [[SLOT:%.*]] = getelementptr inbounds ptr, ptr {{.*}}, i32 1
    // CHECK: call { ptr, i1 } @llvm.type.checked.load(ptr [[SLOT]], i32 0, metadata !"$s4Main15MyLocalProtocolP9local_reqyyFTq")
    // CHECK: ret void
  }
}

extension MyLibraryProtocol {
  func func2() {
    // CHECK: define hidden swiftcc void @"$s7Library02MyA8ProtocolP4MainE5func2yyF"
    self.library_req()
    // CHECK: call swiftcc void @"$s7Library02MyA8ProtocolP11library_reqyyFTj"

    // CHECK-NOT: @llvm.type.checked.load
    // CHECK: ret void
  }
}

#endif
