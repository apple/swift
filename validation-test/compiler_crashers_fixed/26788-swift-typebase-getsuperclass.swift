// This source file is part of the Swift.org open source project
// See http://swift.org/LICENSE.txt for license information

// RUN: not %target-swift-frontend %s -parse
class a}struct B<T where g:a{class A<T{struct B<class B:a{let h=B
