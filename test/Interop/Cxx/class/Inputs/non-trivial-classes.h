#ifndef __NON_TRIVIAL_CLASSES__
#define __NON_TRIVIAL_CLASSES__

struct NonTrivial {
  NonTrivial();
  ~NonTrivial();
  NonTrivial(const NonTrivial &);
  int x;
};

void cfunc(void (^ _Nonnull)(NonTrivial));

struct ARCWeak {
  __weak _Nullable id m;
};

void cfuncARCWeak(void (^ _Nonnull)(ARCWeak));

#endif // __NON_TRIVIAL_CLASSES__
