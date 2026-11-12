#include "hnsw.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_set>

namespace mvdb {

void Hnsw::insert(int64_t pk, const Vector& vec) {
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    double r = uni(rng_);
    if (r <= 0.0) r = 1e-12;
    int level = static_cast<int>(std::floor(-std::log(r) / std::log(static_cast<double>(kM))));

    int id = static_cast<int>(nodes_.size());
    Node node;
    node.pk = pk;
    node.vec = vec;
    node.links.resize(level + 1);
    nodes_.push_back(std::move(node));
    pk_nodes_[pk].push_back(id);

    if (entry_ < 0) {
        entry_ = id;
        max_level_ = level;
        return;
    }

    int cur = entry_;
    for (int l = max_level_; l > level; --l) cur = greedy_closest(vec, cur, l);

    for (int l = std::min(level, max_level_); l >= 0; --l) {
        auto cands = search_layer(vec, {cur}, kEfConstruction, l);
        int m = std::min<int>(kM, static_cast<int>(cands.size()));
        std::vector<int> neighbors;
        neighbors.reserve(m);
        for (int j = 0; j < m; ++j) neighbors.push_back(cands[j].second);
        nodes_[id].links[l] = neighbors;
        for (int n : neighbors) {
            auto& nl = nodes_[n].links[l];
            nl.push_back(id);
            int cap = max_m(l);
            if (static_cast<int>(nl.size()) > cap) {
                std::vector<std::pair<double, int>> scored;
                scored.reserve(nl.size());
                for (int x : nl) scored.emplace_back(dist(nodes_[n].vec, nodes_[x].vec), x);
                std::sort(scored.begin(), scored.end());
                nl.clear();
                for (int j = 0; j < cap; ++j) nl.push_back(scored[j].second);
            }
        }
        if (!cands.empty()) cur = cands[0].second;
    }
    if (level > max_level_) {
        entry_ = id;
        max_level_ = level;
    }
}

void Hnsw::erase(int64_t pk) {
    auto it = pk_nodes_.find(pk);
    if (it == pk_nodes_.end()) return;
    for (int id : it->second) nodes_[id].active = false;
    pk_nodes_.erase(it);
}

int Hnsw::greedy_closest(const Vector& q, int entry, int level) const {
    int cur = entry;
    double best = dist(q, nodes_[cur].vec);
    bool improved = true;
    while (improved) {
        improved = false;
        if (level < static_cast<int>(nodes_[cur].links.size())) {
            for (int n : nodes_[cur].links[level]) {
                double d = dist(q, nodes_[n].vec);
                if (d < best) {
                    best = d;
                    cur = n;
                    improved = true;
                }
            }
        }
    }
    return cur;
}

std::vector<std::pair<double, int>> Hnsw::search_layer(const Vector& q,
                                                       const std::vector<int>& entries,
                                                       int ef, int level) const {
    using Item = std::pair<double, int>;
    std::unordered_set<int> visited;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> cand;
    std::priority_queue<Item> result;
    for (int e : entries) {
        if (!visited.insert(e).second) continue;
        double d = dist(q, nodes_[e].vec);
        cand.emplace(d, e);
        result.emplace(d, e);
    }
    while (!cand.empty()) {
        auto [cd, cid] = cand.top();
        cand.pop();
        if (cd > result.top().first && static_cast<int>(result.size()) >= ef) break;
        if (level >= static_cast<int>(nodes_[cid].links.size())) continue;
        for (int n : nodes_[cid].links[level]) {
            if (!visited.insert(n).second) continue;
            double d = dist(q, nodes_[n].vec);
            if (static_cast<int>(result.size()) < ef || d < result.top().first) {
                cand.emplace(d, n);
                result.emplace(d, n);
                if (static_cast<int>(result.size()) > ef) result.pop();
            }
        }
    }
    std::vector<Item> out;
    out.reserve(result.size());
    while (!result.empty()) {
        out.push_back(result.top());
        result.pop();
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Hnsw::Candidate> Hnsw::search(const Vector& query, int k, int ef,
                                          const std::function<bool(int64_t)>& accept) const {
    std::vector<Candidate> out;
    if (entry_ < 0 || k <= 0) return out;
    int cur = entry_;
    for (int l = max_level_; l > 0; --l) cur = greedy_closest(query, cur, l);
    auto cands = search_layer(query, {cur}, std::max(ef, k), 0);
    std::unordered_map<int64_t, double> best;
    for (auto& [d, id] : cands) {
        const Node& node = nodes_[id];
        if (!node.active) continue;
        if (accept && !accept(node.pk)) continue;
        auto it = best.find(node.pk);
        if (it == best.end() || d < it->second) best[node.pk] = d;
    }
    out.reserve(best.size());
    for (auto& [pk, d] : best) out.push_back({pk, d});
    std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
        if (a.dist != b.dist) return a.dist < b.dist;
        return a.pk < b.pk;
    });
    if (static_cast<int>(out.size()) > k) out.resize(k);
    return out;
}

}  // namespace mvdb