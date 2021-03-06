#include "stats.h"

#include <math.h>
#include <iomanip>

using namespace std;
using namespace chrono;

namespace pbrt {
namespace global {
WorkerStats workerStats;
WorkerDiagnostics workerDiagnostics;
}  // namespace global

void RayStats::reset() {
    sentRays = 0;
    receivedRays = 0;
    waitingRays = 0;
    processedRays = 0;
    demandedRays = 0;
    sendingRays = 0;
    pendingRays = 0;
}

void RayStats::merge(const RayStats& other) {
    sentRays += other.sentRays;
    receivedRays += other.receivedRays;
    waitingRays += other.waitingRays;
    processedRays += other.processedRays;
    demandedRays += other.demandedRays;
    sendingRays += other.sendingRays;
    pendingRays += other.pendingRays;
}

#define INCREMENT_FIELD(name__)                                           \
    do {                                                                  \
        aggregateStats.name__ += 1;                                       \
        objectStats[ObjectKey{ObjectType::Treelet, ray.CurrentTreelet()}] \
            .name__ += 1;                                                 \
    } while (false)

void WorkerStats::recordFinishedPath() { _finishedPaths += 1; }

void WorkerStats::recordSentRay(const RayState& ray) {
    INCREMENT_FIELD(sentRays);
}

void WorkerStats::recordReceivedRay(const RayState& ray) {
    INCREMENT_FIELD(receivedRays);
}

void WorkerStats::recordWaitingRay(const RayState& ray) {
    INCREMENT_FIELD(waitingRays);
}

void WorkerStats::recordProcessedRay(const RayState& ray) {
    INCREMENT_FIELD(processedRays);
}

void WorkerStats::recordDemandedRay(const RayState& ray) {
    INCREMENT_FIELD(demandedRays);
}

void WorkerStats::recordSendingRay(const RayState& ray) {
    INCREMENT_FIELD(sendingRays);
}

void WorkerStats::recordPendingRay(const RayState& ray) {
    INCREMENT_FIELD(pendingRays);
}

#undef INCREMENT_FIELD

void WorkerStats::reset() {
    _finishedPaths = 0;
    aggregateStats.reset();
    queueStats = {};
    objectStats.clear();
}

void WorkerStats::merge(const WorkerStats& other) {
    _finishedPaths += other._finishedPaths;
    aggregateStats.merge(other.aggregateStats);
    queueStats = other.queueStats;
    for (const auto& kv : other.objectStats) {
        objectStats[kv.first].merge(kv.second);
    }
}

/* WorkerDiagnostics */

WorkerDiagnostics::Recorder::Recorder(WorkerDiagnostics& diagnostics,
                                      const string& name)
    : diagnostics(diagnostics), name(name) {}

WorkerDiagnostics::Recorder::~Recorder() {
    auto end = now();
    diagnostics.timePerAction[name] +=
        duration_cast<microseconds>((end - start)).count();

#ifdef PER_INTERVAL_STATS
    diagnostics.intervalsPerAction[name].push_back(make_tuple(
        duration_cast<microseconds>((start - diagnostics.startTime)).count(),
        duration_cast<microseconds>((end - diagnostics.startTime)).count()));
#endif
    diagnostics.nameStack.pop_back();
}

void WorkerDiagnostics::reset() {
    bytesReceived = 0;
    bytesSent = 0;

    timePerAction.clear();
    intervalsPerAction.clear();
    metricsOverTime.clear();
}

WorkerDiagnostics::Recorder WorkerDiagnostics::recordInterval(
    const std::string& name) {
    nameStack.push_back(name);
    std::string recorderName = "";
    for (const auto& n : nameStack) {
        recorderName += n + ":";
    }
    recorderName.resize(recorderName.size() - 1);
    return Recorder(*this, recorderName);
}

void WorkerDiagnostics::recordMetric(const string& name, timepoint_t time,
                                     double metric) {
    metricsOverTime[name].push_back(make_tuple(
        (uint64_t)duration_cast<microseconds>(time - startTime).count(),
        metric));
}

DemandTracker::DemandTracker()
    : estimators(), byWorker(), byTreelet(), total(0.0) {}

void DemandTracker::submit(WorkerId wid, const WorkerStats& stats) {
    for (const auto& kv : stats.objectStats) {
        if (kv.first.type == ObjectType::Treelet) {
            TreeletId tid = kv.first.id;
            double oldRate = workerTreeletDemand(wid, tid);
            RateEstimator<double>& estimator = estimators[make_pair(wid, tid)];

            estimator.update(double(kv.second.demandedRays));
            double rateChange = workerTreeletDemand(wid, tid) - oldRate;
            total += rateChange;
            byWorker[wid] += rateChange;
            byTreelet[tid] += rateChange;
        }
    }
}

double DemandTracker::workerDemand(WorkerId wid) const {
    if (byWorker.count(wid)) {
        return byWorker.at(wid);
    } else {
        return 0.0;
    }
}

double DemandTracker::treeletDemand(TreeletId tid) const {
    if (byTreelet.count(tid)) {
        return byTreelet.at(tid);
    } else {
        return 0.0;
    }
}

double DemandTracker::workerTreeletDemand(WorkerId wid, TreeletId tid) const {
    const auto key = make_pair(wid, tid);
    if (estimators.count(key)) {
        return estimators.at(key).getRate();
    } else {
        return 0.0;
    }
}

double DemandTracker::netDemand() const { return total; }
}  // namespace pbrt
