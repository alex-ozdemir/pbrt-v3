#include "lambda-worker.h"

#include <getopt.h>
#include <glog/logging.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/timerfd.h>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "cloud/bvh.h"
#include "cloud/integrator.h"
#include "cloud/manager.h"
#include "cloud/raystate.h"
#include "cloud/stats.h"
#include "core/camera.h"
#include "core/geometry.h"
#include "core/light.h"
#include "core/sampler.h"
#include "core/spectrum.h"
#include "core/transform.h"
#include "execution/loop.h"
#include "execution/meow/message.h"
#include "messages/utils.h"
#include "net/address.h"
#include "net/requests.h"
#include "storage/backend.h"
#include "util/exception.h"
#include "util/path.h"
#include "util/random.h"
#include "util/system_runner.h"
#include "util/temp_dir.h"
#include "util/temp_file.h"

using namespace std;
using namespace chrono;
using namespace meow;
using namespace pbrt;
using namespace pbrt::global;
using namespace PollerShortNames;

using OpCode = Message::OpCode;
using PollerResult = Poller::Result::Type;

constexpr size_t UDP_MTU_BYTES{1'400};
constexpr milliseconds PEER_CHECK_INTERVAL{1'000};
constexpr milliseconds WORKER_STATS_INTERVAL{500};
constexpr milliseconds WORKER_DIAGNOSTICS_INTERVAL{2'000};
constexpr char LOG_STREAM_ENVAR[] = "AWS_LAMBDA_LOG_STREAM_NAME";

LambdaWorker::LambdaWorker(const string& coordinatorIP,
                           const uint16_t coordinatorPort,
                           const string& storageUri, const bool sendReliably)
    : sendReliably(sendReliably),
      coordinatorAddr(coordinatorIP, coordinatorPort),
      workingDirectory("/tmp/pbrt-worker"),
      storageBackend(StorageBackend::create_backend(storageUri)),
      peerTimer(PEER_CHECK_INTERVAL),
      workerStatsTimer(WORKER_STATS_INTERVAL),
      workerDiagnosticsTimer(WORKER_DIAGNOSTICS_INTERVAL) {
    cerr << "* starting worker in " << workingDirectory.name() << endl;

    roost::chdir(workingDirectory.name());

    FLAGS_log_dir = ".";
    google::InitGoogleLogging(logBase.c_str());
    diagnosticsOstream.open(diagnosticsName, ios::out | ios::trunc);

    diagnosticsOstream << "start "
                       << duration_cast<microseconds>(
                              workerDiagnostics.startTime.time_since_epoch())
                              .count()
                       << endl;

    PbrtOptions.nThreads = 1;
    manager.init(".");

    bvh = make_shared<CloudBVH>();

    srand(time(nullptr));
    do {
        mySeed = rand();
    } while (mySeed == 0);

    coordinatorConnection = loop.make_connection<TCPConnection>(
        coordinatorAddr,
        [this](shared_ptr<TCPConnection>, string&& data) {
            RECORD_INTERVAL("parseTCP");
            this->tcpMessageParser.parse(data);

            while (!this->tcpMessageParser.empty()) {
                this->messageParser.push(move(this->tcpMessageParser.front()));
                this->tcpMessageParser.pop();
            }

            return true;
        },
        []() { LOG(INFO) << "Connection to coordinator failed."; },
        [this]() { this->terminate(); });

    udpConnection = loop.make_udp_connection(
        [this](shared_ptr<UDPConnection>, Address&& addr, string&& data) {
            RECORD_INTERVAL("parseUDP");
            this->messageParser.parse(data);
            return true;
        },
        []() { LOG(INFO) << "UDP connection to coordinator failed."; },
        [this]() { this->terminate(); }, true);

    /* trace rays */
    loop.poller().add_action(Poller::Action(
        dummyFD, Direction::Out, bind(&LambdaWorker::handleRayQueue, this),
        [this]() { return !rayQueue.empty(); },
        []() { throw runtime_error("ray queue failed"); }));

    /* send processed rays */
    loop.poller().add_action(Poller::Action(
        dummyFD, Direction::Out, bind(&LambdaWorker::handleOutQueue, this),
        [this]() { return outQueueSize > 0; },
        []() { throw runtime_error("out queue failed"); }));

    /* send finished rays */
    /* FIXME we're throwing out finished rays, for now */
    loop.poller().add_action(Poller::Action(
        dummyFD, Direction::Out, bind(&LambdaWorker::handleFinishedQueue, this),
        [this]() { return finishedQueue.size() > 1000; },
        []() { throw runtime_error("finished queue failed"); }));

    /* handle peers */
    loop.poller().add_action(Poller::Action(
        peerTimer.fd, Direction::In, bind(&LambdaWorker::handlePeers, this),
        [this]() { return !peers.empty(); },
        []() { throw runtime_error("peers failed"); }));

    /* handle received messages */
    loop.poller().add_action(Poller::Action(
        dummyFD, Direction::Out, bind(&LambdaWorker::handleMessages, this),
        [this]() { return !messageParser.empty(); },
        []() { throw runtime_error("messages failed"); }));

    /* request new peers for neighboring treelets */
    loop.poller().add_action(Poller::Action(
        dummyFD, Direction::Out,
        bind(&LambdaWorker::handleNeededTreelets, this),
        [this]() { return !neededTreelets.empty(); },
        []() { throw runtime_error("treelet request failed"); }));

    /* send updated stats */
    loop.poller().add_action(Poller::Action(
        workerStatsTimer.fd, Direction::In,
        bind(&LambdaWorker::handleWorkerStats, this), [this]() { return true; },
        []() { throw runtime_error("worker stats failed"); }));

    /* record diagnostics */
    loop.poller().add_action(Poller::Action(
        workerDiagnosticsTimer.fd, Direction::In,
        bind(&LambdaWorker::handleDiagnostics, this), [this]() { return true; },
        []() { throw runtime_error("handle diagnostics failed"); }));

    coordinatorConnection->enqueue_write(
        Message(OpCode::Hey, safe_getenv_or(LOG_STREAM_ENVAR, "")).str());
}

Message LambdaWorker::createConnectionRequest(const Worker& peer) {
    protobuf::ConnectRequest proto;
    proto.set_worker_id(*workerId);
    proto.set_my_seed(mySeed);
    proto.set_your_seed(peer.seed);
    return {OpCode::ConnectionRequest, protoutil::to_string(proto)};
}

Message LambdaWorker::createConnectionResponse(const Worker& peer) {
    protobuf::ConnectResponse proto;
    proto.set_worker_id(*workerId);
    proto.set_my_seed(mySeed);
    proto.set_your_seed(peer.seed);
    for (const auto& treeletId : treeletIds) {
        proto.add_treelet_ids(treeletId);
    }
    return {OpCode::ConnectionResponse, protoutil::to_string(proto)};
}

ResultType LambdaWorker::handleRayQueue() {
    RECORD_INTERVAL("handleRayQueue");
    deque<RayState> processedRays;

    constexpr size_t MAX_RAYS = 20'000;

    for (size_t i = 0; i < MAX_RAYS && !rayQueue.empty(); i++) {
        RayState ray = popRayQueue();
        if (!ray.toVisit.empty()) {
            const uint32_t rayTreelet = ray.toVisit.back().treelet;
            auto newRay = CloudIntegrator::Trace(move(ray), bvh);

            const bool hit = newRay.hit.initialized();
            const bool emptyVisit = newRay.toVisit.empty();

            if (newRay.isShadowRay) {
                if (hit || emptyVisit) {
                    newRay.Ld = hit ? 0.f : newRay.Ld;
                    finishedQueue.push_back(move(newRay));
                } else {
                    processedRays.push_back(move(newRay));
                }
            } else if (!emptyVisit || hit) {
                processedRays.push_back(move(newRay));
            } else if (emptyVisit) {
                newRay.Ld = 0.f;
                finishedQueue.push_back(move(newRay));
                workerStats.recordFinishedPath();
            }
        } else if (ray.hit.initialized()) {
            auto newRays =
                CloudIntegrator::Shade(move(ray), bvh, lights, sampler, arena);
            for (auto& newRay : newRays) {
                processedRays.push_back(move(newRay));
            }
        } else {
            throw runtime_error("invalid ray in ray queue");
        }
    }

    while (!processedRays.empty()) {
        RayState ray = move(processedRays.front());
        processedRays.pop_front();

        const TreeletId nextTreelet = ray.currentTreelet();
        workerStats.recordDemandedRay(
            ObjectKey{ObjectType::Treelet, nextTreelet});

        if (treeletIds.count(nextTreelet)) {
            pushRayQueue(move(ray));
        } else {
            if (treeletToWorker.count(nextTreelet)) {
                workerStats.recordSendingRay(
                    ObjectKey{ObjectType::Treelet, nextTreelet});
                outQueue[nextTreelet].push_back(move(ray));
                outQueueSize++;
            } else {
                workerStats.recordPendingRay(
                    ObjectKey{ObjectType::Treelet, nextTreelet});
                neededTreelets.insert(nextTreelet);
                pendingQueue[nextTreelet].push_back(move(ray));
                pendingQueueSize++;
            }
        }
    }

    return ResultType::Continue;
}

ResultType LambdaWorker::handleOutQueue() {
    RECORD_INTERVAL("handleOutQueue");

    for (auto& q : outQueue) {
        if (q.second.empty()) continue;

        auto& workerCandidates = treeletToWorker[q.first];
        auto& peer = peers.at(
            *random::sample(workerCandidates.begin(), workerCandidates.end()));

        string unpackedRay;

        while (!q.second.empty() || !unpackedRay.empty()) {
            ostringstream oss;
            size_t packetLen = 5;

            {
                protobuf::RecordWriter writer{&oss};

                if (!unpackedRay.empty()) {
                    writer.write(unpackedRay);
                    unpackedRay.clear();
                }

                while (packetLen < UDP_MTU_BYTES && !q.second.empty()) {
                    RayState ray = move(q.second.front());
                    q.second.pop_front();

                    string rayStr = protoutil::to_string(to_protobuf(ray));

                    outQueueSize--;
                    workerStats.recordSentRay(
                        ObjectKey{ObjectType::Treelet, q.first});

                    const size_t len = rayStr.length() + 4;
                    if (len + packetLen > UDP_MTU_BYTES) {
                        unpackedRay.swap(rayStr);
                        break;
                    }

                    packetLen += len;
                    writer.write(rayStr);
                }
            }

            oss.flush();
            Message message{OpCode::SendRays, oss.str()};
            auto messageStr = message.str();
            udpConnection->enqueue_datagram(
                peer.address, move(messageStr), PacketPriority::Normal,
                sendReliably ? PacketType::Reliable : PacketType::Unreliable);
        }
    }

    return ResultType::Continue;
}

ResultType LambdaWorker::handleFinishedQueue() {
    RECORD_INTERVAL("handleFinishedQueue");
    finishedQueue.clear();
    return ResultType::Continue;
}

ResultType LambdaWorker::handlePeers() {
    RECORD_INTERVAL("handlePeers");
    peerTimer.reset();

    for (auto& kv : peers) {
        auto& peerId = kv.first;
        auto& peer = kv.second;

        switch (peer.state) {
        case Worker::State::Connecting: {
            auto message = createConnectionRequest(peer);
            udpConnection->enqueue_datagram(peer.address, message.str(),
                                            PacketPriority::High);
            peer.tries++;
            break;
        }

        case Worker::State::Connected:
            /* send keep alive */
            break;
        }
    }

    return ResultType::Continue;
}

ResultType LambdaWorker::handleMessages() {
    RECORD_INTERVAL("handleMessages");
    MessageParser unprocessedMessages;
    while (!messageParser.empty()) {
        Message message = move(messageParser.front());
        messageParser.pop();

        if (!processMessage(message)) {
            unprocessedMessages.push(move(message));
        }
    }

    swap(messageParser, unprocessedMessages);

    return ResultType::Continue;
}

ResultType LambdaWorker::handleNeededTreelets() {
    RECORD_INTERVAL("handleNeededTreelets");
    for (const auto& treeletId : neededTreelets) {
        if (requestedTreelets.count(treeletId)) {
            continue;
        }

        protobuf::GetWorker proto;
        proto.set_treelet_id(treeletId);
        Message message(OpCode::GetWorker, protoutil::to_string(proto));
        coordinatorConnection->enqueue_write(message.str());
        requestedTreelets.insert(treeletId);
    }

    neededTreelets.clear();
    return ResultType::Continue;
}

ResultType LambdaWorker::handleWorkerStats() {
    RECORD_INTERVAL("handleWorkerStats");
    workerStatsTimer.reset();

    auto& qStats = workerStats.queueStats;
    qStats.ray = rayQueue.size();
    qStats.finished = finishedQueue.size();
    qStats.pending = pendingQueueSize;
    qStats.out = outQueueSize;
    qStats.connecting =
        count_if(peers.begin(), peers.end(), [](const auto& peer) {
            return peer.second.state == Worker::State::Connecting;
        });
    qStats.connected = peers.size() - qStats.connecting;
    qStats.outstandingUdp = this->udpConnection->queue_size();

    auto proto = to_protobuf(workerStats);
    Message message{OpCode::WorkerStats, protoutil::to_string(proto)};
    coordinatorConnection->enqueue_write(message.str());
    workerStats.reset();
    return ResultType::Continue;
}

ResultType LambdaWorker::handleDiagnostics() {
    RECORD_INTERVAL("handleDiagnostics");
    workerDiagnosticsTimer.reset();

    workerDiagnostics.bytesSent =
        this->udpConnection->bytes_sent - lastDiagnostics.bytesSent;

    workerDiagnostics.bytesReceived =
        this->udpConnection->bytes_received - lastDiagnostics.bytesReceived;

    workerDiagnostics.outstandingUdp = this->udpConnection->queue_size();

    lastDiagnostics.bytesSent = this->udpConnection->bytes_sent;
    lastDiagnostics.bytesReceived = this->udpConnection->bytes_received;

    const auto timestamp =
        duration_cast<microseconds>(now() - workerDiagnostics.startTime)
            .count();

    diagnosticsOstream << timestamp << " "
                       << protoutil::to_json(to_protobuf(workerDiagnostics))
                       << endl;

    workerDiagnostics.reset();

    return ResultType::Continue;
}

void LambdaWorker::generateRays(const Bounds2i& bounds) {
    const Bounds2i sampleBounds = camera->film->GetSampleBounds();
    const Vector2i sampleExtent = sampleBounds.Diagonal();
    const uint8_t maxDepth = 5;
    const auto samplesPerPixel = sampler->samplesPerPixel;
    const Float rayScale = 1 / sqrt((Float)samplesPerPixel);

    for (size_t sample = 0; sample < sampler->samplesPerPixel; sample++) {
        for (const Point2i pixel : bounds) {
            sampler->StartPixel(pixel);
            if (!InsideExclusive(pixel, sampleBounds)) continue;
            sampler->SetSampleNumber(sample);

            CameraSample cameraSample = sampler->GetCameraSample(pixel);

            RayState state;
            state.sample.id =
                (pixel.x + pixel.y * sampleExtent.x) * samplesPerPixel + sample;
            state.sample.num = sample;
            state.sample.pixel = pixel;
            state.sample.pFilm = cameraSample.pFilm;
            state.sample.weight =
                camera->GenerateRayDifferential(cameraSample, &state.ray);
            state.ray.ScaleDifferentials(rayScale);
            state.remainingBounces = maxDepth;
            state.StartTrace();

            pushRayQueue(move(state));
        }
    }
}

void LambdaWorker::getObjects(const protobuf::GetObjects& objects) {
    vector<storage::GetRequest> requests;
    for (const protobuf::ObjectKey& objectKey : objects.object_ids()) {
        const ObjectKey id = from_protobuf(objectKey);
        if (id.type == ObjectType::TriangleMesh) {
            /* triangle meshes are packed into treelets, so ignore */
            continue;
        }
        if (id.type == ObjectType::Treelet) {
            treeletIds.insert(id.id);
        }
        const string filePath = id.to_string();
        requests.emplace_back(filePath, filePath);
    }
    storageBackend->get(requests);
}

void LambdaWorker::pushRayQueue(RayState&& state) {
    workerStats.recordWaitingRay(
        ObjectKey{ObjectType::Treelet, state.currentTreelet()});
    rayQueue.push_back(move(state));
}

RayState LambdaWorker::popRayQueue() {
    RayState state = move(rayQueue.front());
    rayQueue.pop_front();

    workerStats.recordProcessedRay(
        ObjectKey{ObjectType::Treelet, state.currentTreelet()});

    return state;
}

bool LambdaWorker::processMessage(const Message& message) {
    /* cerr << "[msg:" << Message::OPCODE_NAMES[to_underlying(message.opcode())]
         << "]\n"; */

    switch (message.opcode()) {
    case OpCode::Hey: {
        workerId.reset(stoull(message.payload()));
        outputName = to_string(*workerId) + ".rays";

        cerr << "worker-id=" << *workerId << endl;

        Address addrCopy{coordinatorAddr};
        peers.emplace(0, Worker{0, move(addrCopy)});

        /* send connection request */
        Message connRequest = createConnectionRequest(peers.at(0));
        udpConnection->enqueue_datagram(coordinatorAddr, connRequest.str(),
                                        PacketPriority::High);

        break;
    }

    case OpCode::Ping: {
        Message pong{OpCode::Pong, ""};
        coordinatorConnection->enqueue_write(pong.str());
        break;
    }

    case OpCode::GetObjects: {
        protobuf::GetObjects proto;
        protoutil::from_string(message.payload(), proto);
        getObjects(proto);
        initializeScene();
        break;
    }

    case OpCode::GenerateRays: {
        RECORD_INTERVAL("generateRays");
        protobuf::GenerateRays proto;
        protoutil::from_string(message.payload(), proto);
        generateRays(from_protobuf(proto.crop_window()));
        break;
    }

    case OpCode::ConnectTo: {
        protobuf::ConnectTo proto;
        protoutil::from_string(message.payload(), proto);

        if (peers.count(proto.worker_id()) == 0) {
            const auto dest = Address::decompose(proto.address());
            peers.emplace(proto.worker_id(),
                          Worker{proto.worker_id(), {dest.first, dest.second}});
        }

        break;
    }

    case OpCode::ConnectionRequest: {
        protobuf::ConnectRequest proto;
        protoutil::from_string(message.payload(), proto);

        const auto otherWorkerId = proto.worker_id();
        if (peers.count(otherWorkerId) == 0) {
            /* we haven't heard about this peer from the master, let's process
             * it later */
            return false;
        }

        auto& peer = peers.at(otherWorkerId);
        auto message = createConnectionResponse(peer);
        udpConnection->enqueue_datagram(peer.address, message.str(),
                                        PacketPriority::High);
        break;
    }

    case OpCode::ConnectionResponse: {
        protobuf::ConnectResponse proto;
        protoutil::from_string(message.payload(), proto);

        const auto otherWorkerId = proto.worker_id();
        if (peers.count(otherWorkerId) == 0) {
            /* we don't know about this worker */
            return true;
        }

        auto& peer = peers.at(otherWorkerId);
        peer.seed = proto.my_seed();
        if (peer.state != Worker::State::Connected &&
            proto.your_seed() == mySeed) {
            peer.state = Worker::State::Connected;

            for (const auto treeletId : proto.treelet_ids()) {
                peer.treelets.insert(treeletId);
                treeletToWorker[treeletId].push_back(otherWorkerId);
                requestedTreelets.erase(treeletId);

                if (pendingQueue.count(treeletId)) {
                    auto& treeletPending = pendingQueue[treeletId];
                    auto& treeletOut = outQueue[treeletId];

                    outQueueSize += treeletPending.size();
                    pendingQueueSize -= treeletPending.size();

                    while (!treeletPending.empty()) {
                        workerStats.recordSendingRay(
                            ObjectKey{ObjectType::Treelet, treeletId});
                        treeletOut.push_back(move(treeletPending.front()));
                        treeletPending.pop_front();
                    }
                }
            }
        }

        break;
    }

    case OpCode::SendRays: {
        protobuf::RecordReader reader{istringstream{message.payload()}};
        protobuf::RayState proto;

        while (!reader.eof()) {
            if (reader.read(&proto)) {
                ObjectKey treeletID;
                if (proto.to_visit_size() > 0) {
                    treeletID = {ObjectType::Treelet,
                                 proto.to_visit(0).treelet()};
                } else {
                    treeletID = {ObjectType::Treelet, proto.hit().treelet()};
                }
                workerStats.recordReceivedRay(treeletID);
                pushRayQueue(move(from_protobuf(proto)));
            }
        }

        break;
    }

    case OpCode::Bye:
        terminate();
        break;

    default:
        throw runtime_error("unhandled message opcode");
    }

    return true;
}

void LambdaWorker::run() {
    while (!terminated) {
        auto res = loop.loop_once().result;
        if (res != PollerResult::Success && res != PollerResult::Timeout) break;
    }
}

void LambdaWorker::loadCamera() {
    auto reader = manager.GetReader(ObjectType::Camera);
    protobuf::Camera proto_camera;
    reader->read(&proto_camera);
    camera = camera::from_protobuf(proto_camera, transformCache);
    filmTile = camera->film->GetFilmTile(camera->film->GetSampleBounds());
}

void LambdaWorker::loadSampler() {
    auto reader = manager.GetReader(ObjectType::Sampler);
    protobuf::Sampler proto_sampler;
    reader->read(&proto_sampler);
    sampler = sampler::from_protobuf(proto_sampler);

    /* if (workerId.initialized()) {
        sampler = sampler->Clone(*workerId);
    } */
}

void LambdaWorker::loadLights() {
    auto reader = manager.GetReader(ObjectType::Lights);
    while (!reader->eof()) {
        protobuf::Light proto_light;
        reader->read(&proto_light);
        lights.push_back(move(light::from_protobuf(proto_light)));
    }
}

void LambdaWorker::loadFakeScene() {
    auto reader = manager.GetReader(ObjectType::Scene);
    protobuf::Scene proto_scene;
    reader->read(&proto_scene);
    fakeScene = make_unique<Scene>(from_protobuf(proto_scene));
}

void LambdaWorker::initializeScene() {
    if (initialized) return;

    loadCamera();
    loadSampler();
    loadLights();
    loadFakeScene();

    for (auto& light : lights) {
        light->Preprocess(*fakeScene);
    }

    initialized = true;
}

void LambdaWorker::uploadLogs() {
    if (!workerId.initialized()) return;

    google::FlushLogFiles(google::INFO);
    diagnosticsOstream.close();

    vector<storage::PutRequest> putLogsRequest = {
        {infoLogName, logPrefix + to_string(*workerId)},
        {diagnosticsName, logPrefix + to_string(*workerId) + ".DIAG"}};

    storageBackend->put(putLogsRequest);
}

void usage(const char* argv0, int exitCode) {
    cerr << "Usage: " << argv0 << " [OPTIONS]" << endl
         << endl
         << "Options:" << endl
         << "  -i --ip IPSTRING           ip of coordinator" << endl
         << "  -p --port PORT             port of coordinator" << endl
         << "  -s --storage-backend NAME  storage backend URI" << endl
         << "  -R --reliable-udp          send ray packets reliably" << endl
         << "  -h --help                  show help information" << endl;
}

int main(int argc, char* argv[]) {
    int exit_status = EXIT_SUCCESS;

    uint16_t listenPort = 50000;
    string publicIp;
    string storageUri;
    bool sendReliably = false;

    struct option long_options[] = {
        {"port", required_argument, nullptr, 'p'},
        {"ip", required_argument, nullptr, 'i'},
        {"storage-backend", required_argument, nullptr, 's'},
        {"reliable-udp", no_argument, nullptr, 'R'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    while (true) {
        const int opt =
            getopt_long(argc, argv, "p:i:s:hR", long_options, nullptr);

        if (opt == -1) break;

        // clang-format off
        switch (opt) {
        case 'p': listenPort = stoi(optarg); break;
        case 'i': publicIp = optarg; break;
        case 's': storageUri = optarg; break;
        case 'R': sendReliably = true; break;
        case 'h': usage(argv[0], EXIT_SUCCESS); break;
        default: usage(argv[0], EXIT_FAILURE);
        }
        // clang-format on
    }

    if (listenPort == 0 || publicIp.empty() || storageUri.empty()) {
        usage(argv[0], EXIT_FAILURE);
    }

    unique_ptr<LambdaWorker> worker;

    try {
        worker = make_unique<LambdaWorker>(publicIp, listenPort, storageUri,
                                           sendReliably);
        worker->run();
    } catch (const exception& e) {
        LOG(INFO) << argv[0] << ": " << e.what();
        exit_status = EXIT_FAILURE;
    }

    if (worker) {
        worker->uploadLogs();
    }

    return exit_status;
}
