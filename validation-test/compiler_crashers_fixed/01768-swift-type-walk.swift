// This source file is part of the Swift.org open source project
// See http://swift.org/LICENSE.txt for license information

// RUN: not %target-swift-frontend %s -parse
var b = {
}
class B<T where B : b<T>() {
var b: b {
func d.Type
}
