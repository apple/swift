// RUN: %target-run-eager-swift
// REQUIRES: executable_test
//
// FIXME: `XORTraining` segfaults with `-O`, possibly due to AD refcounting bugs.
// UNSUPPORTED: swift_test_mode_optimize
//
// Machine learning API AD runtime tests.

import TensorFlow
import StdlibUnittest
import TensorFlowUnittest

var ModelADTests = TestSuite("ModelAD")

ModelADTests.testAllBackends("SimpleLayerAD") {
  let ones = Tensor<Float>(ones: [2, 2])
  let dense = Dense<Float>(inputSize: 2, outputSize: 2, activation: { $0 })
  let grad = gradient(at: dense) { dense in
    dense.applied(to: ones).sum()
  }
  expectEqual([[2, 2], [2, 2]], grad.weight)
  expectEqual([2, 2], grad.bias)
}

ModelADTests.testAllBackends("XORTraining") {
  struct Classifier: Layer {
    var l1, l2: Dense<Float>
    init(hiddenSize: Int) {
        l1 = Dense<Float>(inputSize: 2, outputSize: hiddenSize, activation: relu)
        l2 = Dense<Float>(inputSize: hiddenSize, outputSize: 1, activation: relu)
    }
    @differentiable(wrt: (self, input))
    func applied(to input: Tensor<Float>) -> Tensor<Float> {
        let h1 = l1.applied(to: input)
        return l2.applied(to: h1)
    }
  }
  var classifier = Classifier(hiddenSize: 4)
  let optimizer = SGD<Classifier, Float>()
  let x: Tensor<Float> = [[0, 0], [0, 1], [1, 0], [1, 1]]
  let y: Tensor<Float> = [0, 1, 1, 0]
  for _ in 0..<1000 {
      let (loss, 𝛁model) = classifier.valueWithGradient { classifier -> Tensor<Float> in
          let ŷ = classifier.applied(to: x)
          return meanSquaredError(predicted: ŷ, expected: y)
      }
      optimizer.update(&classifier.allDifferentiableVariables, along: 𝛁model)
  }
  print(classifier.applied(to: [[0, 0], [0, 1], [1, 0], [1, 1]]))
}

runAllTests()
