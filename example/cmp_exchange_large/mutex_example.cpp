#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <barrier>
#include <iomanip>
#include <mutex>
#include <shared_mutex>

std::atomic<size_t> total_writer_ops{0};
std::atomic<size_t> total_reader_ops{0};
int duration_seconds = 0;

struct Data {
	int64_t values[512];
};

Data *global_ptr = new Data{ 0, };

std::shared_mutex rwlock;

void writer(std::barrier<> &sync) {
	sync.arrive_and_wait();
	auto start = std::chrono::steady_clock::now();
	size_t ops = 0;

	while (true) {
		auto now = std::chrono::steady_clock::now();
		int sec = std::chrono::duration_cast<std::chrono::seconds>
			(now - start).count();

		if (sec >= duration_seconds) {
			break;
		}

		{
			std::unique_lock<std::shared_mutex> lock(rwlock);

			for (int i = 0; i < 512; i++) {
				global_ptr->values[i] += 1;
			}
		}

		ops++;
	}

	total_writer_ops.fetch_add(ops, std::memory_order_relaxed);
}

void reader(std::barrier<> &sync) {
	sync.arrive_and_wait();
	auto start = std::chrono::steady_clock::now();
	size_t ops = 0;
	int64_t prev_value = 0, current_value = 0;

	while (true) {
		auto now = std::chrono::steady_clock::now();
		int sec = std::chrono::duration_cast<std::chrono::seconds>
			(now - start).count();

		if (sec >= duration_seconds) {
			break;
		}

		{
			std::shared_lock<std::shared_mutex> lock(rwlock);
			current_value = global_ptr->values[0];

			for (int i = 0; i < 512; i++) {
				if (global_ptr->values[i] != current_value) {
					fprintf(stderr, "Invalid data\n");
					exit(1);
				}
			}
		}

		if (current_value < prev_value) {
			fprintf(stderr, "Invalid value, prev: %ld, now: %ld\n",
				prev_value, current_value);
			exit(1);
		}

		prev_value = current_value;

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
