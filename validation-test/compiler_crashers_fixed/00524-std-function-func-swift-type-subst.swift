// This source file is part of the Swift.org open source project
// See http://swift.org/LICENSE.txt for license information

// RUN: not %target-swift-frontend %s -parse
protocol B : a {
case A, Any, AnyObject, (c(t: d {
}
protocol a {
}
typealias e : e
