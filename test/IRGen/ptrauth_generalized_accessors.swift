// RUN: %swift -swift-version 5 -target arm64e-apple-ios12.0 -parse-stdlib %s -emit-ir -disable-llvm-optzns -o - | %FileCheck %s --check-prefix=CHECK

// REQUIRES: CODEGENERATOR=ARM

// REQUIRES: CPU=arm64e
// REQUIRES: OS=ios

// Skipping one accessor: Value -> C

// CHECK:       [[PROTOTYPE_HOLDER_INOUT_VALUE:@"\$s29ptrauth_generalized_accessors6HolderVAA5ValueVIetMAlYl_TC.ptrauth"]] =
// CHECK-SAME:     private constant { i8*, i32, i64, i64 } { i8* bitcast (void (i8*, i1)* @"$s29ptrauth_generalized_accessors6HolderVAA5ValueVIetMAlYl_TC" to i8*), i32 0, i64 1, i64 [[PTRAUTH_INOUT:3909]] }, section "llvm.ptrauth"
// CHECK:       [[PROTOTYPE_HOLDER_BORROWED_VALUE:@"\$s29ptrauth_generalized_accessors6HolderVAA5ValueVIetMAgYg_TC.ptrauth"]] =
// CHECK-SAME:     private constant { i8*, i32, i64, i64 } { i8* bitcast (void (i8*, i1)* @"$s29ptrauth_generalized_accessors6HolderVAA5ValueVIetMAgYg_TC" to i8*), i32 0, i64 1, i64 [[PTRAUTH_BORROWED_VALUE:51173]] }, section "llvm.ptrauth"
// CHECK:       [[PROTOTYPE_HOLDER_INOUT_VALUE_1:@"\$s29ptrauth_generalized_accessors6HolderVAA5ValueVIetMAlYl_TC.ptrauth.1"]] =
// CHECK-SAME:     private constant { i8*, i32, i64, i64 } { i8* bitcast (void (i8*, i1)* @"$s29ptrauth_generalized_accessors6HolderVAA5ValueVIetMAlYl_TC" to i8*), i32 0, i64 1, i64 [[PTRAUTH_INOUT]] }, section "llvm.ptrauth"

// Skipping one accessor: Owner -> Holder

// CHECK:       [[PROTOTYPE_OPAQUEOWNER_INOUT_HOLDER:@"\$s29ptrauth_generalized_accessors11OpaqueOwnerVAA6HolderVIetMAlYl_TC.ptrauth"]] =
// CHECK-SAME:     private constant { i8*, i32, i64, i64 } { i8* bitcast (void (i8*, i1)* @"$s29ptrauth_generalized_accessors11OpaqueOwnerVAA6HolderVIetMAlYl_TC" to i8*), i32 0, i64 1, i64 [[PTRAUTH_INOUT]] }, section "llvm.ptrauth"

// Other stuff...

// CHECK:       [[PROTOTYPE_OPAQUEOWNER_OPAQUE_INOUT_HOLDER:@"\$sxq_r0_ly29ptrauth_generalized_accessors11OpaqueOwnerVAA6HolderVIsetWAlYl_TC.ptrauth"]] =
// CHECK-SAME:     private constant { i8*, i32, i64, i64 } { i8* bitcast (void (i8*, i1)* @"$sxq_r0_ly29ptrauth_generalized_accessors11OpaqueOwnerVAA6HolderVIsetWAlYl_TC" to i8*), i32 0, i64 1, i64 [[PTRAUTH_INOUT]] }, section "llvm.ptrauth"
// CHECK:       [[PROTOTYPE_OPAQUEOWNER_OPAQUE_BORROWED_HOLDER:@"\$sxq_r0_ly29ptrauth_generalized_accessors11OpaqueOwnerVAA6HolderVIsetWAnYn_TC.ptrauth"]] =
// CHECK-SAME:     private constant { i8*, i32, i64, i64 } { i8* bitcast (void (i8*, i1)* @"$sxq_r0_ly29ptrauth_generalized_accessors11OpaqueOwnerVAA6HolderVIsetWAnYn_TC" to i8*), i32 0, i64 1, i64 [[PTRAUTH_OPAQUE:56769]] }, section "llvm.ptrauth"
// CHECK:       [[PROTOTYPE_OPAQUEOWNER_BORROWED_HOLDER:@"\$s29ptrauth_generalized_accessors11OpaqueOwnerVAA6HolderVIetMAgYg_TC.ptrauth"]] =
// CHECK-SAME:     private constant { i8*, i32, i64, i64 } { i8* bitcast (void (i8*, i1)* @"$s29ptrauth_generalized_accessors11OpaqueOwnerVAA6HolderVIetMAgYg_TC" to i8*), i32 0, i64 1, i64 [[PTRAUTH_BORROWED_HOLDER:11564]] }, section "llvm.ptrauth"
// CHECK:       [[PROTOTYPE_OWNER_INOUT_HOLDER:@"\$sx29ptrauth_generalized_accessors6HolderVlyAA5OwnerVIsetWAlYl_TC.ptrauth"]] =
// CHECK-SAME:     private constant { i8*, i32, i64, i64 } { i8* bitcast (void (i8*, i1)* @"$sx29ptrauth_generalized_accessors6HolderVlyAA5OwnerVIsetWAlYl_TC" to i8*), i32 0, i64 1, i64 [[PTRAUTH_INOUT]] }, section "llvm.ptrauth"

// Note: This one is slightly different from most of the other checks as `self` has to be taken indirectly but the yield value doesn't, so the param is @in_guaranteed but the return is @guaranteed.

// CHECK:       [[PROTOTYPE_OWNER_BORROWED_HOLDER:@"\$sx29ptrauth_generalized_accessors6HolderVlyAA5OwnerVIsetWAnYg_TC.ptrauth"]] =
// CHECK-SAME:     private constant { i8*, i32, i64, i64 } { i8* bitcast (void (i8*, i1)* @"$sx29ptrauth_generalized_accessors6HolderVlyAA5OwnerVIsetWAnYg_TC" to i8*), i32 0, i64 1, i64 [[PTRAUTH_BORROWED_HOLDER]] }, section "llvm.ptrauth"

public class C {}

public struct Value {
  public var c: C
  func use() {}
  mutating func mutate() {}
}

//   Concrete accessor.
// CHECK-LABEL: define {{.*}} @"$s29ptrauth_generalized_accessors6HolderV5valueAA5ValueVvr"
// CHECK:         call token @llvm.coro.id.retcon.once
// CHECK-SAME:      [[PROTOTYPE_HOLDER_BORROWED_VALUE]]
// CHECK-LABEL: define {{.*}} @"$s29ptrauth_generalized_accessors6HolderV5valueAA5ValueVvM"
// CHECK:         call token @llvm.coro.id.retcon.once
// CHECK-SAME:      [[PROTOTYPE_HOLDER_INOUT_VALUE_1]]
public struct Holder {
  public var _value: Value
  public var value: Value {
    _read { yield _value }
    _modify { yield &_value }
  }
}

public protocol Owning {
  @_borrowed
  var holder: Holder { get set }
}

public protocol OpaqueOwning {
  associatedtype Holding

  @_borrowed
  var holder: Holding { get set }
}

//   Thunks for Owning, which uses concrete types.
// CHECK-LABEL: define {{.*}} @"$s29ptrauth_generalized_accessors5OwnerVAA6OwningA2aDP6holderAA6HolderVvrTW"
// CHECK:         call token @llvm.coro.id.retcon.once
// CHECK-SAME:      [[PROTOTYPE_OWNER_BORROWED_HOLDER]]
// CHECK-LABEL: define {{.*}} @"$s29ptrauth_generalized_accessors5OwnerVAA6OwningA2aDP6holderAA6HolderVvMTW"
// CHECK:         call token @llvm.coro.id.retcon.once
// CHECK-SAME:      [[PROTOTYPE_OWNER_INOUT_HOLDER]]
public struct Owner: Owning {
  public var holder: Holder
}

//   Concrete accessor.
// CHECK-LABEL: define {{.*}} @"$s29ptrauth_generalized_accessors11OpaqueOwnerV6holderAA6HolderVvM"
// CHECK:         call token @llvm.coro.id.retcon.once
// CHECK-SAME:      [[PROTOTYPE_OPAQUEOWNER_INOUT_HOLDER]]
//   Thunks for OpaqueOwning, which uses abstract types.
// CHECK-LABEL: define {{.*}} @"$s29ptrauth_generalized_accessors11OpaqueOwnerVAA0D6OwningA2aDP6holder7HoldingQzvrTW"
// CHECK:         call token @llvm.coro.id.retcon.once
// CHECK-SAME:      [[PROTOTYPE_OPAQUEOWNER_OPAQUE_BORROWED_HOLDER]]
//   Concrete accessor.
// CHECK-LABEL: define {{.*}} @"$s29ptrauth_generalized_accessors11OpaqueOwnerV6holderAA6HolderVvr"
// CHECK:         call token @llvm.coro.id.retcon.once
// CHECK-SAME:      [[PROTOTYPE_OPAQUEOWNER_BORROWED_HOLDER]]
public struct OpaqueOwner: OpaqueOwning {
  public var holder: Holder
}

//   Thunks for OpaqueOwning, which uses abstract types.
// CHECK-LABEL: define {{.*}} @"$s29ptrauth_generalized_accessors11OpaqueOwnerVAA0D6OwningA2aDP6holder7HoldingQzvMTW"
// CHECK:         call token @llvm.coro.id.retcon.once
// CHECK-SAME:      [[PROTOTYPE_OPAQUEOWNER_OPAQUE_INOUT_HOLDER]]

// CHECK-LABEL: define {{.*}} @"$s29ptrauth_generalized_accessors17testInOutConcrete6holderyAA6HolderVz_tF"
// CHECK:         [[T0:%.*]] = alloca [32 x i8], align 8
// CHECK-NEXT:    [[BUFFER:%.*]] = getelementptr inbounds [32 x i8], [32 x i8]* [[T0]], i32 0, i32 0
// CHECK:         [[T0:%.*]] = call swiftcc { i8*, %T29ptrauth_generalized_accessors5ValueV* } {{.*}} [[BUFFER]]
// CHECK:         [[T1:%.*]] = extractvalue { i8*, %T29ptrauth_generalized_accessors5ValueV* } [[T0]], 0
// CHECK:         call swiftcc void @"$s29ptrauth_generalized_accessors5ValueV6mutateyyF"
// CHECK:         [[RESUME:%.*]] = bitcast i8* [[T1]] to void (i8*, i1)*
// CHECK-NEXT:    [[T0:%.*]] = ptrtoint i8* [[BUFFER]] to i64
// CHECK-NEXT:    [[DISCRIMINATOR:%.*]] = call i64 @llvm.ptrauth.blend.i64(i64 [[T0]], i64 [[PTRAUTH_INOUT]])
// CHECK-NEXT:    call swiftcc void [[RESUME]](i8* noalias dereferenceable(32) [[BUFFER]], i1 false) [ "ptrauth"(i32 0, i64 [[DISCRIMINATOR]]) ]
public func testInOutConcrete(holder: inout Holder) {
  holder.value.mutate()
}

// CHECK-LABEL: define {{.*}} @"$s29ptrauth_generalized_accessors20testBorrowedConcrete6holderyAA6HolderVz_tF"
// CHECK:         [[T0:%.*]] = alloca [32 x i8], align 8
// CHECK:         [[BUFFER:%.*]] = getelementptr inbounds [32 x i8], [32 x i8]* [[T0]], i32 0, i32 0
// CHECK:         [[T0:%.*]] = call swiftcc { i8*, %T29ptrauth_generalized_accessors1CC* } {{.*}} [[BUFFER]]
// CHECK:         [[T1:%.*]] = extractvalue { i8*, %T29ptrauth_generalized_accessors1CC* } [[T0]], 0
// CHECK:         [[RESUME:%.*]] = bitcast i8* [[T1]] to void (i8*, i1)*
// CHECK-NEXT:    [[T0:%.*]] = ptrtoint i8* [[BUFFER]] to i64
// CHECK-NEXT:    [[DISCRIMINATOR:%.*]] = call i64 @llvm.ptrauth.blend.i64(i64 [[T0]], i64 [[PTRAUTH_BORROWED_VALUE]])
// CHECK-NEXT:    call swiftcc void [[RESUME]](i8* noalias dereferenceable(32) [[BUFFER]], i1 false) [ "ptrauth"(i32 0, i64 [[DISCRIMINATOR]]) ]
public func testBorrowedConcrete(holder: inout Holder) {
  holder.value.use()
}

// CHECK-LABEL: define {{.*}} @"$s29ptrauth_generalized_accessors9testInOut5valueyxz_tAA6OwningRzlF"
// CHECK:         [[T0:%.*]] = alloca [32 x i8], align 8
// CHECK:         [[BUFFER:%.*]] = getelementptr inbounds [32 x i8], [32 x i8]* [[T0]], i32 0, i32 0
// CHECK:         [[T0:%.*]] = call swiftcc { i8*, %T29ptrauth_generalized_accessors6HolderV* } {{.*}} [[BUFFER]]
// CHECK:         [[T1:%.*]] = extractvalue { i8*, %T29ptrauth_generalized_accessors6HolderV* } [[T0]], 0
// CHECK:         call swiftcc void @"$s29ptrauth_generalized_accessors5ValueV6mutateyyF"
// CHECK:         [[RESUME:%.*]] = bitcast i8* [[T1]] to void (i8*, i1)*
// CHECK-NEXT:    [[T0:%.*]] = ptrtoint i8* [[BUFFER]] to i64
// CHECK-NEXT:    [[DISCRIMINATOR:%.*]] = call i64 @llvm.ptrauth.blend.i64(i64 [[T0]], i64 [[PTRAUTH_INOUT]])
// CHECK-NEXT:    call swiftcc void [[RESUME]](i8* noalias dereferenceable(32) [[BUFFER]], i1 false) [ "ptrauth"(i32 0, i64 [[DISCRIMINATOR]]) ]
public func testInOut<T: Owning>(value: inout T) {
  value.holder.value.mutate()
}

// CHECK-LABEL: define {{.*}} @"$s29ptrauth_generalized_accessors12testBorrowed5valueyxz_tAA6OwningRzlF"
// CHECK:         [[T0:%.*]] = alloca [32 x i8], align 8
// CHECK:         [[BUFFER:%.*]] = getelementptr inbounds [32 x i8], [32 x i8]* [[T0]], i32 0, i32 0
// CHECK:         [[T0:%.*]] = call swiftcc { i8*, %T29ptrauth_generalized_accessors1CC* } {{.*}} [[BUFFER]]
// CHECK:         [[T1:%.*]] = extractvalue { i8*, %T29ptrauth_generalized_accessors1CC* } [[T0]], 0
// CHECK:         [[RESUME:%.*]] = bitcast i8* [[T1]] to void (i8*, i1)*
// CHECK-NEXT:    [[T0:%.*]] = ptrtoint i8* [[BUFFER]] to i64
// CHECK-NEXT:    [[DISCRIMINATOR:%.*]] = call i64 @llvm.ptrauth.blend.i64(i64 [[T0]], i64 [[PTRAUTH_BORROWED_HOLDER]])
// CHECK-NEXT:    call swiftcc void [[RESUME]](i8* noalias dereferenceable(32) [[BUFFER]], i1 false) [ "ptrauth"(i32 0, i64 [[DISCRIMINATOR]]) ]
public func testBorrowed<T: Owning>(value: inout T) {
  value.holder.value.use()
}

// CHECK-LABEL: define {{.*}} @"$s29ptrauth_generalized_accessors15testOpaqueInOut5valueyxz_tAA0E6OwningRzAA6HolderV7HoldingRtzlF"
// CHECK:         [[T0:%.*]] = alloca [32 x i8], align 8
// CHECK:         [[BUFFER:%.*]] = getelementptr inbounds [32 x i8], [32 x i8]* [[T0]], i32 0, i32 0
// CHECK:         [[T0:%.*]] = call swiftcc { i8*, %swift.opaque* } {{.*}} [[BUFFER]]
// CHECK:         [[T1:%.*]] = extractvalue { i8*, %swift.opaque* } [[T0]], 0
// CHECK:         call swiftcc void @"$s29ptrauth_generalized_accessors5ValueV6mutateyyF"
// CHECK:         [[RESUME:%.*]] = bitcast i8* [[T1]] to void (i8*, i1)*
// CHECK-NEXT:    [[T0:%.*]] = ptrtoint i8* [[BUFFER]] to i64
// CHECK-NEXT:    [[DISCRIMINATOR:%.*]] = call i64 @llvm.ptrauth.blend.i64(i64 [[T0]], i64 [[PTRAUTH_INOUT]])
// CHECK-NEXT:    call swiftcc void [[RESUME]](i8* noalias dereferenceable(32) [[BUFFER]], i1 false) [ "ptrauth"(i32 0, i64 [[DISCRIMINATOR]]) ]
public func testOpaqueInOut<T: OpaqueOwning>(value: inout T)
    where T.Holding == Holder {
  value.holder.value.mutate()
}

// CHECK-LABEL: define {{.*}} @"$s29ptrauth_generalized_accessors18testOpaqueBorrowed5valueyxz_tAA0E6OwningRzAA6HolderV7HoldingRtzlF"
// CHECK:         [[T0:%.*]] = alloca [32 x i8], align 8
// CHECK:         [[BUFFER:%.*]] = getelementptr inbounds [32 x i8], [32 x i8]* [[T0]], i32 0, i32 0
// CHECK:         [[T0:%.*]] = call swiftcc { i8*, %swift.opaque* } {{.*}} [[BUFFER]]
// CHECK:         [[T1:%.*]] = extractvalue { i8*, %swift.opaque* } [[T0]], 0
// CHECK:         [[RESUME:%.*]] = bitcast i8* [[T1]] to void (i8*, i1)*
// CHECK-NEXT:    [[T0:%.*]] = ptrtoint i8* [[BUFFER]] to i64
// CHECK-NEXT:    [[DISCRIMINATOR:%.*]] = call i64 @llvm.ptrauth.blend.i64(i64 [[T0]], i64 [[PTRAUTH_OPAQUE]])
// CHECK-NEXT:    call swiftcc void [[RESUME]](i8* noalias dereferenceable(32) [[BUFFER]], i1 false) [ "ptrauth"(i32 0, i64 [[DISCRIMINATOR]]) ]
public func testOpaqueBorrowed<T: OpaqueOwning>(value: inout T)
    where T.Holding == Holder {
  value.holder.value.use()
}
