//===--- TaskLocal.h - ABI of task local values -----------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Swift ABI describing task locals.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_ABI_TASKLOCAL_H
#define SWIFT_ABI_TASKLOCAL_H

#include "swift/ABI/HeapObject.h"
#include "swift/ABI/Metadata.h"
#include "swift/ABI/MetadataValues.h"

namespace swift {
class AsyncTask;
struct OpaqueValue;
struct SwiftError;
class TaskStatusRecord;
class TaskGroup;

// ==== Task Locals Values ---------------------------------------------------

class TaskLocal {
public:
  class Storage;

  /// Type of item in the task local item linked list.
  enum class ItemKind : intptr_t {
    /// Regular value item.
    /// Has @c valueType and @c key .
    /// Value is stored in the trailing storage.
    /// @c next pointer points to another item owned by the same task as current
    /// item.
    Value = -1,

    /// Item that marks end of sequence of items owned by the current task.
    /// @c next pointer points to an item owned by another AsyncTask.
    ///
    /// Note that this may not necessarily be the same as the task's parent
    /// task -- we may point to a super-parent if we know / that the parent
    /// does not "contribute" any task local values. This is to speed up
    /// lookups by skipping empty parent tasks during @c get() , and explained
    /// in depth in @c createParentLink() .
    ParentLink = 0,

    /// Stop-item that blocks further lookup.
    /// Inserting stop-node allows to temporary disable all inserted task-local
    /// values in O(1),
    /// while maintaining immutable linked list nature of the task-local values
    /// implementation.
    Stop = 1,
  };

  class Item {
  private:
    /// Pointer to the next item in the chain.
    Item *const next;

    union KeyOrKind {
      /// The type of the key with which this value is associated.
      /// Set if valueType is not null
      const HeapObject *key;

      /// Kind of the node
      /// Set if valueType is null
      ItemKind kind;

      KeyOrKind(const HeapObject *key) : key(key) {}
      KeyOrKind(ItemKind kind) : kind(kind) {}
    } const keyOrKind;

    /// The type of the value stored by this item.
    const Metadata *const valueType;

    // Trailing storage for an instance of \c valueType if kind is
    // ItemKind::Value

  protected:
    explicit Item(Item *next, ItemKind kind)
        : next(next), keyOrKind(kind), valueType(nullptr) {}

    explicit Item(Item *next, const HeapObject *key, const Metadata *valueType)
        : next(next), keyOrKind(key), valueType(valueType) {
      assert(valueType != nullptr);
    }

    static void *allocate(size_t amountToAllocate, AsyncTask *task);

  public:
    /// Item which does not by itself store any value, but only points
    /// to the nearest task-local-value containing parent's first task item.
    ///
    /// This item type is used to link to the appropriate parent task's item,
    /// when the current task itself does not have any task local values itself.
    ///
    /// When a task actually has its own task locals, it should rather point
    /// to the parent's *first* task-local item in its *last* item, extending
    /// the Item linked list into the appropriate parent.
    static Item *createParentLink(AsyncTask *task, AsyncTask *parent);

    static Item *createValue(Item *next, AsyncTask *task, const HeapObject *key,
                             const Metadata *valueType);

    static Item *createStop(Item *next, AsyncTask *task);

    /// Destroys value and frees memory using specified task for deallocation.
    /// If task is null, then th
    void destroy(AsyncTask *task);

    Item *getNext() { return next; }

    /// Returns kind of this item.
    ItemKind getKind() const {
      return valueType ? ItemKind::Value : keyOrKind.kind;
    }

    /// Returns key of the value item.
    /// Available only if @c getKind() is @c ItemKind::Value .
    const HeapObject *getKey() const {
      assert(getKind() == ItemKind::Value);
      return keyOrKind.key;
    }

    /// Returns value type of the value item.
    /// Available only if @c getKind() is @c ItemKind::Value .
    const Metadata *getValueType() const {
      assert(getKind() == ItemKind::Value);
      return valueType;
    }

    /// Retrieve a pointer to the storage of the value.
    /// Available only if @c getKind() is @c ItemKind::Value .
    OpaqueValue *getStoragePtr() {
      assert(getKind() == ItemKind::Value);
      return reinterpret_cast<OpaqueValue *>(
        reinterpret_cast<char *>(this) + storageOffset(valueType));
    }

    void copyTo(Storage *target, AsyncTask *task);

    /// Compute the offset of the storage from the base of the item.
    static size_t storageOffset(const Metadata *valueType) {
      size_t offset = sizeof(Item);

      if (valueType) {
        size_t alignment = valueType->vw_alignment();
        return (offset + alignment - 1) & ~(alignment - 1);
      } else {
        return offset;
      }
    }

    /// Determine the size of the item given a particular value type.
    static size_t itemSize(const Metadata *valueType) {
      size_t offset = storageOffset(valueType);
      if (valueType) {
        offset += valueType->vw_size();
      }
      return offset;
    }
  };

  class Storage {
    friend class TaskLocal::Item;
  private:
    /// A stack (single-linked list) of task local values.
    ///
    /// Once task local values within this task are traversed, the list
    /// continues to the "next parent that contributes task local values," or if
    /// no such parent exists it terminates with null.
    ///
    /// If the TaskLocalValuesFragment was allocated, it is expected that this
    /// value should be NOT null; it either has own values, or at least one
    /// parent that has values. If this task does not have any values, the head
    /// pointer MAY immediately point at this task's parent task which has
    /// values.
    ///
    /// ### Concurrency
    /// Access to the head is only performed from the task itself, when it
    /// creates child tasks, the child during creation will inspect its parent's
    /// task local value stack head, and point to it. This is done on the
    /// calling task, and thus needs not to be synchronized. Subsequent
    /// traversal is performed by child tasks concurrently, however they use
    /// their own pointers/stack and can never mutate the parent's stack.
    ///
    /// The stack is only pushed/popped by the owning task, at the beginning and
    /// end a `body` block of `withLocal(_:boundTo:body:)` respectively.
    ///
    /// Correctness of the stack strongly relies on the guarantee that child
    /// tasks never outlive a scope in which they are created. Thanks to this,
    /// if child tasks are created inside the `body` of
    /// `withLocal(_:,boundTo:body:)` all child tasks created inside the
    /// `withLocal` body must complete before it returns, as such, any child
    /// tasks potentially accessing the value stack are guaranteed to be
    /// completed by the time we pop values off the stack (after the body has
    /// completed).
    TaskLocal::Item *head = nullptr;

  public:

    void initializeLinkParent(AsyncTask *task, AsyncTask *parent);

    void pushValue(AsyncTask *task,
                   const HeapObject *key,
                   /* +1 */ OpaqueValue *value, const Metadata *valueType);

    void pushStop(AsyncTask *task);

    OpaqueValue* getValue(AsyncTask *task, const HeapObject *key);

    /// Returns `true` if more bindings remain in this storage,
    /// and `false` if the just popped value was the last one and the storage
    /// can be safely disposed of.
    bool pop(AsyncTask *task);

    /// Copy all task-local bindings to the target task.
    ///
    /// The new bindings allocate their own items and can out-live the current task.
    ///
    /// ### Optimizations
    /// Only the most recent binding of a value is copied over, i.e. given
    /// a key bound to `A` and then `B`, only the `B` binding will be copied.
    /// This is safe and correct because the new task would never have a chance
    /// to observe the `A` value, because it semantically will never observe a
    /// "pop" of the `B` value - it was spawned from a scope where only B was observable.
    void copyTo(Storage *target, AsyncTask *task);

    /// Destroy and deallocate all items stored by this specific task.
    /// If @c task is null, then this is a task-less storage and items are
    /// deallocated using free().
    ///
    /// Items owned by a parent task are left untouched, since we do not own
    /// them.
    void destroy(AsyncTask *task);
  };

  /// Copy all task locals from the current context to the target storage.
  /// To prevent data races, there should be no other accesses to the target
  /// storage, while copying. Target storage is asserted to be empty, as a proxy
  /// for being not in use. If @c task is specified, it will be used for memory
  /// management. If @c task is nil, items will be allocated using malloc(). The
  /// same value of @c task should be passed to @c TaskLocal::Storage::destroy()
  /// .
  static void copyTo(Storage *target, AsyncTask *task);

  class WithResetValuesScope {
    bool didPush;
  public:
    WithResetValuesScope();
    ~WithResetValuesScope();
  };
};

} // end namespace swift

#endif
