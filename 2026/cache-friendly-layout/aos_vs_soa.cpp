/**
 * AoS vs SoA - Cache-Friendly Data Layout Demo
 *
 * Simulates a package manager scenario (like Bun's lockfile).
 *
 * Key design: the AoS struct is 512 bytes (spans 8 cache lines),
 * simulating a real npm package entry with many fields:
 *   name, description, homepage, repository, keywords, scripts,
 *   all dependency maps, maintainers, license, etc.
 *
 * When you only need 1-2 fields out of 512 bytes, AoS wastes 98%+
 * of every cache line loaded. SoA only loads the fields it needs.
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <vector>
#include <algorithm>

static constexpr size_t NUM_PACKAGES = 100'000;
static constexpr int    NUM_ROUNDS   = 200;

// ============================================================
// AoS: 512 bytes per package — spans 8 cache lines
//
// Simulates a real package entry with many fields that a parser
// would produce. Most operations only need 1-3 of these fields.
// ============================================================
struct PackageAoS {
    // --- cache line 0 (bytes 0-63) ---
    uint32_t name_offset;           // 4B
    uint16_t name_len;              // 2B
    uint8_t  version_major;         // 1B
    uint8_t  version_minor;         // 1B
    uint8_t  version_patch;         // 1B
    uint8_t  num_deps;              // 1B
    uint16_t dep_list_offset;       // 2B
    uint32_t tarball_offset;        // 4B
    uint32_t tarball_size;          // 4B
    uint8_t  flags;                 // 1B
    char     name_inline[44];       // 44B (short name storage)

    // --- cache line 1 (bytes 64-127) ---
    uint64_t integrity_hash;        // 8B
    char     description[56];       // 56B

    // --- cache line 2 (bytes 128-191) ---
    char     homepage[64];          // 64B

    // --- cache line 3 (bytes 192-255) ---
    char     repository[64];        // 64B

    // --- cache line 4 (bytes 256-319) ---
    char     license[16];           // 16B
    char     keywords[48];          // 48B

    // --- cache line 5 (bytes 320-383) ---
    char     scripts_build[32];     // 32B
    char     scripts_test[32];      // 32B

    // --- cache line 6 (bytes 384-447) ---
    uint32_t peer_dep_ids[16];      // 64B

    // --- cache line 7 (bytes 448-511) ---
    uint32_t optional_dep_ids[12];  // 48B
    uint32_t engines_node_min;      // 4B
    uint32_t engines_node_max;      // 4B
    uint64_t publish_time;          // 8B
};
// static_assert(sizeof(PackageAoS) == 512);

// ============================================================
// SoA: each field in its own contiguous array
// ============================================================
struct PackagesSoA {
    std::vector<uint32_t> name_offset;
    std::vector<uint16_t> name_len;
    std::vector<uint8_t>  version_major;
    std::vector<uint8_t>  version_minor;
    std::vector<uint8_t>  version_patch;
    std::vector<uint8_t>  num_deps;
    std::vector<uint16_t> dep_list_offset;
    std::vector<uint64_t> integrity_hash;
    std::vector<uint32_t> tarball_offset;
    std::vector<uint32_t> tarball_size;
    std::vector<uint8_t>  flags;
    std::vector<uint64_t> publish_time;

    void resize(size_t n) {
        name_offset.resize(n);   name_len.resize(n);
        version_major.resize(n); version_minor.resize(n);
        version_patch.resize(n); num_deps.resize(n);
        dep_list_offset.resize(n); integrity_hash.resize(n);
        tarball_offset.resize(n); tarball_size.resize(n);
        flags.resize(n); publish_time.resize(n);
    }
};

// ============================================================
template <typename T>
static void do_not_optimize(T const& val) {
    asm volatile("" : : "r,m"(val) : "memory");
}

using Clock = std::chrono::high_resolution_clock;
using ns    = std::chrono::nanoseconds;

static double median_ns(std::vector<int64_t>& samples) {
    std::sort(samples.begin(), samples.end());
    size_t n = samples.size();
    if (n % 2 == 0)
        return (samples[n/2 - 1] + samples[n/2]) / 2.0;
    return samples[n/2];
}

// ============================================================
// Fill data
// ============================================================
static void fill_aos(std::vector<PackageAoS>& pkgs) {
    pkgs.resize(NUM_PACKAGES);
    uint32_t seed = 42;
    for (size_t i = 0; i < NUM_PACKAGES; ++i) {
        seed = seed * 1103515245 + 12345;
        memset(&pkgs[i], 0, sizeof(PackageAoS));
        pkgs[i].name_offset    = seed & 0xFFFFF;
        pkgs[i].name_len       = (seed >> 8) & 0xFF;
        pkgs[i].version_major  = (seed >> 16) & 0xF;
        pkgs[i].version_minor  = (seed >> 20) & 0xF;
        pkgs[i].version_patch  = (seed >> 24) & 0xF;
        pkgs[i].num_deps       = (seed >> 4) & 0x1F;   // 0-31
        pkgs[i].dep_list_offset= (seed >> 2) & 0xFFFF;
        pkgs[i].integrity_hash = static_cast<uint64_t>(seed) * 6364136223846793005ULL;
        pkgs[i].tarball_offset = seed & 0xFFFFFF;
        pkgs[i].tarball_size   = (seed >> 8) & 0xFFFF;
        pkgs[i].flags          = seed & 0x7;
        pkgs[i].publish_time   = static_cast<uint64_t>(seed) * 2862933555777941757ULL;
    }
}

static void fill_soa(PackagesSoA& pkgs) {
    pkgs.resize(NUM_PACKAGES);
    uint32_t seed = 42;
    for (size_t i = 0; i < NUM_PACKAGES; ++i) {
        seed = seed * 1103515245 + 12345;
        pkgs.name_offset[i]    = seed & 0xFFFFF;
        pkgs.name_len[i]       = (seed >> 8) & 0xFF;
        pkgs.version_major[i]  = (seed >> 16) & 0xF;
        pkgs.version_minor[i]  = (seed >> 20) & 0xF;
        pkgs.version_patch[i]  = (seed >> 24) & 0xF;
        pkgs.num_deps[i]       = (seed >> 4) & 0x1F;
        pkgs.dep_list_offset[i]= (seed >> 2) & 0xFFFF;
        pkgs.integrity_hash[i] = static_cast<uint64_t>(seed) * 6364136223846793005ULL;
        pkgs.tarball_offset[i] = seed & 0xFFFFFF;
        pkgs.tarball_size[i]   = (seed >> 8) & 0xFFFF;
        pkgs.flags[i]          = seed & 0x7;
        pkgs.publish_time[i]   = static_cast<uint64_t>(seed) * 2862933555777941757ULL;
    }
}

// ============================================================
// Benchmark 1: Single-field scan — sum num_deps
//   AoS loads 512 bytes per package (8 cache lines), only uses 1 byte
//   SoA loads 100KB contiguous uint8 array
// ============================================================
static int64_t bench_single_aos(const std::vector<PackageAoS>& pkgs) {
    auto t0 = Clock::now();
    uint64_t total = 0;
    for (size_t i = 0; i < pkgs.size(); ++i)
        total += pkgs[i].num_deps;
    do_not_optimize(total);
    return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
}

static int64_t bench_single_soa(const PackagesSoA& pkgs) {
    auto t0 = Clock::now();
    uint64_t total = 0;
    for (size_t i = 0; i < pkgs.num_deps.size(); ++i)
        total += pkgs.num_deps[i];
    do_not_optimize(total);
    return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
}

// ============================================================
// Benchmark 2: Two fields on DIFFERENT cache lines
//   Access num_deps (cache line 0, offset 9) and integrity_hash (cache line 1, offset 64)
//   AoS: must load 2 cache lines per package out of 8
//   SoA: two tight contiguous arrays
// ============================================================
static int64_t bench_two_field_aos(const std::vector<PackageAoS>& pkgs) {
    auto t0 = Clock::now();
    uint64_t acc = 0;
    for (size_t i = 0; i < pkgs.size(); ++i) {
        acc += pkgs[i].num_deps;           // cache line 0
        acc ^= pkgs[i].integrity_hash;     // cache line 1
    }
    do_not_optimize(acc);
    return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
}

static int64_t bench_two_field_soa(const PackagesSoA& pkgs) {
    auto t0 = Clock::now();
    uint64_t acc = 0;
    for (size_t i = 0; i < pkgs.num_deps.size(); ++i) {
        acc += pkgs.num_deps[i];
        acc ^= pkgs.integrity_hash[i];
    }
    do_not_optimize(acc);
    return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
}

// ============================================================
// Benchmark 3: Three fields across 3 different cache lines
//   num_deps (line 0) + integrity_hash (line 1) + publish_time (line 7)
//   AoS: 3 cache lines per package, worst case stride
//   SoA: 3 contiguous arrays
// ============================================================
static int64_t bench_three_field_aos(const std::vector<PackageAoS>& pkgs) {
    auto t0 = Clock::now();
    uint64_t acc = 0;
    for (size_t i = 0; i < pkgs.size(); ++i) {
        acc += pkgs[i].num_deps;           // cache line 0
        acc ^= pkgs[i].integrity_hash;     // cache line 1
        acc += pkgs[i].publish_time;       // cache line 7
    }
    do_not_optimize(acc);
    return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
}

static int64_t bench_three_field_soa(const PackagesSoA& pkgs) {
    auto t0 = Clock::now();
    uint64_t acc = 0;
    for (size_t i = 0; i < pkgs.num_deps.size(); ++i) {
        acc += pkgs.num_deps[i];
        acc ^= pkgs.integrity_hash[i];
        acc += pkgs.publish_time[i];
    }
    do_not_optimize(acc);
    return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
}

// ============================================================
// Benchmark 4: Filtered scan — sparse access pattern
//   Filter: num_deps > 24 (~22% hit rate for uniform 0-31)
//   Then access tarball_size (same cache line 0) AND publish_time (cache line 7)
//   AoS: every package loads line 0 for filter, hits also load line 7
//   SoA: tight filter on num_deps array, sparse access to tarball_size + publish_time
// ============================================================
static int64_t bench_filtered_aos(const std::vector<PackageAoS>& pkgs) {
    auto t0 = Clock::now();
    uint64_t total = 0;
    for (size_t i = 0; i < pkgs.size(); ++i) {
        if (pkgs[i].num_deps > 24) {
            total += pkgs[i].tarball_size;
            total += pkgs[i].publish_time;  // cache line 7 — far from filter field
        }
    }
    do_not_optimize(total);
    return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
}

static int64_t bench_filtered_soa(const PackagesSoA& pkgs) {
    auto t0 = Clock::now();
    uint64_t total = 0;
    for (size_t i = 0; i < pkgs.num_deps.size(); ++i) {
        if (pkgs.num_deps[i] > 24) {
            total += pkgs.tarball_size[i];
            total += pkgs.publish_time[i];
        }
    }
    do_not_optimize(total);
    return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
}

// ============================================================

static void run_benchmark(const char* name,
                          std::vector<PackageAoS>& aos,
                          PackagesSoA& soa,
                          int64_t (*fn_aos)(const std::vector<PackageAoS>&),
                          int64_t (*fn_soa)(const PackagesSoA&))
{
    std::vector<int64_t> times_aos(NUM_ROUNDS), times_soa(NUM_ROUNDS);
    for (int i = 0; i < 10; ++i) { fn_aos(aos); fn_soa(soa); }
    for (int i = 0; i < NUM_ROUNDS; ++i) {
        times_aos[i] = fn_aos(aos);
        times_soa[i] = fn_soa(soa);
    }
    double med_aos = median_ns(times_aos);
    double med_soa = median_ns(times_soa);
    printf("  %-36s  AoS: %8.1f us   SoA: %8.1f us   speedup: %.2fx\n",
           name, med_aos / 1000.0, med_soa / 1000.0, med_aos / med_soa);
}

int main() {
    printf("=== AoS vs SoA Benchmark ===\n");
    printf("  Packages: %zu    Rounds: %d\n\n", NUM_PACKAGES, NUM_ROUNDS);
    printf("  sizeof(PackageAoS) = %zu bytes (%zu cache lines per struct)\n",
           sizeof(PackageAoS), sizeof(PackageAoS) / 64);
    printf("  SoA num_deps array = %zu bytes (contiguous)\n\n", NUM_PACKAGES * sizeof(uint8_t));

    std::vector<PackageAoS> aos;
    PackagesSoA soa;
    fill_aos(aos);
    fill_soa(soa);

    printf("  AoS total memory: %.1f MB\n", NUM_PACKAGES * sizeof(PackageAoS) / (1024.0 * 1024.0));
    printf("  SoA total memory: %.1f KB (only fields actually used)\n\n",
           (NUM_PACKAGES * (sizeof(uint8_t) + sizeof(uint64_t)*2 + sizeof(uint32_t))) / 1024.0);

    run_benchmark("1. Single-field (num_deps)",       aos, soa, bench_single_aos,     bench_single_soa);
    run_benchmark("2. Two fields (2 cache lines)",    aos, soa, bench_two_field_aos,   bench_two_field_soa);
    run_benchmark("3. Three fields (3 cache lines)",  aos, soa, bench_three_field_aos, bench_three_field_soa);
    run_benchmark("4. Filtered (sparse access)",      aos, soa, bench_filtered_aos,    bench_filtered_soa);

    printf("\n  Why AoS loses:\n");
    printf("    Each PackageAoS = %zu bytes = %zu cache lines.\n", sizeof(PackageAoS), sizeof(PackageAoS)/64);
    printf("    Accessing 1 field loads 1 cache line but strides %zu bytes to next package.\n", sizeof(PackageAoS));
    printf("    For 100K packages, AoS touches %.1f MB; SoA touches just %.0f KB.\n",
           NUM_PACKAGES * sizeof(PackageAoS) / (1024.0*1024.0),
           NUM_PACKAGES * sizeof(uint8_t) / 1024.0);
    printf("    The more fields your struct has, the bigger SoA's advantage when\n");
    printf("    you only need a few of them — which is the common case in Bun's lockfile.\n");

    return 0;
}
