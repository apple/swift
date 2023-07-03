//===--- Lazy.h - A lazily-initialized object -------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_LAZY_H
#define SWIFT_BASIC_LAZY_H

#include <memory>
#include <functional>

#include "swift/Basic/Malloc.h"
#include "swift/Basic/type_traits.h"
#include "swift/Threading/Once.h"

namespace swift {

/// A template for lazy-initialized values.
/// Usage:
///
///   LazyValue<std::string> value([]() { return createString(); })
///   if (condition) {
///     // 'createString()' is evaluated only when 'value` is dereferenced.
///     doSomething(*value);
///   }
template <typename T, typename Initializer = std::function<T()>>
class LazyValue {
  Initializer Init;
  llvm::Optional<T> Value;

public:
  LazyValue(Initializer Init) : Init(Init){};

  T &get() {
    if (!Value.has_value()) {
      Value = Init();
    }
    return Value.value();
  }

  T *operator->() { return &get(); }
  T &operator*() { return get(); }
};

/// A template for lazily-constructed, zero-initialized, leaked-on-exit
/// global objects.
template <class T> class Lazy {
  alignas(T) char Value[sizeof(T)] = { 0 };

  swift::once_t OnceToken = {};

  static void defaultInitCallback(void *ValueAddr) {
    ::new (ValueAddr) T();
  }

public:
  using Type = T;

  T &get(void (*initCallback)(void *) = defaultInitCallback);

  template<typename Arg1>
  T &getWithInit(Arg1 &&arg1);

  /// Get the value, assuming it must have already been initialized by this
  /// point.
  T &unsafeGetAlreadyInitialized() { return *reinterpret_cast<T *>(&Value); }

  constexpr Lazy() = default;

  T *operator->() { return &get(); }
  T &operator*() { return get(); }

private:
  Lazy(const Lazy &) = delete;
  Lazy &operator=(const Lazy &) = delete;
};

template <typename T> inline T &Lazy<T>::get(void (*initCallback)(void*)) {
  swift::once(OnceToken, initCallback, &Value);
  return unsafeGetAlreadyInitialized();
}

template <typename T>
template <typename Arg1> inline T &Lazy<T>::getWithInit(Arg1 &&arg1) {
  struct Data {
    void *address;
    Arg1 &&arg1;

    static void init(void *context) {
      Data *data = static_cast<Data *>(context);
      ::new (data->address) T(static_cast<Arg1&&>(data->arg1));
    }
  } data{&Value, static_cast<Arg1&&>(arg1)};

  swift::once(OnceToken, &Data::init, &data);
  return unsafeGetAlreadyInitialized();
}

} // end namespace swift

#define SWIFT_LAZY_CONSTANT(INITIAL_VALUE) \
  ([]{ \
    using T = ::std::remove_reference<decltype(INITIAL_VALUE)>::type; \
    static ::swift::Lazy<T> TheLazy; \
    return TheLazy.get([](void *ValueAddr){ ::new(ValueAddr) T{INITIAL_VALUE}; });\
  }())

#endif // SWIFT_BASIC_LAZY_H
