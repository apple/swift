class NonBitwiseTakableCXXType {
public:
  bool state = false;

  NonBitwiseTakableCXXType() {}

  NonBitwiseTakableCXXType(const NonBitwiseTakableCXXType &other) {
    state = false;
  }

  NonBitwiseTakableCXXType(NonBitwiseTakableCXXType &&other) {
    state = !other.state;
  }

  ~NonBitwiseTakableCXXType() {}
};
