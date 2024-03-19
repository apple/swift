#ifndef TEST_INTEROP_CXX_VALUE_WITNESS_TABLE_INPUTS_CLASS_TEMPLATE_METADATA_H
#define TEST_INTEROP_CXX_VALUE_WITNESS_TABLE_INPUTS_CLASS_TEMPLATE_METADATA_H

struct RecordRecord {

};

enum class RecordEnumClass { a, b, c };

struct ParentRecord {
  struct RecordRecord {

  };

  enum class RecordEnumClass { a, b, c };
};

struct NamespaceRecord {

};

enum class NamespaceEnumClass { a, b, c };

struct NamespaceNamespaceRecord {

};

namespace Namespace {
  struct NamespaceRecord {

  };

  enum class NamespaceEnumClass { a, b, c };

  namespace NamespaceNamespace {
    struct NamespaceRecord {

    };
  }
}

#endif // TEST_INTEROP_CXX_VALUE_WITNESS_TABLE_INPUTS_CLASS_TEMPLATE_METADATA_H
