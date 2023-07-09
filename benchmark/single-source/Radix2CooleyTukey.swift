// Radix2CooleyTukey benchmark
//
// Originally written by @owensd. Used with his permission.

#if canImport(Glibc)
import Glibc
#elseif canImport(Musl)
import Musl
#elseif os(Windows)
import MSVCRT
#else
import Darwin
#endif
import TestsUtils

public let benchmarks = [
  BenchmarkInfo(
    name: "Radix2CooleyTukey",
    runFunction: run_Radix2CooleyTukey,
    tags: [.validation, .algorithm],
    setUpFunction: setUpRadix2CooleyTukey,
    tearDownFunction: tearDownRadix2CooleyTukey,
    legacyFactor: 48),
  BenchmarkInfo(
    name: "Radix2CooleyTukeyf",
    runFunction: run_Radix2CooleyTukeyf,
    tags: [.validation, .algorithm],
    setUpFunction: setUpRadix2CooleyTukeyf,
    tearDownFunction: tearDownRadix2CooleyTukeyf,
  legacyFactor: 48),
]

//===----------------------------------------------------------------------===//
// Double Benchmark
//===----------------------------------------------------------------------===//

var double_input_real: UnsafeMutablePointer<Double>?
var double_input_imag: UnsafeMutablePointer<Double>?
var double_output_real: UnsafeMutablePointer<Double>?
var double_output_imag: UnsafeMutablePointer<Double>?
var double_temp_real: UnsafeMutablePointer<Double>?
var double_temp_imag: UnsafeMutablePointer<Double>?

let doubleN = 2_048
let doubleSize = { MemoryLayout<Double>.size * doubleN }()

func setUpRadix2CooleyTukey() {
  let size = doubleSize

  double_input_real = UnsafeMutablePointer<Double>.allocate(capacity: size)
  double_input_imag = UnsafeMutablePointer<Double>.allocate(capacity: size)
  double_output_real = UnsafeMutablePointer<Double>.allocate(capacity: size)
  double_output_imag = UnsafeMutablePointer<Double>.allocate(capacity: size)
  double_temp_real = UnsafeMutablePointer<Double>.allocate(capacity: size)
  double_temp_imag = UnsafeMutablePointer<Double>.allocate(capacity: size)
}

func tearDownRadix2CooleyTukey() {
  double_input_real?.deallocate()
  double_input_imag?.deallocate()
  double_output_real?.deallocate()
  double_output_imag?.deallocate()
  double_temp_real?.deallocate()
  double_temp_imag?.deallocate()
}

func radix2CooleyTukey(_ level: Int,
  input_real: UnsafeMutablePointer<Double>,
  input_imag: UnsafeMutablePointer<Double>,
  stride: Int, output_real: UnsafeMutablePointer<Double>,
  output_imag: UnsafeMutablePointer<Double>,
  temp_real: UnsafeMutablePointer<Double>,
  temp_imag: UnsafeMutablePointer<Double>) {
  if level == 0 {
    output_real[0] = input_real[0];
    output_imag[0] = input_imag[0];
    return
  }
  let length = 1 << level
  let half = length >> 1
  radix2CooleyTukey(level - 1,
    input_real: input_real,
    input_imag: input_imag,
    stride: stride << 1,
    output_real: temp_real,
    output_imag: temp_imag,
    temp_real: output_real,
    temp_imag: output_imag)
  radix2CooleyTukey(level - 1,
    input_real: input_real + stride,
    input_imag: input_imag + stride,
    stride: stride << 1,
    output_real: temp_real + half,
    output_imag: temp_imag + half,
    temp_real: output_real + half,
    temp_imag: output_imag + half)
  for idx in 0..<half {
    let angle = -Double.pi * Double(idx) / Double(half)
    let _cos = cos(angle)
    let _sin = sin(angle)
    output_real[idx] = temp_real[idx] + _cos *
    temp_real[idx + half] - _sin * temp_imag[idx + half]
    output_imag[idx] = temp_imag[idx] + _cos *
    temp_imag[idx + half] + _sin *
    temp_real[idx + half]
    output_real[idx + half] = temp_real[idx] - _cos *
    temp_real[idx + half] + _sin *
    temp_imag[idx + half]
    output_imag[idx + half] = temp_imag[idx] - _cos *
    temp_imag[idx + half] - _sin *
    temp_real[idx + half]
  }
}

func testDouble(iter: Int) {
  let stride = 1

  let size = doubleSize
  let level = Int(log2(Double(doubleN)))

  let input_real = double_input_real.unsafelyUnwrapped
  let input_imag = double_input_imag.unsafelyUnwrapped
  let output_real = double_output_real.unsafelyUnwrapped
  let output_imag = double_output_imag.unsafelyUnwrapped
  let temp_real = double_temp_real.unsafelyUnwrapped
  let temp_imag = double_temp_imag.unsafelyUnwrapped

  for _ in 0..<iter {
    memset(UnsafeMutableRawPointer(input_real), 0, size)
    memset(UnsafeMutableRawPointer(input_imag), 0, size)
    memset(UnsafeMutableRawPointer(output_real), 0, size)
    memset(UnsafeMutableRawPointer(output_imag), 0, size)
    memset(UnsafeMutableRawPointer(temp_real), 0, size)
    memset(UnsafeMutableRawPointer(temp_imag), 0, size)

    radix2CooleyTukey(level,
      input_real: input_real,
      input_imag: input_imag,
      stride: stride,
      output_real: output_real,
      output_imag: output_imag,
      temp_real: temp_real,
      temp_imag: temp_imag)
  }
}

@inline(never)
public func run_Radix2CooleyTukey(_ n: Int) {
  testDouble(iter: n)
}

//===----------------------------------------------------------------------===//
// Float Benchmark
//===----------------------------------------------------------------------===//

let floatN = 2_048
let floatSize = { MemoryLayout<Float>.size * floatN }()

var float_input_real: UnsafeMutablePointer<Float>?
var float_input_imag: UnsafeMutablePointer<Float>?
var float_output_real: UnsafeMutablePointer<Float>?
var float_output_imag: UnsafeMutablePointer<Float>?
var float_temp_real: UnsafeMutablePointer<Float>?
var float_temp_imag: UnsafeMutablePointer<Float>?

func setUpRadix2CooleyTukeyf() {
  let size = floatSize
  float_input_real = UnsafeMutablePointer<Float>.allocate(capacity: size)
  float_input_imag = UnsafeMutablePointer<Float>.allocate(capacity: size)
  float_output_real = UnsafeMutablePointer<Float>.allocate(capacity: size)
  float_output_imag = UnsafeMutablePointer<Float>.allocate(capacity: size)
  float_temp_real = UnsafeMutablePointer<Float>.allocate(capacity: size)
  float_temp_imag = UnsafeMutablePointer<Float>.allocate(capacity: size)
}

func tearDownRadix2CooleyTukeyf() {
  float_input_real?.deallocate()
  float_input_imag?.deallocate()
  float_output_real?.deallocate()
  float_output_imag?.deallocate()
  float_temp_real?.deallocate()
  float_temp_imag?.deallocate()
}

func radix2CooleyTukeyf(_ level: Int,
  input_real: UnsafeMutablePointer<Float>,
  input_imag: UnsafeMutablePointer<Float>,
  stride: Int, output_real: UnsafeMutablePointer<Float>,
  output_imag: UnsafeMutablePointer<Float>,
  temp_real: UnsafeMutablePointer<Float>,
  temp_imag: UnsafeMutablePointer<Float>) {
  if level == 0 {
    output_real[0] = input_real[0];
    output_imag[0] = input_imag[0];
    return
  }
  let length = 1 << level
  let half = length >> 1
  radix2CooleyTukeyf(level - 1,
    input_real: input_real,
    input_imag: input_imag,
    stride: stride << 1,
    output_real: temp_real,
    output_imag: temp_imag,
    temp_real: output_real,
    temp_imag: output_imag)
  radix2CooleyTukeyf(level - 1,
    input_real: input_real + stride,
    input_imag: input_imag + stride,
    stride: stride << 1,
    output_real: temp_real + half,
    output_imag: temp_imag + half,
    temp_real: output_real + half,
    temp_imag: output_imag + half)
  for idx in 0..<half {
    let angle = -Float.pi * Float(idx) / Float(half)
    let _cos = cosf(angle)
    let _sin = sinf(angle)
    output_real[idx] = temp_real[idx] + _cos *
    temp_real[idx + half] - _sin * temp_imag[idx + half]
    output_imag[idx] = temp_imag[idx] + _cos *
    temp_imag[idx + half] + _sin * temp_real[idx + half]
    output_real[idx + half] = temp_real[idx] - _cos *
    temp_real[idx + half] + _sin *
    temp_imag[idx + half]
    output_imag[idx + half] = temp_imag[idx] - _cos *
    temp_imag[idx + half] - _sin *
    temp_real[idx + half]
  }
}

func testFloat(iter: Int) {
  let stride = 1
  let n = floatN
  let size = floatSize

  let input_real = float_input_real.unsafelyUnwrapped
  let input_imag = float_input_imag.unsafelyUnwrapped
  let output_real = float_output_real.unsafelyUnwrapped
  let output_imag = float_output_imag.unsafelyUnwrapped
  let temp_real = float_temp_real.unsafelyUnwrapped
  let temp_imag = float_temp_imag.unsafelyUnwrapped

  let level = Int(log2(Float(n)))

  for _ in 0..<iter {
    memset(UnsafeMutableRawPointer(input_real), 0, size)
    memset(UnsafeMutableRawPointer(input_imag), 0, size)
    memset(UnsafeMutableRawPointer(output_real), 0, size)
    memset(UnsafeMutableRawPointer(output_imag), 0, size)
    memset(UnsafeMutableRawPointer(temp_real), 0, size)
    memset(UnsafeMutableRawPointer(temp_imag), 0, size)

    radix2CooleyTukeyf(level,
      input_real: input_real,
      input_imag: input_imag,
      stride: stride,
      output_real: output_real,
      output_imag: output_imag,
      temp_real: temp_real,
      temp_imag: temp_imag)
  }
}

@inline(never)
public func run_Radix2CooleyTukeyf(_ n: Int) {
  testFloat(iter: n)
}
