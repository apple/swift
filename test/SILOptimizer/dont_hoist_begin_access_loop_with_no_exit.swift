// RUN: %target-swiftc_driver -O %s

// https://github.com/apple/swift/issues/67084
// Don't crash failing to sink the corresponding end_access after
// hoisting the begin_access when the loop has no exit.

var i = 0
while true {
  i += 1
}
