#pragma once

#include <cstdint>
#include <libpmemobj++/persistent_ptr.hpp>
#include <libpmemobj.h>
#include "iterator.h"
#include "debug.h"

namespace combotree {

#define LEAF_ENTRYS   16
#define INDEX_ENTRYS  8   // must be even

class CLevel {
 public:

  void InitLeaf();

  bool Insert(uint64_t key, uint64_t value);
  bool Update(uint64_t key, uint64_t value);
  bool Get(uint64_t key, uint64_t& value) const;
  bool Delete(uint64_t key);

  class Iter;

  Iterator* begin();
  Iterator* end();

  static void SetPoolBase(pmem::obj::pool_base pool_base) {
    pop_ = pool_base;
  }

  static pmem::obj::pool_base& GetPoolBase() {
    return pop_;
  }

 private:
  struct Entry;
  struct LeafNode;
  struct IndexNode;

  enum class NodeType {
    LEAF,
    INDEX,
  };

  pmem::obj::persistent_ptr_base root_;
  pmem::obj::persistent_ptr<LeafNode> head_;
  NodeType type_;

  static pmem::obj::pool_base pop_;

  pmem::obj::persistent_ptr<LeafNode> leaf_root_() const {
    return static_cast<pmem::obj::persistent_ptr<LeafNode>>(root_.raw());
  }

  pmem::obj::persistent_ptr<IndexNode> index_root_() const {
    return static_cast<pmem::obj::persistent_ptr<IndexNode>>(root_.raw());
  }
};

struct CLevel::Entry {
  Entry() : key(0), value(0) {}

  uint64_t key;
  union {
    uint64_t value;
    // pmem::obj::persistent_ptr<void> pvalue;
  };
};

struct CLevel::LeafNode {
  LeafNode() : prev(nullptr), next(nullptr), parent(nullptr),
               sorted_array(0), nr_entry(0), next_entry(0) {}

  pmem::obj::persistent_ptr<LeafNode> prev;
  pmem::obj::persistent_ptr<LeafNode> next;
  pmem::obj::persistent_ptr<IndexNode> parent;
  uint64_t sorted_array;  // used as an array of uint4_t
  int nr_entry;
  int next_entry;
  Entry entry[LEAF_ENTRYS];

  bool Insert(uint64_t key, uint64_t value, pmem::obj::persistent_ptr_base& root);
  bool Update(uint64_t key, uint64_t value, pmem::obj::persistent_ptr_base& root);
  bool Get(uint64_t key, uint64_t& value) const;
  bool Delete(uint64_t key, pmem::obj::persistent_ptr_base& root);

  void PrintSortedArray() const;

  friend Iter;

 private:
  bool Split_(pmem::obj::persistent_ptr_base& root);

  /*
   * find entry index which is equal or bigger than key
   *
   * @find   if equal, return true
   * @return entry index
   */
  int Find_(uint64_t key, bool& find) const;

  uint64_t GetSortedArrayMask_(int index) const {
    return (uint64_t)0x0FUL << ((LEAF_ENTRYS - 1 - index) * 4);
  }

  /*
   * get entry index in sorted array
   */
  int GetSortedEntry_(int sorted_index) const {
    uint64_t mask = GetSortedArrayMask_(sorted_index);
    return (sorted_array & mask) >> ((LEAF_ENTRYS - 1 - sorted_index) * 4);
  }

  int GetFreeIndex_() const {
    debug_assert(next_entry == LEAF_ENTRYS);
    int nr_free = next_entry - nr_entry;
    uint64_t mask = (uint64_t)0x0FUL << ((nr_free - 1) * 4);
    return (sorted_array & mask) >> ((nr_free - 1) * 4);
  }

  uint64_t GetEntryKey_(int entry_idx) const {
    return entry[entry_idx].key;
  }
};

struct CLevel::IndexNode {
  pmem::obj::persistent_ptr<IndexNode> parent;
  uint64_t keys[INDEX_ENTRYS + 1];
  pmem::obj::persistent_ptr_base child[INDEX_ENTRYS + 2];
  NodeType child_type;
  int nr_entry;
  int next_entry;
  uint8_t sorted_array[INDEX_ENTRYS + 1];

  bool Insert(uint64_t key, uint64_t value, pmem::obj::persistent_ptr_base& root);
  bool Update(uint64_t key, uint64_t value, pmem::obj::persistent_ptr_base& root);
  bool Get(uint64_t key, uint64_t& value) const;
  bool Delete(uint64_t key, pmem::obj::persistent_ptr_base& root);

  bool InsertChild(uint64_t child_key, pmem::obj::persistent_ptr_base child,
                   pmem::obj::persistent_ptr_base& root);

 private:
  bool Split_(pmem::obj::persistent_ptr_base& root);

  void AdoptChild_();

  pmem::obj::persistent_ptr<LeafNode> FindLeafNode_(uint64_t key) const;

  pmem::obj::persistent_ptr<LeafNode> leaf_child_(int index) const {
    return static_cast<pmem::obj::persistent_ptr<LeafNode>>(child[index].raw());
  }

  pmem::obj::persistent_ptr<IndexNode> index_child_(int index) const {
    return static_cast<pmem::obj::persistent_ptr<IndexNode>>(child[index].raw());
  }
};

// TODO: Begin(), End(), CLevel::begin(), CLevel::end()
// is not the same.
class CLevel::Iter : public Iterator {
 public:
  Iter(CLevel* clevel)
      : clevel_(clevel), leaf_(clevel_->head_), sorted_index_(0)  {}
  ~Iter() {};

  bool Begin() const {
    return OID_EQUALS(leaf_.raw(), clevel_->head_.raw()) &&
           sorted_index_ == 0;
  }

  // leaf_ point to the last, sorted_index_ equals nr_entry
  bool End() const {
    return OID_EQUALS(leaf_.raw(), clevel_->head_->prev.raw()) &&
           sorted_index_ == leaf_->nr_entry;
  }

  void SeekToFirst() {
    leaf_ = clevel_->head_;
    while (!OID_EQUALS(leaf_.raw(), clevel_->head_->prev.raw()) &&
           leaf_->nr_entry == 0) {
      leaf_ = leaf_->next;
    }
    sorted_index_ = 0;
  }

  void SeekToLast() {
    leaf_ = clevel_->head_->prev;
    while (!OID_EQUALS(leaf_.raw(), clevel_->head_.raw()) &&
           leaf_->nr_entry == 0) {
      leaf_ = leaf_->prev;
    }
    sorted_index_ = std::max(0, leaf_->nr_entry - 1);
  }

  void Seek(uint64_t target) {
    assert(0);
  }

  void Next() {
    if (sorted_index_ < leaf_->nr_entry - 1) {
      sorted_index_++;
    } else if (OID_EQUALS(leaf_.raw(), clevel_->head_->prev.raw())) {
      sorted_index_ = leaf_->nr_entry;
    } else {
      do {
        leaf_ = leaf_->next;
      } while (!OID_EQUALS(leaf_.raw(), clevel_->head_->prev.raw()) &&
               leaf_->nr_entry == 0);
      sorted_index_ = 0;
    }
  }

  void Prev() {
    if (sorted_index_ > 0) {
      sorted_index_--;
    } else if (OID_EQUALS(leaf_.raw(), clevel_->head_.raw())) {
      sorted_index_ = 0;
    } else {
      do {
        leaf_ = leaf_->prev;
      } while (!OID_EQUALS(leaf_.raw(), clevel_->head_.raw()) &&
               leaf_->nr_entry == 0);
      sorted_index_ = std::max(0, leaf_->nr_entry - 1);
    }
  }

  uint64_t key() const {
    int entry_idx = leaf_->GetSortedEntry_(sorted_index_);
    return leaf_->entry[entry_idx].key;
  }

  uint64_t value() const {
    int entry_idx = leaf_->GetSortedEntry_(sorted_index_);
    return leaf_->entry[entry_idx].value;
  }

 private:
  CLevel* clevel_;
  pmem::obj::persistent_ptr<LeafNode> leaf_;
  int sorted_index_;
};

} // namespace combotree