#include <iostream>
#include <thread>
#include <atomic>
#include <cstdint>
#include "spsc.h"

template <template <typename> typename spsc>
void test() {
  spsc<int64_t> q(100);
  int pushPopCnt = 1e6;

  std::atomic<bool> start = false;

  auto consumer = std::thread([&start, &q, pushPopCnt]() {
    while (!start) {
    }
    int i = 0;
    auto counter = pushPopCnt;
    while (counter--) {
      int64_t t;
      while (!q.pop(t))
        ;
      if (t != i) {
        std::cerr << "Assert failed\n";
        std::cerr << i << " " << t << "\n";
        exit(1);
      }
      i++;
    }
  });

  auto producer = std::thread([&start, &q, pushPopCnt]() {
    while (!start) {
    }
    int i = 0;
    auto counter = pushPopCnt;
    while (counter--) {
      while (!q.push(i))
        ;
      i++;
    }
  });

  start = true;
  consumer.join();
  producer.join();
}

int main() {
  std::cout << "Running SPSC Queue Tests\n";
  std::cout << "========================\n\n";

  std::cout << "Testing MutexSpsc... ";
  test<my::MutexSpsc>();
  std::cout << "PASSED\n";

  std::cout << "Testing LockfreeSizeAtomicSeqCstSpsc... ";
  test<my::LockfreeSizeAtomicSeqCstSpsc>();
  std::cout << "PASSED\n";

  std::cout << "Testing LockfreeSizeAtomicAcqRelSpsc... ";
  test<my::LockfreeSizeAtomicAcqRelSpsc>();
  std::cout << "PASSED\n";

  std::cout << "Testing LockfreeSizeAtomicAcqRelSpsc_AlignmentOpt... ";
  test<my::LockfreeSizeAtomicAcqRelSpsc_AlignmentOpt>();
  std::cout << "PASSED\n";

  std::cout << "Testing LockfreeAtomicPushPopPtrSpsc... ";
  test<my::LockfreeAtomicPushPopPtrSpsc>();
  std::cout << "PASSED\n";

  std::cout << "Testing LockfreeAtomicPushPopPtrNoDivSpsc... ";
  test<my::LockfreeAtomicPushPopPtrNoDivSpsc>();
  std::cout << "PASSED\n";

  std::cout << "Testing LockfreeAtomicPushPopSeparateCacheLineSpsc... ";
  test<my::LockfreeAtomicPushPopSeparateCacheLineSpsc>();
  std::cout << "PASSED\n";

  std::cout << "Testing LockfreeAtomicPushPopSeparateCacheLineOptSpsc... ";
  test<my::LockfreeAtomicPushPopSeparateCacheLineOptSpsc>();
  std::cout << "PASSED\n";

  std::cout << "Testing LockfreeAtomicPushPopPtrCachingSpsc... ";
  test<my::LockfreeAtomicPushPopPtrCachingSpsc>();
  std::cout << "PASSED\n";

  std::cout << "\n========================\n";
  std::cout << "All tests passed!\n";
  return 0;
}



