// This source file is part of the Swift.org open source project
// See http://swift.org/LICENSE.txt for license information

// RUN: not %target-swift-frontend %s -parse
class A {
protocol P {
}
func b() -> [B
}
protocol B : d {
typealias d
