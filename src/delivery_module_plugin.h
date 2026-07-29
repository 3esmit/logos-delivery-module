#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <logos_module_context.h>
#include <logos_result.h>

/**
 * @brief Pure C++ implementation of the delivery messaging module.
 *
 * This class adapts the universal module API to liblogosdelivery C-FFI calls
 * and forwards asynchronous events back to the host through typed events
 * declared in the `logos_events:` section.
 *
 * Lifecycle contract:
 * - call @ref createNode exactly once per context
 * - call @ref start before message operations
 * - use @ref subscribe / @ref send / @ref unsubscribe as needed
 * - call @ref stop before shutdown
 * - request V1 `destroy` from the stopped state before releasing the context
 *
 * @ref createNode is synchronous. @ref start and @ref stop return once the
 * request is dispatched; completion is reported via the `nodeStarted` /
 * `nodeStopped` events.
 *
 * Asynchronous events are emitted via typed `logos_events:` declarations.
 * The codegen generates method bodies that route through
 * LogosModuleContext::emitEventImpl_.
 *
 * The raw FFI `eventType` values mapped into these typed events are:
 * - `message_sent` -> `messageSent`
 * - `message_error` -> `messageError`
 * - `message_propagated` -> `messagePropagated`
 * - `message_received` -> `messageReceived`
 * - `connection_status_change` -> `connectionStateChanged`
 *
 * As a general concept consider using proper content_topic format for your purpose.
 * --> https://lip.logos.co/messaging/informational/23/topics.html#content-topics
 */
class DeliveryModuleImpl : public LogosModuleContext
{
public:
    DeliveryModuleImpl();
    ~DeliveryModuleImpl();

    /**
     * @brief Creates a liblogosdelivery node from a WakuNodeConf JSON document.
     *
     * The JSON is parsed by logos-delivery (liblogosdelivery folder) side and maps to
     * `WakuNodeConf` from `tools/confutils/cli_args.nim`
     * (https://github.com/logos-messaging/logos-delivery).
     *
     * The configuration is a **flat** JSON object whose keys correspond to
     * `WakuNodeConf` Nim field names (camelCase). Unknown keys are silently
     * ignored. Every field has a built-in default, so only the values that
     * differ from defaults need to be supplied.
     *
     * ## Commonly used keys
     * | Key                  | Type             | Default    | Description                                 |
     * |----------------------|------------------|------------|---------------------------------------------|
     * | `mode`               | string           | `"noMode"` | `"Core"`, `"Edge"`, or `"noMode"`           |
     * | `preset`             | string           | `""`       | Network preset (`"twn"`, `"logos.dev"`, …)  |
     * | `clusterId`          | number (uint16)  | `0`        | Cluster identifier                          |
     * | `entryNodes`         | array of string  | `[]`       | Bootstrap peers (enrtree / multiaddress)    |
     * | `relay`              | boolean          | `false`    | Enable relay protocol                       |
     * | `rlnRelay`           | boolean          | `false`    | Enable RLN rate-limit nullifier             |
     * | `tcpPort`            | number (uint16)  | `60000`    | P2P TCP listen port                         |
     * | `numShardsInNetwork` | number (uint16)  | `1`        | Auto-sharding shard count                   |
     * | `logLevel`           | string           | `"INFO"`   | `"TRACE"`, `"DEBUG"`, `"INFO"`, `"WARN"`, … |
     * | `logFormat`          | string           | `"TEXT"`   | `"TEXT"` or `"JSON"`                        |
     * | `maxMessageSize`     | string           | `"150KiB"` | Maximum message payload size                |
     *
     * ## Presets
     * Using a `preset` populates cluster ID, entry nodes, sharding, RLN, and
     * other network-specific defaults automatically. Individual keys supplied
     * alongside a preset override the preset values.
     * - `"twn"` – The RLN-protected Waku Network (cluster 1).
     * - `"logos.dev"` – Logos Dev Network (cluster 2, mix enabled,
     *   p2pReliability on, 8 auto-shards, built-in bootstrap nodes).
     *
     * Minimal `logos.dev` example:
     * @code{.json}
     * {
     *   "logLevel": "INFO",
     *   "mode": "Core",
     *   "preset": "logos.dev"
     * }
     * @endcode
     *
     * Full override example:
     * @code{.json}
     * {
     *   "mode": "Core",
     *   "clusterId": 42,
     *   "entryNodes": ["enrtree://TREE@nodes.example.com"],
     *   "relay": true,
     *   "tcpPort": 60000,
     *   "numShardsInNetwork": 8,
     *   "maxMessageSize": "150KiB",
     *   "logLevel": "INFO",
     *   "logFormat": "TEXT"
     * }
     * @endcode
     *
     * @param cfg UTF-8 JSON payload string.
     * @return `true` if context creation succeeds and callback returns `RET_OK`,
     *         otherwise `false`.
     */
    StdLogosResult createNode(const std::string& cfg);

    /**
     * @brief Starts the delivery node.
     * @return `true` once dispatched; completion is reported via `nodeStarted`.
     */
    StdLogosResult start();

    /**
     * @brief Stops the delivery node.
     * @return `true` once dispatched; completion is reported via `nodeStopped`.
     */
    StdLogosResult stop();

    /**
     * @brief Returns the bounded V1 managed-node lifecycle snapshot.
     *
     * This is callable before createNode() and deliberately keeps the legacy
     * Delivery lifecycle surface unchanged.
     */
    std::string nodeStatus();

    /**
     * @brief Accepts a versioned, caller-correlated lifecycle command.
     *
     * The command is a JSON object. The immediate result is only an
     * acknowledgement; accepted asynchronous lifecycle requests settle through
     * nodeChanged(). `destroy` is accepted only from `stopped` and returns the
     * context to `uninitialized` after the FFI release has completed.
     */
    std::string nodeAction(const std::string& request);

    /**
     * @brief Sends a message over the active node.
     *
     * Builds a JSON envelope expected by `logosdelivery_send`:
     * `{ "contentTopic": string, "payload": base64, "ephemeral": false }`.
     *
     * Returns a requestId on success. Async results come via typed events:
     * - `messageError` emitted if the module can't send the message
     * - `messagePropagated` emitted if the message has hit the network
     * - `messageSent` emitted after the message is validated by the network
     *
     * @param contentTopic Destination content topic.
     * @param payload Raw message bytes; base64-encoded before crossing the FFI boundary.
     * @return Success with request id, or error details.
     */
    StdLogosResult send(const std::string& contentTopic, const std::vector<uint8_t>& payload);

    /**
     * @brief Subscribes to the supplied content topic.
     * @param contentTopic Topic identifier.
     * @return `true` when subscribed successfully, otherwise `false`.
     */
    StdLogosResult subscribe(const std::string& contentTopic);

    /**
     * @brief Unsubscribes from the supplied content topic.
     * @param contentTopic Topic identifier.
     * @return `true` when unsubscribed successfully, otherwise `false`.
     */
    StdLogosResult unsubscribe(const std::string& contentTopic);

    /**
     * @brief Queries an explicit Waku Store provider for retained messages.
     *
     * @param queryJson Store query JSON accepted by logos-delivery.
     * @param peerAddr Explicit Store-provider multiaddress. The module does
     *        not select a peer implicitly.
     * @param timeoutMs Request timeout in milliseconds. It is passed to the
     *        FFI call and bounds the module callback wait; must be positive.
     * @return The Store response JSON on success.
     */
    StdLogosResult storeQuery(
        const std::string& queryJson,
        const std::string& peerAddr,
        int64_t timeoutMs);

    /**
     * @brief Returns connected Delivery peer metadata from liblogosdelivery.
     *
     * The returned value is the raw JSON object produced by
     * `waku_get_connected_peers_info`: peer IDs map to objects with
     * `protocols` and `addresses` arrays. This method does not dial, select,
     * or otherwise mutate peers; callers can use the advertised protocols to
     * select an explicit Store provider for @ref storeQuery.
     */
    StdLogosResult getConnectedPeersInfo();

    StdLogosResult getAvailableNodeInfoIDs();

    /**
     * @brief Returns information for the given node info item.
     * @param nodeInfoId Identifier for the requested node info item.
     * @return JSON data string on success, or error details.
     */
    StdLogosResult getNodeInfo(const std::string& nodeInfoId);

    /**
     * @brief Information about the available configuration parameters for `createNode`.
     */
    StdLogosResult getAvailableConfigs();

    /**
     * @brief Returns the node's metrics as an OpenMetrics/Prometheus text
     *        document, so the `openmetrics` module can scrape this module.
     *
     * liblogosdelivery already aggregates Prometheus metrics in its global
     * registry and renders them as exposition text behind the `"Metrics"`
     * node-info attribute. This method just hands that text back verbatim — no
     * reshaping — which satisfies the openmetrics `metrics_source` interface's
     * `collectOpenMetricsText()` convention. The openmetrics scraper parses the
     * text, injects a `module="delivery_module"` label on every series, and
     * merges it with other modules. Select this method per-module in the
     * openmetrics `start` config with `{"name":"delivery_module","format":"text"}`.
     *
     * Returns an empty string before a node has been created, or when the
     * underlying read fails, so a scrape never errors out on this module.
     *
     * @return OpenMetrics/Prometheus exposition text (possibly empty).
     */
    std::string collectOpenMetricsText();

    std::string name() const { return "delivery_module"; }

    std::string version() const;

logos_events:
    /// Emits versioned managed-node lifecycle observations.
    void nodeChanged(const std::string& event);

    void messageSent(const std::string& requestId, const std::string& messageHash, int64_t timestamp);
    void messageError(const std::string& requestId, const std::string& messageHash, const std::string& error, int64_t timestamp);
    void messagePropagated(const std::string& requestId, const std::string& messageHash, int64_t timestamp);
    void messageReceived(const std::string& messageHash, const std::string& contentTopic, const std::vector<uint8_t>& payload, int64_t timestamp);
    void connectionStateChanged(const std::string& connectionStatus, int64_t timestamp);

    void nodeStarted(bool success, const std::string& message, int64_t timestamp);
    void nodeStopped(bool success, const std::string& message, int64_t timestamp);

private:
    enum class LifecycleState : std::uint8_t {
        Uninitialized,
        Initializing,
        Stopped,
        Starting,
        Running,
        Stopping,
        Destroying,
    };

    enum class LifecycleDispatchDisposition {
        Dispatch,
        Duplicate,
        Rejected,
        Noop,
    };

    struct LifecycleOperation {
        std::string action;
        std::string requestFingerprint;
        std::string acknowledgement;
        LifecycleState previousState = LifecycleState::Uninitialized;
        bool settled = false;
        std::string outcome;
    };

    struct LifecycleDispatch {
        LifecycleDispatchDisposition disposition = LifecycleDispatchDisposition::Rejected;
        std::string action;
        std::string operationId;
        LifecycleState previousState = LifecycleState::Uninitialized;
        std::uint64_t generation = 0;
        std::string acknowledgement;
        std::vector<std::string> events;
    };

    void* deliveryCtx;

    std::mutex createNodeMutex;
    mutable std::mutex lifecycleMutex;
    std::mutex lifecycleWorkerMutex;
    std::thread lifecycleInitializeWorker;
    std::thread lifecycleDestroyWorker;
    LifecycleState lifecycleState = LifecycleState::Uninitialized;
    std::uint64_t lifecycleGeneration = 0;
    std::string lifecycleInstanceId;
    std::uint64_t lifecycleEpoch = 0;
    std::uint64_t lifecycleSequence = 0;
    std::int64_t lifecycleUpdatedAtMs = 0;
    std::int64_t lifecycleErrorAtMs = 0;
    std::string lifecycleErrorCode;
    std::string lifecycleError;
    bool lifecyclePending = false;
    std::string activeLifecycleOperationId;
    std::string activeLifecycleAction;
    std::uint64_t activeLifecycleGeneration = 0;
    std::unordered_map<std::string, LifecycleOperation> lifecycleOperations;
    std::deque<std::string> completedLifecycleOperationIds;

    static constexpr std::chrono::seconds CALLBACK_TIMEOUT{30};

    /**
     * @brief Global C callback used by liblogosdelivery to report async events.
     * @param callerRet FFI return code associated with callback dispatch.
     * @param msg UTF-8 JSON event payload buffer.
     * @param len Message length in bytes.
     * @param userData Opaque pointer expected to be `DeliveryModuleImpl*`.
     */
    static void event_callback(int callerRet, const char* msg, size_t len, void* userData);

    // Completion callbacks for start()/stop(); emit nodeStarted / nodeStopped.
    // userData is the DeliveryModuleImpl*.
    static void start_callback(int callerRet, const char* msg, size_t len, void* userData);
    static void stop_callback(int callerRet, const char* msg, size_t len, void* userData);

    LifecycleDispatch beginLifecycleAction(const std::string& action,
                                           const std::string& operationId,
                                           const std::string& requestFingerprint,
                                           bool hasExpectedSnapshot,
                                           const std::string& expectedInstanceId,
                                           std::uint64_t expectedEpoch,
                                           std::uint64_t expectedSequence,
                                           bool strictAction);
    StdLogosResult createNodePrepared(const std::string& cfg,
                                      const LifecycleDispatch& dispatch);
    bool launchInitializeWorker(const std::string& cfg,
                                const LifecycleDispatch& dispatch);
    bool launchDestroyWorker(const LifecycleDispatch& dispatch);
    StdLogosResult startPrepared(const LifecycleDispatch& dispatch);
    StdLogosResult stopPrepared(const LifecycleDispatch& dispatch);
    StdLogosResult destroyPrepared(const LifecycleDispatch& dispatch);
    void settleLifecycleAction(const std::string& action,
                               const std::string& operationId,
                               std::uint64_t generation,
                               LifecycleState previousState,
                               LifecycleState successState,
                               LifecycleState failureState,
                               bool success);
    void settleLifecycleCallback(const std::string& action, bool success);
    std::string lifecycleSnapshotLocked() const;
    std::string lifecycleEventLocked(const std::string& action,
                                     const std::string& operationId,
                                     const std::string& phase,
                                     const std::string& outcome,
                                     LifecycleState previousState,
                                     const std::string& errorCode = {},
                                     const std::string& errorMessage = {}) const;
    void emitLifecycleEvents(const std::vector<std::string>& events);
    void rememberCompletedLifecycleOperationLocked(const std::string& operationId);
    static const char* lifecycleStateName(LifecycleState state);
    static std::vector<std::string> lifecycleActions(LifecycleState state);
    static const char* lifecycleFailureCode(const std::string& action);
    static const char* lifecycleFailureMessage(const std::string& action);
};
