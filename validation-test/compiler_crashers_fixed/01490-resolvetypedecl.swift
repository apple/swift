// This source file is part of the Swift.org open source project
// See http://swift.org/LICENSE.txt for license information

// RUN: not %target-swift-frontend %s -parse
class b<T where g: U : e(T) -> {
}
struct A {
let start = b
