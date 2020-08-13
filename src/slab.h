#pragma once

#include <vector>
#include <atomic>
#include <thread>
#include <shared_mutex>
#include <libpmemobj++/persistent_ptr.hpp>
#include <libpmemobj++/make_persistent_array_atomic.hpp>

namespace combotree {

template<typename T>
class Slab {
 public:
  Slab(pmem::obj::pool_base pop, int array_size)
      : pop_(pop), array_size_(array_size), next_(0) {
    assert(array_size > 5);
    pmem::obj::persistent_ptr<T[]> data;
    pmem::obj::make_persistent_atomic<T[]>(pop_, data, array_size_);
    data_.push_back(data);
  }

  ~Slab() {
    for (auto data : data_) {
      pmem::obj::delete_persistent_atomic<T[]>(data, array_size_);
    }
  }

  T* Allocate() {
    while (true) {
      int next;
      {
        std::shared_lock<std::shared_mutex> lock(lock_);
        next = next_.fetch_add(1);
        if (next < array_size_) {
          CLevel* buf = data_.back().get();
          return &buf[next];
        }
      }
      {
        next++;
        std::lock_guard<std::shared_mutex> lock(lock_);
        if (next_.compare_exchange_strong(next, 1)) {
          pmem::obj::persistent_ptr<T[]> data;
          pmem::obj::make_persistent_atomic<T[]>(pop_, data, array_size_);
          data_.push_back(data);
          CLevel* buf = data_.back().get();
          return &buf[0];
        }
      }
      std::this_thread::sleep_for(std::chrono::microseconds(2));
    }
  }

 private:
  pmem::obj::pool_base pop_;
  std::vector<pmem::obj::persistent_ptr<T[]>> data_;
  const int array_size_;
  std::atomic<int> next_;
  std::shared_mutex lock_;
};

} // namespace combotree