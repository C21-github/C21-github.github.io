#include <sys/resource.h>
#include <sys/syscall.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

namespace my {

/** Base class for non-copyable, non-movable types **/

// kept it non template for simplicity, in real usage this should be
// templatized, tag to force different types and prevent EBO from being disabled
// template<typename T>
class NonCopyableNonMovable {
 public:
  NonCopyableNonMovable() = default;
  ~NonCopyableNonMovable() = default;

  NonCopyableNonMovable(const NonCopyableNonMovable&) = delete;
  NonCopyableNonMovable& operator=(const NonCopyableNonMovable&) = delete;
  NonCopyableNonMovable(NonCopyableNonMovable&&) = delete;
  NonCopyableNonMovable& operator=(NonCopyableNonMovable&&) = delete;
};

/*******************  Mutex based spsc **********************/

template <typename T>
class MutexSpsc : NonCopyableNonMovable {
  static_assert(std::is_trivial_v<T>, "spsc T must be trivial");
  size_t cap_;
  size_t pushInd_ = 0, popInd_ = 0;
  std::unique_ptr<T[]> buf_;
  std::mutex mut_;

 public:
   MutexSpsc(size_t sz) {
     cap_ = sz;
     buf_ = std::make_unique<T[]>(cap_);
   }

   bool push(const T& t) {
     std::lock_guard<std::mutex> lck(mut_);
     if (pushInd_ - popInd_ == cap_) return false;
     buf_[pushInd_++ % cap_] = t;
     return true;
   }

   bool pop(T& t) {
     std::lock_guard<std::mutex> lck(mut_);
     if (pushInd_ - popInd_ == 0) return false;
     t = buf_[popInd_++ % cap_];
     return true;
   }
};

/******************* atomic size Seq-cst Lock-free spsc **********************/

template <typename T>
class LockfreeSizeAtomicSeqCstSpsc : NonCopyableNonMovable {
  static_assert(std::is_trivial_v<T>, "spsc T must be trivial");
  size_t cap_;
  size_t pushInd_ = 0, popInd_ = 0;
  std::unique_ptr<T[]> buf_;
  std::atomic<uint64_t> size_ = 0;

 public:
   LockfreeSizeAtomicSeqCstSpsc(size_t sz) {
     cap_ = sz;
     buf_ = std::make_unique<T[]>(cap_);
   }

   bool push(const T& t) {
     if (size_ == cap_) return false;
     buf_[pushInd_++ % cap_] = t;
     size_++;
     return true;
   }

   bool pop(T& t) {
     if (size_ == 0) return false;
     t = buf_[popInd_++ % cap_];
     size_--;
     return true;
   }
};

/***************** atomic size Acq-Release Lock-free spsc ********************/

template <typename T>
class Members_LockfreeSizeAtomicAcqRelSpsc {
 protected:
   size_t cap_;
   size_t pushInd_ = 0, popInd_ = 0;
   std::unique_ptr<T[]> buf_;
   std::atomic<uint64_t> size_ = 0;
};

template <typename T>
class AlignmentOptMembers_LockfreeSizeAtomicAcqRelSpsc {
 protected:
   size_t cap_;
   std::unique_ptr<T[]> buf_;
   alignas(64) size_t pushInd_ = 0;
   alignas(64) size_t popInd_ = 0;
   alignas(64) std::atomic<uint64_t> size_ = 0;
};

template <typename T, typename Members>
class _LockfreeSizeAtomicAcqRelSpsc : Members, NonCopyableNonMovable {
  static_assert(std::is_trivial_v<T>, "spsc T must be trivial");

 public:
   _LockfreeSizeAtomicAcqRelSpsc(size_t sz) {
     this->cap_ = sz;
     this->buf_ = std::make_unique<T[]>(this->cap_);
   }

   bool push(const T& t) {
     if (this->size_.load(std::memory_order_acquire) == this->cap_)
       return false;
     this->buf_[this->pushInd_++ % this->cap_] = t;
     this->size_.fetch_add(1, std::memory_order_release);
     return true;
   }

   bool pop(T& t) {
     if (this->size_.load(std::memory_order_acquire) == 0) return false;
     t = this->buf_[this->popInd_++ % this->cap_];
     this->size_.fetch_sub(1, std::memory_order_release);
     return true;
   }
};

template <typename T>
using LockfreeSizeAtomicAcqRelSpsc =
    _LockfreeSizeAtomicAcqRelSpsc<T, Members_LockfreeSizeAtomicAcqRelSpsc<T>>;

template <typename T>
using LockfreeSizeAtomicAcqRelSpsc_AlignmentOpt = _LockfreeSizeAtomicAcqRelSpsc<
    T, AlignmentOptMembers_LockfreeSizeAtomicAcqRelSpsc<T>>;

/*********** atomic push-pop Lock-free spsc ***********/
/*
 * The fetch_add/sub uses lock instr in x86 and is slow. But we need to do
 * atomic increment as long as long we use a single size atomic. If we instead
 * maintain separate index for push pop then we can avoid the fetch_add and our
 * code should be drastically faster
 */
template <typename T>
class LockfreeAtomicPushPopPtrSpsc : NonCopyableNonMovable {
  static_assert(std::is_trivial_v<T>, "spsc T must be trivial");
  size_t cap_;
  std::unique_ptr<T[]> buf_;
  std::atomic<uint64_t> pushInd_ = 0;
  std::atomic<uint64_t> popInd_ = 0;

 public:
   LockfreeAtomicPushPopPtrSpsc(size_t sz) {
     cap_ = sz;
     buf_ = std::make_unique<T[]>(cap_);
   }

   bool push(const T& t) {
     auto curPush = pushInd_.load(std::memory_order_relaxed);
     if (curPush - popInd_.load(std::memory_order_acquire) == cap_)
       return false;
     buf_[curPush % cap_] = t;
     pushInd_.store(curPush + 1, std::memory_order_release);
     return true;
   }

   bool pop(T& t) {
     auto curPop = popInd_.load(std::memory_order_relaxed);
     if (curPop == pushInd_.load(std::memory_order_acquire)) return false;
     t = buf_[curPop % cap_];
     popInd_.store(curPop + 1, std::memory_order_release);
     return true;
   }
};

/*********** atomic push-pop mask based modulo Lock-free spsc ***********/

template <typename T>
class LockfreeAtomicPushPopPtrNoDivSpsc : NonCopyableNonMovable {
  static_assert(std::is_trivial_v<T>, "spsc T must be trivial");
  size_t msk_;
  std::unique_ptr<T[]> buf_;
  std::atomic<uint64_t> pushInd_ = 0;
  std::atomic<uint64_t> popInd_ = 0;

 public:
   LockfreeAtomicPushPopPtrNoDivSpsc(size_t sz) {
     size_t pow2Sz = std::bit_ceil(sz);
     msk_ = pow2Sz - 1;
     buf_ = std::make_unique<T[]>(pow2Sz);
   }

   bool push(const T& t) {
     auto curPush = pushInd_.load(std::memory_order_relaxed);
     if (curPush - popInd_.load(std::memory_order_acquire) == msk_ + 1)
       return false;
     buf_[curPush & msk_] = t;
     pushInd_.store(curPush + 1, std::memory_order_release);
     return true;
   }

   bool pop(T& t) {
     auto curPop = popInd_.load(std::memory_order_relaxed);
     if (curPop == pushInd_.load(std::memory_order_acquire)) return false;
     t = buf_[curPop & msk_];
     popInd_.store(curPop + 1, std::memory_order_release);
     return true;
   }
};

/**** atomic push-pop mask separate cache-line Lock-free spsc ***/
template <typename T>
class LockfreeAtomicPushPopSeparateCacheLineSpsc : NonCopyableNonMovable {
  static_assert(std::is_trivial_v<T>, "spsc T must be trivial");
  alignas(64) size_t msk_;
  std::unique_ptr<T[]> buf_;
  alignas(64) std::atomic<uint64_t> pushInd_ = 0;
  alignas(64) std::atomic<uint64_t> popInd_ = 0;

 public:
   LockfreeAtomicPushPopSeparateCacheLineSpsc(size_t sz) {
     size_t pow2Sz = std::bit_ceil(sz);
     msk_ = pow2Sz - 1;
     buf_ = std::make_unique<T[]>(pow2Sz);
   }

   bool push(const T& t) {
     auto curPush = pushInd_.load(std::memory_order_relaxed);
     if (curPush - popInd_.load(std::memory_order_acquire) == msk_ + 1)
       return false;
     buf_[curPush & msk_] = t;
     pushInd_.store(curPush + 1, std::memory_order_release);
     return true;
   }

   bool pop(T& t) {
     auto curPop = popInd_.load(std::memory_order_relaxed);
     if (curPop == pushInd_.load(std::memory_order_acquire)) return false;
     t = buf_[curPop & msk_];
     popInd_.store(curPop + 1, std::memory_order_release);
     return true;
   }
};

/****** atomic push-pop separate cache-line opt with 2 cache-lines Lock-free
 * spsc ********/

template <typename T>
class LockfreeAtomicPushPopSeparateCacheLineOptSpsc : NonCopyableNonMovable {
  static_assert(std::is_trivial_v<T>, "spsc T must be trivial");
  alignas(64) std::atomic<uint64_t> pushInd_ = 0;
  size_t mskPush_;
  std::unique_ptr<T[]> buf_;
  alignas(64) std::atomic<uint64_t> popInd_ = 0;
  size_t mskPop_;
  T* bufPop_;

 public:
   LockfreeAtomicPushPopSeparateCacheLineOptSpsc(size_t sz) {
     size_t pow2Sz = std::bit_ceil(sz);
     mskPush_ = mskPop_ = pow2Sz - 1;
     buf_ = std::make_unique<T[]>(pow2Sz);
     bufPop_ = buf_.get();
   }

   bool push(const T& t) {
     auto curPush = pushInd_.load(std::memory_order_relaxed);
     if (curPush - popInd_.load(std::memory_order_acquire) == mskPush_ + 1)
       return false;
     buf_[curPush & mskPush_] = t;
     pushInd_.store(curPush + 1, std::memory_order_release);
     return true;
   }

   bool pop(T& t) {
     auto curPop = popInd_.load(std::memory_order_relaxed);
     if (curPop == pushInd_.load(std::memory_order_acquire)) return false;
     t = bufPop_[curPop & mskPop_];
     popInd_.store(curPop + 1, std::memory_order_release);
     return true;
   }
};

/****** atomic push-pop separate 2 cache-line opt with "caching" of ptrs
 * Lock-free spsc ********/

template <typename T>
class LockfreeAtomicPushPopPtrCachingSpsc : NonCopyableNonMovable {
  static_assert(std::is_trivial_v<T>, "spsc T must be trivial");
  alignas(64) std::atomic<uint64_t> pushInd_ = 0;
  size_t mskPush_;
  std::unique_ptr<T[]> buf_;
  uint64_t popCache = 0;
  alignas(64) std::atomic<uint64_t> popInd_ = 0;
  size_t mskPop_;
  T* bufPop_;
  uint64_t pushCache = 0;

 public:
   LockfreeAtomicPushPopPtrCachingSpsc(size_t sz) {
     size_t pow2Sz = std::bit_ceil(sz);
     mskPush_ = mskPop_ = pow2Sz - 1;
     buf_ = std::make_unique<T[]>(pow2Sz);
     bufPop_ = buf_.get();
   }

   bool push(const T& t) {
     auto curPush = pushInd_.load(std::memory_order_relaxed);
     if (curPush - popCache == mskPush_ + 1) {
       popCache = popInd_.load(std::memory_order_acquire);
       if (curPush - popCache == mskPush_ + 1) return false;
     }

     buf_[curPush & mskPush_] = t;
     pushInd_.store(curPush + 1, std::memory_order_release);
     return true;
   }

   bool pop(T& t) {
     auto curPop = popInd_.load(std::memory_order_relaxed);

     if (curPop == pushCache) {
       pushCache = pushInd_.load(std::memory_order_acquire);
       if (curPop == pushCache) return false;
     }

     t = bufPop_[curPop & mskPop_];
     popInd_.store(curPop + 1, std::memory_order_release);
     return true;
   }
};
}  // namespace my
