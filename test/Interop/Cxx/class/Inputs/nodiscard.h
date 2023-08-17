#ifndef TEST_INTEROP_CXX_CLASS_NODISCARD_H
#define TEST_INTEROP_CXX_CLASS_NODISCARD_H

[[nodiscard]] int NoDiscardAdd(int x, int y);

class NoDiscardMultiply {
 public:
  NoDiscardMultiply() {}
  NoDiscardMultiply(const NoDiscardMultiply &) { }
  ~NoDiscardMultiply() {}

  [[nodiscard]] int Multiply(int x, int y) { return x * y; }

  int Divide(int x, int y) { return x / y; }
};

struct [[nodiscard]] NoDiscardError {
 public:
  NoDiscardError(int value) : value_(value) {}
  int value_;
};

NoDiscardError NoDiscardReturnError(int x, int y);

#endif
