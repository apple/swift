static inline int funcBar(void) {
  int result = 0;
  if (__builtin_available(macos 11.0, *)) {
    result += 1;
  } else {
    result -= 1;
  }
  return result;
}
