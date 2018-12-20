#ifndef PBRT_SADJAD_STATS_H
#define PBRT_SADJAD_STATS_H

#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

struct SadjadProfile {
    std::ofstream writer;
    size_t currentDepth{0};
    bool shadowRay{false};

    std::unordered_set<const void *> nodePerTile{};
    size_t nodePerTileCount{0};

    struct Depth {
        std::unordered_set<const void *> nodePerRay{};
        std::unordered_set<const void *> nodePerShadowRay{};
        size_t nodePerRayCount{0};
        size_t nodePerShadowRayCount{0};
    } depth[11];

    void init(const int tile) {
        writer = std::ofstream("/tmp/pbrt-" + std::to_string(tile) + ".log",
                               std::ios::trunc);
    }

    void registerNode(const void * node) {
        nodePerTile.insert(node);
        nodePerTileCount++;

        if (shadowRay) {
            depth[currentDepth].nodePerShadowRay.insert(node);
            depth[currentDepth].nodePerShadowRayCount++;
        }
        else {
            depth[currentDepth].nodePerRay.insert(node);
            depth[currentDepth].nodePerRayCount++;
        }
    }

    void resetTile() {
        nodePerTile.clear();
        nodePerTileCount = 0;
        for (size_t i = 0; i < 11; i++) {
            depth[i].nodePerRay.clear();
            depth[i].nodePerShadowRay.clear();
            depth[i].nodePerRayCount = 0;
            depth[i].nodePerShadowRayCount = 0;
        }
    }

    void writeRayStats() {
        for (size_t i = 0; i < 11; i++) {
            writer << "DEPTH " << i << '\n'
                   << "UNIQUE_NODES " << depth[i].nodePerRay.size() << '\n'
                   << "UNIQUE_NODES_SHADOW " << depth[i].nodePerShadowRay.size() << '\n'
                   << "NODES " << depth[i].nodePerRayCount << '\n'
                   << "NODES_SHADOW " << depth[i].nodePerShadowRayCount << '\n';
        }
    }

    void writeTileStats() {
        writer << "TILE_UNIQUE_NODES " << nodePerTile.size() << '\n'
               << "TILE_NODES " << nodePerTileCount << '\n';
    }
};

namespace global {
extern thread_local SadjadProfile _sfp_;
}

#endif /* PBRT_SADJAD_STATS_H */
