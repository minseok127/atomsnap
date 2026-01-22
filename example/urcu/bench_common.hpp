/* bench_common.hpp */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
static inline void cpu_relax(void)
{
	__asm__ __volatile__("pause" ::: "memory");
}
#elif defined(__aarch64__)
static inline void cpu_relax(void)
{
	__asm__ __volatile__("yield" ::: "memory");
}
#else
static inline void cpu_relax(void)
{
	std::atomic_signal_fence(std::memory_order_seq_cst);
}
#endif

struct CsBurner {
	double iters_per_ns;

	CsBurner() : iters_per_ns(0.0) {}

	void calibrate(uint64_t target_ns = 20ULL * 1000 * 1000)
	{
		for (int i = 0; i < 1000; i++) {
			cpu_relax();
		}

		uint64_t iters = 1ULL << 16;

		for (;;) {
			auto t0 = std::chrono::steady_clock::now();
			for (uint64_t i = 0; i < iters; i++) {
				cpu_relax();
			}
			auto t1 = std::chrono::steady_clock::now();

			uint64_t ns;
			ns = (uint64_t)std::chrono::duration_cast<
				std::chrono::nanoseconds>(t1 - t0).count();

			if (ns >= target_ns) {
				iters_per_ns = (double)iters / (double)ns;
				break;
			}

			iters <<= 1;
			if (iters > (1ULL << 36)) {
				iters_per_ns = 1.0;
				break;
			}
		}

		if (iters_per_ns <= 0.0) {
			iters_per_ns = 1.0;
		}
	}

	inline void burn_ns(uint64_t ns) const
	{
		if (ns == 0) {
			return;
		}

		uint64_t iters = (uint64_t)((double)ns * iters_per_ns);
		if (iters < 8) {
			iters = 8;
		}

		for (uint64_t i = 0; i < iters; i++) {
			cpu_relax();
		}
	}
};

static inline uint64_t now_ns(void)
{
	return (uint64_t)std::chrono::duration_cast<
		std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

static inline long get_peak_rss_kb(void)
{
#if defined(__linux__)
	struct rusage r;
	if (getrusage(RUSAGE_SELF, &r) == 0) {
		return r.ru_maxrss;
	}
#endif
	return 0;
}

static inline void pin_thread_to_cpu(int cpu)
{
#if defined(__linux__)
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	(void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
	(void)cpu;
#endif
}

