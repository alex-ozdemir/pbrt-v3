#ifndef PBRT_CLOUD_STATS_H
#define PBRT_CLOUD_STATS_H

#include <chrono>
#include <cstdint>

#include "cloud/estimators.h"
#include "cloud/lambda.h"
#include "cloud/manager.h"
#include "cloud/raystate.h"

namespace pbrt {

/* timing utility functions */
using timepoint_t = std::chrono::time_point<std::chrono::system_clock>;
inline timepoint_t now() { return std::chrono::system_clock::now(); };

#define PER_RAY_STATS
// #define PER_INTERVAL_STATS
#define RECORD_METRICS

constexpr double RAY_PERCENTILES[] = {0.5, 0.9, 0.99, 0.999, 0.9999};
constexpr size_t NUM_PERCENTILES = sizeof(RAY_PERCENTILES) / sizeof(double);

struct RayStats {
    /* rays sent to this scene object */
    uint64_t sentRays{0};
    /* rays received for this scene object */
    uint64_t receivedRays{0};
    /* rays waiting to be processed for this scene object */
    uint64_t waitingRays{0};
    /* rays processed for this scene object */
    uint64_t processedRays{0};
    /* rays that require (or required) this scence object to render */
    uint64_t demandedRays{0};
    /* rays that are waiting to be sent to another worker */
    uint64_t sendingRays{0};
    /* rays that are waiting to be sent to another worker, but we do not know
       which worker to send them to */
    uint64_t pendingRays{0};

    void reset();
    void merge(const RayStats& other);
};

struct QueueStats {
    uint64_t ray{0};
    uint64_t finished{0};
    uint64_t pending{0};
    uint64_t out{0};
    uint64_t connecting{0};
    uint64_t connected{0};
    uint64_t outstandingUdp{0};
    uint64_t queuedUdp{0};
};

struct WorkerStats {
    /* required stats */
    uint64_t _finishedPaths{0};
    RayStats aggregateStats;
    QueueStats queueStats;
    std::map<ObjectKey, RayStats> objectStats;

    uint64_t finishedPaths() const { return _finishedPaths; }
    uint64_t sentRays() const { return aggregateStats.sentRays; }
    uint64_t receivedRays() const { return aggregateStats.receivedRays; }
    uint64_t waitingRays() const { return aggregateStats.waitingRays; }
    uint64_t processedRays() const { return aggregateStats.processedRays; }
    uint64_t sendingRays() const { return aggregateStats.sendingRays; }
    uint64_t pendingRays() const { return aggregateStats.pendingRays; }

    void recordFinishedPath();
    void recordSentRay(const RayState& ray);
    void recordReceivedRay(const RayState& ray);
    void recordWaitingRay(const RayState& ray);
    void recordProcessedRay(const RayState& ray);
    void recordDemandedRay(const RayState& ray);
    void recordSendingRay(const RayState& ray);
    void recordPendingRay(const RayState& ray);

    void reset();
    void merge(const WorkerStats& other);
};

struct WorkerDiagnostics {
    const timepoint_t startTime{now()};

    /* diagnostic stats */
    uint64_t bytesSent{0};
    uint64_t bytesReceived{0};
    uint64_t outstandingUdp{0};

    std::map<std::string, double> timePerAction;
    std::map<std::string, std::vector<std::tuple<uint64_t, uint64_t>>>
        intervalsPerAction;
    std::map<std::string, std::vector<std::tuple<uint64_t, double>>>
        metricsOverTime;

    /* used for nesting interval names */
    std::vector<std::string> nameStack;

    /* for recording action intervals */
    class Recorder {
      public:
        ~Recorder();

      private:
        friend WorkerDiagnostics;

        Recorder(WorkerDiagnostics& diagnostics, const std::string& name);

        WorkerDiagnostics& diagnostics;
        std::string name;
        timepoint_t start{now()};
    };

    Recorder recordInterval(const std::string& name);

    void recordMetric(const std::string& name, timepoint_t time, double metric);
    void reset();
};

class DemandTracker {
  public:
    DemandTracker();
    void submit(WorkerId wid, const WorkerStats& stats);
    double workerDemand(WorkerId wid) const;
    double treeletDemand(TreeletId tid) const;
    double workerTreeletDemand(WorkerId wid, TreeletId tid) const;
    double netDemand() const;

  private:
    std::map<std::pair<WorkerId, TreeletId>, RateEstimator<double>> estimators;
    std::map<WorkerId, double> byWorker;
    std::map<TreeletId, double> byTreelet;
    double total;
};

namespace global {
extern WorkerStats workerStats;
extern WorkerDiagnostics workerDiagnostics;
}  // namespace global

#define RECORD_INTERVAL(x) \
    auto __REC__ = pbrt::global::workerDiagnostics.recordInterval(x)

}  // namespace pbrt

#endif /* PBRT_CLOUD_STATS_H */
