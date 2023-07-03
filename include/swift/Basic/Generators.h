//===--- Generators.h - "Coroutines" for doing traversals -------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines a few types for defining types that follow this
// simple generator concept:
//
// concept Generator {
//   // ...some number of accessors for the current value...
//
//   /// Is this generator finished producing values?
//   bool isFinished() const;
//
//   /// Given that this generator is not finished, advance to the
//   /// next value.
//   void advance();
//
//   /// Finish the generator, asserting that all values have been
//   /// produced.
//   void finish();
// };
//
// concept SimpleGenerator : Generator {
//   type reference;
//
//   // Get the current value.
//   reference get();
//
//   // Get the current value and then advance the generator.
//   reference claimNext();
// }
//
// Generators are useful when some structure needs to be traversed but
// that traversal can't be done in a simple lexical loop.  For example,
// you can't do two traversals in parallel with a single loop unless you
// break down all the details of the traversal.  This is a minor problem
// for simple traversals like walking a flat array, but it's a significant
// problem when traversals get more complex, like when different components
// of an array are grouped together according to some additional structure
// (such as the abstraction pattern of a function's parameter list).
// It's tempting to write those traversals as higher-order functions that
// invoke a callback for each element, but this breaks down when parallel
// traversal is required.  Expressing the traversal as a generator
// allows the traversal logic to to be reused without that limitation.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_GENERATORS_H
#define SWIFT_BASIC_GENERATORS_H

#include "llvm/ADT/ArrayRef.h"

namespace swift {

namespace generator_details {

template <class T>
struct is_array_ref_like {
  enum { value = false };
};

template <class T>
struct is_array_ref_like<llvm::ArrayRef<T>> {
  enum { value = true };
};

template <class T>
struct is_array_ref_like<llvm::MutableArrayRef<T>> {
  enum { value = true };
};
}

/// A class for generating the elements of an ArrayRef-like collection.
template <class CollectionType>
class ArrayRefGenerator {
  static_assert(generator_details::is_array_ref_like<CollectionType>::value,
                "ArrayRefGenerator should only be used with ArrayRef-like "
                "types");

  CollectionType values;

public:
  using reference =
    typename std::iterator_traits<typename CollectionType::iterator>::reference;

  ArrayRefGenerator() {}
  ArrayRefGenerator(CollectionType values) : values(values) {}

  // Prevent accidental copying of the generator.
  ArrayRefGenerator(const ArrayRefGenerator &other) = delete;
  ArrayRefGenerator &operator=(const ArrayRefGenerator &other) = delete;

  ArrayRefGenerator(ArrayRefGenerator &&other) = default;
  ArrayRefGenerator &operator=(ArrayRefGenerator &&other) = default;

  /// Explicitly copy the current generator state.
  ArrayRefGenerator clone() const {
    return ArrayRefGenerator(values);
  }

  /// Return the current element of the array.
  reference get() const {
    assert(!isFinished());
    return values.front();
  }

  /// Claim the current element of the array and advance past it.
  reference claimNext() {
    assert(!isFinished());
    reference result = get();
    advance();
    return result;
  }

  /// Claim the next N elements of the array and advance past them.
  CollectionType claimNext(size_t count) {
    assert(count <= values.size() && "claiming too many values");
    CollectionType result = values.slice(0, count);
    values = values.slice(count);
    return result;
  }

  /// Is this generation finished?
  bool isFinished() const {
    return values.empty();
  }

  /// Given that this generation is not finished, advance to the
  /// next element.
  void advance() {
    assert(!isFinished());
    values = values.slice(1);
  }

  /// Perform any final work required to complete the generation.
  void finish() {
    assert(isFinished() && "didn't finish generating the collection");
  }
};

namespace generator_details {
template <class T> struct is_simple_generator_ref;
}

/// An abstracting reference to an existing generator.
///
/// The implementation of this type holds the reference to the existing
/// generator without allocating any additional storage; it is sufficient
/// for the caller ensures that the object passed to the constructor
/// stays valid.  Values of this type can otherwise be safely copied
/// around.
template <class T>
class SimpleGeneratorRef {
public:
  using reference = T;

private:
  struct VTable {
    bool (*isFinished)(const void *impl);
    reference (*claimNext)(void *impl);
    void (*advance)(void *impl);
    void (*finish)(void *impl);
  };

  template <class G> struct VTableImpl {
    static constexpr VTable vtable = {
      [](const void *p) { return static_cast<const G*>(p)->isFinished(); },
      [](void *p) -> reference { return static_cast<G*>(p)->claimNext(); },
      [](void *p) { static_cast<G*>(p)->advance(); },
      [](void *p) { static_cast<G*>(p)->finish(); },
    };
  };

  const VTable *vtable;
  void *pointer;

public:
  constexpr SimpleGeneratorRef() : vtable(nullptr), pointer(nullptr) {}

  template <class G>
  constexpr SimpleGeneratorRef(G &generator,
      typename std::enable_if<!generator_details::is_simple_generator_ref<G>::value, bool>::type = false)
    : vtable(&VTableImpl<G>::vtable), pointer(&generator) {}

  /// Test whether this generator ref was initialized with a
  /// valid reference to a generator.
  explicit operator bool() const {
    return pointer != nullptr;
  }

  bool isFinished() const {
    assert(pointer);
    return vtable->isFinished(pointer);
  }

  reference claimNext() {
    assert(pointer);
    return vtable->claimNext(pointer);
  }

  void advance() {
    assert(pointer);
    vtable->advance(pointer);
  }

  void finish() {
    assert(pointer);
    vtable->finish(pointer);
  }
};

namespace generator_details {

template <class T>
struct is_simple_generator_ref<SimpleGeneratorRef<T>> {
  static constexpr bool value = true;
};
template <class T>
struct is_simple_generator_ref {
  static constexpr bool value = false;
};

}

} // end namespace swift

#endif
