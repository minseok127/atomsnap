/* lf_pool.hpp */
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

class TaggedFreeList {
public:
	explicit TaggedFreeList(size_t block_size, size_t align = 16)
		: head_(0),
		  block_size_(block_size),
		  align_(align)
	{
		if (align_ < 16) {
			align_ = 16;
		}
		if ((align_ & (align_ - 1)) != 0) {
			align_ = 16;
		}
		if (block_size_ < sizeof(Node)) {
			block_size_ = sizeof(Node);
		}
	}

	void *alloc(void)
	{
		uint64_t head = head_.load(std::memory_order_acquire);

		for (;;) {
			Node *p = ptr_from(head);
			if (!p) {
				return aligned_alloc_block();
			}

			uint64_t next;
			next = pack(p->next, (tag_from(head) + 1) & 0xFULL);

			if (head_.compare_exchange_weak(
					head, next,
					std::memory_order_acq_rel,
					std::memory_order_acquire)) {
				return (void *)p;
			}
		}
	}

	void free(void *mem)
	{
		if (!mem) {
			return;
		}

		Node *n = (Node *)mem;
		uint64_t head = head_.load(std::memory_order_acquire);

		for (;;) {
			n->next = ptr_from(head);

			uint64_t next;
			next = pack(n, (tag_from(head) + 1) & 0xFULL);

			if (head_.compare_exchange_weak(
					head, next,
					std::memory_order_acq_rel,
					std::memory_order_acquire)) {
				return;
			}
		}
	}

	~TaggedFreeList() = default;

private:
	struct Node {
		Node *next;
	};

	std::atomic<uint64_t> head_;
	size_t block_size_;
	size_t align_;

	static inline uint64_t pack(Node *p, uint64_t tag4)
	{
		uintptr_t up = (uintptr_t)p;
		return (uint64_t)(up | (uintptr_t)(tag4 & 0xFULL));
	}

	static inline Node *ptr_from(uint64_t v)
	{
		return (Node *)(uintptr_t)(v & ~0xFULL);
	}

	static inline uint64_t tag_from(uint64_t v)
	{
		return v & 0xFULL;
	}

	static inline size_t round_up(size_t x, size_t a)
	{
		return (x + a - 1) & ~(a - 1);
	}

	void *aligned_alloc_block(void)
	{
		size_t sz = round_up(block_size_, align_);
		void *p = nullptr;

#if (__cplusplus >= 201703L)
		p = std::aligned_alloc(align_, sz);
#else
		if (posix_memalign(&p, align_, sz) != 0) {
			p = nullptr;
		}
#endif
		if (!p) {
			throw std::bad_alloc();
		}

		std::memset(p, 0, block_size_);
		return p;
	}
};

