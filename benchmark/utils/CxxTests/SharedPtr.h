#ifndef BENCHMARK_SHARED_PTR_H
#define BENCHMARK_SHARED_PTR_H

#include <memory>

struct BigStruct {
  char buff[64];
  uint32_t value = 42;
};

using SharedPtrT = std::shared_ptr<BigStruct>;

static inline SharedPtrT ptr;

void initPtr() {
  ptr = std::make_shared<BigStruct>();
}

std::tuple<SharedPtrT, SharedPtrT, SharedPtrT, SharedPtrT, SharedPtrT>
copyX5() {
  return { ptr, ptr, ptr, ptr, ptr };
}


inline uint32_t testSharedPtrCopies() {
  auto ptrs = copyX5();

  return std::get<0>(ptrs)->value +
         std::get<1>(ptrs)->value +
         std::get<2>(ptrs)->value +
         std::get<3>(ptrs)->value +
         std::get<4>(ptrs)->value;
}

#endif // BENCHMARK_SHARED_PTR_H
