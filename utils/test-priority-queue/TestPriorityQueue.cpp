#include <cassert>
#include <iostream>
#include <random>
#include <vector>

#define private public
#include "swift/Basic/PriorityQueue.h"

using namespace swift;

namespace {

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

  void remove(Entry *e) {
    auto it = std::find_if(
        entries.begin(), entries.end(),
        [=](const std::unique_ptr<Entry> &ptr) { return ptr.get() == e; });
    assert(it != entries.end());
    entries.erase(it);
  }

  /// Sort the entries in this list.
  ///
  /// \param reverseCreationOrder - if true, then order equal-value
  ///   nodes in the reverse of creation order; otherwise
  ///   creator order of creation order
  void sort() {
    std::sort(entries.begin(), entries.end(),
              [=](const std::unique_ptr<Entry> &lhs,
                  const std::unique_ptr<Entry> &rhs) {
                if (lhs->value != rhs->value)
                  return lhs->value < rhs->value;
                return lhs->id < rhs->id;
              });
  }

  void checkSameAs(Entry *list, unsigned line) {
    std::cout << " <<<  check " << line << std::endl;
    for (auto &entry : entries) {
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
  enum { prioritiesCount = 4 };
  static int getPriorityIndex(Entry *e) {
    assert(e->value < prioritiesCount);
    return (int)e->value;
  }
};

using EntryPriorityQueue = PriorityQueue<Entry *, EntryListTraits>;

struct ListBuilder {
  Entry *head;
  Entry **pTail;

  ListBuilder(): head(), pTail(&head) {}
  ListBuilder(ListBuilder const &) = delete;
  ListBuilder(ListBuilder &&) = delete;

  void append(Entry *e) {
    *pTail = e;
    e->next = nullptr;
    pTail = &e->next;
  }

  Entry *take() {
    Entry* result = head;
    head = nullptr;
    pTail = &head;
    return result;
  }
};

static void runEnqueueDequeueTest() {
  std::cout << "runEnqueueDequeueTest()" << std::endl;
  EntryFactory entries;
  EntryPriorityQueue queue;
  assert(!queue.head);
  assert(!queue.tails[0]);
  assert(!queue.tails[1]);
  assert(!queue.tails[2]);
  assert(!queue.tails[3]);

  auto first = entries.create(3);
  queue.enqueue(first);
  assert(queue.head == first);
  assert(!queue.tails[0]);
  assert(!queue.tails[1]);
  assert(!queue.tails[2]);
  assert(queue.tails[3] == first);

  auto second = entries.create(3);
  queue.enqueue(second);
  assert(queue.head == first);
  assert(!queue.tails[0]);
  assert(!queue.tails[1]);
  assert(!queue.tails[2]);
  assert(queue.tails[3] == second);

  auto third = entries.create(1);
  queue.enqueue(third);
  assert(queue.head == third);
  assert(!queue.tails[0]);
  assert(queue.tails[1] == third);
  assert(!queue.tails[2]);
  assert(queue.tails[3] == second);

  auto fourth = entries.create(2);
  queue.enqueue(fourth);
  assert(queue.head == third);
  assert(!queue.tails[0]);
  assert(queue.tails[1] == third);
  assert(queue.tails[2] == fourth);
  assert(queue.tails[3] == second);

  entries.sort();
  entries.checkSameAs(queue.head, __LINE__);

  auto pop = [&](Entry *expected) {
    auto e = queue.dequeue();
    assert(e == expected);
    entries.remove(e);
    entries.sort();
    entries.checkSameAs(queue.head, __LINE__);
  };

  pop(third);
  assert(queue.head == fourth);
  assert(!queue.tails[0]);
  assert(!queue.tails[1]);
  assert(queue.tails[2] == fourth);
  assert(queue.tails[3] == second);

  pop(fourth);
  assert(queue.head == first);
  assert(!queue.tails[0]);
  assert(!queue.tails[1]);
  assert(!queue.tails[2]);
  assert(queue.tails[3] == second);

  pop(first);
  assert(queue.head == second);
  assert(!queue.tails[0]);
  assert(!queue.tails[1]);
  assert(!queue.tails[2]);
  assert(queue.tails[3] == second);

  pop(second);
  assert(!queue.head);
  assert(!queue.tails[0]);
  assert(!queue.tails[1]);
  assert(!queue.tails[2]);
  assert(!queue.tails[3]);

  assert(!queue.dequeue());
  assert(!queue.head);
  assert(!queue.tails[0]);
  assert(!queue.tails[1]);
  assert(!queue.tails[2]);
  assert(!queue.tails[3]);
}

static void runEnqueueContentsOfTest() {
  std::cout << "runEnqueueContentsOfTest()" << std::endl;
  EntryFactory entries;
  EntryPriorityQueue queue;
  ListBuilder builder;
  assert(!queue.head);
  assert(!queue.tails[0]);
  assert(!queue.tails[1]);
  assert(!queue.tails[2]);
  assert(!queue.tails[3]);

  queue.enqueueContentsOf(builder.take());
  assert(!queue.head);
  assert(!queue.tails[0]);
  assert(!queue.tails[1]);
  assert(!queue.tails[2]);
  assert(!queue.tails[3]);

  auto first = entries.create(3);
  builder.append(first);
  auto second = entries.create(3);
  builder.append(second);
  auto third = entries.create(1);
  builder.append(third);
  auto fourth = entries.create(2);
  builder.append(fourth);
  auto fifth = entries.create(2);
  builder.append(fifth);
  auto sixth = entries.create(1);
  builder.append(sixth);

  queue.enqueueContentsOf(builder.take());
  assert(queue.head == third);
  assert(!queue.tails[0]);
  assert(queue.tails[1] == sixth);
  assert(queue.tails[2] == fifth);
  assert(queue.tails[3] == second);
  entries.sort();
  entries.checkSameAs(queue.head, __LINE__);

  auto seventh = entries.create(0);
  builder.append(seventh);
  auto eighth = entries.create(0);
  builder.append(eighth);
  auto ninth = entries.create(0);
  builder.append(ninth);
  auto tenth = entries.create(3);
  builder.append(tenth);
  queue.enqueueContentsOf(builder.take());

  assert(queue.head == seventh);
  assert(queue.tails[0] == ninth);
  assert(queue.tails[1] == sixth);
  assert(queue.tails[2] == fifth);
  assert(queue.tails[3] == tenth);
  entries.sort();
  entries.checkSameAs(queue.head, __LINE__);
}

static void runEnqueueDequeueTests(unsigned numTests, unsigned maxEntries) {
  std::random_device randomDevice;
  std::default_random_engine e(randomDevice());
  std::uniform_int_distribution<unsigned> valueDist(
      0, EntryListTraits::prioritiesCount - 1);
  std::uniform_int_distribution<unsigned> numEntriesDist(0, maxEntries);

  for (unsigned testN = 0; testN < numTests; ++testN) {
    std::cout << "runEnqueueDequeueTests() #" << testN << std::endl;
    EntryFactory entries;
    EntryPriorityQueue queue;
    unsigned numEntries = numEntriesDist(e);
    std::cout << "numEntries = " << numEntries << std::endl;
    for (unsigned i = 0; i < numEntries; ++i) {
      auto value = valueDist(e);
      std::cout << "-- " << value << std::endl;
      queue.enqueue(entries.create(value));
    }
    entries.sort();
    entries.checkSameAs(queue.head, __LINE__);
    for (unsigned i = 0; i < numEntries; ++i) {
      auto e = queue.dequeue();
      std::cout << "pop " << e->value << std::endl;
      entries.remove(e);
      entries.sort();
      entries.checkSameAs(queue.head, __LINE__);
    }
  }
}

static void runEnqueueContentsOfTests(unsigned numTests, unsigned maxChains, unsigned maxEntries) {
  std::random_device randomDevice;
  std::default_random_engine e(randomDevice());
  std::uniform_int_distribution<unsigned> valueDist(
      0, EntryListTraits::prioritiesCount - 1);
  std::uniform_int_distribution<unsigned> numChainsDist(1, maxChains);
  std::uniform_int_distribution<unsigned> numEntriesDist(0, maxEntries);

  for (unsigned testN = 0; testN < numTests; ++testN) {
    std::cout << "runEnqueueContentsOfTests() #" << testN << std::endl;
    EntryFactory entries;
    EntryPriorityQueue queue;
    unsigned totalEntries = 0;
    unsigned numChains = numChainsDist(e);
    std::cout << "numChains = " << numChains << std::endl;
    for (unsigned i = 0; i < numChains; ++i) {
      unsigned numEntries = numEntriesDist(e);
      std::cout << "numEntries = " << numEntries << std::endl;
      totalEntries += numEntries;

      ListBuilder builder;
      for (unsigned j = 0; j < numEntries; ++j) {
        auto value = valueDist(e);
        std::cout << "-- " << value << std::endl;
        builder.append(entries.create(value));
      }
      queue.enqueueContentsOf(builder.take());
    }
    entries.sort();
    entries.checkSameAs(queue.head, __LINE__);
    for (unsigned i = 0; i < totalEntries; ++i) {
      auto e = queue.dequeue();
      std::cout << "pop " << e->value << std::endl;
      entries.remove(e);
      entries.sort();
      entries.checkSameAs(queue.head, __LINE__);
    }
  }
}

} // namespace

int main() {
  runEnqueueDequeueTest();
  runEnqueueContentsOfTest();
  runEnqueueDequeueTests(1000, 20);
  runEnqueueContentsOfTests(1000, 10, 20);
}
