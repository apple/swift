class MyException {
public:
  virtual ~MyException() {}
};

// Calls f and catches any exceptions thrown by f.
// Returns whether an exception was caught.
inline bool callAndCatchExceptions(void (*f)()) {
  try {
    f();
  } catch (...) {
    return true;
  }

  return false;
}

inline void throwException() {
  throw MyException();
}
