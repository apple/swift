inline void calledFromConceptBody(int x) {}
inline void calledFromMethodBody(int x) {}

struct MyStruct {
  template <typename T>
  void foo(T x)
    requires requires(const T x) { calledFromConceptBody(x); }
  {
    calledFromMethodBody(x);
  }
};
