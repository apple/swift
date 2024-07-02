struct HasThreeFields {
  int a = 1;
  int b = 2;
  int c = 3;
};

struct DerivedWithSameField : HasThreeFields {
  int a = 2;
};

struct DerivedWithOneField : HasThreeFields {
  int d = 4;
};

struct HasOneField {
  int e = 5;
};

struct DerivedFromAll : HasOneField, DerivedWithOneField {
  int f = 6;
};

struct OneField {
  int value = 42;
};

struct DerivedFromOneField : OneField {};

// Non trivial types:

struct __attribute__((swift_attr("import_unsafe"))) NonTrivial {
  NonTrivial() {}
  NonTrivial(const NonTrivial &) {}
  ~NonTrivial() {}
};

struct NonTrivialHasThreeFields : NonTrivial {
  int a = 1;
  int b = 2;
  int c = 3;
};

struct NonTrivialDerivedWithOneField : NonTrivialHasThreeFields {
  int d = 4;
};

struct __attribute__((swift_attr("import_unsafe"))) NonTrivialHasOneField {
  NonTrivialHasOneField() {}
  NonTrivialHasOneField(const NonTrivialHasOneField &other) : e(other.e) {}
  ~NonTrivialHasOneField() {}

  int e = 5;
};

struct __attribute__((swift_attr("import_unsafe"))) NonTrivialDerivedFromAll
    : NonTrivialHasOneField,
      NonTrivialDerivedWithOneField {
  int f = 6;
};

// Templates:

template<class T>
struct ClassTemplate {
  T value;
};

struct DerivedFromClassTemplate : ClassTemplate<int> {};

int &getCopyCounter() {
    static int copyCounter = 0;
    return copyCounter;
}

class CopyTrackedBaseClass {
public:
    CopyTrackedBaseClass(int x) : x(x) {}
    CopyTrackedBaseClass(const CopyTrackedBaseClass &other) : x(other.x) {
        ++getCopyCounter();
    }

    int x;
};

class CopyTrackedDerivedClass: public CopyTrackedBaseClass {
public:
    CopyTrackedDerivedClass(int x) : CopyTrackedBaseClass(x) {}
};

class NonEmptyBase {
public:
    int getY() const {
        return y;
    }
private:
    int y = 11;
};

class CopyTrackedDerivedDerivedClass: public NonEmptyBase, public CopyTrackedDerivedClass {
public:
    CopyTrackedDerivedDerivedClass(int x) : CopyTrackedDerivedClass(x) {}
};

// Types with virtual methods, make sure field offsets are right. rdar://126754931

struct HasOneFieldWithVirtualMethod {
  int a;
  virtual ~HasOneFieldWithVirtualMethod() {}
};

struct HasTwoFieldsWithVirtualMethod {
  bool b;
  bool c;
  virtual ~HasTwoFieldsWithVirtualMethod() = default;
};

struct InheritFromStructsWithVirtualMethod: HasOneFieldWithVirtualMethod, HasTwoFieldsWithVirtualMethod {
  int d;
  virtual ~InheritFromStructsWithVirtualMethod() = default;
};
