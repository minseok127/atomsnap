#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <barrier>
#include <iomanip>

std::atomic<size_t> total_writer_ops{0};
std::atomic<size_t> total_reader_ops{0};
int duration_seconds = 0;

struct Data {
	int64_t values[512];
};

std::shared_ptr<Data> global_ptr = std::make_shared<Data>();

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

		auto old_data = std::atomic_load(&global_ptr);
		auto new_data = std::make_shared<Data>(*old_data);

		for (int i = 0; i < 512; i++) {
			new_data->values[i] = old_data->values[i];
		}

		if (std::atomic_compare_exchange_strong(&global_ptr,
				&old_data, new_data)) {
			ops++;
		}
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

		auto current_data = std::atomic_load(&global_ptr);

		current_value = current_data->values[0];
		for (int i = 0; i < 512; i++) {
			if (current_data->values[i] != current_value) {
				fprintf(stderr, "Invalid data\n");
				exit(1);
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
