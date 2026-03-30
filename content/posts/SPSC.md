+++
slug = 'spsc'
date = '2026-03-30T00:00:01+05:30'
draft = false
title = 'Optimizing a Lock-Free SPSC Queue'
+++

Yet another SPSC blog! The goal isn't just to explain a final solution, such as the **lock-free, index-cached implementation** some of you might already know. Instead, I want to walk through the iterative process: testing different approaches, analyzing the results, and determining how to squeeze out performance.

By walking through the analysis, I hope this process provides a template for tackling similar performance challenges in the future.


The optimization discussions are focused on the **x86 architecture**. All measurements were taken on an **AMD Ryzen 7 6800HS** using **gcc 13.3.0**. This CPU has a single **CCX (Core Complex)**, and the benchmarks use cores 0 and 2 - two distinct physical cores, rather than hyperthreaded siblings.

## Mutex-Based
Let's start with a simple mutex implementation. It uses a ring buffer of fixed size; if the buffer is full, the producer's push operation fails (returns false) until the consumer pops from the buffer.

```cpp {hl_Lines="26-31 33-38"}
class NonCopyableNonMovable {
 public:
  NonCopyableNonMovable() = default;
  ~NonCopyableNonMovable() = default;

  NonCopyableNonMovable(const NonCopyableNonMovable&) = delete;
  NonCopyableNonMovable& operator=(const NonCopyableNonMovable&) = delete;
  NonCopyableNonMovable(NonCopyableNonMovable&&) = delete;
  NonCopyableNonMovable& operator=(NonCopyableNonMovable&&) = delete;
};

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
```

```
Producer core: 0, Consumer core: 2
Queue size: 100,000
Iterations: 1,000,000 per benchmark
10 runs per benchmark for throughput statistics calculation 

Queue Name   Thru (ops/s)        std-Dev
----------------------------------------
 MutexSpsc     19,296,994      1,591,781
```

All the following benchmarks results mentioned correspond to the Queue Size `100,000` variant unless otherwise specified. The benchmark code includes other queue size variants. Obviously these numbers will differ based on the size of type `T` used. For all the benchmarks mentioned in this post, we are using `T = int64_t`.


{{< fold "But wait we are just incrementing `pushInd_` and never doing modulo, what if it overflows ?">}}
Why not do `pushInd_ = (pushInd_ + 1) % cap_` instead to avoid overflow concerns?

The issue with modulo approach is, it creates ambiguity: when `pushInd_` equals `popInd_`, we can't distinguish between a full queue and an empty queue. One simple solution is to avoid this case altogether by blocking push when size reaches `cap_-1` instead.

However, overflow isn't really a concern here. The indices are `uint64_t` with a maximum value of `2^64 - 1`. If we keep the capacity a power of 2, the overflow behavior is [well defined](https://www.gnu.org/software/c-intro-and-ref/manual/html_node/Unsigned-Overflow.html) and the logic works correctly despite overflow, because 
$$ \forall k \in \mathbb{Z} \text{ such that } 0 \le k \le 64, a \bmod 2^k = (a \bmod 2^{64}) \bmod 2^k $$ 
Even if we don't restrict the capacity to a power of 2, assuming we achieve a throughput of 1 billion push/pop operations per second, it would take approximately 585 years of continuous operation to overflow. Therefore, we can safely ignore the overflow possibility.
{{< /fold >}}

{{< fold "Code used for benchmarking" >}}
```cpp
template <template <typename> typename spsc, bool UsePause>
double benchmarkThroughput(int consumerCore, int producerCore, size_t queueSize, int64_t iterations) {
  std::align_val_t alignment{std::max<size_t>(alignof(spsc<int64_t>), 64)};
	std::unique_ptr<spsc<int64_t>> qUniquePtr(new (alignment) spsc<int64_t>(queueSize));
	auto& q = *qUniquePtr;

  std::atomic<bool> start = false;

  // Consumer thread
  auto consumer = std::thread([&]() {
    pin(consumerCore);
    while (!start);  // Wait for start signal

    for (int64_t i = 0; i < iterations; ++i) {
      int64_t val;
      while (!q.pop(val)) {
        if (UsePause) _mm_pause();
      }
      volatile int64_t v = val;  // Prevent optimization
    }
  });

  // Producer thread
  auto producer = std::thread([&]() {
    pin(producerCore);
    while (!start);  // Wait for start signal

    for (int64_t i = 0; i < iterations; ++i) {
      while (!q.push(i)) {
        if (UsePause) _mm_pause();
      }
    }
  });

  // Start timing
  auto startTime = std::chrono::steady_clock::now();
  start = true;

  // Wait for completion
  consumer.join();
  producer.join();

  // Stop timing
  auto stopTime = std::chrono::steady_clock::now();

  // Calculate ops/second
  auto elapsed_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(stopTime - startTime)
          .count();
  return (iterations * 1000000000.0) / elapsed_ns;  // ops per second
}

template <template <typename> typename spsc, bool UsePause>
std::vector<uint64_t> benchmarkLatency(int consumerCore, int producerCore, size_t queueSize, int64_t iterations) {
  std::align_val_t alignment{std::max<size_t>(alignof(spsc<int64_t>), 64)};
	std::unique_ptr<spsc<int64_t>> q1UniquePtr(new (alignment) spsc<int64_t>(queueSize));
	auto& q1 = *q1UniquePtr;

  std::atomic<bool> producerTurn = false;
	std::vector<uint64_t> latencies(iterations, 0);
	
	// Write one entry then wait for consumer to consume. repeat this iterations time.
  auto consumer = std::thread([&]() {
    pin(consumerCore);
    for (int64_t i = 0; i < iterations; ++i) {
      int64_t val;
      while (!q1.pop(val)) {
        if (UsePause) _mm_pause();
      }
			latencies[i] = curTime() - val;
			producerTurn = true;
     }
  });

  auto producer = std::thread([&]() {
    pin(producerCore);
    for (int64_t i = 0; i < iterations; ++i) {
			while(!producerTurn){}
			producerTurn = false;
       while (!q1.push(curTime())) {
       }
     }
  });

  producerTurn = true;

  consumer.join();
  producer.join();

	return latencies;
}
```
{{< /fold >}}

## Making lock free
> If there’s no enforced ordering between two accesses to a single memory location
from separate threads, one or both of those accesses is not atomic, and one or both is
a write, then this is a data race and causes undefined behavior.
{cite="C++ Concurrency In Action"}

How do we make it lock free? In the previous mutex-based code, both producer and consumer threads access both `pushInd_` and `popInd_`. To make this lock-free, we could make both indices atomic.

But do we really need to read both variables from each thread? Notice that what we actually need is the difference `pushInd_ - popInd_` (the queue size) to check if the queue is full or empty. This suggests an alternative approach: instead of making both indices atomic, we could maintain an atomic size variable that both threads read and update.

* Option 1: Make push and pop indices atomic
* Option 2: Add size and only make it atomic

Let's proceed with option 2 first. We'll change the code to have an atomic size variable. Note that for `std::atomic`, the `operator++` and `operator--` are shorthand for `fetch_add(1, std::memory_order_seq_cst)` and `fetch_sub(1, std::memory_order_seq_cst)`, respectively. So the below code defaults to using sequential consistency ordering.

```cpp {hl_Lines="15-20 22-26"}
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
```

```console {hl_Lines=4}
Queue Name                       Thru (ops/s)        std-Dev
------------------------------------------------------------
MutexSpsc                          19,296,994      1,591,781
LockfreeSizeAtomicSeqCstSpsc       17,741,894        530,680
```
To verify the thread-safety we can run a simple test with `-fsanitize=thread`.

{{< fold "Test code" >}}

```cpp
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
```
{{< /fold >}}

Ok, now how do we optimize further? Let's try changing the memory orderings to acquire-release.
```cpp
  bool push(const T& t) {
    if (size_.load(std::memory_order_acquire) == cap_) return false;
    buf_[pushInd_++ % cap_] = t;
    size_.fetch_add(1, std::memory_order_release);
    return true;
  }

  bool pop(T& t) {
    if (size_.load(std::memory_order_acquire) == 0) return false;
    t = buf_[popInd_++ % cap_];
    size_.fetch_sub(1, std::memory_order_release);
    return true;
  }
```

```console {hl_Lines=5}
Queue Name                       Thru (ops/s)        std-Dev
------------------------------------------------------------
MutexSpsc                          19,296,994      1,591,781
LockfreeSizeAtomicSeqCstSpsc       17,741,894        530,680
LockfreeSizeAtomicAcqRelSpsc       17,337,915        299,840
```

But there is no real improvement (the difference is just noise and within std-Dev) in throughput due to this change. 

## Why no throughput improvement

Looking at the machine code, both versions generate the same machine code. Changing the memory ordering has no effect, as the machine code generated for `size_--` (i.e., `size_.fetch_sub(1, std::memory_order_seq_cst)`) is `lock sub`, which is the same as what's used for all `fetch_add` and `fetch_sub` operations irrespective of the memory ordering.
```asm {hl_Lines=3}
memory_order __m = memory_order_seq_cst) noexcept
return __atomic_fetch_sub(&_M_i, __i, int(__m)); }
lock    subq    $0x1,0x20(%rcx)
```

Lock instructions are slow for two main reasons:

1. The core must send an invalidation-broadcast signal across the CPU's internal interconnect (the communication bus between cores private caches and L3 cache), instructing other cores to invalidate their cached copies. The core then waits for acknowledgments from other cores and informs the cache controller: "I am performing an atomic operation; do not release this cache line until I am finished."

2. Modern x86 processors are "out-of-order" execution machines. They constantly reorder instructions and use a **store buffer** to delay writing data to memory so the CPU doesn't have to wait. The lock instruction acts as a full memory barrier. This forces the CPU to drain its store buffer and stalls the pipeline until the lock instruction is completed. This significantly reduces the throughput of an otherwise out-of-order system.

## What now

Okay, alright, the take away is `lock` instruction is slow, how can we avoid it. The reason we required lock instruction for `size_++` operations is because it's a read-write operation, so we need the cache-line exclusivity while we do the read-write operation (`++` and `--` are read-modify-write ops). Compared to this a simple store like `size_.store(1, std::memory_order_release)` doesn't require this `lock` prefix instruction.

So we resort to option 1 of making both `pushInd_` and `popInd_` indices atomic.

```cpp {hl_Lines="15-22 24-30"}
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
```

```console {hl_Lines=6}
Queue Name                       Thru (ops/s)        std-Dev
------------------------------------------------------------
MutexSpsc                          19,296,994      1,591,781
LockfreeSizeAtomicSeqCstSpsc       17,741,894        530,680
LockfreeSizeAtomicAcqRelSpsc       17,337,915        299,840
LockfreeAtomicPushPopPtrSpsc       65,065,925      1,001,645 
```

We are atomically loading and storing values. 
We can also look at the machine code and confirm there is no `lock` instruction involved.
On x86, C++ atomic load/store operations with any of relaxed/acquire/release memory orderings convert to normal load/store machine instructions because the x86 memory model provides TSO (Total Store Ordering). 
While we enforce specific memory orderings at code level using `std::memory_order_xxx`, the architecture itself provides default guarantees. For instance, the ARM architecture is generally weakly ordered, whereas x86 provides TSO.


>[!TIP] Sequential Consistency
In simple terms, there is a global ordering of operations (loads and store) which satisfy the observed behaviour of all the processors. So the behaviour is as if one instruction of one of the processors is executed at a time.

>[!TIP] Total Store Order (TSO)
Any two stores are seen in a consistent order by processors **other than those performing the stores**.  Apart from this, the memory is consistent/coherence.

>[!Quote] Atomic operations cost for x86
on x86 architectures, atomic load operations are always the same, whether tagged `memory_order_relaxed` or `memory_order_seq_cst`
CPUs that use the x86 or x86-64 architectures don’t require any additional instructions for acquire-release ordering beyond those necessary for ensuring atomicity, and even sequentially-consistent ordering doesn’t require any special treatment for load operations.
{cite="C++ concurrency in Action"}

Note: The atomic load/store approach compared to the atomic read-update-write operations of `++/--` works correctly because in SPSC, only the producer writes to `pushInd_` and only the consumer writes to `popInd_`, so each atomic variable has a single writer, avoiding write-write races.


What else can be optimized? The `%` modulo can be replaced (which is costly compared to simple `and` instruction) by a mask operation if we restrict the capacity to power of 2. This gives us:

```console {hl_Lines=7}
Queue Name                              Thru (ops/s)      std-Dev
------------------------------------------------------------------
MutexSpsc                               19,296,994      1,591,781
LockfreeSizeAtomicSeqCstSpsc            17,741,894        530,680
LockfreeSizeAtomicAcqRelSpsc            17,337,915        299,840
LockfreeAtomicPushPopPtrSpsc            65,065,925      1,001,645 
LockfreeAtomicPushPopPtrNoDivSpsc       70,229,371      2,195,707
```

## Cache line Optimization

With the current implementation, the `pushInd, popInd, cap, buf` all fall in same cache-line. (The benchmark we use allocates the spsc in new-64-aligned memory). 
```cpp
  size_t cap_;
  std::unique_ptr<T[]> buf_;
  std::atomic<uint64_t> pushInd_ = 0;
  std::atomic<uint64_t> popInd_ = 0;
```
What if we separate `msk_` and `buf_` into one cacheline (This cacheline will be in shared state in both cores and not modified) and `pushInd_`, `popInd_` in separate cachelines. The data members take 3 cachelines in total. The `pushInd_` and `popInd_` cachelines bounce between shared and exclusive state between the producer and consumer cores.

Making the changes we get:
```cpp {hl_Lines="4-7"}
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
```

We can instead get away with 2 cachelines by keeping a copy of `msk_` and `buf_` near each index. One cacheline contains `[pushInd_, mskPush_, buf_]` for the producer thread, and another cacheline contains `[popInd_, mskPop_, bufPop_]` for the consumer thread. This way, each thread primarily accesses its own cacheline.

We get:
```cpp
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
 ... <Rest remains same>...
};
```

```console {hl_Lines="8-9"}
Queue Name                                      Thru (ops/s)      std-Dev
--------------------------------------------------------------------------
MutexSpsc                                        19,296,994      1,591,781
LockfreeSizeAtomicSeqCstSpsc                     17,741,894        530,680
LockfreeSizeAtomicAcqRelSpsc                     17,337,915        299,840
LockfreeAtomicPushPopPtrSpsc                     65,065,925      1,001,645 
LockfreeAtomicPushPopPtrNoDivSpsc                70,229,371      2,195,707
LockfreeAtomicPushPopSeparateCacheLineSpsc       84,364,170      1,206,316
LockfreeAtomicPushPopSeparateCacheLineOptSpsc   119,191,268      2,711,806
```

Now for reasons I don't fully understand, the 3 cacheline approach gives considerably worse results than the 2 cacheline version and there is considerably more `L1-dcache-load-misses` compared to the 2 cacheline approach.

The machine code generated for the 3 cacheline and 2 cacheline is almost completely same. Except that some of the mov instructions displacement value (eg the 42 part in `mov rax, [rdi+42]`). I initially thought the throughput difference was due to the **128 byte rule**, but that does not explain the considerably higher `perf stat -e L1-dcache-load-misses` in 3 cacheline approach vs 2 cacheline. Share if you have any thoughts on what might be causing this.

{{< fold "The 128 byte rule">}}
The 128 byte rule refers to x86-64 instruction encoding efficiency. The machine code for accessing a structure member is more compact if the member's offset from the structure's base is less than 128 bytes, because the offset can be expressed as an 8-bit signed displacement in the instruction encoding (e.g., `mov rax, [rdi+42]`). 

If the offset is 128 bytes or more, the instruction must use a 32-bit displacement, making the instruction longer (e.g., `mov rax, [rdi+200]` requires a 4-byte immediate instead of 1-byte). This doesn't just affect instruction cache efficiency, modern CPUs decode instructions in chunks, and longer instructions can reduce decode throughput.
{{< /fold >}}


Anyways, let's move forward.

----
## Index Caching Optimization

Even with the optimized 2-cacheline layout, running `perf mem` shows a lot of "core, same node any cache hit" memory accesses. The actual data buffer is written by one core and read by another, so cross-core access is unavoidable there. But the same is true for `pushInd_` and `popInd_`, even though we've moved them to separate cachelines, both consumer and producer threads still need to read both indices.

However, we don't need to read the other thread's index on every operation. For example, if the last time we checked `pushInd_` during pop(), its value was say 100 higher than `popInd_`, then we don't need to check its value again for the next 100 successful pop operations. So we can instead cache the value of `pushInd_` in the consumer core (let's name this variable `pushCache`) and only re-read the actual `pushInd_` when the cached `pushCache` says there are no more elements to pop. We can do a similar thing for push().


Making the code changes give:

```cpp
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
```

```console {hl_Lines=10}
Queue Name                                      Thru (ops/s)      std-Dev
--------------------------------------------------------------------------
MutexSpsc                                        19,296,994      1,591,781
LockfreeSizeAtomicSeqCstSpsc                     17,741,894        530,680
LockfreeSizeAtomicAcqRelSpsc                     17,337,915        299,840
LockfreeAtomicPushPopPtrSpsc                     65,065,925      1,001,645 
LockfreeAtomicPushPopPtrNoDivSpsc                70,229,371      2,195,707
LockfreeAtomicPushPopSeparateCacheLineSpsc       84,364,170      1,206,316
LockfreeAtomicPushPopSeparateCacheLineOptSpsc   119,191,268      2,711,806
LockfreeAtomicPushPopPtrCachingSpsc             153,880,387     44,177,331
```

Now this optimization almost always benefits the push() operation, as we will only have roughly load `popInd_` once per `capacity` successful push operations. (assuming the consumer pop() throughput on average keep up with average producer push throughput)

For the pop() operation, the benefit depends on the producer-consumer timing. If the consumer has a higher peak throughput than the producer, then `popInd_` will always be close behind `pushInd_`, and we'll frequently have to reload `pushInd_` bypassing the cached value. In this case, the caching of `pushInd_` as `pushCache` and checking it first adds unnecessary overhead. 

However, for most practical use-cases, the consumer won't always be toe-to-toe with the producer (for example, the producer might push in bursts and then go silent for some time), so this optimization serves well. This is also the reason why the std-dev of `LockfreeAtomicPushPopPtrCachingSpsc` is significantly higher for our benchmarks.

## Time To Pause

> [!Quote] Pause instruction
> Improves the performance of spin-wait loops. When executing a “spin-wait loop,” processors will suffer a severe performance penalty when exiting the loop because it detects a possible memory order violation. The PAUSE instruction provides a hint to the processor that the code sequence is a spin-wait loop. The processor uses this hint to avoid the memory order violation in most situations, which greatly improves processor performance. For this reason, it is recommended that a PAUSE instruction be placed in all spin-wait loops.
> 
> An additional function of the PAUSE instruction is to reduce the power consumed by a processor while executing a spin loop. A processor can execute a spin-wait loop extremely quickly, causing the processor to consume a lot of power while it waits for the resource it is spinning on to become available. Inserting a pause instruction in a spin-wait loop greatly reduces the processor’s power consumption.
{cite="https://www.felixcloutier.com/x86/pause"}

I won't go into detail about the memory order violation that `Pause` instruction avoids. The `while(!q.push(someVal))` behaves similar to a spin-loop, we are looping until the `popInd_` value gets updated and visible to producer thread, and similar thing for the consumer thread. So adding pause in the `while(!q.push(someVal)){_mm_pause();}` can be beneficial.


```console {hl_Lines=11}
Queue Name                                     MMPause Thru (ops/s)      std-Dev
---------------------------------------------------------------------------------
MutexSpsc                                         ✗     19,296,994      1,591,781
LockfreeSizeAtomicSeqCstSpsc                      ✗     17,741,894        530,680
LockfreeSizeAtomicAcqRelSpsc                      ✗     17,337,915        299,840
LockfreeAtomicPushPopPtrSpsc                      ✗     65,065,925      1,001,645 
LockfreeAtomicPushPopPtrNoDivSpsc                 ✗     70,229,371      2,195,707
LockfreeAtomicPushPopSeparateCacheLineSpsc        ✗     84,364,170      1,206,316
LockfreeAtomicPushPopSeparateCacheLineOptSpsc     ✗    119,191,268      2,711,806
LockfreeAtomicPushPopPtrCachingSpsc               ✗    153,880,387     44,177,331
LockfreeAtomicPushPopPtrCachingSpsc               ✓    164,352,175     25,155,610
```

The benchmark code also measures the latency, time between a push succeeds and the corresponding pop is able to consume the data. Obviously these numbers and all the other benchmarks so far differ based on size of type T used. For all the benchmarks mentioned we are using `T = int64_t`.

```console
                                   Queue Name   MMPause   Thru (ops/s)      std-Dev         |--------------------------------------------- Latency ------------------------------------------|
                                                                                            Avg (ns)        Std-Dev       Min (ns)       P50 (ns)       P90 (ns)       P95 (ns)       P99 (ns) 
-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
                                    MutexSpsc       ✗     19,296,994      1,591,781            590           1005            120            601            782            921           1022   
                                    MutexSpsc       ✓     20,309,189        399,289            519            617            110            582            631            661            972   
                 LockfreeSizeAtomicSeqCstSpsc       ✗     17,741,894        530,680            155            398            110            151            170            171            181   
                 LockfreeSizeAtomicSeqCstSpsc       ✓     17,651,749        484,875            153             81            100            150            170            171            181   
                 LockfreeSizeAtomicAcqRelSpsc       ✗     17,337,915        299,840            154             77            110            151            170            171            180   
                 LockfreeSizeAtomicAcqRelSpsc       ✓     17,161,401        287,205            163            457            110            150            170            171            181   
                 LockfreeAtomicPushPopPtrSpsc       ✗     65,065,925      1,001,645            130             72             90            130            131            131            151   
                 LockfreeAtomicPushPopPtrSpsc       ✓     62,249,170      1,605,805            143             65            100            140            150            151            151   
            LockfreeAtomicPushPopPtrNoDivSpsc       ✗     70,229,371      2,195,707            125             69             90            121            131            131            140   
            LockfreeAtomicPushPopPtrNoDivSpsc       ✓     71,733,051      1,891,317            140             69             90            140            150            150            151   
   LockfreeAtomicPushPopSeparateCacheLineSpsc       ✗     84,364,170      1,206,316            124             69             90            120            130            131            131   
   LockfreeAtomicPushPopSeparateCacheLineSpsc       ✓     83,838,467      1,866,768            103             75             80            100            110            111            130   
LockfreeAtomicPushPopSeparateCacheLineOptSpsc       ✗    119,191,268      2,711,806            116             88             80            120            121            121            131   
LockfreeAtomicPushPopSeparateCacheLineOptSpsc       ✓    119,166,603      2,608,769             95             76             70             91            101            101            111   
          LockfreeAtomicPushPopPtrCachingSpsc       ✗    153,880,387     44,177,331            112             41             80            110            120            121            130
          LockfreeAtomicPushPopPtrCachingSpsc       ✓    164,352,175     25,155,610             95             37             80             91            101            101            111
```

## API Design Note

The `push()` and `pop()` methods shown here are actually non-blocking try operations (more accurately should be named `try_push()` and `try_pop()`).

If your usage pattern involves retrying until the operation succeeds (e.g., `while (!q.push(val)) { /* spin */ }`), it's more efficient to move the retry loop inside the `push()` function itself:

```cpp

void push(const T& t) {
     auto curPush = pushInd_.load(std::memory_order_relaxed);
     while(curPush - popCache == mskPush_ + 1) {
       popCache = popInd_.load(std::memory_order_acquire);
       if (curPush - popCache == mskPush_ + 1) {
        _mm_pause();
       }
     }
     buf_[curPush & mskPush_] = t;
     pushInd_.store(curPush + 1, std::memory_order_release);
}
```

This approach avoids repeatedly reloading `pushInd_` on every retry when compared to the `while(!q.try_push(val)){}` approach. Similar change follows for the `pop(T& t){}`


## Closing Thoughts
Hopefully this post has been less about "here's how an efficient SPSC looks" and more about the process: analyze, hypothesize, measure, repeat. The specific optimizations matter less than the approach, using `perf`, occasionally peeking at generated assembly, and understanding *why* something is slow before trying to fix it.
