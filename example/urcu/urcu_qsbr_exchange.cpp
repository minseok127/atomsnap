#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>
#include <barrier>
#include <iomanip>
#include <cstring>

#include <urcu-qsbr.h>
#include <urcu/uatomic.h>

std::atomic<size_t> total_writer_ops{0};
std::atomic<size_t> total_reader_ops{0};
int duration_seconds = 0;

struct Data {
    int64_t value1;
    int64_t value2;
};

Data *global_ptr = nullptr;

void writer(std::barrier<> &sync) {
    rcu_register_thread();
    sync.arrive_and_wait();
    
    auto start = std::chrono::steady_clock::now();
    size_t ops = 0;

    while (true) {
        auto now = std::chrono::steady_clock::now();
        int sec = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
        if (sec >= duration_seconds) {
            break;
        }

        Data *old_data = CMM_LOAD_SHARED(global_ptr);
        Data *new_data = new Data;
        if (old_data) {
            new_data->value1 = old_data->value1 + 1;
            new_data->value2 = old_data->value2 + 1;
        } else {
            new_data->value1 = 0;
            new_data->value2 = 0;
        }

        Data *reclaimed_data = (Data *)uatomic_xchg(&global_ptr, new_data);

        synchronize_rcu();

        if (reclaimed_data) {
            delete reclaimed_data;
        }

        ops++;
    }

    rcu_unregister_thread();
    total_writer_ops.fetch_add(ops, std::memory_order_relaxed);
}

void reader(std::barrier<> &sync) {
    rcu_register_thread();
    sync.arrive_and_wait();
    
    auto start = std::chrono::steady_clock::now();
    size_t ops = 0;

    while (true) {
        auto now = std::chrono::steady_clock::now();
        int sec = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
        if (sec >= duration_seconds) {
            break;
        }

        rcu_read_lock();

        Data *current_data = rcu_dereference(global_ptr);
        if (current_data) {
            if (current_data->value1 != current_data->value2) {
                fprintf(stderr, "Invalid data, value1: %ld, value2: %ld\n",
                        current_data->value1, current_data->value2);
                exit(1);
            }
        }

        rcu_read_unlock();

        rcu_quiescent_state();

        ops++;
    }

    rcu_unregister_thread();
    total_reader_ops.fetch_add(ops, std::memory_order_relaxed);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <writer_count> <reader_count> <duration_seconds>\n";
        return -1;
    }

    int writer_count = std::atoi(argv[1]);
    int reader_count = std::atoi(argv[2]);
    duration_seconds = std::atoi(argv[3]);

    if (writer_count <= 0 || reader_count <= 0 || duration_seconds < 0) {
        std::cerr << "Invalid arguments\n";
        return -1;
    }

    global_ptr = new Data{0, 0};

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
              << total_writer_ops.load(std::memory_order_relaxed) / static_cast<double>(duration_seconds)
              << " ops/sec\n";
    std::cout << "Total reader throughput: "
              << total_reader_ops.load(std::memory_order_relaxed) / static_cast<double>(duration_seconds)
              << " ops/sec\n";

    return 0;
}
