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
    std::unordered_set<const void *> visitedNodePerRay{};
    std::unordered_set<const void *> visitedNodePerPath{};
    std::unordered_set<const void *> visitedNodePerTile{};
    std::unordered_set<const void *> visitedPrimPerRay{};
    std::unordered_set<const void *> visitedPrimPerPath{};
    std::unordered_set<const void *> visitedPrimPerTile{};
    size_t visitedNodePerRayCount{0};
    size_t visitedPrimPerRayCount{0};
    size_t visitedNodePerPathCount{0};
    size_t visitedPrimPerPathCount{0};
    size_t visitedNodePerTileCount{0};
    size_t visitedPrimPerTileCount{0};
    size_t pathLength{0};
    std::ofstream writer;

    bool initialized{false};

    void init(const int tile) {
        if (initialized) throw std::runtime_error("already initialized");

        resetPath();
        writer = std::ofstream("/tmp/pbrt-" + std::to_string(tile) + ".log",
                               std::ios::trunc);

        initialized = true;
    }

    void resetRay() {
        visitedNodePerRay.clear();
        visitedPrimPerRay.clear();
        visitedNodePerRayCount = 0;
        visitedPrimPerRayCount = 0;
    }

    void resetPath() {
        visitedNodePerRay.clear();
        visitedNodePerPath.clear();
        visitedPrimPerRay.clear();
        visitedPrimPerPath.clear();
        visitedNodePerRayCount = 0;
        visitedNodePerPathCount = 0;
        visitedPrimPerRayCount = 0;
        visitedPrimPerPathCount = 0;
        pathLength = 0;
    }

    void resetTile() {
        visitedNodePerRay.clear();
        visitedNodePerPath.clear();
        visitedNodePerTile.clear();
        visitedPrimPerRay.clear();
        visitedPrimPerPath.clear();
        visitedPrimPerTile.clear();
        visitedNodePerRayCount = 0;
        visitedNodePerPathCount = 0;
        visitedNodePerTileCount = 0;
        visitedPrimPerRayCount = 0;
        visitedPrimPerPathCount = 0;
        visitedPrimPerTileCount = 0;
        pathLength = 0;
    }

    void writeRayStats(const bool shadowRay) {
        return;
        writer << "RAY_DEPTH " << pathLength << (shadowRay ? " (SHADOW)" : "")
               << '\n'
               << "UNIQUE_NODES " << visitedNodePerRay.size() << '\n'
               << "UNIQUE_PRIMS " << visitedPrimPerRay.size() << '\n'
               << "NODES " << visitedNodePerRayCount << '\n'
               << "PRIMS " << visitedPrimPerRayCount << '\n';
    }

    void WritePathStats() {
        return;
        writer << "PATH_UNIQUE_NODES " << visitedNodePerPath.size() << '\n'
               << "PATH_UNIQUE_PRIMS " << visitedPrimPerPath.size() << '\n'
               << "PATH_NODES " << visitedNodePerPathCount << '\n'
               << "PATH_PRIMS " << visitedPrimPerPathCount << '\n'
               << "PATH_PATH_LEN " << pathLength << '\n';
    }

    void writeTileStats() {
        writer << "TILE_UNIQUE_NODES " << visitedNodePerTile.size() << '\n'
               << "TILE_UNIQUE_PRIMS " << visitedPrimPerTile.size() << '\n'
               << "TILE_NODES " << visitedNodePerTileCount << '\n'
               << "TILE_PRIMS " << visitedPrimPerTileCount << '\n';
    }
};

namespace global {
extern thread_local SadjadProfile _sfp_;
}

#endif /* PBRT_SADJAD_STATS_H */
