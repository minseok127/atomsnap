#include <atomic>
#include <barrier>
#include <chrono>
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
 *
 * Memory layout:
 * ┌──────────────────────┬─────────────────────────────┐
 * │  PoolBlock (header)  │     User Data (returned)    │
 * │  next + owner_tid    │                             │
 * └──────────────────────┴─────────────────────────────┘
 *                         ↑ returned pointer
 */

struct PoolBlock {
	PoolBlock* next;
	uint32_t owner_tid;
	uint32_t _pad;  // align to 16 bytes
};
static_assert(sizeof(PoolBlock) == 16, "PoolBlock should be 16 bytes");

struct ThreadArena {
	std::atomic<PoolBlock*> shared_head{nullptr};  // MPSC: others push here
	PoolBlock* local_head = nullptr;                // only owner accesses
};

struct GlobalArenaPool {
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

		using Pool = GlobalArenaPool;
		uint32_t my_tid = Pool::get_tid();
		ThreadArena& arena = Pool::arenas[my_tid];

		// 1. Pop from local list
		if (arena.local_head) {
			PoolBlock* block = arena.local_head;
			arena.local_head = block->next;
			return reinterpret_cast<T*>(block + 1);  // return data part
		}

		// 2. Bulk steal from shared list (atomic_exchange)
		PoolBlock* stolen = arena.shared_head.exchange(
			nullptr, std::memory_order_acquire);
		if (stolen) {
			arena.local_head = stolen->next;
			return reinterpret_cast<T*>(stolen + 1);  // return data part
		}

		// 3. Allocate new
		std::size_t sz = sizeof(PoolBlock) + sizeof(T);
		PoolBlock* block = static_cast<PoolBlock*>(::operator new(sz));
		block->owner_tid = my_tid;
		return reinterpret_cast<T*>(block + 1);  // return data part
	}

	void deallocate(T* p, std::size_t n) {
		if (!p) {
			return;
		}
		if (n > 1) {
			::operator delete(p);
			return;
		}

		// Get block header from user pointer
		PoolBlock* block = reinterpret_cast<PoolBlock*>(p) - 1;
		uint32_t owner_tid = block->owner_tid;

		ThreadArena& owner_arena = GlobalArenaPool::arenas[owner_tid];

		// CAS push to owner's shared list (MPSC pattern)
		PoolBlock* head = owner_arena.shared_head.load(
			std::memory_order_relaxed);
		do {
			block->next = head;
		} while (!owner_arena.shared_head.compare_exchange_weak(
			head, block,
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

	while (true) {
		auto now = std::chrono::steady_clock::now();
		int sec = (int)std::chrono::duration_cast<
			std::chrono::seconds>(now - start).count();

		if (sec >= duration_seconds) {
			break;
		}

		auto old_data = std::atomic_load_explicit(
			&global_ptr, std::memory_order_acquire);

		Data* raw = new Data;
		raw->value1 = old_data->value1 + 1;
		raw->value2 = old_data->value2 + 1;

		std::shared_ptr<Data> new_data(
			raw, std::default_delete<Data>(), alloc);

		std::atomic_store_explicit(
			&global_ptr, new_data, std::memory_order_release);

		ops++;
	}

	total_writer_ops.fetch_add(ops, std::memory_order_relaxed);
}

void reader(std::barrier<>& sync) {
	sync.arrive_and_wait();
	auto start = std::chrono::steady_clock::now();
	std::size_t ops = 0;

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

