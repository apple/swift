// RUN: %target-swift-frontend -Xllvm -tf-dump-intermediates -Xllvm -tf-strict-deabstraction=false -O -emit-sil %s | %FileCheck %s
// RUN: %target-swift-frontend -Xllvm -tf-dump-intermediates -Xllvm -tf-strict-deabstraction -O -emit-sil -verify %s | %FileCheck %s -check-prefix=STRICTDA

import TensorFlow

// This test is intended to verify that all of the operations end up in the
// graph: that there are no host/accelerator copies generated.  This tests a
// combination of the partitioning pass being able to recognize various forms,
// but also checks that certain ops implementations are promotable as well.

// Please keep it so no errors or warnings are generated by functions in this
// file.


/* TODO build this out.
STRICTDA: --- TFDeabstraction Result: {{.*}}testSelect
*/
public func testSelect(conds1: Tensor<Bool>, x1: Tensor<Float>, y1: Tensor<Float>)
  -> Tensor<Float> {
  let conds = conds1.toAccelerator()
  let x = x1.toAccelerator()
  let y = y1.toAccelerator()

  let result = conds.selecting(x+x, y)*y

  return result.toHost()
}

/*
 CHECK-LABEL: --- TFPartition Accelerator Result: {{.*}}testSelect
 CHECK: sil private @{{.*}}testSelect{{.*}} : $@callee_owned (TensorHandle<Float>, TensorHandle<Bool>, TensorHandle<Float>) -> TensorHandle<Float> {
 CHECK: bb0(%0 : $TensorHandle<Float>, %1 : $TensorHandle<Bool>, %2 : $TensorHandle<Float>):
 CHECK:       %5 = builtin "__tfop_Add,$in,$in,T,__device"(%0 : $TensorHandle<Float>, %0 : $TensorHandle<Float>
 CHECK:       %8 = builtin "__tfop_Select,$in,$in,$in,T,__device"(%1 : $TensorHandle<Bool>, %5 : $TensorHandle<Float>, %2 : $TensorHandle<Float>
 CHECK:       %11 = builtin "__tfop_Mul,$in,$in,T,__device"(%8 : $TensorHandle<Float>, %2 : $TensorHandle<Float>
 CHECK-NEXT:  return %11 : $TensorHandle<Float>
 CHECK-NEXT:}
*/
/*
 STRICTDA-LABEL: --- TFPartition Accelerator Result: {{.*}}testSelect
 STRICTDA: sil private @{{.*}}testSelect{{.*}} : $@callee_owned (TensorHandle<Float>, TensorHandle<Bool>, TensorHandle<Float>) -> TensorHandle<Float> {
 STRICTDA: bb0(%0 : $TensorHandle<Float>, %1 : $TensorHandle<Bool>, %2 : $TensorHandle<Float>):
 STRICTDA:       [[A:%.*]] = graph_op "Add,i,i"(%0 : $TensorHandle<Float>, %0 : $TensorHandle<Float>
 STRICTDA:       [[B:%.*]] = graph_op "Select,i,i,i"(%1 : $TensorHandle<Bool>, [[A]] : $TensorHandle<Float>, %2 : $TensorHandle<Float>
 STRICTDA:      [[C:%.*]] = graph_op "Mul,i,i"([[B]] : $TensorHandle<Float>, %2 : $TensorHandle<Float>
 STRICTDA-NEXT:  return [[C]] : $TensorHandle<Float>
 STRICTDA-NEXT:}
*/

public func testEmptyScalarsArray() {
  let y = Tensor<Int32>(shape: [0, 20, 30], scalars: [])
  _ = y+y
}
/*
 CHECK-LABEL: --- TFPartition Accelerator Result: {{.*}}testEmptyScalarsArray
 CHECK: sil private @{{.*}}testEmptyScalarsArray{{.*}} : $@callee_owned () -> () {
 CHECK: bb0:
 CHECK: graph_op "Const"() {dtype: $Int32, value$tensor: [$Int32: ], value$shape: [$Int32: (i32 0), (i32 20), (i32 30)],
 CHECK: builtin "__tfop_Add,$in,$in,T,__device"({{.*}} : $TensorHandle<Int32>, {{.*}} : $TensorHandle<Int32>
 */
/*
 STRICTDA-LABEL: --- TFPartition Accelerator Result: {{.*}}testEmptyScalarsArray
 STRICTDA: sil private @{{.*}}testEmptyScalarsArray{{.*}} : $@callee_owned () -> () {
 STRICTDA: bb0:
 STRICTDA: graph_op "Const"() {dtype: $Int32, value$tensor: [$Int32: ], value$shape: [$Int32: (i32 0), (i32 20), (i32 30)],
 STRICTDA: graph_op "Add,i,i"({{.*}} : $TensorHandle<Int32>, {{.*}} : $TensorHandle<Int32>
 */

// This tests the attributes necessary to get arrays of integers and strings going.
public func testConvolution(x: Tensor<Float>, filter: Tensor<Float>) -> Tensor<Float> {
  return x.toAccelerator().convolved2D(withFilter: filter.toAccelerator(),
                       strides: (1, 2, 3, 4), padding: .same)
}

// CHECK-LABEL: --- TFPartition Accelerator Result: {{.*}}testConvolution
// CHECK: sil private @{{.*}}testConvolution{{.*}} : $@callee_owned (TensorHandle<Float>, TensorHandle<Float>) -> TensorHandle<Float> {
// CHECK: bb0(%0 : $TensorHandle<Float>, %1 : $TensorHandle<Float>):
// CHECK-NEXT:  %2 = metatype $@thick Float.Type
// CHECK-NEXT:  %3 = metatype $@thin Int32.Type
// CHECK-NEXT:  %4 = integer_literal $Builtin.Int32, 1
// CHECK-NEXT:  %5 = integer_literal $Builtin.Int32, 2
// CHECK-NEXT:  %6 = integer_literal $Builtin.Int32, 3
// CHECK-NEXT:  %7 = integer_literal $Builtin.Int32, 4
// CHECK-NEXT:  %8 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT:  %9 = string_literal utf8 "SAME"
// CHECK:       builtin "__tfop_Conv2D,$in,$in,T,strides$array,$elt,$elt,$elt,$elt,use_cudnn_on_gpu,padding,data_format,dilations$array,$elt,$elt,$elt,$elt,__device"(%0 : $TensorHandle<Float>, %1 : $TensorHandle<Float>, %2 : $@thick Float.Type, %3 : $@thin Int32.Type, %4 : $Builtin.Int32, %5 : $Builtin.Int32, %6 : $Builtin.Int32, %7 : $Builtin.Int32, %8 : $Builtin.Int1, %9 : $Builtin.RawPointer, %10 : $Builtin.RawPointer, %11 : $@thin Int32.Type, %12 : $Builtin.Int32, %13 : $Builtin.Int32, %14 : $Builtin.Int32, %15 : $Builtin.Int32, %16 : $Builtin.RawPointer) : $TensorHandle<Float>
// CHECK-NEXT:  return %17 : $TensorHandle<Float>
// CHECK-NEXT:}

// STRICTDA-LABEL: --- TFPartition Accelerator Result: {{.*}}testConvolution
// STRICTDA: sil private @{{.*}}testConvolution{{.*}} : $@callee_owned (TensorHandle<Float>, TensorHandle<Float>) -> TensorHandle<Float> {
// STRICTDA: bb0(%0 : $TensorHandle<Float>, %1 : $TensorHandle<Float>):
// STRICTDA: [[A:%.*]] = graph_op "Conv2D,i,i"(%0 : $TensorHandle<Float>, %1 : $TensorHandle<Float>) {T: $Float, strides: [$Int32: (i32 1), (i32 2), (i32 3), (i32 4)], use_cudnn_on_gpu: i1 -1, padding: "SAME", data_format: "NHWC", dilations: [$Int32: (i32 1), (i32 1), (i32 1), (i32 1)], __device: "/device:CPU:0"} : $TensorHandle<Float> 
// STRICTDA-NEXT:  return [[A]] : $TensorHandle<Float>
// STRICTDA-NEXT:}

// Testcase for an op that uses the $tensor and $shape modifiers.
public func testConstantArray() -> TensorHandle<Float> {
  return #tfop("Const", dtype: Float.self, value$tensor: [1.0, 2.0], value$shape: [2])
}

// CHECK-LABEL: --- TFPartition Accelerator Result: {{.*}}testConstantArray
// CHECK: sil private @{{.*}}testConstantArray{{.*}} : $@callee_owned () -> TensorHandle<Float> {
// CHECK: bb0:
// CHECK-NEXT:  %0 = metatype $@thin Float.Type
// CHECK-NEXT:  %1 = metatype $@thin Double.Type
// CHECK-NEXT:  %2 = float_literal $Builtin.FPIEEE64, 0x3FF0000000000000 // 1
// CHECK-NEXT:  %3 = float_literal $Builtin.FPIEEE64, 0x4000000000000000 // 2
// CHECK-NEXT:  %4 = metatype $@thin Int.Type
// CHECK-NEXT:  %5 = integer_literal $Builtin.Int64, 2
// CHECK:       %7 = builtin "__tfop_Const,dtype,value$tensor,$elt,$elt,value$shape,$elt,__device"(%0 : $@thin Float.Type, %1 : $@thin Double.Type, %2 : $Builtin.FPIEEE64, %3 : $Builtin.FPIEEE64, %4 : $@thin Int.Type, %5 : $Builtin.Int64
// CHECK-NEXT:  return %7 : $TensorHandle<Float>

// STRICTDA-LABEL: --- TFPartition Accelerator Result: {{.*}}testConstantArray
// STRICTDA: sil private @{{.*}}testConstantArray{{.*}} : $@callee_owned () -> TensorHandle<Float> {
// STRICTDA: bb0:
// STRICTDA:  %0 = graph_op "Const"() {dtype: $Float, value$tensor: [$Double: (f64 0x3FF0000000000000 /* 1 */), (f64 0x4000000000000000 /* 2 */)], value$shape: [$Int: (i64 2)], __device: "/device:CPU:0"} : $TensorHandle<Float> 
// STRICTDA-NEXT:  return %0 : $TensorHandle<Float>

// Sigmoid shouldn't cause copies.  This should compile with no copy warnings/errors.
public func testSigmoid(x: Tensor<Float>, y: Tensor<Float>) -> (Tensor<Float>, Tensor<Float>) {
  let a = sigmoid(x.toAccelerator())
  let b = sigmoid(y.toAccelerator()).toHost()
  // FIXME: b/76177896 the toHost() call should be movable up.
  return (a.toHost(), b)
}

// Likewise, mean and max shouldn't cause send/receive errors.
public func testMeanMax(x: Tensor<Float>) -> Float {
  let y = x.toAccelerator()
  let a = y.mean()
  let b = y.max()
  return a+b
}

public func testZeros() -> Tensor<Float> {
  let b1 = Tensor<Float>(zeros: [1, 4])
  let b2 = Tensor<Float>(zeros: [1, 4])
  return (b1+b2).toHost()
}

// Verify that we are able to run randomUniform on the accelerator, or at least hoist
// it to being an argument so we don't get copy-ins.
public func randomUniformHoisting() -> Tensor<Float> {
  let x = Tensor<Float>(ones: [2, 2, 2])
  let y = Tensor<Float>(randomUniform: [2, 2, 2])
  let z = Tensor<Float>(randomUniform: [2, 2, 2])

  return (x+y+z).toHost()
}

// Here ".mean()" contains a tensor2scalar operation, and we then convert that
// scalar back to a tensor.  This checks to make sure that tf-partition can pull
// this whole mess in graph without leaving anything on the host that will cause
// a send/receive.
public func tensorToScalarToTensor(a: Tensor<Int32>) -> Tensor<Int32> {
  let scalar = a.toAccelerator().mean()
  let b = Tensor(scalar)
  return (b+b).toHost()
}


// The tensor value inside the loop was getting copied back to the host because of
// the use by a branch instruction.  b/75494462
public func test75494462() {
  var x = Tensor<Float>(1)
  var i: Int32 = 1
  repeat {
    x += 1
    i += 1
  } while i < 5
  print(x.array)
}

public func paddingTuplesHoistable() {
  let matrix: Tensor<Float> = Tensor([[1, 2, 3], [4, 5, 6]]) + 1
  let padded = matrix.padded(forSizes: [(before: 1, after: 1), (before: 2, after: 2)]).toAccelerator()
  _ = padded.array
}

// b/76184126
public func rangeLiteral() -> Tensor<Float> {
  var x = Tensor<Float>(33)
  for _ in 1...10 {
    x += 1
  }
  return x.toHost()
}

/// b/76222306
struct Classifier {
  // Parameters
  var w1 = Tensor<Float>(randomUniform: [784, 30])
  var w2 = Tensor<Float>(randomUniform: [30, 10])
  var b1 = Tensor<Float>(zeros: [1, 30])
  var b2 = Tensor<Float>(zeros: [1, 10])

  func prediction(for input: Tensor<Float>) -> Tensor<Float> {
    let h1 = sigmoid(input • w1 + b1)
    return sigmoid(h1 • w2 + b2)
  }

  mutating func train(images: Tensor<Float>, labels: Tensor<Float>,
                      learningRate: Float, epochCount: Int) -> Float {
    var loss: Float
    var epochCount = epochCount
    repeat {
      // Forward pass
      let z1 = images • w1 + b1
      let h1 = sigmoid(z1)
      let z2 = h1 • w2 + b2
      let pred = sigmoid(z2)

      // Backward pass
      let dz2 = pred - labels
      let dw2 = h1.transposed(withPermutations: 1, 0) • dz2
      let db2 = dz2.sum(squeezingAxes: 0)
      let dz1 = matmul(dz2, w2.transposed(withPermutations: 1, 0)) * h1 * (1 - h1)
      let dw1 = images.transposed(withPermutations: 1, 0) • dz1
      let db1 = dz1.sum(squeezingAxes: 0)

      // Gradient descent
      w1 -= dw1 * learningRate
      b1 -= db1 * learningRate
      w2 -= dw2 * learningRate
      b2 -= db2 * learningRate

      loss = dz2.squared().mean(squeezingAxes: 1, 0).scalarized()

      epochCount -= 1
    } while epochCount > 0

    return loss
  }
}

public func mnist() {
  // Training data
  let images = Tensor<Float>(randomNormal: [10, 784])
  let labels = Tensor<Float>(randomNormal: [10, 10])
  var classifier = Classifier()
  let loss = classifier.train(images: images, labels: labels,
                              learningRate: 0.3, epochCount: 100)
  print(loss)
}

// A TF op that produces multiple outputs.
public func testMultiOutputs() {
  let d = Tensor<Float>(0.0)
  // FIXME: Support promoting scalar false to a Tensor<Bool>
  let c = Tensor<Bool>(false)
  let (x1, y1): (TensorHandle<Float>, TensorHandle<Float>) = #tfop("Switch", d, c)
  // FIXME: Remove the uses of Identity nodes here.
  let x: TensorHandle<Float> = #tfop("Identity", x1)
  let y: TensorHandle<Float> = #tfop("Identity", y1)
  _hostOp(x)
  _hostOp(y)
}

// TODO: Eliminate the sends/recvs when -Onone is enabled.
public func SR8395() {
  let x: Tensor<Float> = [[1, 2], [3, 4]]
  print(matmul(x, x) + x)
}
