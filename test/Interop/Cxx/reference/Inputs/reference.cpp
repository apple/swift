#include "reference.h"

static int staticInt = 42;

int getStaticInt() { return staticInt; }
int &getStaticIntRef() { return staticInt; }
int &&getStaticIntRvalueRef() { return static_cast<int &&>(staticInt); }
const int &getConstStaticIntRef() { return staticInt; }
const int &&getConstStaticIntRvalueRef() { return static_cast<int &&>(staticInt); }

void setStaticInt(int i) { staticInt = i; }
void setStaticIntRef(int &i) { staticInt = i; }
void setStaticIntRvalueRef(int &&i) { staticInt = i; }
void setConstStaticIntRef(const int &i) { staticInt = i; }
void setConstStaticIntRvalueRef(const int &&i) { staticInt = i; }

auto getFuncRef() -> int (&)() { return getStaticInt; }
auto getFuncRvalueRef() -> int (&&)() { return getStaticInt; }

void takeConstRef(const int &value) {
  staticInt = value;
}

void setConstStaticIntRefTypealias(ConstIntRefTypealias ref) {
  staticInt = ref;
}

void setStaticIntRefTypealias(IntRefTypealias ref) {
  staticInt = ref;
}
