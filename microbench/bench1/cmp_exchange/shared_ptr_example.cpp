#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

struct Node {
	Node* next;
};

template <typename T>
struct PoolState {
	static std::atomic<Node*> ghead;
	static thread_local Node* lhead;
	static thread_local std::size_t lcount;
};

template <typename T>
std::atomic<Node*> PoolState<T>::ghead{nullptr};

template <typename T>
thread_local Node* PoolState<T>::lhead = nullptr;

template <typename T>
thread_local std::size_t PoolState<T>::lcount = 0;

template <typename T>
struct PoolAllocator {
	using value_type = T;

	PoolAllocator() noexcept = default;

	template <typename U>
	PoolAllocator(const PoolAllocator<U>&) noexcept {}

	static constexpr std::size_t kFlush = 1u << 20;

	static T* pop_local() {
		using ST = PoolState<T>;
		Node* n = ST::lhead;
		if (!n) {
			return nullptr;
		}
		ST::lhead = n->next;
		ST::lcount--;
		return reinterpret_cast<T*>(n);
	}

	static T* pop_global() {
		using ST = PoolState<T>;
		Node* head = ST::ghead.load(std::memory_order_acquire);
		while (head) {
			Node* next = head->next;
			if (ST::ghead.compare_exchange_weak(
					head, next,
					std::memory_order_acq_rel,
					std::memory_order_acquire)) {
				return reinterpret_cast<T*>(head);
			}
		}
		return nullptr;
	}

	static void push_local(T* p) {
		using ST = PoolState<T>;
		Node* n = reinterpret_cast<Node*>(p);
		n->next = ST::lhead;
		ST::lhead = n;
		ST::lcount++;
	}

	static void push_global(Node* n) {
		using ST = PoolState<T>;
		Node* head = ST::ghead.load(std::memory_order_relaxed);
		do {
			n->next = head;
		} while (!ST::ghead.compare_exchange_weak(
				head, n,
				std::memory_order_release,
				std::memory_order_relaxed));
	}

	static void flush_local() {
		using ST = PoolState<T>;
		while (ST::lcount > kFlush / 2) {
			Node* n = ST::lhead;
			if (!n) {
				break;
			}
			ST::lhead = n->next;
			ST::lcount--;
			push_global(n);
		}
	}

	T* allocate(std::size_t n) {
		if (n > 1) {
			return static_cast<T*>(
				::operator new(n * sizeof(T)));
		}

		if (T* p = pop_local()) {
			return p;
		}
		if (T* p = pop_global()) {
			return p;
		}

		std::size_t sz = sizeof(T);
		if (sz < sizeof(Node)) {
			sz = sizeof(Node);
		}
		return static_cast<T*>(::operator new(sz));
	}

	void deallocate(T* p, std::size_t n) {
		if (!p) {
			return;
		}
		if (n > 1) {
			::operator delete(p);
			return;
		}

		push_local(p);
		if (PoolState<T>::lcount >= kFlush) {
			flush_local();
		}
	}

	template <typename U>
	bool operator==(const PoolAllocator<U>&) const noexcept {
		return true;
	}
	template <typename U>
	bool operator!=(const PoolAllocator<U>&) const noexcept {
		return false;
	}
};

std::atomic<std::size_t> total_writer_ops{0};
std::atomic<std::size_t> total_reader_ops{0};
int duration_seconds = 0;

struct Data {
	std::int64_t value1;
	std::int64_t value2;
};

std::shared_ptr<Data> global_ptr(
	new Data{0, 0},
	std::default_delete<Data>(),
	PoolAllocator<Data>());

void writer(std::barrier<>& sync) {
	sync.arrive_and_wait();
	auto start = std::chrono::steady_clock::now();
	std::size_t ops = 0;

	PoolAllocator<Data> alloc;

	Data* raw = new Data{0, 0};
	std::shared_ptr<Data> new_data(
		raw, std::default_delete<Data>(), alloc);

	while (true) {
		auto now = std::chrono::steady_clock::now();
		int sec = (int)std::chrono::duration_cast<
			std::chrono::seconds>(now - start).count();

		if (sec >= duration_seconds) {
			break;
		}

		auto expected = std::atomic_load_explicit(
			&global_ptr, std::memory_order_acquire);

		new_data->value1 = expected->value1 + 1;
		new_data->value2 = expected->value2 + 1;

		bool ok = std::atomic_compare_exchange_strong_explicit(
			&global_ptr, &expected, new_data,
			std::memory_order_acq_rel,
			std::memory_order_acquire);

		if (ok) {
			ops++;

			raw = new Data;
			new_data = std::shared_ptr<Data>(
				raw, std::default_delete<Data>(), alloc);
		}
	}

	total_writer_ops.fetch_add(ops, std::memory_order_relaxed);
}

void reader(std::barrier<>& sync) {
	sync.arrive_and_wait();
	auto start = std::chrono::steady_clock::now();
	std::size_t ops = 0;
	std::int64_t prev_value = 0;

	while (true) {
		auto now = std::chrono::steady_clock::now();
		int sec = (int)std::chrono::duration_cast<
			std::chrono::seconds>(now - start).count();

		if (sec >= duration_seconds) {
			break;
		}

		auto cur = std::atomic_load_explicit(
			&global_ptr, std::memory_order_acquire);

		if (cur->value1 != cur->value2) {
			std::fprintf(stderr,
				"Invalid data, value1: %ld, value2: %ld\n",
				(long)cur->value1, (long)cur->value2);
			std::exit(1);
		}
		if (cur->value1 < prev_value) {
			std::fprintf(stderr,
				"Invalid value, prev: %ld, now: %ld\n",
				(long)prev_value, (long)cur->value1);
			std::exit(1);
		}
		prev_value = cur->value1;

		ops++;
	}

	total_reader_ops.fetch_add(ops, std::memory_order_relaxed);
}

int main(int argc, char** argv) {
	int writer_count = 0;
	int reader_count = 0;

	if (argc < 4) {
		std::cerr << "Usage: " << argv[0]
			<< " <writer_count> <reader_count>"
			<< " <duration_seconds>\n";
		return -1;
	}

	writer_count = std::atoi(argv[1]);
	reader_count = std::atoi(argv[2]);
	duration_seconds = std::atoi(argv[3]);

	if (writer_count <= 0 || reader_count <= 0 ||
		duration_seconds < 0) {
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

	for (auto& t : threads) {
		t.join();
	}

	std::cout << std::fixed << std::setprecision(0);

	double wps = total_writer_ops.load(std::memory_order_relaxed) /
		(double)duration_seconds;
	double rps = total_reader_ops.load(std::memory_order_relaxed) /
		(double)duration_seconds;

	std::cout << "Total writer throughput: " << wps << " ops/sec\n";
	std::cout << "Total reader throughput: " << rps << " ops/sec\n";
}

