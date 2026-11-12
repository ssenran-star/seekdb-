#pragma once
#include <cstdint>
#include <functional>
#include <random>
#include <unordered_map>
#include <vector>

#include "common.h"

namespace mvdb {

class Hnsw {
public:
    struct Candidate {
        int64_t pk;
        double dist;
    };

    void insert(int64_t pk, const Vector& vec);
    void erase(int64_t pk);
    std::vector<Candidate> search(const Vector& query, int k, int ef,
                                  const std::function<bool(int64_t)>& accept) const;
    size_t size() const { return nodes_.size(); }

private:
    struct Node {
        int64_t pk = 0;
        Vector vec;
        std::vector<std::vector<int>> links;
        bool active = true;
    };

    double dist(const Vector& a, const Vector& b) const { return l2_distance(a, b); }
    int greedy_closest(const Vector& q, int entry, int level) const;
    std::vector<std::pair<double, int>> search_layer(const Vector& q,
                                                     const std::vector<int>& entries,
                                                     int ef, int level) const;
    int max_m(int level) const { return level == 0 ? kM * 2 : kM; }

    static constexpr int kM = 16;
    static constexpr int kEfConstruction = 400;
    std::vector<Node> nodes_;
    std::unordered_map<int64_t, std::vector<int>> pk_nodes_;
    int entry_ = -1;
    int max_level_ = -1;
    mutable std::mt19937_64 rng_{0x5EED2026};
};

}  // namespace mvdb