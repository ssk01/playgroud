/**
 * HashMap vs Flat Array — Cache-Friendly Lookup Demo
 *
 * Simulates package dependency resolution (like Bun's lockfile):
 *   - HashMap approach: std::unordered_map<string, Package> (npm/yarn style)
 *   - Flat array approach: vector<Package> indexed by integer ID (Bun style)
 *
 * All three tests use 100,000 operations for fair comparison.
 * Output shows both total time and per-operation cost (ns/op).
 *
 * Expected: sequential < random < chain (per-op), because:
 *   - Sequential: prefetcher-friendly, pipelined
 *   - Random: independent accesses but cache-unfriendly
 *   - Chain: serial dependency, CPU must wait for each load
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>

static constexpr size_t NUM_PACKAGES = 100'000;
static constexpr size_t NUM_OPS      = 100'000;  // all tests use the same count
static constexpr int    NUM_ROUNDS   = 200;

struct Package {
    uint32_t name_offset;
    uint8_t  version_major;
    uint8_t  version_minor;
    uint8_t  version_patch;
    uint8_t  num_deps;
    uint64_t integrity_hash;
    uint32_t tarball_size;
    uint32_t dep_ids[8];
};

using Clock = std::chrono::high_resolution_clock;
using ns    = std::chrono::nanoseconds;

template <typename T>
static void do_not_optimize(T const& val) {
    asm volatile("" : : "r,m"(val) : "memory");
}

static double median_ns(std::vector<int64_t>& samples) {
    std::sort(samples.begin(), samples.end());
    size_t n = samples.size();
    if (n % 2 == 0)
        return (samples[n/2 - 1] + samples[n/2]) / 2.0;
    return samples[n/2];
}

static std::vector<std::string> generate_names(size_t count) {
    std::vector<std::string> names(count);
    for (size_t i = 0; i < count; ++i) {
        char buf[64];
        snprintf(buf, sizeof(buf), "@scope/package-%05zu", i);
        names[i] = buf;
    }
    return names;
}

static void fill_packages(std::vector<Package>& pkgs) {
    pkgs.resize(NUM_PACKAGES);
    std::mt19937 rng(42);

    // Build a single Hamiltonian cycle for dep_ids[0]:
    // Shuffle [0..N-1] into a random order, then link them as a cycle:
    //   perm[0] → perm[1] → ... → perm[N-1] → perm[0]
    // This guarantees the chain visits ALL N packages exactly once.
    std::vector<uint32_t> perm(NUM_PACKAGES);
    std::iota(perm.begin(), perm.end(), 0);
    std::shuffle(perm.begin(), perm.end(), rng);

    for (size_t i = 0; i < NUM_PACKAGES; ++i) {
        pkgs[i].name_offset    = static_cast<uint32_t>(i);
        pkgs[i].version_major  = rng() & 0xF;
        pkgs[i].version_minor  = rng() & 0xF;
        pkgs[i].version_patch  = rng() & 0xF;
        pkgs[i].num_deps       = std::min<uint8_t>(rng() & 0x7, 8);
        pkgs[i].integrity_hash = static_cast<uint64_t>(rng()) * 6364136223846793005ULL;
        pkgs[i].tarball_size   = rng() & 0xFFFF;
        for (int d = 0; d < 8; ++d)
            pkgs[i].dep_ids[d] = rng() % NUM_PACKAGES;
    }
    // Wire dep_ids[0] as a single cycle
    for (size_t i = 0; i < NUM_PACKAGES; ++i)
        pkgs[perm[i]].dep_ids[0] = perm[(i + 1) % NUM_PACKAGES];
}

static std::unordered_map<std::string, Package>
build_hashmap(const std::vector<Package>& pkgs, const std::vector<std::string>& names) {
    std::unordered_map<std::string, Package> map;
    map.reserve(pkgs.size());
    for (size_t i = 0; i < pkgs.size(); ++i)
        map[names[i]] = pkgs[i];
    return map;
}

static std::vector<uint32_t> random_indices(size_t count, uint32_t seed) {
    std::vector<uint32_t> indices(count);
    for (size_t i = 0; i < count; ++i) {
        seed = seed * 1103515245 + 12345;
        indices[i] = seed % NUM_PACKAGES;
    }
    return indices;
}

// ============================================================
// Benchmark 1: Sequential traversal (100K ops)
//   CPU prefetcher can predict the next address → very fast
// ============================================================
static int64_t bench_seq_flat(const std::vector<Package>& pkgs) {
    auto t0 = Clock::now();
    uint64_t total = 0;
    for (size_t i = 0; i < NUM_OPS; ++i)
        total += pkgs[i].tarball_size;
    do_not_optimize(total);
    return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
}

static int64_t bench_seq_hashmap(const std::unordered_map<std::string, Package>& map) {
    auto t0 = Clock::now();
    uint64_t total = 0;
    for (auto& [k, v] : map)
        total += v.tarball_size;
    do_not_optimize(total);
    return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
}

// ============================================================
// Benchmark 2: Random lookups (100K ops)
//   Each access jumps to a random index → cache-unfriendly
//   But loads are independent → CPU can issue multiple in parallel
// ============================================================
static int64_t bench_rand_flat(const std::vector<Package>& pkgs,
                               const std::vector<uint32_t>& indices) {
    auto t0 = Clock::now();
    uint64_t acc = 0;
    for (size_t i = 0; i < NUM_OPS; ++i)
        acc += pkgs[indices[i]].tarball_size;
    do_not_optimize(acc);
    return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
}

static int64_t bench_rand_hashmap(const std::unordered_map<std::string, Package>& map,
                                   const std::vector<std::string>& names,
                                   const std::vector<uint32_t>& indices) {
    auto t0 = Clock::now();
    uint64_t acc = 0;
    for (size_t i = 0; i < NUM_OPS; ++i) {
        auto it = map.find(names[indices[i]]);
        if (it != map.end())
            acc += it->second.tarball_size;
    }
    do_not_optimize(acc);
    return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
}

// ============================================================
// Benchmark 3: Dependency chain walk (100K ops)
//   Serial dependency: must finish loading pkg[cur] before knowing
//   the next index → CPU CANNOT issue loads in parallel
//   This is the worst case for memory latency
// ============================================================
static int64_t bench_chain_flat(const std::vector<Package>& pkgs) {
    auto t0 = Clock::now();
    uint64_t acc = 0;
    uint32_t cur = 0;
    for (size_t step = 0; step < NUM_OPS; ++step) {
        acc += pkgs[cur].tarball_size;
        cur = pkgs[cur].dep_ids[0] % pkgs.size();
    }
    do_not_optimize(acc);
    return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
}

static int64_t bench_chain_hashmap(const std::unordered_map<std::string, Package>& map,
                                    const std::vector<std::string>& names) {
    auto t0 = Clock::now();
    uint64_t acc = 0;
    uint32_t cur = 0;
    for (size_t step = 0; step < NUM_OPS; ++step) {
        auto it = map.find(names[cur]);
        if (it != map.end()) {
            acc += it->second.tarball_size;
            cur = it->second.dep_ids[0] % NUM_PACKAGES;
        }
    }
    do_not_optimize(acc);
    return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
}

// ============================================================
// Benchmark 4: Serial Random (100K ops) — CONTROL EXPERIMENT
//   Same serial dependency as chain walk, but uses pre-generated
//   random indices instead of dep_ids.
//   This isolates: is chain's advantage from serialization or locality?
// ============================================================
static int64_t bench_serial_rand_flat(const std::vector<Package>& pkgs,
                                      const std::vector<uint32_t>& indices) {
    auto t0 = Clock::now();
    uint64_t acc = 0;
    uint32_t cur = 0;
    for (size_t i = 0; i < NUM_OPS; ++i) {
        cur = indices[(cur + i) % NUM_OPS]; // serial dep: next index depends on cur
        acc += pkgs[cur].tarball_size;
    }
    do_not_optimize(acc);
    return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
}

static int64_t bench_serial_rand_hashmap(const std::unordered_map<std::string, Package>& map,
                                          const std::vector<std::string>& names,
                                          const std::vector<uint32_t>& indices) {
    auto t0 = Clock::now();
    uint64_t acc = 0;
    uint32_t cur = 0;
    for (size_t i = 0; i < NUM_OPS; ++i) {
        cur = indices[(cur + i) % NUM_OPS];
        auto it = map.find(names[cur]);
        if (it != map.end())
            acc += it->second.tarball_size;
    }
    do_not_optimize(acc);
    return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
}

// ============================================================

static void print_result(const char* name, size_t ops,
                         std::vector<int64_t>& t_flat,
                         std::vector<int64_t>& t_map) {
    double mf = median_ns(t_flat), mm = median_ns(t_map);
    printf("  %-28s  Flat: %7.1f us (%4.1f ns/op)   HashMap: %7.1f us (%5.1f ns/op)   speedup: %.1fx\n",
           name, mf/1000, mf/ops, mm/1000, mm/ops, mm/mf);
}

static void print_single(const char* name, size_t ops, std::vector<int64_t>& times) {
    double m = median_ns(times);
    printf("  %-40s  %7.1f us (%5.1f ns/op)\n", name, m/1000, m/ops);
}

int main() {
    printf("=== HashMap vs Flat Array Benchmark ===\n");
    printf("  Packages: %zu    Ops per test: %zu    Rounds: %d\n\n",
           NUM_PACKAGES, NUM_OPS, NUM_ROUNDS);

    std::vector<Package> flat;
    fill_packages(flat);
    auto names = generate_names(NUM_PACKAGES);
    auto map = build_hashmap(flat, names);
    auto rand_idx = random_indices(NUM_OPS, 12345);

    printf("  Flat array memory: %.1f MB (contiguous)\n",
           flat.size() * sizeof(Package) / (1024.0 * 1024.0));
    printf("  HashMap nodes: scattered across heap\n\n");

    // --- Analyze chain walk locality ---
    {
        std::vector<bool> visited(NUM_PACKAGES, false);
        uint32_t cur = 0;
        size_t unique = 0;
        for (size_t step = 0; step < NUM_OPS; ++step) {
            if (!visited[cur]) { visited[cur] = true; ++unique; }
            cur = flat[cur].dep_ids[0] % NUM_PACKAGES;
        }
        printf("  Chain walk unique packages visited: %zu / %zu (%.1f%%)\n\n",
               unique, NUM_OPS, 100.0 * unique / NUM_OPS);
    }

    // --- Main benchmarks ---
    printf("  --- Main comparison (all 100K ops) ---\n\n");

    // Sequential
    {
        std::vector<int64_t> tf(NUM_ROUNDS), tm(NUM_ROUNDS);
        for (int i = 0; i < 10; ++i) { bench_seq_flat(flat); bench_seq_hashmap(map); }
        for (int i = 0; i < NUM_ROUNDS; ++i) {
            tf[i] = bench_seq_flat(flat);
            tm[i] = bench_seq_hashmap(map);
        }
        print_result("1. Sequential traversal", NUM_OPS, tf, tm);
    }

    // Random (independent)
    {
        std::vector<int64_t> tf(NUM_ROUNDS), tm(NUM_ROUNDS);
        for (int i = 0; i < 10; ++i) { bench_rand_flat(flat, rand_idx); bench_rand_hashmap(map, names, rand_idx); }
        for (int i = 0; i < NUM_ROUNDS; ++i) {
            tf[i] = bench_rand_flat(flat, rand_idx);
            tm[i] = bench_rand_hashmap(map, names, rand_idx);
        }
        print_result("2. Random (independent)", NUM_OPS, tf, tm);
    }

    // Chain (serial + structured)
    {
        std::vector<int64_t> tf(NUM_ROUNDS), tm(NUM_ROUNDS);
        for (int i = 0; i < 10; ++i) { bench_chain_flat(flat); bench_chain_hashmap(map, names); }
        for (int i = 0; i < NUM_ROUNDS; ++i) {
            tf[i] = bench_chain_flat(flat);
            tm[i] = bench_chain_hashmap(map, names);
        }
        print_result("3. Chain (serial+structured)", NUM_OPS, tf, tm);
    }

    // Serial Random (serial + random) — CONTROL
    {
        std::vector<int64_t> tf(NUM_ROUNDS), tm(NUM_ROUNDS);
        for (int i = 0; i < 10; ++i) { bench_serial_rand_flat(flat, rand_idx); bench_serial_rand_hashmap(map, names, rand_idx); }
        for (int i = 0; i < NUM_ROUNDS; ++i) {
            tf[i] = bench_serial_rand_flat(flat, rand_idx);
            tm[i] = bench_serial_rand_hashmap(map, names, rand_idx);
        }
        print_result("4. Serial Random (control)", NUM_OPS, tf, tm);
    }

    printf("\n  --- Diagnosis: why is HashMap chain faster than random? ---\n\n");
    printf("  Variable isolation (HashMap ns/op):\n");
    printf("    Compare 2 vs 4: same randomness, different serialization → MLP effect\n");
    printf("    Compare 3 vs 4: same serialization, different locality   → locality effect\n");

    return 0;
}
