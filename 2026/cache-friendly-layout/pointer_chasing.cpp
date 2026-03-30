/**
 * JSON Object Tree vs AoS vs SoA+StringPool — Three-Way Comparison
 *
 * Models the ACTUAL difference between npm and Bun:
 *
 *   1. JSON trees (npm): JSON.parse() → V8 heap creates ~15 small objects
 *      per package, scattered across memory. Accessing one field requires
 *      2-3 pointer chases (e.g. pkg→dist→tarball→data).
 *
 *   2. AoS (naive flat): all data for one package in a contiguous struct,
 *      stored in a vector. No pointer chasing, but accessing one field
 *      loads the entire 528-byte struct into cache.
 *
 *   3. SoA + StringPool (Bun): each field stored in its own contiguous
 *      array. Strings deduplicated in a single buffer, referenced by offset.
 *      Deps referenced by integer ID. This is what Bun actually does.
 *
 * All three store the SAME logical data and access the SAME fields.
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>
#include <algorithm>

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

// ============================================================
// Layout A: JSON Object Trees (npm after JSON.parse)
// ============================================================

struct HeapString {
    char     data[64];
    uint32_t length;
    uint32_t _pad;
};

struct VersionObj {
    uint8_t major, minor, patch;
    uint8_t _pad[5];
    HeapString* raw;
};

struct DistObj {
    HeapString* tarball;
    HeapString* integrity;
    uint32_t    file_count;
    uint32_t    unpack_size;
};

struct DepEntry {
    HeapString* name;
    HeapString* version;
};

struct DepsObj {
    DepEntry entries[8];
    uint8_t  count;
    uint8_t  _pad[7];
};

struct ScriptsObj {
    HeapString* build;
    HeapString* test;
};

struct JsonPackage {
    HeapString*   name;
    VersionObj*   version;
    DistObj*      dist;
    DepsObj*      dependencies;
    ScriptsObj*   scripts;
    uint8_t       has_install_script;
    uint8_t       _pad[7];
    JsonPackage*  resolved_deps[8];
    uint8_t       num_resolved;
    uint8_t       _pad2[7];
};

// ============================================================
// Layout B: AoS — one fat struct per package, contiguous vector
// ============================================================

struct AoSPackage {
    char     name[64];
    uint8_t  version_major, version_minor, version_patch;
    uint8_t  num_deps;
    char     tarball_url[128];
    char     integrity[96];
    uint32_t file_count;
    uint32_t unpack_size;
    uint32_t dep_ids[8];
    char     script_build[64];
    char     script_test[64];
    uint8_t  has_install_script;
    uint8_t  _pad[3];
};

// ============================================================
// Layout C: SoA + StringPool — Bun's actual approach
// ============================================================

struct SoAPackages {
    // String pool: all strings stored contiguously, deduplicated
    std::vector<char>     string_pool;

    // Per-package SoA arrays — each field in its own contiguous array
    std::vector<uint32_t> name_offsets;       // offset into string_pool
    std::vector<uint8_t>  name_lengths;
    std::vector<uint8_t>  version_major;
    std::vector<uint8_t>  version_minor;
    std::vector<uint8_t>  version_patch;
    std::vector<uint32_t> tarball_offsets;    // offset into string_pool
    std::vector<uint8_t>  tarball_lengths;
    std::vector<uint32_t> integrity_offsets;  // offset into string_pool
    std::vector<uint8_t>  integrity_lengths;
    std::vector<uint32_t> file_count;
    std::vector<uint32_t> unpack_size;
    std::vector<uint8_t>  has_install_script;

    // Dependency graph — CSR format
    std::vector<uint32_t> dep_offsets;        // dep_offsets[i]..dep_offsets[i+1]
    std::vector<uint32_t> dep_list;           // flat list of integer dep IDs

    size_t num_packages;
};

// ============================================================
static volatile uint8_t polluter_sink;

static void pollute_cache(std::vector<uint8_t>& garbage) {
    uint8_t acc = 0;
    for (size_t i = 0; i < garbage.size(); i += 64)
        acc += garbage[i];
    polluter_sink = acc;
}

static std::vector<void*> gap_allocs;

static void* alloc_with_gap(size_t size, std::mt19937& rng) {
    size_t gap = 256 + (rng() % 4096);
    void* g = malloc(gap);
    memset(g, 0xCC, gap);
    gap_allocs.push_back(g);
    void* p = malloc(size);
    memset(p, 0, size);
    return p;
}

// ============================================================

static constexpr size_t NUM_PACKAGES  = 1000;
static constexpr size_t DEPS_PER_PKG  = 5;
static constexpr size_t TOTAL_LOOKUPS = NUM_PACKAGES * DEPS_PER_PKG;
static constexpr int    NUM_ROUNDS    = 50;
static constexpr size_t GARBAGE_SIZE  = 64 * 1024 * 1024;

int main() {
    printf("=== JSON Trees vs AoS vs SoA+StringPool ===\n");
    printf("  Three-way comparison: npm (JSON.parse) vs naive flat vs Bun (SoA)\n");
    printf("  Scenario: %zu packages x %zu deps = %zu lookups\n\n",
           NUM_PACKAGES, DEPS_PER_PKG, TOTAL_LOOKUPS);

    std::mt19937 rng(42);

    // ============================================================
    // Build Layout A: JSON object trees (~15 mallocs per package)
    // ============================================================
    std::vector<JsonPackage*> json_pkgs(NUM_PACKAGES);

    for (size_t i = 0; i < NUM_PACKAGES; ++i) {
        auto* pkg = (JsonPackage*)alloc_with_gap(sizeof(JsonPackage), rng);
        json_pkgs[i] = pkg;

        pkg->name = (HeapString*)alloc_with_gap(sizeof(HeapString), rng);
        snprintf(pkg->name->data, 64, "@scope/package-%04zu", i);
        pkg->name->length = strlen(pkg->name->data);

        pkg->version = (VersionObj*)alloc_with_gap(sizeof(VersionObj), rng);
        pkg->version->major = rng() & 0xF;
        pkg->version->minor = rng() & 0xF;
        pkg->version->patch = rng() & 0xF;
        pkg->version->raw = (HeapString*)alloc_with_gap(sizeof(HeapString), rng);

        pkg->dist = (DistObj*)alloc_with_gap(sizeof(DistObj), rng);
        pkg->dist->tarball = (HeapString*)alloc_with_gap(sizeof(HeapString), rng);
        snprintf(pkg->dist->tarball->data, 64, "https://registry.npmjs.org/pkg-%04zu.tgz", i);
        pkg->dist->integrity = (HeapString*)alloc_with_gap(sizeof(HeapString), rng);
        snprintf(pkg->dist->integrity->data, 64, "sha512-%016llx", (unsigned long long)rng());
        pkg->dist->file_count = rng() % 100;
        pkg->dist->unpack_size = rng() & 0xFFFFFF;

        pkg->dependencies = (DepsObj*)alloc_with_gap(sizeof(DepsObj), rng);
        pkg->dependencies->count = DEPS_PER_PKG;
        for (size_t d = 0; d < DEPS_PER_PKG; ++d) {
            pkg->dependencies->entries[d].name = (HeapString*)alloc_with_gap(sizeof(HeapString), rng);
            pkg->dependencies->entries[d].version = (HeapString*)alloc_with_gap(sizeof(HeapString), rng);
        }

        pkg->scripts = (ScriptsObj*)alloc_with_gap(sizeof(ScriptsObj), rng);
        pkg->scripts->build = (HeapString*)alloc_with_gap(sizeof(HeapString), rng);
        snprintf(pkg->scripts->build->data, 64, "tsc && rollup -c");
        pkg->scripts->test = (HeapString*)alloc_with_gap(sizeof(HeapString), rng);
        snprintf(pkg->scripts->test->data, 64, "jest --coverage");

        pkg->num_resolved = DEPS_PER_PKG;
        pkg->has_install_script = rng() & 1;
    }

    for (size_t i = 0; i < NUM_PACKAGES; ++i)
        for (size_t d = 0; d < 8; ++d)
            json_pkgs[i]->resolved_deps[d] = json_pkgs[rng() % NUM_PACKAGES];

    // Shuffled access order
    std::vector<size_t> access_order(NUM_PACKAGES);
    std::iota(access_order.begin(), access_order.end(), 0);
    std::shuffle(access_order.begin(), access_order.end(), rng);

    // ============================================================
    // Build Layout B: AoS — one fat struct per package
    // ============================================================
    std::vector<AoSPackage> aos(NUM_PACKAGES);
    for (size_t i = 0; i < NUM_PACKAGES; ++i) {
        auto* jp = json_pkgs[i];
        memset(&aos[i], 0, sizeof(AoSPackage));
        memcpy(aos[i].name, jp->name->data, 64);
        aos[i].version_major = jp->version->major;
        aos[i].version_minor = jp->version->minor;
        aos[i].version_patch = jp->version->patch;
        aos[i].num_deps = DEPS_PER_PKG;
        memcpy(aos[i].tarball_url, jp->dist->tarball->data, 64);
        memcpy(aos[i].integrity, jp->dist->integrity->data, 64);
        aos[i].file_count = jp->dist->file_count;
        aos[i].unpack_size = jp->dist->unpack_size;
        for (size_t d = 0; d < 8; ++d)
            aos[i].dep_ids[d] = rng() % NUM_PACKAGES;
        memcpy(aos[i].script_build, jp->scripts->build->data, 64);
        memcpy(aos[i].script_test, jp->scripts->test->data, 64);
        aos[i].has_install_script = jp->has_install_script;
    }

    // ============================================================
    // Build Layout C: SoA + StringPool — Bun's approach
    // ============================================================
    SoAPackages soa;
    soa.num_packages = NUM_PACKAGES;
    soa.name_offsets.resize(NUM_PACKAGES);
    soa.name_lengths.resize(NUM_PACKAGES);
    soa.version_major.resize(NUM_PACKAGES);
    soa.version_minor.resize(NUM_PACKAGES);
    soa.version_patch.resize(NUM_PACKAGES);
    soa.tarball_offsets.resize(NUM_PACKAGES);
    soa.tarball_lengths.resize(NUM_PACKAGES);
    soa.integrity_offsets.resize(NUM_PACKAGES);
    soa.integrity_lengths.resize(NUM_PACKAGES);
    soa.file_count.resize(NUM_PACKAGES);
    soa.unpack_size.resize(NUM_PACKAGES);
    soa.has_install_script.resize(NUM_PACKAGES);
    soa.dep_offsets.resize(NUM_PACKAGES + 1);

    // Build string pool + fill SoA arrays
    for (size_t i = 0; i < NUM_PACKAGES; ++i) {
        auto* jp = json_pkgs[i];

        // Name → string pool
        soa.name_offsets[i] = soa.string_pool.size();
        size_t nlen = strlen(jp->name->data);
        soa.name_lengths[i] = nlen;
        soa.string_pool.insert(soa.string_pool.end(), jp->name->data, jp->name->data + nlen);

        // Tarball → string pool
        soa.tarball_offsets[i] = soa.string_pool.size();
        size_t tlen = strlen(jp->dist->tarball->data);
        soa.tarball_lengths[i] = tlen;
        soa.string_pool.insert(soa.string_pool.end(),
                               jp->dist->tarball->data, jp->dist->tarball->data + tlen);

        // Integrity → string pool
        soa.integrity_offsets[i] = soa.string_pool.size();
        size_t ilen = strlen(jp->dist->integrity->data);
        soa.integrity_lengths[i] = ilen;
        soa.string_pool.insert(soa.string_pool.end(),
                               jp->dist->integrity->data, jp->dist->integrity->data + ilen);

        // Scalar fields
        soa.version_major[i] = jp->version->major;
        soa.version_minor[i] = jp->version->minor;
        soa.version_patch[i] = jp->version->patch;
        soa.file_count[i]    = jp->dist->file_count;
        soa.unpack_size[i]   = jp->dist->unpack_size;
        soa.has_install_script[i] = jp->has_install_script;

        // Deps — CSR format
        soa.dep_offsets[i] = soa.dep_list.size();
        for (size_t d = 0; d < DEPS_PER_PKG; ++d)
            soa.dep_list.push_back(aos[i].dep_ids[d]); // reuse same random IDs
    }
    soa.dep_offsets[NUM_PACKAGES] = soa.dep_list.size();

    // ============================================================
    // Print memory footprint comparison
    // ============================================================
    size_t soa_total = soa.string_pool.size()
        + NUM_PACKAGES * (sizeof(uint32_t)*5 + sizeof(uint8_t)*6)
        + (NUM_PACKAGES + 1) * sizeof(uint32_t)
        + soa.dep_list.size() * sizeof(uint32_t);

    printf("  Memory footprint:\n");
    printf("    JSON trees:     ~15,000 scattered heap objects\n");
    printf("    AoS:            %6.1f KB  (1 vector, %zu bytes/pkg)\n",
           NUM_PACKAGES * sizeof(AoSPackage) / 1024.0, sizeof(AoSPackage));
    printf("    SoA+StringPool: %6.1f KB  (%zu bytes string pool + parallel arrays)\n\n",
           soa_total / 1024.0, soa.string_pool.size());

    std::vector<uint8_t> cache_garbage(GARBAGE_SIZE, 0xBB);

    // ============================================================
    // Benchmark: access tarball[0] + integrity[0] + version + follow deps
    // Same logical operation, three different data layouts
    // ============================================================

    auto run_bench = [&](const char* label, auto bench_fn) {
        std::vector<int64_t> times(NUM_ROUNDS);
        // Warmup
        for (int i = 0; i < 10; ++i) bench_fn(false);
        for (int r = 0; r < NUM_ROUNDS; ++r) {
            pollute_cache(cache_garbage);
            times[r] = bench_fn(true);
        }
        double cold = median_ns(times);

        // Warm
        for (int i = 0; i < 30; ++i) bench_fn(false);
        for (int r = 0; r < NUM_ROUNDS; ++r)
            times[r] = bench_fn(false);
        double warm = median_ns(times);

        printf("  %-26s  cold: %7.1f us (%3.0f ns/op)   warm: %6.1f us (%2.0f ns/op)\n",
               label, cold/1000, cold/TOTAL_LOOKUPS, warm/1000, warm/TOTAL_LOOKUPS);
        return std::make_pair(cold, warm);
    };

    printf("  --- Results ---\n\n");

    // JSON trees
    auto [json_cold, json_warm] = run_bench("JSON trees (npm)", [&](bool) -> int64_t {
        auto t0 = Clock::now();
        uint64_t acc = 0;
        for (size_t idx = 0; idx < NUM_PACKAGES; ++idx) {
            size_t i = access_order[idx];
            auto* pkg = json_pkgs[i];
            acc += pkg->dist->tarball->data[0];      // 3 hops
            acc += pkg->dist->integrity->data[0];     // 3 hops (dist cached)
            acc += pkg->version->major;               // 2 hops
            acc += pkg->version->minor;
            for (size_t d = 0; d < DEPS_PER_PKG; ++d)
                acc += pkg->resolved_deps[d]->dist->unpack_size; // 3 hops
        }
        do_not_optimize(acc);
        return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
    });

    // AoS
    auto [aos_cold, aos_warm] = run_bench("AoS flat struct", [&](bool) -> int64_t {
        auto t0 = Clock::now();
        uint64_t acc = 0;
        for (size_t idx = 0; idx < NUM_PACKAGES; ++idx) {
            size_t i = access_order[idx];
            acc += aos[i].tarball_url[0];             // 1 index, 0 hops
            acc += aos[i].integrity[0];
            acc += aos[i].version_major;
            acc += aos[i].version_minor;
            for (size_t d = 0; d < DEPS_PER_PKG; ++d)
                acc += aos[aos[i].dep_ids[d]].unpack_size;
        }
        do_not_optimize(acc);
        return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
    });

    // SoA + StringPool
    auto [soa_cold, soa_warm] = run_bench("SoA+StringPool (Bun)", [&](bool) -> int64_t {
        auto t0 = Clock::now();
        uint64_t acc = 0;
        for (size_t idx = 0; idx < NUM_PACKAGES; ++idx) {
            size_t i = access_order[idx];
            acc += soa.string_pool[soa.tarball_offsets[i]];     // 2 array reads
            acc += soa.string_pool[soa.integrity_offsets[i]];   // 2 array reads
            acc += soa.version_major[i];                        // 1 array read
            acc += soa.version_minor[i];                        // 1 array read
            uint32_t dstart = soa.dep_offsets[i];
            uint32_t dend   = soa.dep_offsets[i + 1];
            for (uint32_t d = dstart; d < dend; ++d)
                acc += soa.unpack_size[soa.dep_list[d]];        // 2 array reads
        }
        do_not_optimize(acc);
        return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
    });

    printf("\n  --- Speedup vs JSON trees ---\n\n");
    printf("    AoS:             %.1fx (cold)  %.1fx (warm)\n",
           json_cold / aos_cold, json_warm / aos_warm);
    printf("    SoA+StringPool:  %.1fx (cold)  %.1fx (warm)\n",
           json_cold / soa_cold, json_warm / soa_warm);
    printf("    SoA vs AoS:      %.1fx (cold)  %.1fx (warm)\n",
           aos_cold / soa_cold, aos_warm / soa_warm);

    printf("\n  --- Access pattern comparison ---\n\n");
    printf("    pkg.dist.tarball[0]:\n");
    printf("      JSON:  pkg->dist->tarball->data[0]                    3 pointer chases\n");
    printf("      AoS:   aos[i].tarball_url[0]                          1 array index\n");
    printf("      SoA:   string_pool[tarball_offsets[i]]                2 array reads\n\n");
    printf("    dep.unpack_size:\n");
    printf("      JSON:  pkg->resolved_deps[d]->dist->unpack_size       3 pointer chases\n");
    printf("      AoS:   aos[aos[i].dep_ids[d]].unpack_size             2 array reads\n");
    printf("      SoA:   unpack_size[dep_list[dep_offsets[i]+d]]         3 array reads\n");

    return 0;
}
