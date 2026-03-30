/**
 * Real Dependency Tree Traversal — Flat Array vs HashMap
 *
 * Uses the REAL dependency graph from kimi-cli/web/package-lock.json:
 *   789 packages, 1730 edges, max depth 9, root fanout 55
 *
 * Compares two approaches for dependency resolution (BFS):
 *   - HashMap: unordered_map<string, Package> + string key lookup (npm/yarn style)
 *   - Flat array: vector<Package> + integer ID indexing (Bun style)
 *
 * Key characteristics of real dep trees:
 *   - Wide and shallow (depth 9, most packages at depth 2-4)
 *   - Shared deps (many packages depend on the same package)
 *   - Siblings are independent (can be resolved in parallel by CPU)
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>

#include "real_deps.h"

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
// Package data — same content, different container
// ============================================================
struct PackageData {
    uint32_t tarball_size;
    uint64_t integrity_hash;
    uint8_t  flags;
};

// Flat array approach: dep graph stored as offset+list arrays
struct FlatGraph {
    std::vector<PackageData>  packages;
    std::vector<uint32_t>     dep_offsets; // dep_offsets[i]..dep_offsets[i+1]
    std::vector<uint32_t>     dep_list;    // flat list of dep IDs
    std::vector<std::string>  names;       // for display only
};

// HashMap approach: dep graph stored as string -> {data, dep_names}
struct HashPackage {
    PackageData data;
    std::vector<std::string> dep_names;
};

// ============================================================
// Build both representations from real_deps.h
// ============================================================
static FlatGraph build_flat() {
    FlatGraph g;
    g.packages.resize(REAL_NUM_PACKAGES);
    g.dep_offsets.assign(REAL_DEP_OFFSETS, REAL_DEP_OFFSETS + REAL_NUM_PACKAGES + 1);
    g.dep_list.assign(REAL_DEP_LIST, REAL_DEP_LIST + REAL_NUM_EDGES);
    g.names.resize(REAL_NUM_PACKAGES);

    uint32_t seed = 42;
    for (size_t i = 0; i < REAL_NUM_PACKAGES; ++i) {
        seed = seed * 1103515245 + 12345;
        g.packages[i].tarball_size   = seed & 0xFFFF;
        g.packages[i].integrity_hash = static_cast<uint64_t>(seed) * 6364136223846793005ULL;
        g.packages[i].flags          = seed & 0x7;
        g.names[i] = REAL_NAMES[i];
    }
    return g;
}

static std::unordered_map<std::string, HashPackage>
build_hashmap(const FlatGraph& flat) {
    std::unordered_map<std::string, HashPackage> map;
    map.reserve(REAL_NUM_PACKAGES);
    for (size_t i = 0; i < REAL_NUM_PACKAGES; ++i) {
        HashPackage hp;
        hp.data = flat.packages[i];
        uint32_t start = flat.dep_offsets[i];
        uint32_t end   = flat.dep_offsets[i + 1];
        for (uint32_t j = start; j < end; ++j)
            hp.dep_names.push_back(flat.names[flat.dep_list[j]]);
        map[flat.names[i]] = std::move(hp);
    }
    return map;
}

// ============================================================
// BFS traversal — the core dependency resolution operation
// ============================================================

// Flat: BFS using integer IDs
static int64_t bench_bfs_flat(const FlatGraph& g) {
    auto t0 = Clock::now();
    uint64_t acc = 0;
    std::vector<bool> visited(REAL_NUM_PACKAGES, false);
    std::queue<uint32_t> q;

    q.push(0);
    visited[0] = true;

    while (!q.empty()) {
        uint32_t cur = q.front(); q.pop();
        acc += g.packages[cur].tarball_size;
        // Follow deps
        uint32_t start = g.dep_offsets[cur];
        uint32_t end   = g.dep_offsets[cur + 1];
        for (uint32_t j = start; j < end; ++j) {
            uint32_t dep = g.dep_list[j];
            if (!visited[dep]) {
                visited[dep] = true;
                q.push(dep);
            }
        }
    }
    do_not_optimize(acc);
    return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
}

// HashMap: BFS using string keys
static int64_t bench_bfs_hashmap(const std::unordered_map<std::string, HashPackage>& map,
                                  const std::string& root_name) {
    auto t0 = Clock::now();
    uint64_t acc = 0;
    std::unordered_map<std::string, bool> visited;
    visited.reserve(REAL_NUM_PACKAGES);
    std::queue<std::string> q;

    q.push(root_name);
    visited[root_name] = true;

    while (!q.empty()) {
        std::string cur = std::move(q.front()); q.pop();
        auto it = map.find(cur);
        if (it == map.end()) continue;
        acc += it->second.data.tarball_size;
        for (const auto& dep_name : it->second.dep_names) {
            if (!visited.count(dep_name)) {
                visited[dep_name] = true;
                q.push(dep_name);
            }
        }
    }
    do_not_optimize(acc);
    return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
}

// ============================================================
// DFS traversal — alternative resolution strategy
// ============================================================

static void dfs_flat_impl(const FlatGraph& g, uint32_t node,
                           std::vector<bool>& visited, uint64_t& acc) {
    visited[node] = true;
    acc += g.packages[node].tarball_size;
    uint32_t start = g.dep_offsets[node];
    uint32_t end   = g.dep_offsets[node + 1];
    for (uint32_t j = start; j < end; ++j) {
        uint32_t dep = g.dep_list[j];
        if (!visited[dep])
            dfs_flat_impl(g, dep, visited, acc);
    }
}

static int64_t bench_dfs_flat(const FlatGraph& g) {
    auto t0 = Clock::now();
    uint64_t acc = 0;
    std::vector<bool> visited(REAL_NUM_PACKAGES, false);
    dfs_flat_impl(g, 0, visited, acc);
    do_not_optimize(acc);
    return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
}

static void dfs_hashmap_impl(const std::unordered_map<std::string, HashPackage>& map,
                              const std::string& node,
                              std::unordered_map<std::string, bool>& visited,
                              uint64_t& acc) {
    visited[node] = true;
    auto it = map.find(node);
    if (it == map.end()) return;
    acc += it->second.data.tarball_size;
    for (const auto& dep_name : it->second.dep_names) {
        if (!visited.count(dep_name))
            dfs_hashmap_impl(map, dep_name, visited, acc);
    }
}

static int64_t bench_dfs_hashmap(const std::unordered_map<std::string, HashPackage>& map,
                                  const std::string& root_name) {
    auto t0 = Clock::now();
    uint64_t acc = 0;
    std::unordered_map<std::string, bool> visited;
    visited.reserve(REAL_NUM_PACKAGES);
    dfs_hashmap_impl(map, root_name, visited, acc);
    do_not_optimize(acc);
    return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
}

// ============================================================
// "Cached resolution" — resolve all, then lookup each once
// Simulates: npm already resolved, now reading lockfile
// ============================================================
static int64_t bench_scan_flat(const FlatGraph& g) {
    auto t0 = Clock::now();
    uint64_t acc = 0;
    for (size_t i = 0; i < REAL_NUM_PACKAGES; ++i) {
        acc += g.packages[i].tarball_size;
        acc += g.packages[i].integrity_hash;
    }
    do_not_optimize(acc);
    return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
}

static int64_t bench_scan_hashmap(const std::unordered_map<std::string, HashPackage>& map) {
    auto t0 = Clock::now();
    uint64_t acc = 0;
    for (auto& [k, v] : map) {
        acc += v.data.tarball_size;
        acc += v.data.integrity_hash;
    }
    do_not_optimize(acc);
    return std::chrono::duration_cast<ns>(Clock::now() - t0).count();
}

// ============================================================

static constexpr int NUM_ROUNDS = 500;

int main() {
    printf("=== Real Dependency Tree Benchmark ===\n");
    printf("  Source: kimi-cli/web/package-lock.json\n");
    printf("  Packages: %zu    Edges: %zu    Max depth: 9\n", REAL_NUM_PACKAGES, REAL_NUM_EDGES);
    printf("  Rounds: %d\n\n", NUM_ROUNDS);

    auto flat = build_flat();
    auto map  = build_hashmap(flat);
    std::string root_name = flat.names[0];

    printf("  Flat graph memory:  %.1f KB (packages + offsets + dep_list)\n",
           (REAL_NUM_PACKAGES * sizeof(PackageData) +
            (REAL_NUM_PACKAGES + 1) * sizeof(uint32_t) +
            REAL_NUM_EDGES * sizeof(uint32_t)) / 1024.0);
    printf("  HashMap memory:     scattered (string keys + vectors + hash buckets)\n\n");

    // --- BFS ---
    {
        std::vector<int64_t> tf(NUM_ROUNDS), tm(NUM_ROUNDS);
        for (int i = 0; i < 20; ++i) { bench_bfs_flat(flat); bench_bfs_hashmap(map, root_name); }
        for (int i = 0; i < NUM_ROUNDS; ++i) {
            tf[i] = bench_bfs_flat(flat);
            tm[i] = bench_bfs_hashmap(map, root_name);
        }
        double mf = median_ns(tf), mm = median_ns(tm);
        printf("  %-28s  Flat: %7.1f us (%4.0f ns/pkg)   HashMap: %7.1f us (%5.0f ns/pkg)   speedup: %.1fx\n",
               "BFS (full resolve)", mf/1000, mf/REAL_NUM_PACKAGES, mm/1000, mm/REAL_NUM_PACKAGES, mm/mf);
    }

    // --- DFS ---
    {
        std::vector<int64_t> tf(NUM_ROUNDS), tm(NUM_ROUNDS);
        for (int i = 0; i < 20; ++i) { bench_dfs_flat(flat); bench_dfs_hashmap(map, root_name); }
        for (int i = 0; i < NUM_ROUNDS; ++i) {
            tf[i] = bench_dfs_flat(flat);
            tm[i] = bench_dfs_hashmap(map, root_name);
        }
        double mf = median_ns(tf), mm = median_ns(tm);
        printf("  %-28s  Flat: %7.1f us (%4.0f ns/pkg)   HashMap: %7.1f us (%5.0f ns/pkg)   speedup: %.1fx\n",
               "DFS (full resolve)", mf/1000, mf/REAL_NUM_PACKAGES, mm/1000, mm/REAL_NUM_PACKAGES, mm/mf);
    }

    // --- Full scan ---
    {
        std::vector<int64_t> tf(NUM_ROUNDS), tm(NUM_ROUNDS);
        for (int i = 0; i < 20; ++i) { bench_scan_flat(flat); bench_scan_hashmap(map); }
        for (int i = 0; i < NUM_ROUNDS; ++i) {
            tf[i] = bench_scan_flat(flat);
            tm[i] = bench_scan_hashmap(map);
        }
        double mf = median_ns(tf), mm = median_ns(tm);
        printf("  %-28s  Flat: %7.1f us (%4.0f ns/pkg)   HashMap: %7.1f us (%5.0f ns/pkg)   speedup: %.1fx\n",
               "Sequential scan (all pkgs)", mf/1000, mf/REAL_NUM_PACKAGES, mm/1000, mm/REAL_NUM_PACKAGES, mm/mf);
    }

    printf("\n  Tree structure characteristics:\n");
    printf("    - Wide & shallow: 55 direct deps, most packages at depth 2-4\n");
    printf("    - Shared deps: visited nodes are skipped (like real resolution cache)\n");
    printf("    - BFS visits siblings at same level → some spatial locality\n");
    printf("    - DFS dives deep first → more random access pattern\n");

    return 0;
}
