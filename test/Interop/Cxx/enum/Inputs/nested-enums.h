#ifndef TEST_INTEROP_CXX_ENUM_INPUTS_NESTED_ENUMS_H
#define TEST_INTEROP_CXX_ENUM_INPUTS_NESTED_ENUMS_H

namespace ns {

enum EnumInNS {
  kA = 0,
  kB
};

enum class ScopedEnumInNS {
  scopeA,
  scopeB
};

namespace nestedNS {

enum EnumInNestedNS {
  kNestedA = 0,
  kNestedB
};

}

}

#endif // TEST_INTEROP_CXX_ENUM_INPUTS_NESTED_ENUMS_H
