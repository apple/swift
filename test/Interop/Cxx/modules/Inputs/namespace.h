#ifndef TEST_INTEROP_CXX_MODULES_INPUTS_NAMESPACE_H
#define TEST_INTEROP_CXX_MODULES_INPUTS_NAMESPACE_H

namespace Namespace {

using SimpleTypealias = int;

struct Parent {
  struct Child {};
};

namespace NestedNamespace {

struct NestedStruct {};

} // namespace NestedNamespace

} // namespace Namespace

#endif // TEST_INTEROP_CXX_MODULES_INPUTS_NAMESPACE_H
