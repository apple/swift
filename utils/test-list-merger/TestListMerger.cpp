#include "swift/Basic/ListMerger.h"
#include "llvm/ADT/ArrayRef.h"

#include <vector>
#include <random>
#include <iostream>

using namespace swift;

static int compare_unsigned(unsigned lhs, unsigned rhs) {
  return (lhs < rhs ? -1 : lhs > rhs ? 1 : 0);
}

namespace {
enum EntryOrder {
  creationOrder,
  reverseCreationOrder
};

struct Entry {
  unsigned id;
  unsigned value;
  Entry *next;
};

class EntryFactory {
  std::vector<std::unique_ptr<Entry>> entries;
  unsigned nextID = 0;
public:
  Entry *create(unsigned value) {
    auto entry = new Entry{nextID++, value, nullptr};
    entries.emplace_back(entry);
    return entry;
  }

  /// Sort the entries in this list.
  ///
  /// \param reverseCreationOrder - if true, then order equal-value
  ///   nodes in the reverse of creation order; otherwise
  ///   creator order of creation order
  void sort(EntryOrder order) {
    std::sort(entries.begin(), entries.end(),
              [=](const std::unique_ptr<Entry> &lhs,
                  const std::unique_ptr<Entry> &rhs) {
      if (lhs->value != rhs->value) return lhs->value < rhs->value;
      return order == creationOrder
               ? lhs->id < rhs->id
               : lhs->id > rhs->id;
    });
  }

  void checkSameAs(Entry *list) {
    for (auto &entry: entries) {
      std::cout << "  " << list->value << " (" << list->id << ")\n";
      assert(list == entry.get());
      list = list->next;
    };
    assert(list == nullptr);
  }
};

struct EntryListTraits {
  static Entry *getNext(Entry *e) { return e->next; }
  static void setNext(Entry *e, Entry *next) { e->next = next; }
  static int compare(Entry *lhs, Entry *rhs) {
    return compare_unsigned(lhs->value, rhs->value);
  }
};

using EntryListMerger = ListMerger<Entry*, EntryListTraits>;

enum Op {
  insert,
  beginMerge,
  endMerge
};

/// An instruction to the test harness: either a simple value
/// (an "insert") or one of the special instructions.
struct Instruction {
  Op op;
  unsigned value;

  Instruction(Op op) : op(op), value(0) { assert(op != insert); }
  Instruction(unsigned value) : op(insert), value(value) {}

  friend std::ostream &operator<<(std::ostream &str, const Instruction &inst) {
    switch (inst.op) {
    case insert: str << inst.value; break;
    case beginMerge: str << "beginMerge"; break;
    case endMerge: str << "endMerge"; break;
    }
    return str;
  }
};
} // end anonymous namespace

template <class T>
static std::ostream &operator<<(std::ostream &str, llvm::ArrayRef<T> list) {
  str << "{";
  for (auto b = list.begin(), i = b, e = list.end(); i != e; ++i) {
    if (i != b) str << ", ";
    str << *i;
  }
  str << "}";
  return str;
}

template <class T>
static std::ostream &operator<<(std::ostream &str, const std::vector<T> &list) {
  return (str << llvm::ArrayRef(list));
}

static void runInsertAndMergeTest(llvm::ArrayRef<Instruction> values) {
  EntryFactory entries;
  EntryListMerger merger;

  // Between beginMerge and endMerge instructions, values don't get
  // inserted immediately: they build up into a separate list of items
  // that will be merged at the time of the endMerge.  We record this
  // mode by making lastMergeEntry non-null.
  Entry *firstMergeEntry;
  Entry **lastMergeEntry = nullptr;

  for (auto &inst : values) {
    switch (inst.op) {
    case insert: {
      // Create the new entry.
      Entry *entry = entries.create(inst.value);

      // If we're building a merge list, append to the end of it.
      if (lastMergeEntry) {
        *lastMergeEntry = entry;
        lastMergeEntry = &entry->next;

      // Otherwise, just do an insertion.
      } else {
        merger.insert(entry);
      }
      break;
    }

    case beginMerge:
      assert(!lastMergeEntry && "already building a merge list");
      lastMergeEntry = &firstMergeEntry;
      break;

    case endMerge:
      assert(lastMergeEntry && "not building a merge list?");
      // Cap off the merge list we built.
      *lastMergeEntry = nullptr;

      // Do the merge.
      merger.merge(firstMergeEntry);

      // We're no longer building a merge list.
      lastMergeEntry = nullptr;
      break;
    }
  }
  assert(!lastMergeEntry && "ended while still building a merge list");

  entries.sort(creationOrder);
  entries.checkSameAs(std::get<0>(merger.release()));
}

static void runInsertAtFrontTest(llvm::ArrayRef<unsigned> values) {
  EntryFactory entries;
  EntryListMerger merger;
  for (auto value: values) {
    merger.insertAtFront(entries.create(value));
  }
  entries.sort(reverseCreationOrder);
  entries.checkSameAs(std::get<0>(merger.release()));
}

static void runConcreteTests() {
  runInsertAndMergeTest({ 5, 0, 3, 0, 1, 0, 7 });
}

namespace {
struct TestConfig {
  unsigned numTests;
  unsigned numEntries;
  unsigned maxValue;
};

}

static void runInsertAndMergeTests(const TestConfig &config) {
  std::random_device randomDevice;
  std::default_random_engine e(randomDevice());
  std::uniform_int_distribution<unsigned> valueDist(0, config.maxValue);
  // Chance of entering or exiting a merge.
  std::uniform_int_distribution<unsigned> mergeDist(0, 20);

  std::vector<Instruction> ins;
  for (unsigned testN = 0; testN < config.numTests; ++testN) {
    ins.clear();

    const size_t noMerge = -1;
    size_t mergeStart = noMerge;
    for (unsigned i = 0; i < config.numEntries || mergeStart != noMerge; ++i) {
      if (mergeDist(e) == 0) {
        if (mergeStart != noMerge) {
          std::sort(ins.begin() + mergeStart, ins.end(),
                    [](const Instruction &lhs, const Instruction &rhs) {
                      return lhs.value < rhs.value;
                    });
          ins.push_back(endMerge);
          mergeStart = noMerge;
        } else {
          ins.push_back(beginMerge);
          mergeStart = ins.size();
        }
      } else {
        ins.push_back(valueDist(e));
      }
    }

    std::cout << "runInsertAndMergeTest(" << ins << ");" << std::endl;
    runInsertAndMergeTest(ins);
  }
}

static void runInsertAtFrontTests(const TestConfig &config) {
  std::random_device randomDevice;
  std::default_random_engine e(randomDevice());
  std::uniform_int_distribution<unsigned> valueDist(0, config.maxValue);

  std::vector<unsigned> ins;
  for (unsigned testN = 0; testN < config.numTests; ++testN) {
    ins.clear();
    for (unsigned i = 0; i < config.numEntries; ++i) {
      ins.push_back(valueDist(e));
    }

    std::cout << "runInsertAtFrontTest(" << ins << ");" << std::endl;
    runInsertAtFrontTest(ins);
  }
}

int main() {
  TestConfig config = {
    .numTests = 1000,
    .numEntries = 2000,
    .maxValue = 3
  };
  runConcreteTests();
  runInsertAndMergeTests(config);
  runInsertAtFrontTests(config);
}
