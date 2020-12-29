#undef NDEBUG

#include <iostream>
#include <cassert>
#include <iomanip>
#include <map>
#include "combotree/combotree.h"
#include "combotree_config.h"
#include "random.h"

#define TEST_SIZE   4000000

using combotree::ComboTree;
using combotree::Random;

int main(void) {
#ifdef SERVER
  ComboTree* tree = new ComboTree("/pmem0/combotree/", (1024*1024*1024*100UL), true);
#else
  ComboTree* tree = new ComboTree("/mnt/pmem0/", (1024*1024*512UL), true);
#endif

#ifdef NDEBUG
static_assert(0, "NDEBUG!");
#endif // NDEBUG

  std::cout << "TEST_SIZE:             " << TEST_SIZE << std::endl;

  std::vector<uint64_t> key;
  std::map<uint64_t, uint64_t> right_kv;

  Random rnd(0, UINT64_MAX - 1);
  for (uint64_t i = 0; i < TEST_SIZE; ++i) {
    uint64_t key = rnd.Next();
    if (right_kv.count(key)) {
      i--;
      continue;
    }
    uint64_t value = rnd.Next();
    right_kv.emplace(key, value);
    tree->Put(key, value);
  }

  uint64_t value;

  // Get
  for (auto &kv : right_kv) {
    assert(tree->Get(kv.first, value) == true);
    assert(value == kv.second);
  }

  for (uint64_t i = 0; i < TEST_SIZE * 2; ++i) {
    if (right_kv.count(i) == 0) {
      assert(tree->Get(i, value) == false);
    }
  }

  // scan
  auto right_iter = right_kv.begin();
  ComboTree::Iter iter(tree);
  while (right_iter != right_kv.end()) {
    assert(right_iter->first == iter.key());
    assert(right_iter->second == iter.value());
    right_iter++;
    iter.next();
  }
  assert(iter.end());

  for (int i = 0; i < 1000; ++i) {
    uint64_t start_key = rnd.Next();
    auto right_iter = right_kv.lower_bound(start_key);
    ComboTree::Iter iter(tree, start_key);
    // ComboTree::Iter riter(tree);
    // while (riter.key() < start_key)
    //   riter.next();
    // assert(right_iter->first == riter.key());
    assert(right_iter->first == iter.key());
    for (int j = 0; j < 100 && right_iter != right_kv.cend(); ++j) {
      assert(!iter.end());
      // assert(!riter.end());
      // uint64_t rkey = riter.key();
      // uint64_t rvalue = riter.value();
      // uint64_t k = iter.key();
      // uint64_t v = iter.value();
      // assert(right_iter->first == riter.key());
      // assert(right_iter->second == riter.value());
      assert(right_iter->first == iter.key());
      assert(right_iter->second == iter.value());
      right_iter++;
      iter.next();
      // riter.next();
    }
  }

  // NoSort Scan
  {
    ComboTree::NoSortIter no_sort_iter(tree, 100);
    for (int i = 0; i < 100; ++i) {
      std::cout << no_sort_iter.key() << " " << no_sort_iter.value() << std::endl;
      assert(no_sort_iter.next());
    }
  }

  {
    // Sort Scan
    ComboTree::Iter sort_iter(tree, 100);
    for (int i = 0; i < 100; ++i) {
      std::cout << sort_iter.key() << " " << sort_iter.value() << std::endl;
      assert(sort_iter.next());
    }
  }

  return 0;
}