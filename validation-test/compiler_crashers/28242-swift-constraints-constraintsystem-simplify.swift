// This source file is part of the Swift.org open source project
// See http://swift.org/LICENSE.txt for license information

// RUN: not --crash %target-swift-frontend %s -parse
{struct b{let a{struct D{let a=b([print{}}}}struct b
