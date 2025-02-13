#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <barrier>
#include <iomanip>

#include "../../atomsnap.h"

std::atomic<size_t> total_writer_ops{0};
std::atomic<size_t> total_reader_ops{0};
int duration_seconds = 0;

struct Data {
	int value;
};

struct atomsnap_gate *gate = NULL;

struct atomsnap_version *atomsnap_alloc_impl(void *arg) {
	auto version = new atomsnap_version;
	auto data = new Data;

	data->value = (int)arg;
	version->object = data;
	version->free_context = NULL;

	return version;
}

void atomsnap_free_impl(struct atomsnap_version *version) {
	delete (Data *)version->object;
	delete version;
}

void writer(std::barrier<> &sync) {
	sync.arrive_and_wait();
	auto start = std::chrono::steady_clock::now();
	size_t ops = 0;
	struct atomsnap_version *old_version, *new_version;

	while (true) {
		auto now = std::chrono::steady_clock::now();
		int sec = std::chrono::duration_cast<std::chrono::seconds>
			(now - start).count();

		if (sec >= duration_seconds) {
			break;
		}

		struct atomsnap_version *old_version = atomsnap_acquire_version(gate);
		auto old_data = static_cast<Data*>(old_version->object);
		new_version = atomsnap_make_version(gate, old_data->value + 1);
		atomsnap_release_version(old_version);

		atomsnap_exchange_version(gate, new_version);
		ops++;
	}

	total_writer_ops.fetch_add(ops, std::memory_order_relaxed);
}

void reader(std::barrier<> &sync) {
	sync.arrive_and_wait();
	auto start = std::chrono::steady_clock::now();
	size_t ops = 0;
	struct atomsnap_version *current_version;

	while (true) {
		auto now = std::chrono::steady_clock::now();
		int sec = std::chrono::duration_cast<std::chrono::seconds>
			(now - start).count();

		if (sec >= duration_seconds) {
			break;
		}

		current_version = atomsnap_acquire_version(gate);
		Data *d = static_cast<Data*>(current_version->object);
		volatile int tmp = d->value;
		(void)tmp;
		atomsnap_release_version(current_version);

		ops++;
	}

	total_reader_ops.fetch_add(ops, std::memory_order_relaxed);
}

int main(int argc, char **argv) {
	int writer_count, reader_count;

	if (argc < 4) {
		std::cerr << "Usage: " << argv[0] << 
			" <writer_count> <reader_count> <duration_seconds>\n";
		return -1;
	}

	writer_count = std::atoi(argv[1]);
	reader_count = std::atoi(argv[2]);
	duration_seconds = std::atoi(argv[3]);

	if (writer_count <= 0 || reader_count <= 0 || duration_seconds <- 0) {
		std::cerr << "Invalid arguments\n";
		return -1;
	}

	struct atomsnap_init_context atomsnap_gate_ctx = {
		.atomsnap_alloc_impl = atomsnap_alloc_impl,
		.atomsnap_free_impl = atomsnap_free_impl
	};

	gate = atomsnap_init_gate(&atomsnap_gate_ctx);
	if (!gate) {
		std::cerr << "Failed to init atomsnap_gate\n";
		return -1;
	}

	struct atomsnap_version *initial_version = atomsnap_make_version(gate, 0);
	atomsnap_exchange_version(gate, initial_version);

	std::barrier sync(writer_count + reader_count);
	std::vector<std::thread> threads;
	threads.reserve(writer_count + reader_count);

	for (int i = 0; i < writer_count; i++) {
		threads.emplace_back(writer, std::ref(sync));
	}

	for (int i = 0; i < reader_count; i++) {
		threads.emplace_back(reader, std::ref(sync));
	}

	for (auto &t : threads) {
		t.join();
	}

	std::cout << std::fixed << std::setprecision(0);
	std::cout << "Total writer throughput: "
		<< total_writer_ops.load(std::memory_order_relaxed) 
			/ static_cast<double>(duration_seconds)
		<< " ops/sec\n";
	std::cout << "Total reader throughput: "
		<< total_reader_ops.load(std::memory_order_relaxed) 
			/ static_cast<double>(duration_seconds)
		<< " ops/sec\n";
}
