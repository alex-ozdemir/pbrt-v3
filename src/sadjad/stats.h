#ifndef PBRT_SADJAD_STATS_H
#define PBRT_SADJAD_STATS_H

#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct SadjadProfile {
    using NodeUseCount = std::unordered_map<const void *, std::vector<size_t>>;
    bool initialized{false};

    std::ofstream writer;
    size_t currentDepth{0};
    bool shadowRay{false};

    NodeUseCount nodeForTile{};

    /* struct Depth {
        NodeUseCount nodeForRay{};
        NodeUseCount nodeForShadowRay{};
    } depth[11]; */

    void init(const int tile) {
        writer = std::ofstream("/tmp/pbrt-" + std::to_string(tile) + ".log",
                               std::ios::trunc);
    }

    void initBVHs(const std::unordered_map<const void *, size_t> &bvhs) {
        if (initialized) return;

        for (auto &kv : bvhs) {
            nodeForTile[kv.first].resize(kv.second, 0);
            /* for (size_t i = 0; i < 11; i++) {
                depth[i].nodeForRay[kv.first].resize(kv.second, 0);
                depth[i].nodeForShadowRay[kv.first].resize(kv.second, 0);
            } */
        }

        initialized = true;
    }

    void registerNode(const void *root, const size_t index) {
        nodeForTile[root][index]++;

        /* if (shadowRay) {
            depth[currentDepth].nodeForShadowRay[root][index]++;
        } else {
            depth[currentDepth].nodeForRay[root][index]++;
        }*/
    }

    void resetTile() {
        for (auto &kv : nodeForTile) {
            std::fill(kv.second.begin(), kv.second.end(), 0);
        }

        /* for (size_t i = 0; i < 11; i++) {
            for (auto &kv : depth[i].nodeForRay) {
                std::fill(kv.second.begin(), kv.second.end(), 0);
            }

            for (auto &kv : depth[i].nodeForShadowRay) {
                std::fill(kv.second.begin(), kv.second.end(), 0);
            }
        } */
    }

    std::pair<size_t, size_t> countUses(const NodeUseCount &nuc) {
        size_t useCount = 0;
        size_t uniqueUseCount = 0;

        for (const auto &kv : nuc) {
            for (const auto val : kv.second) {
                useCount += val;
                uniqueUseCount += (val ? 1 : 0);
            }
        }

        return {uniqueUseCount, useCount};
    }

    void writeRayStats() {
        /* for (size_t i = 0; i < 11; i++) {
            auto rayStats = countUses(depth[i].nodeForRay);
            auto shadowRayStats = countUses(depth[i].nodeForShadowRay);

            writer << "DEPTH " << i << '\n'
                   << "UNIQUE_NODES " << rayStats.first << '\n'
                   << "UNIQUE_NODES_SHADOW " << shadowRayStats.first << '\n'
                   << "NODES " << rayStats.second << '\n'
                   << "NODES_SHADOW " << shadowRayStats.second << '\n';
        } */
    }

    void writeTileStats() {
        auto tileStats = countUses(nodeForTile);
        writer << "TILE_UNIQUE_NODES " << tileStats.first << '\n'
               << "TILE_NODES " << tileStats.second << '\n';
    }
};

namespace global {
extern std::unordered_map<const void *, size_t> _bvhs_;
extern thread_local SadjadProfile _sfp_;
}  // namespace global

#endif /* PBRT_SADJAD_STATS_H */
