// RUN: %target-swift-frontend -emit-sil -verify %s | %FileCheck %s
import _Differentiation

public struct NonDiffableStruct {}

public struct DiffableStruct : Differentiable {}

@differentiable(reverse)
func test1(x: Int, y: inout NonDiffableStruct, z: Float) -> Float { return 42.0 }

@differentiable(reverse)
func test2(x: Int, y: inout DiffableStruct, z: Float) {  }

@differentiable(reverse)
func test3(x: Int, y: inout DiffableStruct, z: Float) -> (Float, Float) { return (42.0, 42.0) }

@differentiable(reverse, wrt: y)
func test4(x: Int, y: inout DiffableStruct, z: Float) -> Void { }

@differentiable(reverse, wrt: (y, z))
func test5(x: Int, y: inout DiffableStruct, z: Float) -> Void { }

@differentiable(reverse, wrt: (y, z))
func test6(x: Int, y: inout DiffableStruct, z: Float) -> (Float, Float) { return (42.0, 42.0) }

// CHECK-LABEL: differentiability witness for test1(x:y:z:)
// CHECK: sil_differentiability_witness hidden [reverse] [parameters 2] [results 0] @$s31inout_differentiability_witness5test11x1y1zSfSi_AA17NonDiffableStructVzSftF : $@convention(thin) (Int, @inout NonDiffableStruct, Float) -> Float {
// CHECK:   jvp: @$s31inout_differentiability_witness5test11x1y1zSfSi_AA17NonDiffableStructVzSftFTJfUUSpSr : $@convention(thin) (Int, @inout NonDiffableStruct, Float) -> (Float, @owned @callee_guaranteed (Float) -> Float)
// CHECK:   vjp: @$s31inout_differentiability_witness5test11x1y1zSfSi_AA17NonDiffableStructVzSftFTJrUUSpSr : $@convention(thin) (Int, @inout NonDiffableStruct, Float) -> (Float, @owned @callee_guaranteed (Float) -> Float)
// CHECK: }

// CHECK-LABEL: differentiability witness for test2(x:y:z:)
// CHECK: sil_differentiability_witness hidden [reverse] [parameters 1 2] [results 0] @$s31inout_differentiability_witness5test21x1y1zySi_AA14DiffableStructVzSftF : $@convention(thin) (Int, @inout DiffableStruct, Float) -> () {
// CHECK:   jvp: @$s31inout_differentiability_witness5test21x1y1zySi_AA14DiffableStructVzSftFTJfUSSpSr : $@convention(thin) (Int, @inout DiffableStruct, Float) -> @owned @callee_guaranteed (@inout DiffableStruct.TangentVector, Float) -> ()
// CHECK:   vjp: @$s31inout_differentiability_witness5test21x1y1zySi_AA14DiffableStructVzSftFTJrUSSpSr : $@convention(thin) (Int, @inout DiffableStruct, Float) -> @owned @callee_guaranteed (@inout DiffableStruct.TangentVector) -> Float
// CHECK: }

// CHECK-LABEL: differentiability witness for test3(x:y:z:)
// CHECK: sil_differentiability_witness hidden [reverse] [parameters 1 2] [results 0 1 2] @$s31inout_differentiability_witness5test31x1y1zSf_SftSi_AA14DiffableStructVzSftF : $@convention(thin) (Int, @inout DiffableStruct, Float) -> (Float, Float) {
// CHECK:   jvp: @$s31inout_differentiability_witness5test31x1y1zSf_SftSi_AA14DiffableStructVzSftFTJfUSSpSSSr : $@convention(thin) (Int, @inout DiffableStruct, Float) -> (Float, Float, @owned @callee_guaranteed (@inout DiffableStruct.TangentVector, Float) -> (Float, Float))
// CHECK:   vjp: @$s31inout_differentiability_witness5test31x1y1zSf_SftSi_AA14DiffableStructVzSftFTJrUSSpSSSr : $@convention(thin) (Int, @inout DiffableStruct, Float) -> (Float, Float, @owned @callee_guaranteed (Float, Float, @inout DiffableStruct.TangentVector) -> Float)
// CHECK: }

// CHECK-LABEL: differentiability witness for test4(x:y:z:)
// CHECK: sil_differentiability_witness hidden [reverse] [parameters 1] [results 0] @$s31inout_differentiability_witness5test41x1y1zySi_AA14DiffableStructVzSftF : $@convention(thin) (Int, @inout DiffableStruct, Float) -> () {
// CHECK:   jvp: @$s31inout_differentiability_witness5test41x1y1zySi_AA14DiffableStructVzSftFTJfUSUpSr : $@convention(thin) (Int, @inout DiffableStruct, Float) -> @owned @callee_guaranteed (@inout DiffableStruct.TangentVector) -> ()
// CHECK:   vjp: @$s31inout_differentiability_witness5test41x1y1zySi_AA14DiffableStructVzSftFTJrUSUpSr : $@convention(thin) (Int, @inout DiffableStruct, Float) -> @owned @callee_guaranteed (@inout DiffableStruct.TangentVector) -> ()
// CHECK: }

// CHECK-LABEL: differentiability witness for test5(x:y:z:)
// CHECK: sil_differentiability_witness hidden [reverse] [parameters 1 2] [results 0] @$s31inout_differentiability_witness5test51x1y1zySi_AA14DiffableStructVzSftF : $@convention(thin) (Int, @inout DiffableStruct, Float) -> () {
// CHECK:   jvp: @$s31inout_differentiability_witness5test51x1y1zySi_AA14DiffableStructVzSftFTJfUSSpSr : $@convention(thin) (Int, @inout DiffableStruct, Float) -> @owned @callee_guaranteed (@inout DiffableStruct.TangentVector, Float) -> ()
// CHECK:   vjp: @$s31inout_differentiability_witness5test51x1y1zySi_AA14DiffableStructVzSftFTJrUSSpSr : $@convention(thin) (Int, @inout DiffableStruct, Float) -> @owned @callee_guaranteed (@inout DiffableStruct.TangentVector) -> Float
// CHECK: }

// CHECK-LABEL: differentiability witness for test6(x:y:z:)
// CHECK: sil_differentiability_witness hidden [reverse] [parameters 1 2] [results 0 1 2] @$s31inout_differentiability_witness5test61x1y1zSf_SftSi_AA14DiffableStructVzSftF : $@convention(thin) (Int, @inout DiffableStruct, Float) -> (Float, Float) {
// CHECK:   jvp: @$s31inout_differentiability_witness5test61x1y1zSf_SftSi_AA14DiffableStructVzSftFTJfUSSpSSSr : $@convention(thin) (Int, @inout DiffableStruct, Float) -> (Float, Float, @owned @callee_guaranteed (@inout DiffableStruct.TangentVector, Float) -> (Float, Float))
// CHECK:   vjp: @$s31inout_differentiability_witness5test61x1y1zSf_SftSi_AA14DiffableStructVzSftFTJrUSSpSSSr : $@convention(thin) (Int, @inout DiffableStruct, Float) -> (Float, Float, @owned @callee_guaranteed (Float, Float, @inout DiffableStruct.TangentVector) -> Float)
// CHECK: }
