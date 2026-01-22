/* bench_all.cpp */
#include <barrier>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "lf_pool.hpp"

extern "C" {
#include "atomsnap.h"
}

#include <urcu/urcu-memb.h>
#include <urcu/uatomic.h>

struct Config {
	std::string backend;
	std::string reclaim;

	int readers;
	int writers;
	int duration_sec;

	int shards;
	bool pin;
	int pin_base;

	uint64_t cs_ns;
	size_t payload_bytes;
	uint64_t updates_per_sec;
	uint32_t sync_batch;

	uint32_t sample_pow2;
	bool csv;

	Config()
		: backend("urcu"),
		  reclaim("async"),
		  readers(1),
		  writers(1),
		  duration_sec(5),
		  shards(1),
		  pin(false),
		  pin_base(0),
		  cs_ns(0),
		  payload_bytes(0),
		  updates_per_sec(0),
		  sync_batch(1024),
		  sample_pow2(0),
		  csv(false)
	{}
};

static void usage(const char *argv0)
{
	std::cerr
		<< "Usage: " << argv0 << " [options]\n"
		<< "  --backend=urcu|atomsnap\n"
		<< "  --readers=N --writers=N --duration=SEC\n"
		<< "  --cs-ns=NS --payload=BYTES\n"
		<< "  --updates-per-sec=U (0=unlimited)\n"
		<< "  --shards=N\n"
		<< "  --reclaim=async|sync-batch (urcu)\n"
		<< "  --sync-batch=N (urcu)\n"
		<< "  --pin=0|1 --pin-base-cpu=N\n"
		<< "  --sample-pow2=K (0=off)\n"
		<< "  --csv=0|1\n";
}

static bool starts_with(const char *s, const char *p)
{
	size_t n = std::strlen(p);
	return std::strncmp(s, p, n) == 0;
}

static uint64_t parse_u64(const char *v)
{
	return (uint64_t)std::strtoull(v, nullptr, 10);
}

static size_t parse_sz(const char *v)
{
	return (size_t)std::strtoull(v, nullptr, 10);
}

static int parse_i(const char *v)
{
	return std::atoi(v);
}

static bool parse_args(int argc, char **argv, Config &c)
{
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];

		auto getv = [&](const char *k) -> const char * {
			size_t n = std::strlen(k);
			if (!starts_with(a, k)) {
				return nullptr;
			}
			if (a[n] != '=') {
				return nullptr;
			}
			return a + n + 1;
		};

		const char *v;

		if ((v = getv("--backend"))) {
			c.backend = v;
		} else if ((v = getv("--reclaim"))) {
			c.reclaim = v;
		} else if ((v = getv("--readers"))) {
			c.readers = parse_i(v);
		} else if ((v = getv("--writers"))) {
			c.writers = parse_i(v);
		} else if ((v = getv("--duration"))) {
			c.duration_sec = parse_i(v);
		} else if ((v = getv("--cs-ns"))) {
			c.cs_ns = parse_u64(v);
		} else if ((v = getv("--payload"))) {
			c.payload_bytes = parse_sz(v);
		} else if ((v = getv("--updates-per-sec"))) {
			c.updates_per_sec = parse_u64(v);
		} else if ((v = getv("--sync-batch"))) {
			c.sync_batch = (uint32_t)parse_u64(v);
		} else if ((v = getv("--shards"))) {
			c.shards = parse_i(v);
		} else if ((v = getv("--pin"))) {
			c.pin = (parse_i(v) != 0);
		} else if ((v = getv("--pin-base-cpu"))) {
			c.pin_base = parse_i(v);
		} else if ((v = getv("--sample-pow2"))) {
			c.sample_pow2 = (uint32_t)parse_u64(v);
		} else if ((v = getv("--csv"))) {
			c.csv = (parse_i(v) != 0);
		} else {
			std::cerr << "Unknown arg: " << a << "\n";
			return false;
		}
	}

	if (c.readers <= 0 || c.writers <= 0 || c.duration_sec <= 0) {
		return false;
	}
	if (c.shards <= 0) {
		return false;
	}
	if (c.backend != "urcu" && c.backend != "atomsnap") {
		return false;
	}
	if (c.backend == "urcu") {
		if (c.reclaim != "async" && c.reclaim != "sync-batch") {
			return false;
		}
	}
	return true;
}

struct LatencyStats {
	std::atomic<uint64_t> samples;
	std::atomic<uint64_t> sum_ns;
	std::atomic<uint64_t> max_ns;

	LatencyStats()
		: samples(0),
		  sum_ns(0),
		  max_ns(0)
	{}

	void add(uint64_t ns)
	{
		samples.fetch_add(1, std::memory_order_relaxed);
		sum_ns.fetch_add(ns, std::memory_order_relaxed);

		uint64_t cur = max_ns.load(std::memory_order_relaxed);
		while (ns > cur) {
			if (max_ns.compare_exchange_weak(
					cur, ns,
					std::memory_order_relaxed,
					std::memory_order_relaxed)) {
				break;
			}
		}
	}
};

struct Results {
	double r_ops_s;
	double w_ops_s;
	long peak_rss_kb;

	uint64_t pending;
	uint64_t freed;

	uint64_t lat_samples;
	double lat_avg_ns;
	uint64_t lat_max_ns;

	Results()
		: r_ops_s(0.0),
		  w_ops_s(0.0),
		  peak_rss_kb(0),
		  pending(0),
		  freed(0),
		  lat_samples(0),
		  lat_avg_ns(0.0),
		  lat_max_ns(0)
	{}
};

static inline void payload_touch(volatile uint8_t *p, size_t n)
{
	const size_t stride = 64;
	uint8_t acc = 0;

	for (size_t i = 0; i < n; i += stride) {
		acc ^= p[i];
	}
	if (n) {
		acc ^= p[n - 1];
	}
	(void)acc;
}

struct Backend {
	virtual ~Backend() = default;

	virtual void init(const Config &cfg) = 0;
	virtual void stop(void) = 0;

	virtual void reader_loop(
		int rid,
		std::barrier<> &br,
		const CsBurner &burner,
		std::atomic<bool> &running,
		std::atomic<uint64_t> &rops,
		LatencyStats &lat) = 0;

	virtual void writer_loop(
		int wid,
		std::barrier<> &br,
		std::atomic<bool> &running,
		std::atomic<uint64_t> &wops) = 0;

	virtual Results finalize(
		const Config &cfg,
		const std::atomic<uint64_t> &rops,
		const std::atomic<uint64_t> &wops,
		const LatencyStats &lat) = 0;
};

/* ---------------- URCU backend ---------------- */

struct UrcuObj {
	uint64_t v1;
	uint64_t v2;
	struct rcu_head rcu;

	TaggedFreeList *pool;
	std::atomic<uint64_t> *pending;
};

static inline volatile uint8_t *urcu_payload_ptr(UrcuObj *o)
{
	return (volatile uint8_t *)((uint8_t *)o + sizeof(UrcuObj));
}

static void urcu_free_cb(struct rcu_head *head)
{
	UrcuObj *o = caa_container_of(head, UrcuObj, rcu);

	o->pending->fetch_sub(1, std::memory_order_relaxed);
	o->pool->free(o);
}

struct UrcuBackend : Backend {
	Config cfg;
	TaggedFreeList *pool;

	std::vector<void *> gptrs;

	std::atomic<uint64_t> pending;
	std::atomic<uint64_t> freed;

	struct RetireList {
		std::vector<void *> v;
	};
	std::vector<RetireList> retire;

	UrcuBackend()
		: pool(nullptr),
		  pending(0),
		  freed(0)
	{}

	void init(const Config &c) override
	{
		cfg = c;

		size_t block;
		block = sizeof(UrcuObj) + cfg.payload_bytes;

		pool = new TaggedFreeList(block, 64);

		gptrs.assign((size_t)cfg.shards, nullptr);
		retire.resize((size_t)cfg.writers);

		for (int s = 0; s < cfg.shards; s++) {
			void *mem = pool->alloc();
			UrcuObj *o = (UrcuObj *)mem;

			o->v1 = 0;
			o->v2 = 0;
			o->pool = pool;
			o->pending = &pending;

			if (cfg.payload_bytes) {
				uint8_t *p;
				p = (uint8_t *)urcu_payload_ptr(o);
				p[0] = 0;
				p[cfg.payload_bytes - 1] = 0;
			}

			uatomic_set(&gptrs[(size_t)s], mem);
		}
	}

	void stop(void) override
	{
		rcu_barrier_memb();

		for (size_t s = 0; s < gptrs.size(); s++) {
			void *p = uatomic_xchg(&gptrs[s], nullptr);
			if (p) {
				pool->free(p);
			}
		}

		delete pool;
		pool = nullptr;
	}

	void reader_loop(
		int rid,
		std::barrier<> &br,
		const CsBurner &burner,
		std::atomic<bool> &running,
		std::atomic<uint64_t> &rops,
		LatencyStats &lat) override
	{
		rcu_register_thread_memb();

		if (cfg.pin) {
			pin_thread_to_cpu(cfg.pin_base + rid);
		}

		int shard = rid % cfg.shards;

		uint32_t mask = 0;
		if (cfg.sample_pow2) {
			mask = (1u << cfg.sample_pow2) - 1u;
		}
		uint32_t ctr = 0;

		br.arrive_and_wait();

		while (running.load(std::memory_order_relaxed)) {
			bool sample = (mask != 0) && ((ctr++ & mask) == 0);
			uint64_t t0 = 0;

			if (sample) {
				t0 = now_ns();
			}

			rcu_read_lock_memb();

			void *p;
			p = (void *)rcu_dereference(gptrs[(size_t)shard]);

			UrcuObj *o = (UrcuObj *)p;
			if (o) {
				if (o->v1 != o->v2) {
					std::fprintf(stderr,
						"URCU mismatch: %" PRIu64
						" != %" PRIu64 "\n",
						o->v1, o->v2);
					std::abort();
				}

				volatile uint8_t *pl;
				pl = urcu_payload_ptr(o);

				payload_touch(pl, cfg.payload_bytes);
				burner.burn_ns(cfg.cs_ns);
			}

			rcu_read_unlock_memb();

			if (sample) {
				lat.add(now_ns() - t0);
			}

			rops.fetch_add(1, std::memory_order_relaxed);
		}

		rcu_unregister_thread_memb();
	}

	void writer_loop(
		int wid,
		std::barrier<> &br,
		std::atomic<bool> &running,
		std::atomic<uint64_t> &wops) override
	{
		rcu_register_thread_memb();

		if (cfg.pin) {
			int cpu = cfg.pin_base + cfg.readers + wid;
			pin_thread_to_cpu(cpu);
		}

		br.arrive_and_wait();

		uint64_t interval = 0;
		if (cfg.updates_per_sec) {
			interval = 1000000000ULL / cfg.updates_per_sec;
		}

		uint64_t next_tick = now_ns();
		uint64_t seq = 0;

		int shard = wid % cfg.shards;

		RetireList &rl = retire[(size_t)wid];
		rl.v.reserve((size_t)cfg.sync_batch * 2);

		while (running.load(std::memory_order_relaxed)) {
			if (interval) {
				uint64_t t = now_ns();
				if (t < next_tick) {
					std::this_thread::yield();
					continue;
				}
				next_tick += interval;
			}

			void *mem = pool->alloc();
			UrcuObj *o = (UrcuObj *)mem;

			seq++;
			o->v1 = seq;
			o->v2 = seq;
			o->pool = pool;
			o->pending = &pending;

			if (cfg.payload_bytes) {
				uint8_t *pl;
				pl = (uint8_t *)urcu_payload_ptr(o);

				pl[0] = (uint8_t)seq;
				pl[cfg.payload_bytes - 1] =
					(uint8_t)(seq >> 8);
			}

			void *old;
			old = uatomic_xchg(&gptrs[(size_t)shard], mem);

			if (old) {
				if (cfg.reclaim == "async") {
					pending.fetch_add(
						1, std::memory_order_relaxed);

					UrcuObj *oo = (UrcuObj *)old;
					call_rcu_memb(&oo->rcu, urcu_free_cb);
				} else {
					rl.v.push_back(old);
					if (rl.v.size() >= cfg.sync_batch) {
						synchronize_rcu_memb();
						for (void *x : rl.v) {
							pool->free(x);
							freed.fetch_add(
								1,
								std::memory_order_relaxed);
						}
						rl.v.clear();
					}
				}
			}

			shard++;
			if (shard >= cfg.shards) {
				shard = 0;
			}

			wops.fetch_add(1, std::memory_order_relaxed);
		}

		if (cfg.reclaim == "sync-batch" && !rl.v.empty()) {
			synchronize_rcu_memb();
			for (void *x : rl.v) {
				pool->free(x);
				freed.fetch_add(1, std::memory_order_relaxed);
			}
			rl.v.clear();
		}

		rcu_unregister_thread_memb();
	}

	Results finalize(
		const Config &c,
		const std::atomic<uint64_t> &rops,
		const std::atomic<uint64_t> &wops,
		const LatencyStats &lat) override
	{
		Results r;

		double dur = (double)c.duration_sec;

		r.r_ops_s = (double)rops.load(std::memory_order_relaxed) / dur;
		r.w_ops_s = (double)wops.load(std::memory_order_relaxed) / dur;

		r.peak_rss_kb = get_peak_rss_kb();

		r.pending = pending.load(std::memory_order_relaxed);
		r.freed = freed.load(std::memory_order_relaxed);

		r.lat_samples = lat.samples.load(std::memory_order_relaxed);

		uint64_t sum = lat.sum_ns.load(std::memory_order_relaxed);
		if (r.lat_samples) {
			r.lat_avg_ns = (double)sum / (double)r.lat_samples;
		}
		r.lat_max_ns = lat.max_ns.load(std::memory_order_relaxed);

		return r;
	}
};

/* ---------------- atomsnap backend ---------------- */

struct AtomObj {
	uint64_t v1;
	uint64_t v2;
};

static inline volatile uint8_t *atom_payload_ptr(AtomObj *o)
{
	return (volatile uint8_t *)((uint8_t *)o + sizeof(AtomObj));
}

static std::atomic<uint64_t> g_atomsnap_freed(0);

static void atomsnap_free_cb(void *obj, void *ctx)
{
	TaggedFreeList *pool = (TaggedFreeList *)ctx;

	if (obj) {
		pool->free(obj);
		g_atomsnap_freed.fetch_add(1, std::memory_order_relaxed);
	}
}

struct AtomSnapBackend : Backend {
	Config cfg;
	TaggedFreeList *pool;
	std::vector<atomsnap_gate *> gates;

	std::atomic<uint64_t> created;

	AtomSnapBackend()
		: pool(nullptr),
		  created(0)
	{}

	void init(const Config &c) override
	{
		cfg = c;

		size_t block;
		block = sizeof(AtomObj) + cfg.payload_bytes;

		pool = new TaggedFreeList(block, 64);

		gates.resize((size_t)cfg.shards);

		for (int s = 0; s < cfg.shards; s++) {
			struct atomsnap_init_context ictx;
			std::memset(&ictx, 0, sizeof(ictx));

			ictx.free_impl = atomsnap_free_func(atomsnap_free_cb);
			ictx.num_extra_control_blocks = 0;

			gates[(size_t)s] = atomsnap_init_gate(&ictx);

			atomsnap_version *ver;
			ver = atomsnap_make_version(gates[(size_t)s]);

			void *obj = pool->alloc();
			AtomObj *o = (AtomObj *)obj;
			o->v1 = 0;
			o->v2 = 0;

			if (cfg.payload_bytes) {
				uint8_t *p = (uint8_t *)atom_payload_ptr(o);
				p[0] = 0;
				p[cfg.payload_bytes - 1] = 0;
			}

			atomsnap_set_object(ver, obj, pool);
			atomsnap_exchange_version_slot(
				gates[(size_t)s], 0, ver);
		}
	}

	void stop(void) override
	{
		for (atomsnap_gate *g : gates) {
			atomsnap_destroy_gate(g);
		}
		gates.clear();

		delete pool;
		pool = nullptr;
	}

	void reader_loop(
		int rid,
		std::barrier<> &br,
		const CsBurner &burner,
		std::atomic<bool> &running,
		std::atomic<uint64_t> &rops,
		LatencyStats &lat) override
	{
		if (cfg.pin) {
			pin_thread_to_cpu(cfg.pin_base + rid);
		}

		int shard = rid % cfg.shards;

		uint32_t mask = 0;
		if (cfg.sample_pow2) {
			mask = (1u << cfg.sample_pow2) - 1u;
		}
		uint32_t ctr = 0;

		br.arrive_and_wait();

		while (running.load(std::memory_order_relaxed)) {
			bool sample = (mask != 0) && ((ctr++ & mask) == 0);
			uint64_t t0 = 0;

			if (sample) {
				t0 = now_ns();
			}

			atomsnap_version *ver;
			ver = atomsnap_acquire_version_slot(
				gates[(size_t)shard], 0);

			if (ver) {
				void *obj = atomsnap_get_object(ver);
				AtomObj *o = (AtomObj *)obj;

				if (o) {
					if (o->v1 != o->v2) {
						std::fprintf(stderr,
							"ATOM mismatch: %" PRIu64
							" != %" PRIu64 "\n",
							o->v1, o->v2);
						std::abort();
					}

					volatile uint8_t *pl;
					pl = atom_payload_ptr(o);

					payload_touch(pl, cfg.payload_bytes);
					burner.burn_ns(cfg.cs_ns);
				}

				atomsnap_release_version(ver);
			}

			if (sample) {
				lat.add(now_ns() - t0);
			}

			rops.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void writer_loop(
		int wid,
		std::barrier<> &br,
		std::atomic<bool> &running,
		std::atomic<uint64_t> &wops) override
	{
		if (cfg.pin) {
			int cpu = cfg.pin_base + cfg.readers + wid;
			pin_thread_to_cpu(cpu);
		}

		br.arrive_and_wait();

		uint64_t interval = 0;
		if (cfg.updates_per_sec) {
			interval = 1000000000ULL / cfg.updates_per_sec;
		}

		uint64_t next_tick = now_ns();
		uint64_t seq = 0;

		int shard = wid % cfg.shards;

		while (running.load(std::memory_order_relaxed)) {
			if (interval) {
				uint64_t t = now_ns();
				if (t < next_tick) {
					std::this_thread::yield();
					continue;
				}
				next_tick += interval;
			}

			atomsnap_gate *g = gates[(size_t)shard];

			atomsnap_version *ver;
			ver = atomsnap_make_version(g);

			void *obj = pool->alloc();
			AtomObj *o = (AtomObj *)obj;

			seq++;
			o->v1 = seq;
			o->v2 = seq;

			if (cfg.payload_bytes) {
				uint8_t *pl;
				pl = (uint8_t *)atom_payload_ptr(o);

				pl[0] = (uint8_t)seq;
				pl[cfg.payload_bytes - 1] =
					(uint8_t)(seq >> 8);
			}

			atomsnap_set_object(ver, obj, pool);
			atomsnap_exchange_version_slot(g, 0, ver);

			created.fetch_add(1, std::memory_order_relaxed);

			shard++;
			if (shard >= cfg.shards) {
				shard = 0;
			}

			wops.fetch_add(1, std::memory_order_relaxed);
		}
	}

	Results finalize(
		const Config &c,
		const std::atomic<uint64_t> &rops,
		const std::atomic<uint64_t> &wops,
		const LatencyStats &lat) override
	{
		Results r;

		double dur = (double)c.duration_sec;

		r.r_ops_s = (double)rops.load(std::memory_order_relaxed) / dur;
		r.w_ops_s = (double)wops.load(std::memory_order_relaxed) / dur;

		r.peak_rss_kb = get_peak_rss_kb();

		r.pending = 0;
		r.freed = g_atomsnap_freed.load(std::memory_order_relaxed);

		r.lat_samples = lat.samples.load(std::memory_order_relaxed);

		uint64_t sum = lat.sum_ns.load(std::memory_order_relaxed);
		if (r.lat_samples) {
			r.lat_avg_ns = (double)sum / (double)r.lat_samples;
		}
		r.lat_max_ns = lat.max_ns.load(std::memory_order_relaxed);

		return r;
	}
};

static void print_csv_header(void)
{
	std::cout
		<< "backend,readers,writers,duration,cs_ns,payload,"
		<< "updates_per_sec,shards,reclaim,sync_batch,"
		<< "r_ops_s,w_ops_s,peak_rss_kb,pending,freed,"
		<< "lat_samples,lat_avg_ns,lat_max_ns\n";
}

static void print_csv_line(const Config &c, const Results &r)
{
	std::cout
		<< c.backend << ","
		<< c.readers << ","
		<< c.writers << ","
		<< c.duration_sec << ","
		<< c.cs_ns << ","
		<< c.payload_bytes << ","
		<< c.updates_per_sec << ","
		<< c.shards << ","
		<< c.reclaim << ","
		<< c.sync_batch << ","
		<< std::fixed << std::setprecision(2)
		<< r.r_ops_s << ","
		<< r.w_ops_s << ","
		<< r.peak_rss_kb << ","
		<< r.pending << ","
		<< r.freed << ","
		<< r.lat_samples << ","
		<< std::setprecision(2) << r.lat_avg_ns << ","
		<< r.lat_max_ns
		<< "\n";
}

static void print_human(const Config &c, const Results &r)
{
	std::cout << "Backend         : " << c.backend << "\n";
	std::cout << "Readers/Writers : " << c.readers
		<< " / " << c.writers << "\n";
	std::cout << "Duration (s)    : " << c.duration_sec << "\n";
	std::cout << "CS (ns)         : " << c.cs_ns << "\n";
	std::cout << "Payload (B)     : " << c.payload_bytes << "\n";
	std::cout << "Updates/sec     : " << c.updates_per_sec << "\n";
	std::cout << "Shards          : " << c.shards << "\n";
	if (c.backend == "urcu") {
		std::cout << "Reclaim         : " << c.reclaim << "\n";
		if (c.reclaim == "sync-batch") {
			std::cout << "Sync batch      : "
				<< c.sync_batch << "\n";
		}
	}
	std::cout << "Reader ops/s    : " << r.r_ops_s << "\n";
	std::cout << "Writer ops/s    : " << r.w_ops_s << "\n";
	std::cout << "Peak RSS (KB)   : " << r.peak_rss_kb << "\n";
	std::cout << "Pending         : " << r.pending << "\n";
	std::cout << "Freed           : " << r.freed << "\n";
	std::cout << "Lat samples     : " << r.lat_samples << "\n";
	std::cout << "Lat avg (ns)    : " << r.lat_avg_ns << "\n";
	std::cout << "Lat max (ns)    : " << r.lat_max_ns << "\n";
}

int main(int argc, char **argv)
{
	Config cfg;

	if (!parse_args(argc, argv, cfg)) {
		usage(argv[0]);
		return 1;
	}

	CsBurner burner;
	burner.calibrate();

	std::unique_ptr<Backend> be;

	if (cfg.backend == "urcu") {
		be.reset(new UrcuBackend());
	} else {
		be.reset(new AtomSnapBackend());
	}

	be->init(cfg);

	std::atomic<bool> running(true);
	std::atomic<uint64_t> rops(0);
	std::atomic<uint64_t> wops(0);
	LatencyStats lat;

	int total = cfg.readers + cfg.writers + 1;
	std::barrier sync(total);

	std::vector<std::thread> th;
	th.reserve((size_t)total);

	for (int i = 0; i < cfg.writers; i++) {
		th.emplace_back([&, i] {
			be->writer_loop(i, sync, running, wops);
		});
	}

	for (int i = 0; i < cfg.readers; i++) {
		th.emplace_back([&, i] {
			be->reader_loop(i, sync, burner, running, rops, lat);
		});
	}

	th.emplace_back([&] {
		if (cfg.pin) {
			int cpu = cfg.pin_base + cfg.readers + cfg.writers;
			pin_thread_to_cpu(cpu);
		}

		sync.arrive_and_wait();

		auto start = std::chrono::steady_clock::now();

		for (;;) {
			auto now = std::chrono::steady_clock::now();
			auto sec = std::chrono::duration_cast<
				std::chrono::seconds>(now - start).count();

			if (sec >= cfg.duration_sec) {
				running.store(false, std::memory_order_relaxed);
				break;
			}
			std::this_thread::sleep_for(
				std::chrono::milliseconds(10));
		}
	});

	for (auto &t : th) {
		t.join();
	}

	be->stop();

	Results r = be->finalize(cfg, rops, wops, lat);

	if (cfg.csv) {
		print_csv_header();
		print_csv_line(cfg, r);
	} else {
		print_human(cfg, r);
	}

	return 0;
}

