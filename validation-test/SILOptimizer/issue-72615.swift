// RUN: %target-swift-frontend \
// RUN:     %s                 \
// RUN:     -emit-sil -verify  \
// RUN:     -sil-verify-all

func doStuffUniquely(with value: consuming [Int]) {
  // If we received the last remaining reference to `value`, we'd like
  // to be able to efficiently update it without incurring more copies.
  var newValue = consume value
  newValue.append(42)
}

func test() {
  var x: [Int] = [1,2,3]

  // x is appended to. After this point, we know that x is unique. We want to
  // preserve that property.
  x.append(5)

  // Pass the current value of x off to another function, that
  doStuffUniquely(with: consume x)

  // Reset x to a new value. Since we don't use the old value anymore,
  x = []
  doMoreStuff(with: &x)
}

func doMoreStuff(with value: inout [Int]) {
}
