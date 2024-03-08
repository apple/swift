# C++ Standard Library Overlay

To refine and improve ergonomics of the C++ standard library classes when they
are used from Swift code, Swift provides an overlay for the C++ standard
library. The overlay is written in Swift, and is shipped as part of the OS SDKs
on Darwin platforms, and along with the Swift standard library on Linux. The
overlay is only available when C++ Interoperability is enabled.

## `std` module

The `std` overlay provides Swift extensions for commonly used C++ standard
library classes, such as `std::string`. It is imported implicitly whenever the
clang module for the C++ standard library module (`std`) is imported.

For instance, `std` overlay allows the developer to write this code, which
wouldn't compile without the overlay:

```swift
import std

let cxxString: std.string = "abc" // initializing with a string literal
let swiftString = String(cxxString: cxxString) // converting to Swift.String
```

This is achieved by providing several Swift extensions within the `std` overlay:

```swift
// std.swift

extension std.string {
  public init(_ string: String)
}

extension std.string: ExpressibleByStringLiteral {
  public init(stringLiteral value: String)
}

extension String {
  public init(cxxString: std.string)
}
```

## `Cxx` module

In addition to the `std` module which extends the C++ standard library classes,
Swift provides another module (`Cxx`) that includes Swift bridging utilities
that do not have a dependency on the C++ standard library. This allows the
developer to rely on certain bridging logic without introducing a dependency on
the C++ standard library. `Cxx` module is re-exported from the `std` overlay,
which makes `Cxx` available whenever `std` is used.

Currently this module only includes iterator bridging logic:
`protocol UnsafeCxxInputIterator`, `protocol CxxSequence`,
`class CxxIterator<T>`.

### Iterator bridging

Raw C++ iterators are generally unsafe to use from Swift: the differences in
memory model between Swift and C++ mean that it is extremely easy to
inadvertently access invalid memory by a dangling pointer. For instance, this
Swift code would produce undesired results despite looking similar to correct
C++ code:

```swift
let str: std.string = returnsStdString()
var iter = str.begin() // dangling iterator!
                       // calling `begin()` makes a copy of `str`.
let endIter = std.end() // also a dangling iterator.
while iter != endIter {
  let value = iter.pointee // potential access to already freed memory!
                           // the lifetime of `str` is not attached to the
                           // lifetime of `iter`, which means `str` might get
                           // deallocated prior to this call.
  iter = iter.successor()
}
```

To solve these issues while still allowing the developer to interact with C++
sequences and collections, Swift provides a bridging mechanism for C++
iterators.

Most C++ sequences get a synthesized conformance to `Swift.Sequence` when they
are imported into Swift. This allows iterating over a C++ sequence with a Swift
for-in loop:

```swift
let str: std.string = returnsStdString()
for value in str {
  // ...
}
```

Under the hood, C++ input iterator types are automatically conformed to
`UnsafeCxxInputIterator`. Swift treats a C++ type as an input iterator if:

* the type defines a subtype `iterator_category` as an inheritor
  of `std::input_iterator_tag`; and
* the type satisfies the requirements
  of [InputIterator](https://en.cppreference.com/w/cpp/named_req/InputIterator),
  specifically:
  * defines a dereference operator (`operator*()`); and
  * defines an equality operator (`operator==(lhs, rhs)`); and
  * defines a prefix increment operator (`operator++()`).

If a conformance was not automatically synthesized, the user can declare a Swift
extension to add the conformance:

```swift
extension MyIteratorType: UnsafeCxxInputIterator {
  // ...
}
```

Additionally, C++ sequence types are automatically conformed to `CxxSequence`.
Swift treats a C++ type as a sequence if the type defines `begin()` and `end()`
methods that return instances of the same type that conforms to
`UnsafeCxxInputIterator`.

Similarly, the user can declare a Swift extension to add the conformance to
`CxxSequence` if it was not synthesized automatically:

```swift
extension MySequenceType: CxxSequence {
  // ...
}
```
