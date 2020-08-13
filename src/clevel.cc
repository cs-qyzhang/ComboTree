#include <iostream>
#include <set>
#include <libpmemobj++/persistent_ptr.hpp>
#include <libpmemobj++/make_persistent_atomic.hpp>
#include <libpmemobj++/transaction.hpp>
#include <libpmemobj.h>
#include "slab.h"
#include "clevel.h"
#include "debug.h"

namespace combotree {

using pmem::obj::persistent_ptr;
using pmem::obj::persistent_ptr_base;
using pmem::obj::make_persistent_atomic;
using pmem::obj::pool_base;

void CLevel::LeafNode::Valid_() {
  std::set<int> idx;
  for (uint32_t i = 0; i < nr_entry; ++i) {
    int index = GetSortedEntry_(i);
    assert(idx.count(index) == 0);
    idx.emplace(index);
  }
  for (uint32_t i = 0; i < next_entry - nr_entry; ++i) {
    int index = GetSortedEntry_(15 - i);
    assert(idx.count(index) == 0);
    idx.emplace(index);
  }
}

void CLevel::InitLeaf(pool_base& pop, Slab<LeafNode>* slab) {
  type_ = NodeType::LEAF;
  head_ = slab->Allocate();
  head_->prev = head_;
  head_->next = head_;
  root_ = head_;
  pop.persist(head_, sizeof(*head_));
  pop.persist(this, sizeof(*this));
}

Status CLevel::Insert(pool_base& pop, Slab<LeafNode>* leaf_slab, uint64_t key, uint64_t value) {
  if (type_ == NodeType::LEAF) {
    void* root = root_;
    Status s = leaf_root_()->Insert(pop, leaf_slab, key, value, root);
    if (root != root_) {
      type_ = NodeType::INDEX;
      root_ = root;
      pop.persist(&type_, sizeof(type_));
      pop.persist(&root_, sizeof(root_));
    }
    return s;
  } else
    return index_root_()->Insert(pop, leaf_slab, key, value, root_);
}

Status CLevel::Update(pool_base& pop, uint64_t key, uint64_t value) {
  if (type_ == NodeType::LEAF)
    return leaf_root_()->Update(pop, key, value, root_);
  else
    return index_root_()->Update(pop, key, value, root_);
}

Status CLevel::Get(uint64_t key, uint64_t& value) const {
  if (type_ == NodeType::LEAF)
    return leaf_root_()->Get(key, value);
  else
    return index_root_()->Get(key, value);
}

Status CLevel::Delete(pool_base& pop, uint64_t key) {
  if (type_ == NodeType::LEAF)
    return leaf_root_()->Delete(pop, key);
  else
    return index_root_()->Delete(pop, key);
}

bool CLevel::Scan(uint64_t max_key, size_t max_size, size_t& size,
                  std::function<void(uint64_t,uint64_t)> callback) {
  LeafNode* node = head_;
  do {
    for (uint32_t i = 0; i < node->nr_entry; ++i) {
      uint64_t entry_key = node->GetEntryKey_(node->GetSortedEntry_(i));
      if (size >= max_size || entry_key > max_key) {
        return true;
      }
      callback(entry_key, node->entry[node->GetSortedEntry_(i)].value);
      size++;
    }
    node = node->next;
  } while (node != head_);
  return false;
}

// find sorted index which is bigger or equal to key
int CLevel::LeafNode::Find_(uint64_t key, bool& find) const {
  int left = 0;
  int right = nr_entry - 1;
  // binary search
  while (left <= right) {
    int middle = (left + right) / 2;
    int mid_idx = GetSortedEntry_(middle);
    uint64_t mid_key = GetEntryKey_(mid_idx);
    if (mid_key == key) {
      find = true;
      return middle; // find
    } else if (mid_key < key) {
      left = middle + 1;
    } else {
      right = middle - 1;
    }
  }
  // not found, return first entry bigger than key
  find = false;
  return left;
}

void CLevel::LeafNode::PrintSortedArray() const {
  for (int i = 0; i < 16; ++i) {
    std::cout << i << ": " << GetSortedEntry_(i) << std::endl;
  }
}

bool CLevel::LeafNode::Split_(pool_base& pop, Slab<LeafNode>* leaf_slab, void*& root) {
  // LOG(Debug::INFO, "Split");
  assert(nr_entry == LEAF_ENTRYS);

  pmem::obj::transaction::run(pop, [&](){
  if (parent == nullptr) {
    persistent_ptr<IndexNode> tmp;
    make_persistent_atomic<IndexNode>(pop, tmp);
    parent = tmp.get();
    parent->child_type = NodeType::LEAF;
    parent->child[0] = this;
    root = parent;
  }

  LeafNode* new_node = leaf_slab->Allocate();
  uint64_t new_sorted_array = 0;
  // copy bigger half to new node
  for (int i = 0; i < LEAF_ENTRYS / 2; ++i) {
    new_node->entry[i].key = entry[GetSortedEntry_(i + LEAF_ENTRYS / 2)].key;
    new_node->entry[i].value = entry[GetSortedEntry_(i + LEAF_ENTRYS / 2)].value;
    new_sorted_array = (new_sorted_array << 4) | i;
  }
  new_node->prev = this;
  new_node->next = next;
  new_node->next->prev = new_node;
  new_node->prev->next = new_node;
  new_node->parent = parent;
  new_node->nr_entry = LEAF_ENTRYS / 2;
  new_node->next_entry = LEAF_ENTRYS / 2;
  new_node->sorted_array = new_sorted_array << ((16 - new_node->nr_entry) * 4);
  pop.persist(new_node, sizeof(*new_node));

#if LEAF_ENTRYS != 16
  // add free entry
  uint64_t free_mask = 0;
  for (int i = 0; i < LEAF_ENTRYS - LEAF_ENTRYS / 2; ++i)
    free_mask = (free_mask << 4) | 0x0FUL;
  free_mask <<= (16 - LEAF_ENTRYS) * 4;
  uint64_t free_index = (sorted_array & free_mask) >> ((16 - LEAF_ENTRYS) * 4);

  uint64_t before_mask = 0;
  for (int i = 0; i < LEAF_ENTRYS / 2; ++i)
    before_mask = (before_mask >> 4) | 0xF000000000000000UL;
  uint64_t before_index = sorted_array & before_mask;
  sorted_array = before_index | free_index;
  pop.persist(&sorted_array, sizeof(sorted_array));
#endif
  nr_entry = LEAF_ENTRYS - LEAF_ENTRYS / 2;
  pop.persist(&nr_entry, sizeof(nr_entry));
  parent->InsertChild(pop, new_node->entry[new_node->GetSortedEntry_(0)].key, new_node, root);
  });
  return true;
}

Status CLevel::LeafNode::Insert(pool_base& pop, Slab<LeafNode>* leaf_slab, uint64_t key, uint64_t value,
                                void*& root) {
  bool find;
  int sorted_index = Find_(key, find);
  // already exist
  if (find) return Status::ALREADY_EXISTS;

  // if not find, insert key in index
  if (nr_entry == LEAF_ENTRYS) {
    // full, split
    Split_(pop, leaf_slab, root);
    // key should insert to the newly split node
    if (key >= next->entry[next->GetSortedEntry_(0)].key)
      return next->Insert(pop, leaf_slab, key, value, root);
  }

  uint64_t new_sorted_array;
  int entry_idx = (next_entry != LEAF_ENTRYS) ? next_entry : GetFreeIndex_();

  // free mask is free index in sorted_array
  // after mask is the index which is bigger than sorted_index
  // before mask is the index which is less than sorted_index
  uint64_t free_mask = 0;
  int nr_free = (next_entry != LEAF_ENTRYS) ? (next_entry - nr_entry)
                                            : (next_entry - nr_entry - 1);
  for (int i = 0; i < nr_free; ++i)
    free_mask = (free_mask << 4) | 0x0FUL;
  uint64_t free_index = sorted_array & free_mask;

  uint64_t after_mask = 0;
  for (uint32_t i = 0; i < nr_entry - sorted_index; ++i)
    after_mask = (after_mask << 4) | 0x0FUL;
  after_mask = after_mask << ((16 - nr_entry) * 4);
  uint64_t after_index = (sorted_array & after_mask) >> 4;

  uint64_t before_mask = 0;
  for (int i = 0; i < sorted_index; ++i)
    before_mask = (before_mask >> 4) | 0xF000000000000000UL;
  uint64_t before_index = sorted_array & before_mask;
  new_sorted_array =
      before_index |
      ((uint64_t)entry_idx << ((15 - sorted_index) * 4)) |
      after_index |
      free_index;

  Entry new_ent;
  new_ent.key = key;
  new_ent.value = value;
  pop.memcpy_persist(&entry[entry_idx], &new_ent, sizeof(new_ent));

  // meta_data include sorted_array, nr_entry and next_entry.
  // meta_data[0-1] is sorted_array
  // meta_data[2]   is nr_entry
  // meta_data[3]   is next_entry
  static_assert(offsetof(CLevel::LeafNode, nr_entry) - offsetof(CLevel::LeafNode, sorted_array) == 8);
  static_assert(offsetof(CLevel::LeafNode, next_entry) - offsetof(CLevel::LeafNode, nr_entry) == 4);
  static_assert(sizeof(next_entry) == 4);
  uint32_t meta_data[4];
  *(uint64_t*)meta_data = new_sorted_array;
  meta_data[2] = nr_entry + 1;
  meta_data[3] = (uint32_t)entry_idx == next_entry ? next_entry + 1 : next_entry;
  pop.memcpy_persist(&sorted_array, meta_data, sizeof(meta_data));

  // Valid_();
  // PrintSortedArray();
  return Status::OK;
}

Status CLevel::LeafNode::Delete(pool_base& pop, uint64_t key) {
  bool find;
  int sorted_index = Find_(key, find);
  if (!find) {
    // key not exist
    return Status::DOES_NOT_EXIST;
  }

  uint64_t entry_index = GetSortedEntry_(sorted_index);

  // free mask is free index in sorted_array
  // after mask is the index which is bigger than sorted_index
  // before mask is the index which is less than sorted_index
  uint64_t free_mask = 0;
  int nr_free = next_entry - nr_entry;
  for (int i = 0; i < nr_free; ++i)
    free_mask = (free_mask << 4) | 0x0FUL;
  uint64_t free_index = sorted_array & free_mask;

  uint64_t after_mask = 0;
  for (uint32_t i = 0; i < nr_entry - sorted_index - 1; ++i)
    after_mask = (after_mask << 4) | 0x0FUL;
  after_mask = after_mask << ((16 - nr_entry) * 4);
  uint64_t after_index = (sorted_array & after_mask) << 4;

  uint64_t before_mask = 0;
  for (int i = 0; i < sorted_index; ++i)
    before_mask = (before_mask >> 4) | 0xF000000000000000UL;
  uint64_t before_index = sorted_array & before_mask;

  uint64_t new_sorted_array =
      before_index |
      after_index |
      (entry_index << ((next_entry - nr_entry) * 4)) |
      free_index;

  // meta_data include sorted_array, nr_entry and next_entry.
  // meta_data[0-1] is sorted_array
  // meta_data[2]   is nr_entry
  static_assert(offsetof(CLevel::LeafNode, nr_entry) - offsetof(CLevel::LeafNode, sorted_array) == 8);
  static_assert(sizeof(nr_entry) == 4);
  uint32_t meta_data[3];
  *(uint64_t*)meta_data = new_sorted_array;
  meta_data[2] = nr_entry - 1;
  pop.memcpy_persist(&sorted_array, meta_data, sizeof(meta_data));

  // Valid_();
  // PrintSortedArray();
  return Status::OK;
}

Status CLevel::LeafNode::Update(pool_base& pop, uint64_t key, uint64_t value, void*& root) {
  assert(0);
}

Status CLevel::LeafNode::Get(uint64_t key, uint64_t& value) const {
  bool find;
  int sorted_index = Find_(key, find);
  if (!find) {
    // key not exist
    return Status::DOES_NOT_EXIST;
  }

  int entry_index = GetSortedEntry_(sorted_index);
  value = entry[entry_index].value;
  return Status::OK;
}

CLevel::LeafNode* CLevel::IndexNode::FindLeafNode_(uint64_t key) const {
  assert(nr_entry > 0);
  if (key >= keys[sorted_array[nr_entry - 1]]) {
    if (child_type == CLevel::NodeType::LEAF)
      return leaf_child_(sorted_array[nr_entry - 1] + 1);
    else
      return index_child_(sorted_array[nr_entry - 1] + 1)->FindLeafNode_(key);
  }

  int left = 0;
  int right = nr_entry - 1;
  // find first entry greater or equal than key
  while (left <= right) {
    int middle = (left + right) / 2;
    uint64_t mid_key = keys[sorted_array[middle]];
    if (mid_key > key) { // TODO:
      right = middle - 1;
    } else if (mid_key <= key) {
      left = middle + 1;
    }
  }

  int child_idx = right == -1 ? 0 : sorted_array[right] + 1;
  if (child_type == CLevel::NodeType::LEAF)
    return leaf_child_(child_idx);
  else
    return index_child_(child_idx)->FindLeafNode_(key);
}

void CLevel::IndexNode::AdoptChild_(pool_base& pop) {
  for (int i = 0; i <= nr_entry; ++i) {
    if (child_type == NodeType::INDEX) {
      index_child_(i)->parent = this;
      pop.persist(&index_child_(i)->parent, sizeof(index_child_(i)->parent));
    } else {
      leaf_child_(i)->parent = this;
      pop.persist(&leaf_child_(i)->parent, sizeof(leaf_child_(i)->parent));
    }
  }
}

bool CLevel::IndexNode::Split_(pool_base& pop, void*& root) {
  // LOG(Debug::INFO, "IndexNode Split");
  assert(nr_entry == INDEX_ENTRYS + 1);

  if (parent == nullptr) {
    persistent_ptr<IndexNode> tmp;
    make_persistent_atomic<IndexNode>(pop, tmp);
    parent = tmp.get();
    parent->child_type = NodeType::INDEX;
    parent->child[0] = this;
    root = parent;
  }

  persistent_ptr<IndexNode> new_node;
  make_persistent_atomic<IndexNode>(pop, new_node);
  uint8_t new_sorted_array[INDEX_ENTRYS];
  new_node->child_type = child_type;
  // copy bigger half to new node
  for (int i = 0; i < INDEX_ENTRYS / 2; ++i) {
    new_node->keys[i] = keys[sorted_array[i + INDEX_ENTRYS / 2 + 1]];
    new_node->child[i + 1] = child[sorted_array[i + INDEX_ENTRYS / 2 + 1] + 1];
    new_sorted_array[i] = i;
  }
  new_node->child[0] = child[sorted_array[INDEX_ENTRYS / 2] + 1];
  new_node->parent = parent;
  new_node->nr_entry = INDEX_ENTRYS / 2;
  new_node->next_entry = INDEX_ENTRYS / 2;
  memcpy(new_node->sorted_array, new_sorted_array, sizeof(uint8_t) * new_node->nr_entry);
  // change children's parent
  new_node->AdoptChild_(pop);
  new_node.persist();

  nr_entry = INDEX_ENTRYS / 2;
  parent->InsertChild(pop, keys[sorted_array[INDEX_ENTRYS / 2]], new_node.get(), root);
  return true;
}

bool CLevel::IndexNode::InsertChild(pool_base& pop, uint64_t child_key,
                                    void* new_child,
                                    void*& root) {
  int left = 0;
  int right = nr_entry - 1;

  while (left <= right) {
    int middle = (left + right) / 2;
    uint64_t mid_key = keys[sorted_array[middle]];
    if (mid_key == child_key) {
      assert(0);
    } else if (mid_key < child_key) {
      left = middle + 1;
    } else {
      right = middle - 1;
    }
  }

  // left is the first entry bigger than leaf
  uint8_t new_sorted_array[INDEX_ENTRYS + 1];
  int entry_idx = (next_entry != INDEX_ENTRYS + 1) ? next_entry : sorted_array[nr_entry];
  memcpy(new_sorted_array, sorted_array, sizeof(sorted_array));
  for (int i = nr_entry; i > left; --i)
    new_sorted_array[i] = new_sorted_array[i - 1];
  new_sorted_array[left] = entry_idx;

  // TODO: persist
  keys[entry_idx] = child_key;
  child[entry_idx + 1] = new_child;
  memcpy(sorted_array, new_sorted_array, sizeof(sorted_array));
  if (child_type == NodeType::INDEX)
    index_child_(entry_idx + 1)->parent = this;
  else
    leaf_child_(entry_idx + 1)->parent = this;
  nr_entry++;
  if (entry_idx == next_entry) next_entry++;

  if (nr_entry == INDEX_ENTRYS + 1) {
    // full, split first
    Split_(pop, root);
  }

  return true;
}

Status CLevel::IndexNode::Insert(pool_base& pop, Slab<LeafNode>* leaf_slab,
                                 uint64_t key, uint64_t value, void*& root) {
  auto leaf = FindLeafNode_(key);
  return leaf->Insert(pop, leaf_slab, key, value, root);
}

Status CLevel::IndexNode::Update(pool_base& pop, uint64_t key, uint64_t value, void*& root) {
  auto leaf = FindLeafNode_(key);
  return leaf->Update(pop, key, value, root);
}

Status CLevel::IndexNode::Get(uint64_t key, uint64_t& value) const {
  auto leaf = FindLeafNode_(key);
  return leaf->Get(key, value);
}

Status CLevel::IndexNode::Delete(pool_base& pop, uint64_t key) {
  auto leaf = FindLeafNode_(key);
  return leaf->Delete(pop, key);
}

Iterator* CLevel::begin() {
  Iterator* iter = new Iter(this);
  iter->SeekToFirst();
  return iter;
}

Iterator* CLevel::end() {
  Iterator* iter = new Iter(this);
  iter->SeekToLast();
  return iter;
}

} // namespace combotree