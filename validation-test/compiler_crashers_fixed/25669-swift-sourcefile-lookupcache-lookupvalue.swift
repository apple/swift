// This source file is part of the Swift.org open source project
// See http://swift.org/LICENSE.txt for license information

// RUN: not %target-swift-frontend %s -parse
let:{struct B<T where B:A{typealias F=Int
protocol a)func a<U
