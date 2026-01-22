#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>
#include <barrier>
#include <iomanip>
#include <cstring>

/*
 * liburcu header requirements.
 * Using "memb" flavor (Memory Barrier) which is most comparable to
 * atomic reference counting in terms of memory ordering.
 */
#include <urcu/urcu-memb.h>
#include <urcu/uatomic.h>

#include "bench_common.hpp"

/*
 * Benchmark Configuration
 */
static int g_writer_count;
static int g_reader_count;
static int g_duration_sec;
static long g_cs_delay_ns;
static int g_reclamation_mode; /* 0: Sync, 1: Async */

/*
 * Metrics
 */
static std::atomic<size_t> g_total_writer_ops{0};
static std::atomic<size_t> g_total_reader_ops{0};
static std::atomic<bool> g_running{true};

/*
 * Data Object
 * Includes 'rcu_head' for asynchronous reclamation (call_rcu).
 */
struct Data {
	int64_t v1;
	int64_t v2;
	struct rcu_head rcu;
};

static Data *g_global_ptr = nullptr;

/*
 * RCU Callback for Async Reclamation.
 * Called when the grace period expires for a specific object.
 */
static void rcu_free_func(struct rcu_head *head)
{
	Data *d = caa_container_of(head, Data, rcu);
	delete d;
}

/*
 * Writer Thread
 * Performs updates using atomic exchange.
 * Handles reclamation based on g_reclamation_mode.
 */
static void writer_thread(std::barrier<> &sync)
{
	rcu_register_thread_memb();
	sync.arrive_and_wait();

	size_t ops = 0;

	/* Create local new data to swap in */
	Data *new_data = new Data;
	new_data->v1 = 0;
	new_data->v2 = 0;

	while (g_running.load(std::memory_order_relaxed)) {
		/* Update values */
		new_data->v1++;
		new_data->v2++;

		/*
		 * Publish new version.
		 * uatomic_xchg returns the old pointer.
		 */
		Data *old_data = (Data *)uatomic_xchg(&g_global_ptr, new_data);

		if (old_data) {
			if (g_reclamation_mode == 0) {
				/* Sync: Block until all readers are done */
				synchronize_rcu_memb();
				delete old_data;
			} else {
				/* Async: Register callback and return immediately */
				call_rcu_memb(&old_data->rcu, rcu_free_func);
			}
		}

		/* Allocate next object for the next iteration */
		new_data = new Data;
		new_data->v1 = 0;
		new_data->v2 = 0;

		ops++;
	}

	/* Cleanup the unused new_data */
	delete new_data;

	rcu_unregister_thread_memb();
	g_total_writer_ops.fetch_add(ops, std::memory_order_relaxed);
}

/*
 * Reader Thread
 * Reads data inside RCU critical section and simulates workload.
 */
static void reader_thread(std::barrier<> &sync)
{
	rcu_register_thread_memb();
	sync.arrive_and_wait();

	size_t ops = 0;

	while (g_running.load(std::memory_order_relaxed)) {
		rcu_read_lock_memb();

		/* Dereference the shared pointer safely */
		Data *d = rcu_dereference(g_global_ptr);

		if (d) {
			/* Verification */
			if (d->v1 != d->v2) {
				fprintf(stderr, "Consistency Error: %ld != %ld\n",
					d->v1, d->v2);
				exit(EXIT_FAILURE);
			}

			/* Simulate critical section work (busy-wait) */
			simulate_cs(g_cs_delay_ns);
		}

		rcu_read_unlock_memb();
		ops++;
	}

	rcu_unregister_thread_memb();
	g_total_reader_ops.fetch_add(ops, std::memory_order_relaxed);
}

/*
 * Monitor Thread
 * Periodically stops the benchmark and measures peak memory.
 */
static void monitor_thread(std::barrier<> &sync)
{
	sync.arrive_and_wait();

	auto start = std::chrono::steady_clock::now();

	while (true) {
		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
			now - start).count();

		if (elapsed >= g_duration_sec) {
			g_running.store(false, std::memory_order_relaxed);
			break;
		}

		/* Sleep 10ms to reduce overhead */
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

int main(int argc, char **argv)
{
	if (argc < 6) {
		std::cerr << "Usage: " << argv[0] 
			<< " <readers> <writers> <duration_sec> "
			<< "<cs_delay_ns> <mode(0:Sync, 1:Async)>\n";
		return -1;
	}

	g_reader_count = std::atoi(argv[1]);
	g_writer_count = std::atoi(argv[2]);
	g_duration_sec = std::atoi(argv[3]);
	g_cs_delay_ns = std::atol(argv[4]);
	g_reclamation_mode = std::atoi(argv[5]);

	if (g_reader_count <= 0 || g_writer_count <= 0 || g_duration_sec <= 0) {
		std::cerr << "Invalid arguments\n";
		return -1;
	}

	/* Initialize global pointer */
	g_global_ptr = new Data{0, 0, {}};

	/* +1 for monitor thread */
	std::barrier sync(g_reader_count + g_writer_count + 1);
	std::vector<std::thread> threads;
	threads.reserve(g_reader_count + g_writer_count + 1);

	/* Launch Writers */
	for (int i = 0; i < g_writer_count; i++) {
		threads.emplace_back(writer_thread, std::ref(sync));
	}

	/* Launch Readers */
	for (int i = 0; i < g_reader_count; i++) {
		threads.emplace_back(reader_thread, std::ref(sync));
	}

	/* Launch Monitor */
	threads.emplace_back(monitor_thread, std::ref(sync));

	/* Wait for completion */
	for (auto &t : threads) {
		t.join();
	}

	/* If Async mode, wait for pending callbacks to clear (optional check) */
	if (g_reclamation_mode == 1) {
		rcu_barrier_memb();
	}

	/* Print Metrics in CSV format: Writers, Readers, PeakRSS(KB) */
	double duration = static_cast<double>(g_duration_sec);
	double w_iops = g_total_writer_ops.load() / duration;
	double r_iops = g_total_reader_ops.load() / duration;
	long peak_rss = get_peak_rss_kb();

	std::cout << std::fixed << std::setprecision(2);
	std::cout << "Writer IOPS: " << w_iops << "\n";
	std::cout << "Reader IOPS: " << r_iops << "\n";
	std::cout << "Peak RSS KB: " << peak_rss << "\n";

	return 0;
}
