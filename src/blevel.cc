#include <iostream>
#include <memory>
#include <vector>
#include <libpmemobj++/persistent_ptr.hpp>
#include <libpmemobj++/make_persistent_atomic.hpp>
#include <libpmemobj++/make_persistent_array_atomic.hpp>
#include "combotree_config.h"
#include "blevel.h"
#include "debug.h"

namespace combotree {

BLevel::BLevel(pmem::obj::pool_base pop, const std::vector<std::pair<uint64_t, uint64_t>>& kv)
    : pop_(pop), in_mem_entry_(nullptr), in_mem_key_(nullptr),
      is_expanding_(false)
{
  assert(kv.size() > 1);
  pmem::obj::pool<Root> pool(pop_);
  root_ = pool.root();
  root_->nr_entry.store(kv[0].first == 0 ? kv.size() : kv.size() + 1);
  root_->size = kv.size();
  root_.persist();
  base_addr_ = (uint64_t)root_.get() - root_.raw().off;
  pmem::obj::make_persistent_atomic<Entry[]>(pop_, root_->entry, root_->nr_entry);
  clevel_slab_ = new Slab<CLevel>(pop_, EntrySize() / 4.0);
  Slab<CLevel::LeafNode>* leaf_slab = new Slab<CLevel::LeafNode>(pop_, EntrySize() / 4.0);
  clevel_mem_ = new CLevel::MemoryManagement(pop, leaf_slab);
  // add one extra lock to make blevel iter's logic simple
  locks_ = new std::shared_mutex[EntrySize() + 1];
  in_mem_entry_ = root_->entry.get();
  assert(GetEntry_(1) == &root_->entry[1]);
  int pos = 0;
  if (kv[0].first != 0) {
    Entry* ent = GetEntry_(pos);
    ent->SetKey(clevel_mem_, 0);
    ent->SetNone(clevel_mem_);
    pos++;
  }
  for (size_t i = 0; i < kv.size(); ++i) {
    std::lock_guard<std::shared_mutex> lock(locks_[pos]);
    Entry* ent = GetEntry_(pos);
    ent->SetKey(clevel_mem_, kv[i].first);
    ent->SetValue(clevel_mem_, kv[i].second);
    pos++;
  }
  in_mem_key_ = new uint64_t[root_->nr_entry];
  for (uint64_t i = 0; i < root_->nr_entry; ++i)
    in_mem_key_[i] = in_mem_entry_[i].key;
}

BLevel::BLevel(pmem::obj::pool_base pop, std::shared_ptr<BLevel> old_blevel)
    : pop_(pop), in_mem_entry_(nullptr), in_mem_key_(nullptr),
      is_expanding_(true)
{
  pmem::obj::pool<Root> pool(pop_);
  root_ = pool.root();
  root_->nr_entry.store(old_blevel->Size() * ENTRY_SIZE_FACTOR);  // reserve some entry for insertion
  root_->size.store(EntrySize());
  root_.persist();
  expanding_entry_index_.store(0, std::memory_order_release);
  base_addr_ = (uint64_t)root_.get() - root_.raw().off;
  pmem::obj::make_persistent_atomic<Entry[]>(pop_, root_->entry, EntrySize());
  clevel_slab_ = new Slab<CLevel>(pop_, 256);
  Slab<CLevel::LeafNode>* leaf_slab = new Slab<CLevel::LeafNode>(pop_, 256);
  clevel_mem_ = new CLevel::MemoryManagement(pop, leaf_slab);
  // add one extra lock to make blevel iter's logic simple
  locks_ = new std::shared_mutex[EntrySize() + 1];
  in_mem_entry_ = root_->entry.get();
  in_mem_key_ = new uint64_t[root_->nr_entry];
  assert(GetEntry_(1) == &root_->entry[1]);
}

void BLevel::ExpandAddEntry_(uint64_t key, uint64_t value, size_t& size) {
  uint64_t entry_index = expanding_entry_index_.load(std::memory_order_acquire);
  if (entry_index < EntrySize()) {
    std::lock_guard<std::shared_mutex> lock(locks_[entry_index]);
    Entry* ent = GetEntry_(entry_index);
    ent->SetKey(clevel_mem_, key);
    ent->SetValue(clevel_mem_, value);
    in_mem_key_[entry_index] = key;
    size++;
    expanding_entry_index_.fetch_add(1, std::memory_order_release);
  } else {
    std::lock_guard<std::shared_mutex> lock(locks_[EntrySize() - 1]);
    Entry* ent = GetEntry_(EntrySize() - 1);
    if (ent->IsClevel()) {
      Entry::GetClevel(clevel_mem_, ent->value)->Insert(clevel_mem_, key, value);
    } else if (ent->IsValue()) {
      CLevel* new_clevel = clevel_slab_->Allocate();
      new_clevel->InitLeaf(clevel_mem_);
      new_clevel->Insert(clevel_mem_, ent->key, Entry::GetValue(ent->value));
      new_clevel->Insert(clevel_mem_, key, value);
      ent->SetCLevel(clevel_mem_, new_clevel);
    } else if (ent->IsNone()) {
      assert(0);
    } else {
      assert(0);
    }
    size++;
  }
}

void BLevel::ExpansionCallback_(uint64_t key, uint64_t value, void* arg) {
  Entry* ent = reinterpret_cast<Entry*>(((void**)arg)[0]);
  CLevel::MemoryManagement* mem = reinterpret_cast<CLevel::MemoryManagement*>(((void**)arg)[1]);
  size_t* size = reinterpret_cast<size_t*>(((void**)arg)[2]);

  *size = *size + 1;
  ent->SetValue(mem, value);
}

void BLevel::Expansion(std::shared_ptr<BLevel> old_blevel, std::atomic<uint64_t>& min_key,
                        std::atomic<uint64_t>& max_key) {
  Timer total_timer;
  Timer scan_timer;
  double scan_total = 0.0;

  total_timer.Start();

  uint64_t old_index = 0;
  expanding_entry_index_.store(0, std::memory_order_release);
  size_t size = 0;
  // handle entry 0 explicit
  {
    std::lock_guard<std::shared_mutex> old_lock(old_blevel->locks_[0]);
    std::lock_guard<std::shared_mutex> lock(locks_[0]);
    min_key.store(0);
    max_key.store(old_blevel->GetKey(1));
    Entry* old_ent = old_blevel->GetEntry_(0);
    Entry* new_ent = GetEntry_(0);
    new_ent->SetKey(clevel_mem_, 0);
    in_mem_key_[0] = 0;
    volatile uint64_t old_data = old_ent->value;
    size_t scan_size = 0;
    CLevel* clevel;
    void* callback_arg[3];
    switch (Entry::GetType(old_data)) {
      case Entry::Type::ENTRY_INVALID:
        assert(0);
      case Entry::Type::ENTRY_VALUE:
        new_ent->SetValue(clevel_mem_, Entry::GetValue(old_data));
        size++;
        old_ent->SetInvalid(old_blevel->clevel_mem_);
        old_index++;
        break;
      case Entry::Type::ENTRY_CLEVEL:
        clevel = Entry::GetClevel(old_blevel->clevel_mem_, old_data);
        callback_arg[0] = new_ent;
        callback_arg[1] = clevel_mem_;
        callback_arg[2] = &size;
        clevel->Scan(old_blevel->clevel_mem_, 0, 0, 1, scan_size, BLevel::ExpansionCallback_, callback_arg);
        if (scan_size == 1)
          clevel->Delete(old_blevel->clevel_mem_, 0);
        else
          new_ent->SetNone(clevel_mem_);
        break;
      case Entry::Type::ENTRY_NONE:
        new_ent->SetNone(clevel_mem_);
        old_ent->SetInvalid(old_blevel->clevel_mem_);
        old_index++;
        break;
      default:
        assert(0);
    }
    expanding_entry_index_.fetch_add(1, std::memory_order_release);
  }

  // handle entry [1, EntrySize()]
  while (old_index < old_blevel->EntrySize()) {
    std::lock_guard<std::shared_mutex> old_ent_lock(old_blevel->locks_[old_index]);
    min_key.store(old_blevel->GetKey(old_index));
    max_key.store(old_index + 1 == old_blevel->EntrySize() ? UINT64_MAX
                                  : old_blevel->GetKey(old_index + 1));
    Entry* old_ent = old_blevel->GetEntry_(old_index);
    volatile uint64_t old_data = old_ent->value;
    CLevel* clevel;

    uint64_t last_seen = 0;
    CLevel::LeafNode* node;
    CLevel::LeafNode::Entry* ent;
    int i;
    switch (Entry::GetType(old_data)) {
      case Entry::Type::ENTRY_INVALID:
        assert(0);
      case Entry::Type::ENTRY_VALUE:
        ExpandAddEntry_(old_ent->key, Entry::GetValue(old_data), size);
        break;
      case Entry::Type::ENTRY_CLEVEL:
        clevel = Entry::GetClevel(old_blevel->clevel_mem_, old_data);

        scan_timer.Start();

        node = clevel->head_;
        while (node) {
          if (node->GetSortedKey_(0) > last_seen) {
            for (i = 0; i < node->nr_entry; ++i) {
              ent = &node->entry[node->GetSortedEntry_(i)];
              ExpandAddEntry_(ent->key, ent->value, size);
            }
          } else {
            // key[0] <= last_seen, special situation
#ifndef NDEBUG
            if (node->nr_entry != 0)
              LOG(Debug::WARNING, "scan special situation");
#endif // NDEBUG
            for (i = 0; i < node->nr_entry; ++i) {
              ent = &node->entry[node->GetSortedEntry_(i)];
              if (ent->key <= last_seen)
                continue;
              ExpandAddEntry_(ent->key, ent->value, size);
            }
          }
          node = node->GetNext(old_blevel->clevel_mem_->BaseAddr());
        }

        scan_total += scan_timer.End();
        break;
      case Entry::Type::ENTRY_NONE:
        break;
      default:
        assert(0);
    }
    old_ent->SetInvalid(old_blevel->clevel_mem_);
    old_index++;
  }

  if (size > EntrySize())
    root_->size.fetch_add(size - EntrySize());
  else
    root_->size.fetch_sub(EntrySize() - size);
  root_->nr_entry.store(expanding_entry_index_.load(std::memory_order_acquire));
  root_.persist();
  is_expanding_.store(false);

  LOG(Debug::INFO, "Expansion finished, total time: %lf, scan time: %lf", total_timer.End(), scan_total);
}

BLevel::BLevel(pmem::obj::pool_base pop)
    : pop_(pop), in_mem_entry_(nullptr), in_mem_key_(nullptr),
      is_expanding_(false)
{
  pmem::obj::pool<Root> pool(pop_);
  root_ = pool.root();
  clevel_slab_ = new Slab<CLevel>(pop_, EntrySize() / 4.0);
  Slab<CLevel::LeafNode>* leaf_slab = new Slab<CLevel::LeafNode>(pop_, EntrySize() / 4.0);
  clevel_mem_ = new CLevel::MemoryManagement(pop, leaf_slab);
  locks_ = new std::shared_mutex[EntrySize() + 1];
}

BLevel::~BLevel() {
  if (locks_)
    delete locks_;
  if (in_mem_key_)
    delete in_mem_key_;
  if (clevel_slab_)
    delete clevel_slab_;
}

Status BLevel::Entry::Get(std::shared_mutex* mutex, CLevel::MemoryManagement* mem,
                          uint64_t pkey, uint64_t& pvalue) const {
  // std::shared_lock<std::shared_mutex> lock(*mutex);
  volatile uint64_t data = value;
  switch (GetType(data)) {
    case Type::ENTRY_INVALID:
      return Status::INVALID;
    case Type::ENTRY_VALUE:
      if (pkey == key) {
        pvalue = GetValue(data);
        return Status::OK;
      } else {
        return Status::DOES_NOT_EXIST;
      }
    case Type::ENTRY_CLEVEL:
      return GetClevel(mem, data)->Get(mem, pkey, pvalue);
    case Type::ENTRY_NONE:
      return Status::DOES_NOT_EXIST;
    default:
      assert(0);
  }
  return Status::INVALID;
}

Status BLevel::Entry::Insert(std::shared_mutex* mutex, CLevel::MemoryManagement* mem,
                             Slab<CLevel>* clevel_slab, uint64_t pkey, uint64_t pvalue) {
  // TODO: lock scope too large. https://stackoverflow.com/a/34995051/7640227
  // std::lock_guard<std::shared_mutex> lock(*mutex);
  volatile uint64_t data = value;
  [[maybe_unused]] Status debug_status;
  CLevel* new_clevel;
  switch (GetType(data)) {
    case Type::ENTRY_INVALID:
      return Status::INVALID;
    case Type::ENTRY_VALUE:
      if (pkey == key)
        return Status::ALREADY_EXISTS;

      new_clevel = clevel_slab->Allocate();
      new_clevel->InitLeaf(mem);

      debug_status = new_clevel->Insert(mem, key, GetValue(data));
      assert(debug_status == Status::OK);
      debug_status = new_clevel->Insert(mem, pkey, pvalue);
      assert(debug_status == Status::OK);

      SetCLevel(mem, new_clevel);
      return Status::OK;
    case Type::ENTRY_CLEVEL:
      return GetClevel(mem, data)->Insert(mem, pkey, pvalue);
    case Type::ENTRY_NONE:
      if (pkey == key) {
        SetValue(mem, pvalue);
        return Status::OK;
      } else {
        new_clevel = clevel_slab->Allocate();
        new_clevel->InitLeaf(mem);

        debug_status = new_clevel->Insert(mem, pkey, pvalue);
        assert(debug_status == Status::OK);

        SetCLevel(mem, new_clevel);
        return Status::OK;
      }
    default:
      assert(0);
  }
  return Status::INVALID;
}

Status BLevel::Entry::Update(std::shared_mutex* mutex, CLevel::MemoryManagement* mem,
                             uint64_t pkey, uint64_t pvalue) {
  assert(0);
  return Status::OK;
}

Status BLevel::Entry::Delete(std::shared_mutex* mutex, CLevel::MemoryManagement* mem,
                             uint64_t pkey) {
  // std::lock_guard<std::shared_mutex> lock(*mutex);
  volatile uint64_t data = value;
  switch (GetType(data)) {
    case Type::ENTRY_INVALID:
      return Status::INVALID;
    case Type::ENTRY_VALUE:
      if (pkey == key) {
        SetNone(mem);
        return Status::OK;
      } else {
        return Status::DOES_NOT_EXIST;
      }
    case Type::ENTRY_CLEVEL:
      return GetClevel(mem, data)->Delete(mem, pkey);
    case Type::ENTRY_NONE:
      return Status::DOES_NOT_EXIST;
    default:
      assert(0);
  }
  return Status::INVALID;
}

Status BLevel::Scan(uint64_t min_key, uint64_t max_key,
                    size_t max_size, size_t& size,
                    callback_t callback, void* arg) {
  if (size >= max_size)
    return Status::OK;

  int end;
  if (is_expanding_.load())
    end = expanding_entry_index_.load(std::memory_order_acquire) - 1;
  else
    end = EntrySize() - 1;
  uint64_t entry_index = Find_(min_key, 0, end);
  {
    // std::shared_lock<std::shared_mutex> lock(locks_[entry_index]);
    Entry* ent = GetEntry_(entry_index);
    volatile uint64_t data = ent->value;
    CLevel* clevel;
    bool finish;
    switch (Entry::GetType(data)) {
      case Entry::Type::ENTRY_INVALID:
        return Status::INVALID;
      case Entry::Type::ENTRY_VALUE:
        if (ent->key >= min_key) {
          callback(ent->key, Entry::GetValue(data), arg);
          size++;
        }
        break;
      case Entry::Type::ENTRY_CLEVEL:
        clevel = Entry::GetClevel(clevel_mem_, data);
        finish = clevel->Scan(clevel_mem_, min_key, max_key, max_size, size, callback, arg);
        if (finish)
          return Status::OK;
        break;
      case Entry::Type::ENTRY_NONE:
        break;
      default:
        assert(0);
    }
  }

  entry_index++;
  while (entry_index < EntrySize()) {
    // std::shared_lock<std::shared_mutex> lock(locks_[entry_index]);
    Entry* ent = GetEntry_(entry_index);
    volatile uint64_t data = ent->value;
    bool finish;
    CLevel* clevel;
    switch (Entry::GetType(data)) {
      case Entry::Type::ENTRY_INVALID:
        return Status::INVALID;
      case Entry::Type::ENTRY_VALUE:
        if (size >= max_size || ent->key > max_key)
          return Status::OK;
        callback(ent->key, Entry::GetValue(data), arg);
        size++;
        break;
      case Entry::Type::ENTRY_CLEVEL:
        clevel = Entry::GetClevel(clevel_mem_, data);
        finish = clevel->Scan(clevel_mem_, max_key, max_size, size, callback, arg);
        if (finish)
          return Status::OK;
        break;
      case Entry::Type::ENTRY_NONE:
        break;
      default:
        assert(0);
    }
    entry_index++;
  }
  return Status::OK;
}

uint64_t BLevel::Find_(uint64_t key, uint64_t begin, uint64_t end) const {
  assert(begin < EntrySize());
  assert(end < EntrySize());
  int_fast32_t left = begin;
  int_fast32_t right = end;
  // binary search
  while (left <= right) {
    int middle = (left + right) / 2;
    uint64_t mid_key = GetKey(middle);
    if (mid_key == key) {
      return middle;
    } else if (mid_key < key) {
      left = middle + 1;
    } else {
      right = middle - 1;
    }
  }
  return right;
}

uint64_t BLevel::MinEntryKey() const {
  assert(EntrySize() > 1);
  return GetKey(1);
}

uint64_t BLevel::MaxEntryKey() const {
  return GetKey(EntrySize() - 1);
}

} // namespace combotree