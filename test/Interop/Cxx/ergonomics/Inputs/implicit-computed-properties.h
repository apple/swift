#ifndef SWIFT_IMPLICIT_COMPUTED_PROPERTIES_H
#define SWIFT_IMPLICIT_COMPUTED_PROPERTIES_H

struct VoidGetter {
  void getX();
  void setX(int);
};

struct VoidSetterNoName {
  void set();
};

struct IllegalIntReturnSetter {
  int setX(int);
};

struct TwoParameterSetter {
  void setX(int, int);
};

struct NoNameSetter {
  void set(int);
};

struct NoNameVoidGetter {
  void get();
};

struct LongNameAllLower {
  int value = 42;
  int getfoo() const { return value; }
  void setfoo(int v) { value = v; }
};

struct LongNameAllUpper {
  int value = 42;
  int getFOO() const { return value; }
  void setFOO(int v) { value = v; }
};

struct UpperCaseMix {
    int value = 42;
    int getFoo() const { return value; }
    void SetFoo(int v) { value = v; }
};

struct UpperCaseGetterSetter {
    int value = 42;
    int GetFoo() const { return value; }
    void SetFoo(int v) { value = v; }
};

struct GetterOnly {
  int getFoo() const { return 42; }
};

struct NoNameUpperGetter {
  int Getter();
};

struct NotypeSetter {
  void setX();
};

struct IntGetterSetter {
  int val = 42;
  int getX() const { return val; }
  void setX(int v) { val = v; }
};

// this should be handled as snake case. See: rdar://89453010
struct IntGetterSetterSnakeCaseUpper {
  int val;
  int Get_X() const { return val; }
  void Set_X(int v) { val = v; }
};

// We should  deprecate methods when we transform them successfully (telling
// users to use
//  the computed properties instead) rdar://89452854.
struct IntGetterSetterSnakeCase {
  int val;
  int get_x() const { return val; }
  void set_x(int v) { val = v; }
};

struct GetterHasArg {
  int getX(int v) const;
  void setX(int v);
};

struct GetterSetterIsUpper {
  int val;
  int GETX() const { return val; }
  void SETX(int v) { val = v; }
};

struct HasXAndY {
  int val;
  int GetXAndY() const { return val; }
  void SetXAndY(int v) { val = v; }
};

struct AllUpper {
  int val;
  int GETFOOANDBAR() const { return val; }
  void SETFOOANDBAR(int v) { val = v; }
};

struct BothUpper {
  int val;
  int getFOOAndBAR() const { return val; }
  void setFOOAndBAR(int v) { val = v; }
};

struct FirstUpper {
  int val;
  int getFOOAndBar() const { return val; }
  void setFOOAndBar(int v) { val = v; }
};

struct NonConstGetter {
  int val;
  int getX() { return val; }
  void setX(int v) { val = v; }
};

struct ConstSetter {
  mutable int val;
  int getX() const { return val; }
  void setX(int v) const { val = v; }
};

struct MultipleArgsSetter {
  int getX() const;
  void setX(int a, int b);
};

struct __attribute__((swift_attr("import_unsafe"))) NonTrivial {
  int value = 42;
  ~NonTrivial() {}
  NonTrivial(const NonTrivial &other) : value(other.value) {}
};

struct PtrGetterSetter {
  int value = 42;
  int *getX() __attribute__((swift_attr("import_unsafe"))) { return &value; }
  void setX(int *v) { value = *v; }
};

struct RefGetterSetter {
  int value = 42;
  const int &getX() __attribute__((swift_attr("import_unsafe"))) {
    return value;
  }
  void setX(const int &v) { value = v; }
};

struct NonTrivialGetterSetter {
  NonTrivial value = {42};
  NonTrivial getX() { return value; }
  void setX(NonTrivial v) { value = v; }
};

struct DifferentTypes {
  NonTrivial value = {42};
  NonTrivial getX() { return value; }
  void setX(int v) { value = {v}; }
};

struct UTF8Str {
  int value = 42;
  int getUTF8Str() const { return value; }
  void setUTF8Str(int v) { value = v; }
};

struct MethodWithSameName {
  int value();
  int getValue() const;
  void setValue(int i);
};

struct PropertyWithSameName {
  int value;
  int getValue() const;
  void setValue(int i);
};

class PrivatePropertyWithSameName {
  int value;

public:
  int getValue() const;
  void setValue(int i);
};

struct SnakeCaseGetterSetter {
  int value = 42;
  int get_foo() const { return value; };
  void set_foo(int v) { value = v; };
};

struct SnakeCaseUTF8Str {
  int value = 42;
  int get_utf8_string() const { return value; };
  void set_utf8_string(int v) { value = v; };
};

struct SnakeCaseTrailing {
  int value = 42;
  int get_x_() const { return value; };
  void set_x_(int v) { value = v; } ;
};

#endif // SWIFT_IMPLICIT_COMPUTED_PROPERTIES_H