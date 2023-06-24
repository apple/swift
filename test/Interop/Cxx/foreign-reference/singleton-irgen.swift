// RUN: %target-swift-emit-ir %use_no_opaque_pointers %s -I %S/Inputs -enable-experimental-cxx-interop -validate-tbd-against-ir=none -disable-llvm-verify -Xcc -fignore-exceptions -disable-availability-checking | %FileCheck %s
// RUN: %target-swift-emit-ir %s -I %S/Inputs -enable-experimental-cxx-interop -validate-tbd-against-ir=none -disable-llvm-verify -Xcc -fignore-exceptions -disable-availability-checking
//
// XFAIL: OS=linux-android, OS=linux-androideabi

import Singleton

// TODO: this should not be opaque.
// CHECK: %TSo21DeletedSpecialMembersV = type opaque
// CHECK: %struct.DeletedSpecialMembers = type { i32 }

// CHECK-LABEL: define {{.*}}swiftcc void @"$s4main4testyyF"

// CHECK: [[X:%.*]] = alloca %TSo21DeletedSpecialMembersV*
// CHECK: [[TMP:%.*]] = alloca %TSo21DeletedSpecialMembersV*

// CHECK: [[CREATED:%.*]] = call %struct.DeletedSpecialMembers* @{{_ZN21DeletedSpecialMembers6createEv|"\?create\@DeletedSpecialMembers\@\@SAPEAU1\@XZ"}}()
// CHECK: [[SWIFT_CREATED:%.*]] = bitcast %struct.DeletedSpecialMembers* [[CREATED]] to %TSo21DeletedSpecialMembersV*
// CHECK: store %TSo21DeletedSpecialMembersV* [[SWIFT_CREATED]], %TSo21DeletedSpecialMembersV** [[X]]
// CHECK: store %TSo21DeletedSpecialMembersV* [[SWIFT_CREATED]], %TSo21DeletedSpecialMembersV** [[TMP]]

// CHECK: [[TMP_LOAD:%.*]] = load %TSo21DeletedSpecialMembersV*, %TSo21DeletedSpecialMembersV** [[TMP]]
// CHECK: [[CLANG_CREATED:%.*]] = bitcast %TSo21DeletedSpecialMembersV* [[TMP_LOAD]] to %struct.DeletedSpecialMembers*
// CHECK: call i32 @{{_ZNK21DeletedSpecialMembers4testEv|"\?test\@DeletedSpecialMembers\@\@QEBAHXZ"}}(%struct.DeletedSpecialMembers* [[CLANG_CREATED]])

// CHECK: [[FROM:%.*]] = bitcast %TSo21DeletedSpecialMembersV* [[SWIFT_CREATED]] to %struct.DeletedSpecialMembers*
// CHECK: call void @{{_Z8mutateItR21DeletedSpecialMembers|"\?mutateIt\@\@YAXAEAUDeletedSpecialMembers\@\@\@Z"}}(%struct.DeletedSpecialMembers* [[FROM]])

// CHECK: ret void

public func test() {
  var x = DeletedSpecialMembers.create()
  _ = x.test()
  mutateIt(x)
}
