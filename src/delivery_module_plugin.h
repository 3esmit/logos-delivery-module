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
 * - `channel_message_received` -> `channelMessageReceived`
 * - `channel_message_sent` -> `channelMessageSent`
 * - `channel_message_error` -> `channelMessageError`
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
     * @brief Creates a liblogosdelivery node from a JSON configuration.
     *
     * The JSON passes through to logos-delivery verbatim; `parseLogosDeliveryConf`
     * (https://github.com/logos-messaging/logos-delivery) owns the grammar.
     * `entryLayer` selects how much of the stack is mounted:
     * - `"kernel"` — transport node only
     * - `"messaging"` — kernel + messaging client
     * - `"channels"` — kernel + messaging + reliable channels (default)
     *
     * Three typical shapes:
     *
     * **App developer** — full stack (default `entryLayer`). `preset` picks the
     * network (`"logos.test"`, `"logos.dev"`, `"twn"`), `mode` picks the protocol
     * flags (`"Core"` = relay node, `"Edge"` = light node). Optional
     * `messagingOverrides` / `channelsOverrides` objects override per-layer
     * defaults:
     * @code{.json}
     * { "mode": "Core", "preset": "logos.test" }
     * @endcode
     *
     * **Node operator** — kernel-only service node on a public network. `mode`
     * is not applied on this layer, so protocol flags are set explicitly in
     * `kernelConf`:
     * @code{.json}
     * {
     *   "entryLayer": "kernel",
     *   "kernelConf": { "preset": "logos.test", "relay": true }
     * }
     * @endcode
     *
     * **Network hoster** — kernel-only node on a self-hosted network;
     * `kernelConf` is a raw `WakuNodeConf` used as-is:
     * @code{.json}
     * {
     *   "entryLayer": "kernel",
     *   "kernelConf": { "clusterId": 42, "relay": true, "entryNodes": ["/dns4/…"] }
     * }
     * @endcode
     *
     * On kernel-only nodes `send` / `subscribe` / `channel*` fail with "node has
     * no messaging client" / "no reliable channel manager"; `getNodeInfo`,
     * `storeQuery` and metrics keep working.
     *
     * The pre-layered flat shape (bare `WakuNodeConf` keys at top level) still
     * parses and boots the full stack.
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
     * @brief Runs a Store (historical message) query against a specific store
     *        service peer.
     *
     * ⚠️ USE AT YOUR OWN RISK: backed by the kernel API (`waku_store_query`,
     * `liblogosdelivery_kernel.h`), which is subject to change at any point
     * without a deprecation cycle. This method's JSON contract follows it.
     *
     * The query JSON maps to logos-delivery's `StoreQueryRequest`
     * (`library/kernel_api/protocols/store_api.nim`):
     * | Key                 | Type            | Required | Description                                        |
     * |---------------------|-----------------|----------|----------------------------------------------------|
     * | `requestId`         | string          | yes      | Caller-chosen id, echoed in the response           |
     * | `includeData`       | boolean         | yes      | `true` returns full messages, `false` hashes only  |
     * | `paginationForward` | boolean         | yes      | Paging direction                                   |
     * | `pubsubTopic`       | string          | no       | Pubsub topic filter                                |
     * | `contentTopics`     | array of string | no       | Content topic filters                              |
     * | `timeStart`         | number/string   | no       | Range start, nanoseconds since Unix epoch          |
     * | `timeEnd`           | number/string   | no       | Range end, nanoseconds since Unix epoch            |
     * | `messageHashes`     | array of string | no       | Hex message hashes for lookup-by-hash queries      |
     * | `paginationCursor`  | string          | no       | Hex cursor from a previous response                |
     * | `paginationLimit`   | number          | no       | Max messages per page                              |
     *
     * On success the result value is the response JSON (`StoreQueryResponseHex`):
     * `{ "requestId", "statusCode", "statusDesc", "messages": [ { "messageHash",
     * "message", "pubsubTopic" } ], "paginationCursor" }` with hashes 0x-hex
     * encoded.
     *
     * @param jsonQuery UTF-8 JSON query document, see above.
     * @param peerAddr Multiaddress of the store service peer to query
     *        (e.g. `/ip4/127.0.0.1/tcp/60000/p2p/16Uiu2...`).
     * @param timeoutMs Query timeout in milliseconds.
     * @return Success with the response JSON, or error details.
     */
    StdLogosResult storeQuery(const std::string& jsonQuery,
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

    /**
     * @brief Creates (or re-opens) a reliable channel.
     *
     * Persisted channel state survives @ref channelClose, so re-creating a
     * channel with the same id restores it.
     *
     * @param channelId Application-chosen channel identifier.
     * @param contentTopic Content topic the channel communicates on.
     * @param senderId This participant's SDS (Scalable Data Sync) sender identifier.
     * @return Success with the channel id, or error details.
     */
    StdLogosResult channelCreate(const std::string& channelId,
                                 const std::string& contentTopic,
                                 const std::string& senderId);

    /**
     * @brief Checks whether a reliable channel is currently open.
     *
     * An unknown channel id is not an error.
     *
     * @param channelId Channel identifier.
     * @return Success with `"true"` or `"false"` (verbatim FFI string), or error details.
     */
    StdLogosResult channelExists(const std::string& channelId);

    /**
     * @brief Sends a message on a reliable channel.
     *
     * Builds the JSON envelope expected by `logosdelivery_channel_send`:
     * `{ "payload": base64, "ephemeral": false }`.
     *
     * Returns a requestId on success. Async results come via typed events:
     * - `channelMessageSent` once every segment of the send is confirmed
     * - `channelMessageError` if the send finalises with a failed segment
     *
     * @param channelId Channel identifier.
     * @param payload Raw message bytes; base64-encoded before crossing the FFI boundary.
     * @return Success with request id, or error details.
     */
    StdLogosResult channelSend(const std::string& channelId, const std::vector<uint8_t>& payload);

    /**
     * @brief Closes a reliable channel: stops its SDS loops.
     *
     * Persisted state survives, so @ref channelCreate with the same id
     * restores the channel.
     *
     * @param channelId Channel identifier.
     * @return `true` when closed successfully, otherwise `false`.
     */
    StdLogosResult channelClose(const std::string& channelId);

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

logos_events:
    /// Emits versioned managed-node lifecycle observations.
    void nodeChanged(const std::string& event);

    void messageSent(const std::string& requestId, const std::string& messageHash, int64_t timestamp);
    void messageError(const std::string& requestId, const std::string& messageHash, const std::string& error, int64_t timestamp);
    void messagePropagated(const std::string& requestId, const std::string& messageHash, int64_t timestamp);
    void messageReceived(const std::string& messageHash, const std::string& contentTopic, const std::vector<uint8_t>& payload, int64_t timestamp);
    void connectionStateChanged(const std::string& connectionStatus, int64_t timestamp);

    void channelMessageReceived(const std::string& channelId, const std::string& senderId, const std::vector<uint8_t>& payload, int64_t timestamp);
    void channelMessageSent(const std::string& channelId, const std::string& requestId, int64_t timestamp);
    void channelMessageError(const std::string& channelId, const std::string& requestId, const std::string& error, int64_t timestamp);

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
