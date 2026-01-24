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

/*
 * Arena-style PoolAllocator (matching atomsnap's memory management pattern)
 *
 * - Each allocating thread owns an arena (ThreadArena)
 * - Allocation: local list -> bulk steal from shared list -> new
 * - Deallocation: CAS push to owner thread's shared list (MPSC)
 */

struct PoolNode {
	PoolNode* next;
	uint32_t owner_tid;
};

struct ThreadArena {
	std::atomic<PoolNode*> shared_head{nullptr};  // MPSC: others push here
	PoolNode* local_head = nullptr;                // only owner accesses
};

template <typename T>
struct ArenaPool {
	static constexpr int kMaxThreads = 128;
	static inline std::atomic<uint32_t> tid_counter{0};
	static inline ThreadArena arenas[kMaxThreads];

	static inline thread_local bool tid_initialized = false;
	static inline thread_local uint32_t my_tid = 0;

	static inline uint32_t get_tid() {
		if (!tid_initialized) {
			my_tid = tid_counter.fetch_add(1, std::memory_order_relaxed);
			tid_initialized = true;
		}
		return my_tid;
	}

	static inline ThreadArena& my_arena() {
		return arenas[get_tid()];
	}
};

template <typename T>
struct PoolAllocator {
	using value_type = T;

	PoolAllocator() noexcept = default;

	template <typename U>
	PoolAllocator(const PoolAllocator<U>&) noexcept {}

	T* allocate(std::size_t n) {
		if (n > 1) {
			return static_cast<T*>(::operator new(n * sizeof(T)));
		}

		using Pool = ArenaPool<T>;
		ThreadArena& arena = Pool::my_arena();

		// 1. Pop from local list
		if (arena.local_head) {
			PoolNode* node = arena.local_head;
			arena.local_head = node->next;
			return reinterpret_cast<T*>(node);
		}

		// 2. Bulk steal from shared list (atomic_exchange)
		PoolNode* stolen = arena.shared_head.exchange(
			nullptr, std::memory_order_acquire);
		if (stolen) {
			arena.local_head = stolen->next;
			return reinterpret_cast<T*>(stolen);
		}

		// 3. Allocate new
		std::size_t sz = sizeof(T);
		if (sz < sizeof(PoolNode)) {
			sz = sizeof(PoolNode);
		}
		PoolNode* node = static_cast<PoolNode*>(::operator new(sz));
		node->owner_tid = Pool::get_tid();
		return reinterpret_cast<T*>(node);
	}

	void deallocate(T* p, std::size_t n) {
		if (!p) {
			return;
		}
		if (n > 1) {
			::operator delete(p);
			return;
		}

		using Pool = ArenaPool<T>;
		PoolNode* node = reinterpret_cast<PoolNode*>(p);
		ThreadArena& owner_arena = Pool::arenas[node->owner_tid];

		// CAS push to owner's shared list (MPSC pattern)
		PoolNode* head = owner_arena.shared_head.load(
			std::memory_order_relaxed);
		do {
			node->next = head;
		} while (!owner_arena.shared_head.compare_exchange_weak(
			head, node,
			std::memory_order_release,
			std::memory_order_relaxed));
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

