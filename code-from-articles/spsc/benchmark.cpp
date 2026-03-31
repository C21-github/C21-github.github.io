#include <emmintrin.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <thread>
#include <vector>

#include "spsc.h"

/*************** Benchmark Configuration ***************/

constexpr int BENCHMARK_RUNS = 10;

/*************** Thread Pinning ***************/

void pin(unsigned int core) {
  pid_t threadId = syscall(SYS_gettid);
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  if (sched_setaffinity(threadId, sizeof(cpu_set_t), &set) == -1) {
    std::cerr << "Couldn't set affinity\n";
    throw std::runtime_error("Could not set affinity");
  }
}

/*************** Statistical Helpers ***************/

template<typename T>
double calculateMean(const std::vector<T>& data) {
  double sum = std::accumulate(data.begin(), data.end(), 0.0);
  return sum / data.size();
}

template<typename T>
double calculateStandardDeviation(const std::vector<T>& data) {
  double mean = calculateMean(data);
  double variance = 0.0;
  for (double value : data) {
    variance += (value - mean) * (value - mean);
  }
  variance /= data.size();
  return std::sqrt(variance);
}

template<typename T>
T calculatePercentile(std::vector<T> data, double percentile) {
  std::sort(data.begin(), data.end());
  size_t index = static_cast<size_t>((percentile / 100.0) * (data.size() - 1));
  return data[index];
}

/*************** Number Formatting Helper ***************/

std::string formatNumberWithCommas(int64_t num) {
  std::string str = std::to_string(num);
  int n = str.length() - 3;
  while (n > 0) {
    str.insert(n, ",");
    n -= 3;
  }
  return str;
}

/*************** Throughput Benchmark ***************/

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
  return (iterations * 1'000'000'000.0) / elapsed_ns;  // ops per second
}

/*************** Latency Benchmark ***************/

int64_t curTime() {
  auto now = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
                  now.time_since_epoch())
                  .count();
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


/*************** Benchmark Runner ***************/

template <template <typename> typename spsc, bool UsePause>
void runBenchmarkT(const std::string& name, int prodCore, int consCore, size_t queueSize, int64_t iterations) {
  // Run throughput benchmark BENCHMARK_RUNS times
  std::vector<double> throughputResults;
  for (int i = 0; i < BENCHMARK_RUNS; i++) {
    double ops_s = benchmarkThroughput<spsc, UsePause>(consCore, prodCore, queueSize, iterations);
    throughputResults.push_back(ops_s);
  }
  double throughput_mean = calculateMean(throughputResults);
  double throughput_stddev = calculateStandardDeviation(throughputResults);

  // Run latency benchmark once
  std::vector<uint64_t> latencies = benchmarkLatency<spsc, UsePause>(consCore, prodCore, queueSize, iterations);
  
  // Calculate latency statistics
  double latency_mean = calculateMean(latencies);
  double latency_stddev = calculateStandardDeviation(latencies);
  uint64_t latency_min = *std::min_element(latencies.begin(), latencies.end());
  uint64_t latency_p50 = calculatePercentile(latencies, 50.0);
  uint64_t latency_p90 = calculatePercentile(latencies, 90.0);
  uint64_t latency_p95 = calculatePercentile(latencies, 95.0);
  uint64_t latency_p99 = calculatePercentile(latencies, 99.0);
  uint64_t latency_max = *std::max_element(latencies.begin(), latencies.end());

  // Print results in formatted table row
  std::cout << std::setw(45) << name << std::setw(10) << (UsePause ? "✓" : "✗") 
            << std::setw(15) << std::fixed << std::setprecision(2) << formatNumberWithCommas(throughput_mean)
            << std::setw(15) << std::fixed << std::setprecision(2)
            << formatNumberWithCommas(throughput_stddev) << std::setw(15)
            << std::fixed << std::setprecision(2)
            << static_cast<int64_t>(latency_mean) << std::setw(15) << std::fixed
            << std::setprecision(2) << static_cast<int64_t>(latency_stddev)
            << std::setw(15) << latency_min
            << std::setw(15) << latency_p50
            << std::setw(15) << latency_p90
            << std::setw(15) << latency_p95
            << std::setw(15) << latency_p99
            << std::setw(15) << latency_max
            << "\n";
}

template <template <typename> typename spsc>
void runBenchmark(const std::string& name, int prodCore, int consCore, size_t queueSize, int64_t iterations) {
	runBenchmarkT<spsc, false>(name, prodCore, consCore, queueSize, iterations);
	runBenchmarkT<spsc, true>(name, prodCore, consCore, queueSize, iterations);
}

/*************** Main ***************/

int main(int argc, char** argv) {
  // Parse command-line arguments
  if (argc < 3) {
    std::cout << "Usage: " << argv[0]
              << " producerCore consumerCore [Optional:queueName]\n";
    exit(1);
  }

  int prodCore = std::stoi(argv[1]);
  int consCore = std::stoi(argv[2]);
  std::string toRun = (argc >= 4) ? argv[3] : "";

  // Define configuration array: {queueSize, iterations}
  const std::vector<std::pair<size_t, int64_t>> configs = {
		{1'000, 1'000'000},
    {10'000, 1'000'000},
    {50'000, 1'000'000},
    {100'000, 1'000'000},
  };

  // Run benchmarks for each configuration
  for (const auto& [queueSize, iterations] : configs) {
    // Print header for this configuration
    std::cout << "SPSC Queue Benchmark\n";
    std::cout << "====================\n";
    std::cout << "Producer core: " << prodCore << ", Consumer core: " << consCore
              << "\n";
    std::cout << "Queue size: " << formatNumberWithCommas(queueSize) << "\n";
    std::cout << "Iterations: " << formatNumberWithCommas(iterations)
               << " per benchmark\n";
    std::cout << BENCHMARK_RUNS << " runs per"
              << " benchmark for throughput statistics calculation \n\n";

    // Print table header with latency section indicator
    std::cout << std::setw(45) << "Queue Name" << std::setw(10) << "MMPause"
              << std::setw(15) << "Thru (ops/s)"
              << std::setw(15) << "std-Dev" << std::string(96, ' ') << "\n";
    
    std::cout << std::setw(85) << "" << "|" << std::string(54, '-') << " Latency " << std::string(54, '-') << "|\n";
    
    std::cout << std::setw(45) << "" << std::setw(10) << ""
              << std::setw(15) << ""
              << std::setw(15) << "" << std::setw(15) << "Avg (ns)"
              << std::setw(15) << "Std-Dev" << std::setw(15) << "Min (ns)"
              << std::setw(15) << "P50 (ns)" << std::setw(15) << "P90 (ns)"
              << std::setw(15) << "P95 (ns)" << std::setw(15) << "P99 (ns)"
              << std::setw(15) << "Max (ns)" << "\n";
    std::cout << std::string(200, '-') << "\n";

    // Run benchmarks for all queues (or specific one)
    if (toRun == "" || toRun == "MutexSpsc")
      runBenchmark<my::MutexSpsc>("MutexSpsc", prodCore, consCore, queueSize, iterations);

    if (toRun == "" || toRun == "LockfreeSizeAtomicSeqCstSpsc")
      runBenchmark<my::LockfreeSizeAtomicSeqCstSpsc>(
          "LockfreeSizeAtomicSeqCstSpsc", prodCore, consCore, queueSize, iterations);

    if (toRun == "" || toRun == "LockfreeSizeAtomicAcqRelSpsc")
      runBenchmark<my::LockfreeSizeAtomicAcqRelSpsc>(
          "LockfreeSizeAtomicAcqRelSpsc", prodCore, consCore, queueSize, iterations);

    if (toRun == "" || toRun == "LockfreeSizeAtomicAcqRelSpsc_AlignmentOpt")
      runBenchmark<my::LockfreeSizeAtomicAcqRelSpsc_AlignmentOpt>(
          "LockfreeSizeAtomicAcqRelSpsc_AlignmentOpt", prodCore, consCore, queueSize, iterations);

    if (toRun == "" || toRun == "LockfreeAtomicPushPopPtrSpsc")
      runBenchmark<my::LockfreeAtomicPushPopPtrSpsc>(
          "LockfreeAtomicPushPopPtrSpsc", prodCore, consCore, queueSize, iterations);

    if (toRun == "" || toRun == "LockfreeAtomicPushPopPtrNoDivSpsc")
      runBenchmark<my::LockfreeAtomicPushPopPtrNoDivSpsc>(
          "LockfreeAtomicPushPopPtrNoDivSpsc", prodCore, consCore, queueSize, iterations);

    if (toRun == "" || toRun == "LockfreeAtomicPushPopSeparateCacheLineSpsc")
      runBenchmark<my::LockfreeAtomicPushPopSeparateCacheLineSpsc>(
          "LockfreeAtomicPushPopSeparateCacheLineSpsc", prodCore, consCore, queueSize, iterations);

    if (toRun == "" || toRun == "LockfreeAtomicPushPopSeparateCacheLineOptSpsc")
      runBenchmark<my::LockfreeAtomicPushPopSeparateCacheLineOptSpsc>(
          "LockfreeAtomicPushPopSeparateCacheLineOptSpsc", prodCore, consCore, queueSize, iterations);

    if (toRun == "" || toRun == "LockfreeAtomicPushPopPtrCachingSpsc")
      runBenchmark<my::LockfreeAtomicPushPopPtrCachingSpsc>(
          "LockfreeAtomicPushPopPtrCachingSpsc", prodCore, consCore, queueSize, iterations);
    
    std::cout << "\n";  // Separator between configurations
  }
   
  return 0;
}
