#include "delivery_module_plugin.h"
#include <algorithm>
#include <atomic>
#include <climits>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <system_error>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <boost/beast/core/detail/base64.hpp>

#include "api_call_handler.h"
#include <liblogosdelivery_kernel.h>

namespace {
namespace b64 = boost::beast::detail::base64;
using nlohmann::json;

constexpr std::size_t MAX_NODE_LIFECYCLE_REQUEST_BYTES = 65536;
constexpr std::size_t MAX_NODE_LIFECYCLE_CONFIG_BYTES = 49152;
constexpr std::size_t MAX_NODE_LIFECYCLE_OPERATION_ID_BYTES = 128;
constexpr std::size_t MAX_COMPLETED_NODE_LIFECYCLE_OPERATIONS = 128;
constexpr const char* NODE_LIFECYCLE_SNAPSHOT_SCHEMA =
    "logos.managed_node_lifecycle.snapshot";
constexpr const char* NODE_LIFECYCLE_COMMAND_SCHEMA =
    "logos.managed_node_lifecycle.command";
constexpr const char* NODE_LIFECYCLE_ACK_SCHEMA =
    "logos.managed_node_lifecycle.ack";
constexpr const char* NODE_LIFECYCLE_EVENT_SCHEMA =
    "logos.managed_node_lifecycle.event";
std::atomic<std::uint64_t> nextNodeLifecycleInstanceSerial{0};

std::string base64Encode(const std::vector<uint8_t>& data) {
    std::string out;
    out.resize(b64::encoded_size(data.size()));
    out.resize(b64::encode(out.data(), data.data(), data.size()));
    return out;
}

std::vector<uint8_t> base64Decode(const std::string& encoded) {
    std::vector<uint8_t> out;
    out.resize(b64::decoded_size(encoded.size()));
    auto [written, read] = b64::decode(out.data(), encoded.data(), encoded.size());
    out.resize(written);
    return out;
}

int64_t currentTimestampNs() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + static_cast<int64_t>(ts.tv_nsec);
}

std::int64_t nodeLifecycleTimestampMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string makeNodeLifecycleInstanceId() {
    const auto serial = nextNodeLifecycleInstanceSerial.fetch_add(1) + 1;
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return "delivery-" + std::to_string(now) + "-" + std::to_string(serial);
}

bool isValidNodeLifecycleOperationId(const std::string& operationId) {
    if (operationId.empty()
        || operationId.size() > MAX_NODE_LIFECYCLE_OPERATION_ID_BYTES) {
        return false;
    }
    return std::all_of(operationId.begin(), operationId.end(), [](unsigned char value) {
        return value >= 0x21 && value <= 0x7e;
    });
}

bool containsEmbeddedNul(const std::string& value) {
    return value.find('\0') != std::string::npos;
}

bool parseLifecycleUnsigned(const json& value, std::uint64_t& parsed) {
    if (value.is_number_unsigned()) {
        parsed = value.get<std::uint64_t>();
        return true;
    }
    if (value.is_number_integer()) {
        const auto signedValue = value.get<std::int64_t>();
        if (signedValue >= 0) {
            parsed = static_cast<std::uint64_t>(signedValue);
            return true;
        }
    }
    return false;
}

bool isLifecycleVersionOne(const json& value) {
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>() == 1;
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>() == 1;
    }
    return false;
}

json nodeLifecycleError(const std::string& code,
                        const std::string& message,
                        std::int64_t occurredAtMs) {
    return {
        {"code", code},
        {"message", message},
        {"occurred_at_ms", occurredAtMs},
    };
}
} // namespace

void DeliveryModuleImpl::start_callback(int callerRet, const char* msg, size_t len, void* userData)
{
    auto* impl = static_cast<DeliveryModuleImpl*>(userData);
    if (!impl) return;
    impl->settleLifecycleCallback("start", callerRet == RET_OK);
    impl->nodeStarted(callerRet == RET_OK,
                      (msg && len > 0) ? std::string(msg, len) : std::string(),
                      currentTimestampNs());
}

void DeliveryModuleImpl::stop_callback(int callerRet, const char* msg, size_t len, void* userData)
{
    auto* impl = static_cast<DeliveryModuleImpl*>(userData);
    if (!impl) return;
    impl->settleLifecycleCallback("stop", callerRet == RET_OK);
    impl->nodeStopped(callerRet == RET_OK,
                      (msg && len > 0) ? std::string(msg, len) : std::string(),
                      currentTimestampNs());
}

DeliveryModuleImpl::DeliveryModuleImpl()
    : deliveryCtx(nullptr),
      lifecycleInstanceId(makeNodeLifecycleInstanceId()),
      lifecycleUpdatedAtMs(nodeLifecycleTimestampMs())
{
    fprintf(stderr, "DeliveryModuleImpl: Initializing...\n");
    fprintf(stderr, "DeliveryModuleImpl: Initialized successfully\n");
}

DeliveryModuleImpl::~DeliveryModuleImpl()
{
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        ++lifecycleGeneration;
        lifecyclePending = false;
        lifecycleState = LifecycleState::Uninitialized;
    }
    std::thread initializeWorker;
    {
        std::lock_guard<std::mutex> lock(lifecycleWorkerMutex);
        initializeWorker = std::move(lifecycleInitializeWorker);
    }
    if (initializeWorker.joinable()) {
        if (initializeWorker.get_id() == std::this_thread::get_id()) {
            initializeWorker.detach();
        } else {
            initializeWorker.join();
        }
    }
    if (deliveryCtx) {
        logosdelivery_destroy(deliveryCtx, nullptr, nullptr);
        deliveryCtx = nullptr;
    }
}

void DeliveryModuleImpl::event_callback(int callerRet, const char* msg, size_t len, void* userData)
{
    fprintf(stderr, "DeliveryModuleImpl::event_callback called with ret: %d\n", callerRet);

    DeliveryModuleImpl* impl = static_cast<DeliveryModuleImpl*>(userData);
    if (!impl) {
        fprintf(stderr, "DeliveryModuleImpl::event_callback: Invalid userData\n");
        return;
    }

    if (msg && len > 0) {
        std::string message(msg, len);
        fprintf(stderr, "DeliveryModuleImpl::event_callback message: %s\n", message.c_str());

        nlohmann::json jsonObj;
        try {
            jsonObj = nlohmann::json::parse(message);
        } catch (const nlohmann::json::parse_error&) {
            fprintf(stderr, "DeliveryModuleImpl::event_callback: Invalid JSON\n");
            return;
        }

        if (!jsonObj.is_object()) {
            fprintf(stderr, "DeliveryModuleImpl::event_callback: Invalid JSON\n");
            return;
        }

        std::string eventType = jsonObj.value("eventType", "");
        int64_t timestamp = currentTimestampNs();

        if (eventType == "message_sent") {
            impl->messageSent(
                jsonObj.value("requestId", ""),
                jsonObj.value("messageHash", ""),
                timestamp);

        } else if (eventType == "message_error") {
            impl->messageError(
                jsonObj.value("requestId", ""),
                jsonObj.value("messageHash", ""),
                jsonObj.value("error", ""),
                timestamp);

        } else if (eventType == "message_propagated") {
            impl->messagePropagated(
                jsonObj.value("requestId", ""),
                jsonObj.value("messageHash", ""),
                timestamp);

        } else if (eventType == "message_received") {
            auto msgObj = jsonObj.value("message", nlohmann::json::object());

            std::string hash = jsonObj.value("messageHash", "");
            std::string topic = msgObj.value("contentTopic", "");

            std::vector<uint8_t> payloadBytes;
            if (msgObj.contains("payload")) {
                auto& payloadValue = msgObj["payload"];
                if (payloadValue.is_array()) {
                    payloadBytes.reserve(payloadValue.size());
                    for (const auto& val : payloadValue) {
                        payloadBytes.push_back(static_cast<uint8_t>(val.get<int>()));
                    }
                } else if (payloadValue.is_string()) {
                    payloadBytes = base64Decode(payloadValue.get<std::string>());
                }
            }

            int64_t msgTimestamp = static_cast<int64_t>(msgObj.value("timestamp", 0.0));
            impl->messageReceived(hash, topic, payloadBytes, msgTimestamp);

        } else if (eventType == "connection_status_change") {
            impl->connectionStateChanged(
                jsonObj.value("connectionStatus", ""),
                timestamp);

        } else {
            fprintf(stderr, "DeliveryModuleImpl::event_callback: Unknown event type: %s\n", eventType.c_str());
        }
    }
}

// Default every listening port (tcpPort, discv5UdpPort, restPort,
// metricsServerPort, websocketPort) to 0 so the OS assigns an ephemeral port
// when the caller did not pin a specific value. Caller-supplied ports are
// preserved so fleet configs that pin ports keep working. logos-delivery now
// accepts port 0 (status-im/nim-confutils#146), which makes this work.
// See logos-delivery-module#18.
static std::optional<std::string> applyPortDefaults(const std::string& cfg)
{
    nlohmann::json cfgObj;
    try {
        cfgObj = nlohmann::json::parse(cfg);
    } catch (const nlohmann::json::parse_error&) {
        fprintf(stderr, "DeliveryModuleImpl: createNode cfg is not valid JSON\n");
        return std::nullopt;
    }

    if (!cfgObj.is_object()) {
        fprintf(stderr, "DeliveryModuleImpl: createNode cfg is not a JSON object\n");
        return std::nullopt;
    }

    for (const char* portKey : {
             "tcpPort",
             "discv5UdpPort",
             "restPort",
             "metricsServerPort",
             "websocketPort",
         }) {
        if (!cfgObj.contains(portKey)) {
            cfgObj[portKey] = 0;
        }
    }

    return cfgObj.dump();
}

const char* DeliveryModuleImpl::lifecycleStateName(LifecycleState state)
{
    switch (state) {
    case LifecycleState::Uninitialized: return "uninitialized";
    case LifecycleState::Initializing: return "initializing";
    case LifecycleState::Stopped: return "stopped";
    case LifecycleState::Starting: return "starting";
    case LifecycleState::Running: return "running";
    case LifecycleState::Stopping: return "stopping";
    case LifecycleState::Destroying: return "destroying";
    }
    return "uninitialized";
}

std::vector<std::string> DeliveryModuleImpl::lifecycleActions(LifecycleState state)
{
    switch (state) {
    case LifecycleState::Uninitialized:
        return {"initialize"};
    case LifecycleState::Stopped:
        return {"start"};
    case LifecycleState::Running:
        return {"stop"};
    case LifecycleState::Initializing:
    case LifecycleState::Starting:
    case LifecycleState::Stopping:
    case LifecycleState::Destroying:
        return {};
    }
    return {};
}

const char* DeliveryModuleImpl::lifecycleFailureCode(const std::string& action)
{
    if (action == "initialize") return "initialize_failed";
    if (action == "start") return "start_failed";
    if (action == "stop") return "stop_failed";
    return "lifecycle_action_failed";
}

const char* DeliveryModuleImpl::lifecycleFailureMessage(const std::string& action)
{
    if (action == "initialize") return "Delivery initialization failed.";
    if (action == "start") return "Delivery start failed.";
    if (action == "stop") return "Delivery stop failed.";
    return "Delivery lifecycle action failed.";
}

std::string DeliveryModuleImpl::lifecycleSnapshotLocked() const
{
    json snapshot;
    snapshot["schema"] = NODE_LIFECYCLE_SNAPSHOT_SCHEMA;
    snapshot["version"] = 1;
    snapshot["instance_id"] = lifecycleInstanceId;
    snapshot["epoch"] = lifecycleEpoch;
    snapshot["sequence"] = lifecycleSequence;
    snapshot["scope"] = {{"kind", "messaging"}};
    snapshot["state"] = lifecycleStateName(lifecycleState);
    snapshot["health"] = lifecycleError.empty() ? "unknown" : "degraded";
    snapshot["supported_actions"] = lifecycleActions(lifecycleState);
    snapshot["pending_operation"] = nullptr;
    if (lifecyclePending && !activeLifecycleOperationId.empty()) {
        const auto pending = lifecycleOperations.find(activeLifecycleOperationId);
        if (pending != lifecycleOperations.end() && !pending->second.settled) {
            snapshot["pending_operation"] = {
                {"operation_id", activeLifecycleOperationId},
                {"action", pending->second.action},
            };
        }
    }
    snapshot["last_completed_operation"] = nullptr;
    if (!completedLifecycleOperationIds.empty()) {
        const auto completed = lifecycleOperations.find(
            completedLifecycleOperationIds.back());
        if (completed != lifecycleOperations.end() && completed->second.settled) {
            snapshot["last_completed_operation"] = {
                {"operation_id", completedLifecycleOperationIds.back()},
                {"action", completed->second.action},
                {"outcome", completed->second.outcome},
            };
        }
    }
    snapshot["last_error"] = lifecycleError.empty()
        ? json(nullptr)
        : nodeLifecycleError(lifecycleErrorCode, lifecycleError, lifecycleErrorAtMs);
    snapshot["updated_at_ms"] = lifecycleUpdatedAtMs;
    return snapshot.dump();
}

std::string DeliveryModuleImpl::lifecycleEventLocked(
    const std::string& action,
    const std::string& operationId,
    const std::string& phase,
    const std::string& outcome,
    LifecycleState previousState,
    const std::string& errorCode,
    const std::string& errorMessage) const
{
    json event;
    event["schema"] = NODE_LIFECYCLE_EVENT_SCHEMA;
    event["version"] = 1;
    event["instance_id"] = lifecycleInstanceId;
    event["epoch"] = lifecycleEpoch;
    event["sequence"] = lifecycleSequence;
    event["scope"] = {{"kind", "messaging"}};
    event["operation_id"] = operationId.empty() ? json(nullptr) : json(operationId);
    event["action"] = action;
    event["phase"] = phase;
    event["outcome"] = outcome;
    event["previous_state"] = lifecycleStateName(previousState);
    event["status"] = json::parse(lifecycleSnapshotLocked());
    event["error"] = errorCode.empty()
        ? json(nullptr)
        : nodeLifecycleError(errorCode, errorMessage, nodeLifecycleTimestampMs());
    event["emitted_at_ms"] = nodeLifecycleTimestampMs();
    return event.dump();
}

void DeliveryModuleImpl::emitLifecycleEvents(const std::vector<std::string>& events)
{
    for (const auto& event : events) {
        nodeChanged(event);
    }
}

void DeliveryModuleImpl::rememberCompletedLifecycleOperationLocked(
    const std::string& operationId)
{
    if (operationId.empty()) return;
    completedLifecycleOperationIds.push_back(operationId);
    while (completedLifecycleOperationIds.size()
           > MAX_COMPLETED_NODE_LIFECYCLE_OPERATIONS) {
        const std::string expired = completedLifecycleOperationIds.front();
        completedLifecycleOperationIds.pop_front();
        const auto found = lifecycleOperations.find(expired);
        if (found != lifecycleOperations.end() && found->second.settled) {
            lifecycleOperations.erase(found);
        }
    }
}

DeliveryModuleImpl::LifecycleDispatch DeliveryModuleImpl::beginLifecycleAction(
    const std::string& action,
    const std::string& operationId,
    const std::string& requestFingerprint,
    bool hasExpectedSnapshot,
    const std::string& expectedInstanceId,
    std::uint64_t expectedEpoch,
    std::uint64_t expectedSequence,
    bool strictAction)
{
    LifecycleDispatch dispatch;
    dispatch.action = action;
    dispatch.operationId = operationId;

    std::lock_guard<std::mutex> lock(lifecycleMutex);
    const auto acknowledgement = [&](bool accepted, bool duplicate,
                                     const std::string& errorCode,
                                     const std::string& errorMessage) {
        json result;
        result["schema"] = NODE_LIFECYCLE_ACK_SCHEMA;
        result["version"] = 1;
        result["operation_id"] = operationId.empty() ? json(nullptr) : json(operationId);
        result["accepted"] = accepted;
        result["duplicate"] = duplicate;
        result["instance_id"] = lifecycleInstanceId;
        result["epoch"] = lifecycleEpoch;
        result["sequence"] = lifecycleSequence;
        result["state"] = lifecycleStateName(lifecycleState);
        result["error"] = errorCode.empty()
            ? json(nullptr)
            : nodeLifecycleError(errorCode, errorMessage, nodeLifecycleTimestampMs());
        return result.dump();
    };
    const auto settleWithoutDispatch = [&](LifecycleDispatchDisposition disposition,
                                           bool accepted,
                                           const std::string& outcome,
                                           const std::string& errorCode,
                                           const std::string& errorMessage) {
        LifecycleOperation operation;
        operation.action = action;
        operation.requestFingerprint = requestFingerprint;
        operation.previousState = lifecycleState;
        operation.settled = true;
        operation.outcome = outcome;
        const auto inserted = lifecycleOperations.emplace(operationId, std::move(operation));
        LifecycleOperation& stored = inserted.first->second;
        if (accepted) {
            ++lifecycleSequence;
            lifecycleUpdatedAtMs = nodeLifecycleTimestampMs();
            dispatch.events.push_back(lifecycleEventLocked(
                action, operationId, "accepted", "accepted", lifecycleState));
        }
        ++lifecycleSequence;
        lifecycleUpdatedAtMs = nodeLifecycleTimestampMs();
        dispatch.disposition = disposition;
        dispatch.events.push_back(lifecycleEventLocked(
            action, operationId, "settled", outcome, lifecycleState,
            accepted ? std::string() : errorCode,
            accepted ? std::string() : errorMessage));
        dispatch.acknowledgement = acknowledgement(
            accepted, false, accepted ? std::string() : errorCode,
            accepted ? std::string() : errorMessage);
        stored.acknowledgement = dispatch.acknowledgement;
        rememberCompletedLifecycleOperationLocked(operationId);
    };

    if (strictAction) {
        const auto existing = lifecycleOperations.find(operationId);
        if (existing != lifecycleOperations.end()) {
            if (existing->second.requestFingerprint != requestFingerprint) {
                dispatch.disposition = LifecycleDispatchDisposition::Rejected;
                dispatch.acknowledgement = acknowledgement(
                    false, false, "operation_id_conflict",
                    "operation_id was already used for a different request.");
                return dispatch;
            }
            dispatch.disposition = LifecycleDispatchDisposition::Duplicate;
            json duplicateAcknowledgement = json::parse(existing->second.acknowledgement);
            duplicateAcknowledgement["duplicate"] = true;
            dispatch.acknowledgement = duplicateAcknowledgement.dump();
            return dispatch;
        }
        if (lifecyclePending) {
            settleWithoutDispatch(LifecycleDispatchDisposition::Rejected, false,
                                  "rejected", "operation_in_progress",
                                  "A lifecycle operation is already in progress.");
            return dispatch;
        }
        if (hasExpectedSnapshot
            && (expectedInstanceId != lifecycleInstanceId
                || expectedEpoch != lifecycleEpoch
                || expectedSequence != lifecycleSequence)) {
            settleWithoutDispatch(LifecycleDispatchDisposition::Rejected, false,
                                  "rejected", "state_mismatch",
                                  "The lifecycle snapshot is stale.");
            return dispatch;
        }
        if (action == "initialize") {
            if (lifecycleState != LifecycleState::Uninitialized) {
                settleWithoutDispatch(LifecycleDispatchDisposition::Rejected, false,
                                      "rejected", "invalid_state",
                                      "Delivery is already initialized.");
                return dispatch;
            }
        } else if (action == "start") {
            if (lifecycleState == LifecycleState::Running) {
                settleWithoutDispatch(LifecycleDispatchDisposition::Noop, true,
                                      "no_op", {}, {});
                return dispatch;
            }
            if (lifecycleState != LifecycleState::Stopped) {
                settleWithoutDispatch(LifecycleDispatchDisposition::Rejected, false,
                                      "rejected", "invalid_state",
                                      "Delivery must be stopped before it can start.");
                return dispatch;
            }
        } else if (action == "stop") {
            if (lifecycleState == LifecycleState::Stopped) {
                settleWithoutDispatch(LifecycleDispatchDisposition::Noop, true,
                                      "no_op", {}, {});
                return dispatch;
            }
            if (lifecycleState != LifecycleState::Running) {
                settleWithoutDispatch(LifecycleDispatchDisposition::Rejected, false,
                                      "rejected", "invalid_state",
                                      "Delivery is not in a stoppable state.");
                return dispatch;
            }
        }
    }

    if (lifecyclePending) {
        const std::string supersededOperationId = activeLifecycleOperationId;
        const std::string supersededAction = activeLifecycleAction;
        LifecycleState supersededPreviousState = lifecycleState;
        if (!supersededOperationId.empty()) {
            const auto active = lifecycleOperations.find(supersededOperationId);
            if (active != lifecycleOperations.end() && !active->second.settled) {
                supersededPreviousState = active->second.previousState;
                active->second.settled = true;
                active->second.outcome = "failed";
                rememberCompletedLifecycleOperationLocked(supersededOperationId);
            }
        }
        ++lifecycleSequence;
        lifecycleUpdatedAtMs = nodeLifecycleTimestampMs();
        dispatch.events.push_back(lifecycleEventLocked(
            supersededAction, supersededOperationId, "settled", "failed",
            supersededPreviousState, "superseded",
            "Superseded by a legacy lifecycle action."));
    }

    dispatch.previousState = lifecycleState;
    dispatch.generation = ++lifecycleGeneration;
    if (action == "initialize") lifecycleState = LifecycleState::Initializing;
    else if (action == "start") lifecycleState = LifecycleState::Starting;
    else lifecycleState = LifecycleState::Stopping;
    lifecycleError.clear();
    lifecycleErrorCode.clear();
    lifecycleErrorAtMs = 0;
    lifecyclePending = true;
    activeLifecycleOperationId = strictAction ? operationId : std::string();
    activeLifecycleAction = action;
    activeLifecycleGeneration = dispatch.generation;
    ++lifecycleSequence;
    lifecycleUpdatedAtMs = nodeLifecycleTimestampMs();
    dispatch.disposition = LifecycleDispatchDisposition::Dispatch;
    if (strictAction) {
        LifecycleOperation operation;
        operation.action = action;
        operation.requestFingerprint = requestFingerprint;
        operation.previousState = dispatch.previousState;
        const auto inserted = lifecycleOperations.emplace(operationId, std::move(operation));
        dispatch.events.push_back(lifecycleEventLocked(
            action, operationId, "accepted", "accepted", dispatch.previousState));
        dispatch.acknowledgement = acknowledgement(true, false, {}, {});
        inserted.first->second.acknowledgement = dispatch.acknowledgement;
    } else {
        dispatch.events.push_back(lifecycleEventLocked(
            action, {}, "accepted", "accepted", dispatch.previousState));
    }
    return dispatch;
}

void DeliveryModuleImpl::settleLifecycleAction(
    const std::string& action,
    const std::string& operationId,
    std::uint64_t generation,
    LifecycleState previousState,
    LifecycleState successState,
    LifecycleState failureState,
    bool success)
{
    std::string event;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        if (!lifecyclePending
            || activeLifecycleGeneration != generation
            || activeLifecycleAction != action) {
            return;
        }
        lifecycleState = success ? successState : failureState;
        lifecycleError = success ? std::string() : lifecycleFailureMessage(action);
        lifecycleErrorCode = success ? std::string() : lifecycleFailureCode(action);
        lifecycleErrorAtMs = success ? 0 : nodeLifecycleTimestampMs();
        if (success && action == "initialize") ++lifecycleEpoch;
        if (!operationId.empty()) {
            const auto operation = lifecycleOperations.find(operationId);
            if (operation != lifecycleOperations.end()) {
                operation->second.settled = true;
                operation->second.outcome = success ? "succeeded" : "failed";
                rememberCompletedLifecycleOperationLocked(operationId);
            }
        }
        lifecyclePending = false;
        activeLifecycleOperationId.clear();
        activeLifecycleAction.clear();
        activeLifecycleGeneration = 0;
        ++lifecycleSequence;
        lifecycleUpdatedAtMs = nodeLifecycleTimestampMs();
        event = lifecycleEventLocked(
            action, operationId, "settled", success ? "succeeded" : "failed",
            previousState, success ? std::string() : lifecycleErrorCode,
            success ? std::string() : lifecycleError);
    }
    nodeChanged(event);
}

void DeliveryModuleImpl::settleLifecycleCallback(const std::string& action, bool success)
{
    std::string operationId;
    std::uint64_t generation = 0;
    LifecycleState previousState = LifecycleState::Uninitialized;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        if (!lifecyclePending || activeLifecycleAction != action) return;
        operationId = activeLifecycleOperationId;
        generation = activeLifecycleGeneration;
        const auto operation = lifecycleOperations.find(operationId);
        if (operation != lifecycleOperations.end()) {
            previousState = operation->second.previousState;
        } else if (action == "start") {
            previousState = LifecycleState::Stopped;
        } else {
            previousState = LifecycleState::Running;
        }
    }
    settleLifecycleAction(
        action, operationId, generation, previousState,
        action == "start" ? LifecycleState::Running : LifecycleState::Stopped,
        previousState, success);
}

StdLogosResult DeliveryModuleImpl::createNodePrepared(
    const std::string& cfg,
    const LifecycleDispatch& dispatch)
{
    std::lock_guard<std::mutex> createNodeLock(createNodeMutex);

    if (deliveryCtx != nullptr) {
        fprintf(stderr, "DeliveryModuleImpl: createNode rejected - context already initialized\n");
        const StdLogosResult result{false, {}, "Context already initialized"};
        settleLifecycleAction(dispatch.action, dispatch.operationId, dispatch.generation,
                              dispatch.previousState, LifecycleState::Stopped,
                              LifecycleState::Uninitialized, false);
        return result;
    }

    // Don't log cfg: it can carry sensitive config.
    fprintf(stderr, "DeliveryModuleImpl::createNode called\n");

    auto cfgWithDefaults = applyPortDefaults(cfg);
    if (!cfgWithDefaults) {
        const StdLogosResult result{false, {}, "Invalid JSON config"};
        settleLifecycleAction(dispatch.action, dispatch.operationId, dispatch.generation,
                              dispatch.previousState, LifecycleState::Stopped,
                              LifecycleState::Uninitialized, false);
        return result;
    }
    const std::string& cfgWithPorts = *cfgWithDefaults;

    struct CallbackContext {
        std::binary_semaphore sem{0};
        int callerRet{RET_ERR};
        std::string message;
    };

    static std::mutex pendingMutex;
    static std::unordered_map<void*, std::shared_ptr<CallbackContext>> pendingContexts;

    auto callbackCtx = std::make_shared<CallbackContext>();
    void* callbackKey = static_cast<void*>(callbackCtx.get());

    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts[callbackKey] = callbackCtx;
    }

    auto callback = +[](int callerRet, const char* msg, size_t len, void* userData) {
        fprintf(stderr, "DeliveryModuleImpl::createNode callback called with ret: %d\n", callerRet);

        std::shared_ptr<CallbackContext> callbackCtx;
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            auto it = pendingContexts.find(userData);
            if (it == pendingContexts.end()) {
                return;
            }
            callbackCtx = it->second;
            pendingContexts.erase(it);
        }

        if (!callbackCtx) {
            return;
        }

        callbackCtx->callerRet = callerRet;
        if (msg && len > 0) {
            callbackCtx->message = std::string(msg, len);
            fprintf(stderr, "DeliveryModuleImpl::createNode callback message: %s\n", callbackCtx->message.c_str());
        }

        callbackCtx->sem.release();
    };

    deliveryCtx = logosdelivery_create_node(cfgWithPorts.c_str(), callback, callbackKey);

    fprintf(stderr, "DeliveryModuleImpl: Waiting for createNode callback...\n");

    if (!callbackCtx->sem.try_acquire_for(CALLBACK_TIMEOUT)) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts.erase(callbackKey);

        deliveryCtx = nullptr;

        fprintf(stderr, "DeliveryModuleImpl: Timeout waiting for createNode callback\n");
        const StdLogosResult result{false, {}, "Timeout waiting for createNode callback"};
        settleLifecycleAction(dispatch.action, dispatch.operationId, dispatch.generation,
                              dispatch.previousState, LifecycleState::Stopped,
                              LifecycleState::Uninitialized, false);
        return result;
    }

    if (callbackCtx->callerRet != RET_OK || deliveryCtx == nullptr) {
        if (!callbackCtx->message.empty()) {
            fprintf(stderr, "DeliveryModuleImpl: createNode callback error: %s\n", callbackCtx->message.c_str());
        }

        deliveryCtx = nullptr;

        fprintf(stderr, "DeliveryModuleImpl: Failed to create Delivery context\n");
        const StdLogosResult result{false, {}, "Failed to create Delivery context"};
        settleLifecycleAction(dispatch.action, dispatch.operationId, dispatch.generation,
                              dispatch.previousState, LifecycleState::Stopped,
                              LifecycleState::Uninitialized, false);
        return result;
    }

    fprintf(stderr, "DeliveryModuleImpl: Delivery context created successfully\n");

    logosdelivery_set_event_callback(deliveryCtx, event_callback, this);
    const StdLogosResult result{true, {}};
    settleLifecycleAction(dispatch.action, dispatch.operationId, dispatch.generation,
                          dispatch.previousState, LifecycleState::Stopped,
                          LifecycleState::Uninitialized, true);
    return result;
}

bool DeliveryModuleImpl::launchInitializeWorker(
    const std::string& cfg,
    const LifecycleDispatch& dispatch)
{
    std::lock_guard<std::mutex> lock(lifecycleWorkerMutex);
    if (lifecycleInitializeWorker.joinable()) {
        if (lifecycleInitializeWorker.get_id() == std::this_thread::get_id()) {
            return false;
        }
        lifecycleInitializeWorker.join();
    }
    try {
        lifecycleInitializeWorker = std::thread([this, cfg, dispatch] {
            createNodePrepared(cfg, dispatch);
        });
    } catch (const std::system_error&) {
        return false;
    }
    return true;
}

StdLogosResult DeliveryModuleImpl::startPrepared(const LifecycleDispatch& dispatch)
{
    if (!deliveryCtx) {
        const StdLogosResult result{false, {}, "Context not initialized"};
        settleLifecycleAction(dispatch.action, dispatch.operationId, dispatch.generation,
                              dispatch.previousState, LifecycleState::Running,
                              dispatch.previousState, false);
        return result;
    }

    // Node start can block for a long time (relay reconnect backoff), so return
    // once dispatched. Completion arrives via nodeStarted.
    if (logosdelivery_start_node(deliveryCtx, start_callback, this) != RET_OK) {
        const StdLogosResult result{false, {}, "failed to initiate start"};
        settleLifecycleAction(dispatch.action, dispatch.operationId, dispatch.generation,
                              dispatch.previousState, LifecycleState::Running,
                              dispatch.previousState, false);
        return result;
    }
    return {true, {}};
}

StdLogosResult DeliveryModuleImpl::stopPrepared(const LifecycleDispatch& dispatch)
{
    if (!deliveryCtx) {
        const StdLogosResult result{false, {}, "Context not initialized"};
        settleLifecycleAction(dispatch.action, dispatch.operationId, dispatch.generation,
                              dispatch.previousState, LifecycleState::Stopped,
                              dispatch.previousState, false);
        return result;
    }

    if (logosdelivery_stop_node(deliveryCtx, stop_callback, this) != RET_OK) {
        const StdLogosResult result{false, {}, "failed to initiate stop"};
        settleLifecycleAction(dispatch.action, dispatch.operationId, dispatch.generation,
                              dispatch.previousState, LifecycleState::Stopped,
                              dispatch.previousState, false);
        return result;
    }
    return {true, {}};
}

StdLogosResult DeliveryModuleImpl::createNode(const std::string& cfg)
{
    {
        std::lock_guard<std::mutex> createNodeLock(createNodeMutex);
        if (deliveryCtx != nullptr) {
            fprintf(stderr, "DeliveryModuleImpl: createNode rejected - context already initialized\n");
            return {false, {}, "Context already initialized"};
        }
    }
    fprintf(stderr, "DeliveryModuleImpl::createNode called\n");
    const LifecycleDispatch dispatch = beginLifecycleAction(
        "initialize", {}, {}, false, {}, 0, 0, false);
    emitLifecycleEvents(dispatch.events);
    return createNodePrepared(cfg, dispatch);
}

StdLogosResult DeliveryModuleImpl::start()
{
    fprintf(stderr, "DeliveryModuleImpl::start called\n");
    if (!deliveryCtx) {
        return {false, {}, "Context not initialized"};
    }
    const LifecycleDispatch dispatch = beginLifecycleAction(
        "start", {}, {}, false, {}, 0, 0, false);
    emitLifecycleEvents(dispatch.events);
    return startPrepared(dispatch);
}

StdLogosResult DeliveryModuleImpl::stop()
{
    fprintf(stderr, "DeliveryModuleImpl::stop called\n");
    if (!deliveryCtx) {
        return {false, {}, "Context not initialized"};
    }
    const LifecycleDispatch dispatch = beginLifecycleAction(
        "stop", {}, {}, false, {}, 0, 0, false);
    emitLifecycleEvents(dispatch.events);
    return stopPrepared(dispatch);
}

std::string DeliveryModuleImpl::nodeStatus()
{
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    return lifecycleSnapshotLocked();
}

std::string DeliveryModuleImpl::nodeAction(const std::string& request)
{
    const auto rejected = [this](const std::string& code,
                                 const std::string& message) {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        json result;
        result["schema"] = NODE_LIFECYCLE_ACK_SCHEMA;
        result["version"] = 1;
        result["operation_id"] = nullptr;
        result["accepted"] = false;
        result["duplicate"] = false;
        result["instance_id"] = lifecycleInstanceId;
        result["epoch"] = lifecycleEpoch;
        result["sequence"] = lifecycleSequence;
        result["state"] = lifecycleStateName(lifecycleState);
        result["error"] = nodeLifecycleError(code, message, nodeLifecycleTimestampMs());
        return result.dump();
    };

    if (request.size() > MAX_NODE_LIFECYCLE_REQUEST_BYTES) {
        return rejected("request_too_large",
                        "Lifecycle request exceeds the supported size.");
    }

    json input;
    try {
        input = json::parse(request);
    } catch (const std::exception&) {
        return rejected("invalid_request", "Lifecycle request must be a JSON object.");
    }
    if (!input.is_object()) {
        return rejected("invalid_request", "Lifecycle request must be a JSON object.");
    }
    for (const auto& item : input.items()) {
        const std::string& key = item.key();
        if (key != "schema" && key != "version" && key != "operation_id"
            && key != "action" && key != "expected" && key != "parameters") {
            return rejected("invalid_request", "Lifecycle request contains an unsupported field.");
        }
    }
    const auto schema = input.find("schema");
    if (schema == input.end() || !schema->is_string()
        || schema->get<std::string>() != NODE_LIFECYCLE_COMMAND_SCHEMA) {
        return rejected("invalid_request", "Unsupported lifecycle request schema.");
    }
    const auto version = input.find("version");
    if (version == input.end() || !isLifecycleVersionOne(*version)) {
        return rejected("invalid_request", "Unsupported lifecycle request version.");
    }
    const auto operationId = input.find("operation_id");
    if (operationId == input.end() || !operationId->is_string()) {
        return rejected("invalid_request", "Lifecycle request requires an operation_id.");
    }
    const std::string operation = operationId->get<std::string>();
    if (!isValidNodeLifecycleOperationId(operation)) {
        return rejected("invalid_request", "Lifecycle operation_id is invalid.");
    }
    const auto actionValue = input.find("action");
    if (actionValue == input.end() || !actionValue->is_string()) {
        return rejected("invalid_request", "Lifecycle request requires an action.");
    }
    const std::string action = actionValue->get<std::string>();
    if (action != "initialize" && action != "start" && action != "stop") {
        return rejected("invalid_request", "Unsupported lifecycle action.");
    }

    bool hasExpectedSnapshot = false;
    std::string expectedInstanceId;
    std::uint64_t expectedEpoch = 0;
    std::uint64_t expectedSequence = 0;
    const auto expected = input.find("expected");
    if (expected != input.end()) {
        if (!expected->is_object() || expected->size() != 3
            || !expected->contains("instance_id")
            || !expected->contains("epoch")
            || !expected->contains("sequence")
            || !expected->at("instance_id").is_string()
            || !parseLifecycleUnsigned(expected->at("epoch"), expectedEpoch)
            || !parseLifecycleUnsigned(expected->at("sequence"), expectedSequence)) {
            return rejected("invalid_request",
                            "Lifecycle expected snapshot must contain instance_id, epoch, and sequence.");
        }
        expectedInstanceId = expected->at("instance_id").get<std::string>();
        hasExpectedSnapshot = true;
    }

    json parameters = json::object();
    const auto parametersValue = input.find("parameters");
    if (parametersValue != input.end()) {
        if (!parametersValue->is_object()) {
            return rejected("invalid_request", "Lifecycle parameters must be an object.");
        }
        parameters = *parametersValue;
    }

    std::string initializationConfig;
    if (action == "initialize") {
        const auto config = parameters.find("config");
        if (config == parameters.end() || !config->is_string()
            || parameters.size() != 1) {
            return rejected("invalid_request", "Initialize requires only parameters.config.");
        }
        initializationConfig = config->get<std::string>();
        if (initializationConfig.size() > MAX_NODE_LIFECYCLE_CONFIG_BYTES
            || containsEmbeddedNul(initializationConfig)) {
            return rejected("invalid_request",
                            "Initialize config is invalid or exceeds the supported size.");
        }
    } else if (!parameters.empty()) {
        return rejected("invalid_request",
                        "This lifecycle action does not accept parameters.");
    }

    const LifecycleDispatch dispatch = beginLifecycleAction(
        action, operation, input.dump(), hasExpectedSnapshot,
        expectedInstanceId, expectedEpoch, expectedSequence, true);
    emitLifecycleEvents(dispatch.events);
    if (dispatch.disposition != LifecycleDispatchDisposition::Dispatch) {
        return dispatch.acknowledgement;
    }
    if (action == "initialize") {
        if (!launchInitializeWorker(initializationConfig, dispatch)) {
            settleLifecycleAction(
                dispatch.action, dispatch.operationId, dispatch.generation,
                dispatch.previousState, LifecycleState::Stopped,
                LifecycleState::Uninitialized, false);
        }
    } else if (action == "start") {
        startPrepared(dispatch);
    } else {
        stopPrepared(dispatch);
    }
    return dispatch.acknowledgement;
}

StdLogosResult DeliveryModuleImpl::send(const std::string& contentTopic, const std::vector<uint8_t>& payload)
{
    fprintf(stderr, "DeliveryModuleImpl::send called with contentTopic: %s\n", contentTopic.c_str());

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot send message - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    nlohmann::json messageObj;
    messageObj["contentTopic"] = contentTopic;
    messageObj["payload"] = base64Encode(payload);
    messageObj["ephemeral"] = false;

    std::string messageJson = messageObj.dump();

    auto outcome = callApiRetValue(
        "send",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_send, deliveryCtx, messageJson.c_str()));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Send failed for topic: %s, reason: %s\n",
                contentTopic.c_str(), outcome.error.c_str());
    }

    if (outcome.success && outcome.value.is_string()) {
        fprintf(stderr, "DeliveryModuleImpl: Send initiated for topic: %s, with success, requestId: %s\n",
                contentTopic.c_str(), outcome.value.get<std::string>().c_str());
    }
    return outcome;
}

StdLogosResult DeliveryModuleImpl::subscribe(const std::string& contentTopic)
{
    fprintf(stderr, "DeliveryModuleImpl::subscribe called with contentTopic: %s\n", contentTopic.c_str());

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot subscribe - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = callApiRetVoid(
        "subscribe",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_subscribe, deliveryCtx, contentTopic.c_str()));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Subscribe failed for topic: %s, reason: %s\n",
                contentTopic.c_str(), outcome.error.c_str());
    }

    fprintf(stderr, "DeliveryModuleImpl: Subscribe completed for topic: %s with success\n", contentTopic.c_str());
    return outcome;
}

StdLogosResult DeliveryModuleImpl::unsubscribe(const std::string& contentTopic)
{
    fprintf(stderr, "DeliveryModuleImpl::unsubscribe called with contentTopic: %s\n", contentTopic.c_str());

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot unsubscribe - context not initialized.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = callApiRetVoid(
        "unsubscribe",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_unsubscribe, deliveryCtx, contentTopic.c_str()));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Unsubscribe failed for topic: %s, reason: %s\n",
                contentTopic.c_str(), outcome.error.c_str());
    }

    fprintf(stderr, "DeliveryModuleImpl: Unsubscribe completed for topic: %s with success\n", contentTopic.c_str());
    return outcome;
}

StdLogosResult DeliveryModuleImpl::storeQuery(
    const std::string& queryJson,
    const std::string& peerAddr,
    int64_t timeoutMs)
{
    if (!deliveryCtx) {
        return {false, {}, "Context not initialized"};
    }
    if (queryJson.empty()) {
        return {false, {}, "Store query JSON must not be empty"};
    }
    if (peerAddr.empty()) {
        return {false, {}, "Store provider address must not be empty"};
    }
    if (timeoutMs <= 0 || timeoutMs > static_cast<int64_t>(INT_MAX)) {
        return {false, {}, "Store query timeout must be a positive 32-bit integer"};
    }

    auto outcome = callApiRetValue(
        "store_query",
        std::chrono::milliseconds(timeoutMs),
        bindApiCall(
            waku_store_query,
            deliveryCtx,
            queryJson.c_str(),
            peerAddr.c_str(),
            static_cast<int>(timeoutMs)));
    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Store query failed: %s\n",
                outcome.error.c_str());
    }
    return outcome;
}

StdLogosResult DeliveryModuleImpl::getConnectedPeersInfo()
{
    fprintf(stderr, "DeliveryModuleImpl::getConnectedPeersInfo called\n");

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot get connected peers - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = callApiRetValue(
        "get_connected_peers_info",
        CALLBACK_TIMEOUT,
        bindApiCall(waku_get_connected_peers_info, deliveryCtx));
    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Get connected peers failed, reason: %s\n",
                outcome.error.c_str());
    }
    return outcome;
}

std::string DeliveryModuleImpl::version() const {
    std::string moduleVersion = "0.1.7";
    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot get version - context not initialized. Call createNode first.\n");
        return moduleVersion + " (liblogosdelivery version unknown, context not initialized)";
    }

    auto liblogosDeliveryVersion = callApiRetValue(
        "get_node_info",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_get_node_info, deliveryCtx, "Version"));

    if (!liblogosDeliveryVersion.success) {
        fprintf(stderr, "DeliveryModuleImpl: Get node info failed getting version, reason: %s\n",
                liblogosDeliveryVersion.error.c_str());
        return moduleVersion + " (liblogosdelivery version unknown)";
    }

    std::string ver = liblogosDeliveryVersion.value.get<std::string>();
    fprintf(stderr, "DeliveryModuleImpl: Get node info completed for attribute: Version, with success: %s\n", ver.c_str());

    return moduleVersion + " (liblogosdelivery version: " + ver + ")";
}

StdLogosResult DeliveryModuleImpl::getAvailableNodeInfoIDs() {
    fprintf(stderr, "DeliveryModuleImpl::getAvailableNodeInfoIDs called\n");

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot get available node info IDs - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }
    auto outcome = callApiRetValue(
        "get_available_node_info_ids",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_get_available_node_info_ids, deliveryCtx));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Get available node info IDs failed, reason: %s\n", outcome.error.c_str());
    }
    return outcome;
}

StdLogosResult DeliveryModuleImpl::getNodeInfo(const std::string& nodeInfoId) {
    fprintf(stderr, "DeliveryModuleImpl::getNodeInfo called with nodeInfoId: %s\n", nodeInfoId.c_str());

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot get node info - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }
    auto outcome = callApiRetValue(
        "get_node_info",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_get_node_info, deliveryCtx, nodeInfoId.c_str()));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Get node info failed for ID: %s, reason: %s\n",
                nodeInfoId.c_str(), outcome.error.c_str());
    }

    return outcome;
}

StdLogosResult DeliveryModuleImpl::getAvailableConfigs() {
    fprintf(stderr, "DeliveryModuleImpl::getAvailableConfigs called\n");

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot get available configs - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }
    auto outcome = callApiRetValue(
        "get_available_configs",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_get_available_configs, deliveryCtx));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Get available configs failed, reason: %s\n", outcome.error.c_str());
    }

    return outcome;
}

std::string DeliveryModuleImpl::collectOpenMetricsText()
{
    if (!deliveryCtx) {
        // No node yet — empty document; the openmetrics scraper renders nothing
        // for this module rather than treating the scrape as a hard error.
        return "";
    }

    auto outcome = callApiRetValue(
        "get_node_info",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_get_node_info, deliveryCtx, "Metrics"));

    if (!outcome.success || !outcome.value.is_string()) {
        fprintf(stderr, "DeliveryModuleImpl: collectOpenMetricsText failed to read Metrics node info: %s\n",
                outcome.error.c_str());
        return "";
    }

    // Hand the exposition text back verbatim; the openmetrics module parses it,
    // injects the module="delivery_module" label, and merges it with others.
    return outcome.value.get<std::string>();
}
