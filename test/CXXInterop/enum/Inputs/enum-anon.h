enum {
  Constant1,
  Constant2
};

enum {
  VarConstant1,
  VarConstant2
} global;

typedef struct SR2511 {
    int x;

    enum {
      SR2511A = 0, SR2511B, SR2511C
    } y;

    int z;
} SR2511;
