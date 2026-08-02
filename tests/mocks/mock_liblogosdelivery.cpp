// Mock implementation of liblogosdelivery C functions.
// Replaces the real Nim library at link time during unit tests.
//
// Callback-taking functions invoke the callback synchronously so the result is
// observable before the wrapping call returns - matching the storage module
// mock pattern. For the blocking wrappers (send/subscribe/...) this releases the
// api_call_handler semaphore before try_acquire_for waits; for the fire-and-
// forget start()/stop() it means the nodeStarted/nodeStopped event is emitted
// synchronously during the dispatch call.
//
// Return values and callback messages are controlled via LogosCMockStore.
// For the int-returning dispatch functions, the return value is the *dispatch*
// code (0 / RET_OK by default); set a non-zero value to simulate a dispatch
// failure, in which case no completion callback is fired:
//   t.mockCFunction("logosdelivery_start_node").returns(1);  // dispatch fails

#include <logos_clib_mock.h>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#define RET_OK  0
#define RET_ERR 1
#define RET_STALE_WARN 3

typedef void (*logosdelivery_callback)(int callerRet, const char* msg, size_t len, void* userData);

// Sentinel address used as a fake non-null delivery context.
static char s_fakeCtx = 0;

struct DeferredLifecycleCallback {
    logosdelivery_callback callback = nullptr;
    void* userData = nullptr;
};

struct EventListener {
    std::string eventName;
    logosdelivery_callback callback = nullptr;
    void* userData = nullptr;
};

std::mutex deferredLifecycleCallbackMutex;
std::optional<DeferredLifecycleCallback> deferredCreateCallback;
std::optional<DeferredLifecycleCallback> deferredStartCallback;
std::optional<DeferredLifecycleCallback> deferredStopCallback;
std::optional<DeferredLifecycleCallback> deferredDestroyCallback;
bool holdNextCreateCallback = false;
bool holdNextStartCallback = false;
bool holdNextStopCallback = false;
bool holdNextDestroyCallback = false;
bool failNextDestroyCallback = false;
std::mutex rawStoreQueryResponseMutex;
std::optional<std::string> rawStoreQueryResponse;
std::mutex eventListenerMutex;
std::unordered_map<std::uint64_t, EventListener> eventListeners;
std::uint64_t nextEventListenerId = 1;
std::size_t eventListenerRegistrationAttempt = 0;
std::size_t eventListenerRegistrationFailureAt = 0;
std::size_t eventListenerRemovalAttempt = 0;
std::size_t eventListenerRemovalFailureAt = 0;
bool failAllEventListenerRemovals = false;
std::mutex heldEventCallbackMutex;
std::condition_variable heldEventCallbackChanged;
bool holdNextEventBeforeCallback = false;
bool eventCallbackHeld = false;
bool releaseHeldEventCallback = false;
std::string lastCreateConfig;

static bool isTerminalLifecycleCallbackResult(int callbackResult)
{
    return callbackResult == RET_OK || callbackResult == RET_ERR;
}

static std::string encodeCborTextPayload(const char* message, std::size_t length)
{
    const std::string text = message ? std::string(message, length) : std::string();
    const std::vector<std::uint8_t> encoded = nlohmann::json::to_cbor(nlohmann::json(text));
    return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

static void invokeCallbackResult(int callbackResult,
                                 logosdelivery_callback cb,
                                 void* userData,
                                 const char* message,
                                 std::size_t length,
                                 bool encodeOkPayload = true)
{
    if (!cb) return;

    if (callbackResult == RET_OK && encodeOkPayload) {
        const std::string encoded = encodeCborTextPayload(message, length);
        cb(callbackResult, encoded.data(), encoded.size(), userData);
        return;
    }

    cb(callbackResult, message ? message : "", message ? length : 0, userData);
}

static bool completeHeldLifecycleCallback(std::optional<DeferredLifecycleCallback>& held,
                                          int callbackResult,
                                          const char* message,
                                          std::size_t length,
                                          bool encodeOkPayload = true)
{
    DeferredLifecycleCallback deferred;
    {
        std::lock_guard<std::mutex> lock(deferredLifecycleCallbackMutex);
        if (!held) return false;
        deferred = *held;
        if (isTerminalLifecycleCallbackResult(callbackResult)) {
            held.reset();
        }
    }
    if (!deferred.callback) return false;

    invokeCallbackResult(callbackResult, deferred.callback, deferred.userData,
                         message, length, encodeOkPayload);
    return true;
}

// Helper: invoke callback with RET_OK and the string configured in the mock store.
static void invokeOk(const char* funcName, logosdelivery_callback cb, void* userData) {
    const char* msg = LogosCMockStore::instance().getReturnString(funcName);
    invokeCallbackResult(RET_OK, cb, userData, msg ? msg : "", msg ? std::strlen(msg) : 0);
}

static const char* eventNameForPayload(const std::string& eventJson) {
    const std::string eventTypeKey = "\"eventType\"";
    const std::size_t keyPos = eventJson.find(eventTypeKey);
    if (keyPos == std::string::npos) return nullptr;

    const std::size_t colonPos = eventJson.find(':', keyPos + eventTypeKey.size());
    if (colonPos == std::string::npos) return nullptr;

    const std::size_t valueStart = eventJson.find('"', colonPos + 1);
    if (valueStart == std::string::npos) return nullptr;

    const std::size_t valueEnd = eventJson.find('"', valueStart + 1);
    if (valueEnd == std::string::npos) return nullptr;

    const std::string eventType = eventJson.substr(valueStart + 1, valueEnd - valueStart - 1);
    if (eventType == "message_sent") return "onMessageSent";
    if (eventType == "message_error") return "onMessageError";
    if (eventType == "message_propagated") return "onMessagePropagated";
    if (eventType == "message_received") return "onMessageReceived";
    if (eventType == "connection_status_change") return "onConnectionStatusChange";
    if (eventType == "channel_message_received") return "onChannelMessageReceived";
    if (eventType == "channel_message_sent") return "onChannelMessageSent";
    if (eventType == "channel_message_error") return "onChannelMessageError";
    return nullptr;
}

extern "C" {

void* logosdelivery_create_node(const char* cfg, logosdelivery_callback cb, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_create_node");
    {
        std::lock_guard<std::mutex> lock(eventListenerMutex);
        lastCreateConfig = cfg ? cfg : "";
    }
    int ok = LOGOS_CMOCK_RETURN(int, "logosdelivery_create_node");
    if (ok && cb) {
        std::lock_guard<std::mutex> lock(deferredLifecycleCallbackMutex);
        if (holdNextCreateCallback) {
            holdNextCreateCallback = false;
            deferredCreateCallback = DeferredLifecycleCallback{cb, userData};
        } else {
            cb(RET_OK, "", 0, userData);
        }
    } else if (!ok && cb) {
        cb(RET_ERR, "mock: create_node fail", 22, userData);
    }
    return ok ? static_cast<void*>(&s_fakeCtx) : nullptr;
}

std::uint64_t logosdelivery_add_event_listener(
    void* ctx, const char* eventName, logosdelivery_callback cb, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_add_event_listener");
    if (!ctx || !eventName || !cb) return 0;

    std::lock_guard<std::mutex> lock(eventListenerMutex);
    ++eventListenerRegistrationAttempt;
    if (eventListenerRegistrationFailureAt != 0
        && eventListenerRegistrationAttempt == eventListenerRegistrationFailureAt) {
        return 0;
    }

    const std::uint64_t listenerId = nextEventListenerId++;
    eventListeners.emplace(listenerId, EventListener{eventName, cb, userData});
    return listenerId;
}

int logosdelivery_remove_event_listener(void* ctx, std::uint64_t listenerId) {
    LOGOS_CMOCK_RECORD("logosdelivery_remove_event_listener");
    if (!ctx) return RET_ERR;

    std::lock_guard<std::mutex> lock(eventListenerMutex);
    ++eventListenerRemovalAttempt;
    if (failAllEventListenerRemovals
        || (eventListenerRemovalFailureAt != 0
            && eventListenerRemovalAttempt == eventListenerRemovalFailureAt)) {
        return RET_ERR;
    }
    return eventListeners.erase(listenerId) == 1 ? RET_OK : RET_ERR;
}

int logosdelivery_destroy(void* /*ctx*/, logosdelivery_callback cb, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_destroy");
    int dispatch = LOGOS_CMOCK_RETURN(int, "logosdelivery_destroy");
    if (dispatch == RET_OK) {
        std::lock_guard<std::mutex> lock(deferredLifecycleCallbackMutex);
        if (holdNextDestroyCallback) {
            holdNextDestroyCallback = false;
            deferredDestroyCallback = DeferredLifecycleCallback{cb, userData};
        } else if (failNextDestroyCallback) {
            failNextDestroyCallback = false;
            const char* message = "mock: destroy callback fail";
            if (cb) {
                cb(RET_ERR, message, std::strlen(message), userData);
            }
            return RET_ERR;
        } else {
            invokeOk("logosdelivery_destroy", cb, userData);
            std::lock_guard<std::mutex> eventLock(eventListenerMutex);
            eventListeners.clear();
            nextEventListenerId = 1;
        }
    }
    return dispatch;
}

int logosdelivery_start_node(void* /*ctx*/, logosdelivery_callback cb, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_start_node");
    // Return value is the dispatch code (default 0 = RET_OK). Only fire the
    // completion callback when dispatch "succeeds", mirroring the real FFI.
    int dispatch = LOGOS_CMOCK_RETURN(int, "logosdelivery_start_node");
    if (dispatch == RET_OK) {
        std::lock_guard<std::mutex> lock(deferredLifecycleCallbackMutex);
        if (holdNextStartCallback) {
            holdNextStartCallback = false;
            deferredStartCallback = DeferredLifecycleCallback{cb, userData};
        } else {
            invokeOk("logosdelivery_start_node", cb, userData);
        }
    }
    return dispatch;
}

int logosdelivery_stop_node(void* /*ctx*/, logosdelivery_callback cb, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_stop_node");
    int dispatch = LOGOS_CMOCK_RETURN(int, "logosdelivery_stop_node");
    if (dispatch == RET_OK) {
        std::lock_guard<std::mutex> lock(deferredLifecycleCallbackMutex);
        if (holdNextStopCallback) {
            holdNextStopCallback = false;
            deferredStopCallback = DeferredLifecycleCallback{cb, userData};
        } else {
            invokeOk("logosdelivery_stop_node", cb, userData);
        }
    }
    return dispatch;
}

int logosdelivery_send(void* /*ctx*/, logosdelivery_callback cb, void* userData, const char* /*msg*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_send");
    invokeOk("logosdelivery_send", cb, userData);
    return RET_OK;
}

int logosdelivery_subscribe(void* /*ctx*/, logosdelivery_callback cb, void* userData, const char* /*topic*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_subscribe");
    invokeOk("logosdelivery_subscribe", cb, userData);
    return RET_OK;
}

int logosdelivery_unsubscribe(void* /*ctx*/, logosdelivery_callback cb, void* userData, const char* /*topic*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_unsubscribe");
    invokeOk("logosdelivery_unsubscribe", cb, userData);
    return RET_OK;
}

int waku_store_query(void* /*ctx*/, logosdelivery_callback cb, void* userData,
                     const char* /*jsonQuery*/, const char* /*peerAddr*/, int /*timeoutMs*/) {
    LOGOS_CMOCK_RECORD("waku_store_query");
    int dispatch = LOGOS_CMOCK_RETURN(int, "waku_store_query_dispatch");
    if (dispatch == RET_OK) {
        int callbackResult = LOGOS_CMOCK_RETURN(int, "waku_store_query_callback_result");
        std::optional<std::string> rawResponse;
        {
            std::lock_guard<std::mutex> lock(rawStoreQueryResponseMutex);
            rawResponse = rawStoreQueryResponse;
        }
        if (rawResponse) {
            invokeCallbackResult(callbackResult, cb, userData,
                                 rawResponse->data(), rawResponse->size(), false);
        } else {
            const char* response = LogosCMockStore::instance().getReturnString(
                "waku_store_query");
            invokeCallbackResult(callbackResult, cb, userData,
                                 response ? response : "",
                                 response ? std::strlen(response) : 0);
        }
    }
    return dispatch;
}

int waku_get_connected_peers_info(void* /*ctx*/, logosdelivery_callback cb, void* userData) {
    LOGOS_CMOCK_RECORD("waku_get_connected_peers_info");
    int dispatch = LOGOS_CMOCK_RETURN(int, "waku_get_connected_peers_info_dispatch");
    if (dispatch == RET_OK) {
        int callbackResult = LOGOS_CMOCK_RETURN(
            int, "waku_get_connected_peers_info_callback_result");
        const char* response = LogosCMockStore::instance().getReturnString(
            "waku_get_connected_peers_info");
        invokeCallbackResult(callbackResult, cb, userData,
                             response ? response : "",
                             response ? std::strlen(response) : 0);
    }
    return dispatch;
}

int logosdelivery_channel_create(void* /*ctx*/, logosdelivery_callback cb, void* userData,
                                 const char* /*channelId*/, const char* /*contentTopic*/, const char* /*senderId*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_channel_create");
    invokeOk("logosdelivery_channel_create", cb, userData);
    return RET_OK;
}

int logosdelivery_channel_exists(void* /*ctx*/, logosdelivery_callback cb, void* userData, const char* /*channelId*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_channel_exists");
    invokeOk("logosdelivery_channel_exists", cb, userData);
    return RET_OK;
}

int logosdelivery_channel_send(void* /*ctx*/, logosdelivery_callback cb, void* userData,
                               const char* /*channelId*/, const char* /*msg*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_channel_send");
    invokeOk("logosdelivery_channel_send", cb, userData);
    return RET_OK;
}

int logosdelivery_channel_close(void* /*ctx*/, logosdelivery_callback cb, void* userData, const char* /*channelId*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_channel_close");
    invokeOk("logosdelivery_channel_close", cb, userData);
    return RET_OK;
}

int logosdelivery_get_node_info(void* /*ctx*/, logosdelivery_callback cb, void* userData, const char* /*attributeName*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_get_node_info");
    invokeOk("logosdelivery_get_node_info", cb, userData);
    return RET_OK;
}

int logosdelivery_get_available_node_info_ids(void* /*ctx*/, logosdelivery_callback cb, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_get_available_node_info_ids");
    invokeOk("logosdelivery_get_available_node_info_ids", cb, userData);
    return RET_OK;
}

int logosdelivery_get_available_configs(void* /*ctx*/, logosdelivery_callback cb, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_get_available_configs");
    invokeOk("logosdelivery_get_available_configs", cb, userData);
    return RET_OK;
}

} // extern "C"

extern "C" void mock_delivery_hold_next_start()
{
    std::lock_guard<std::mutex> lock(deferredLifecycleCallbackMutex);
    holdNextStartCallback = true;
}

extern "C" void mock_delivery_hold_next_stop()
{
    std::lock_guard<std::mutex> lock(deferredLifecycleCallbackMutex);
    holdNextStopCallback = true;
}

extern "C" void mock_delivery_hold_next_create()
{
    std::lock_guard<std::mutex> lock(deferredLifecycleCallbackMutex);
    holdNextCreateCallback = true;
}

extern "C" void mock_delivery_hold_next_destroy()
{
    std::lock_guard<std::mutex> lock(deferredLifecycleCallbackMutex);
    holdNextDestroyCallback = true;
}

extern "C" void mock_delivery_fail_next_destroy_callback()
{
    std::lock_guard<std::mutex> lock(deferredLifecycleCallbackMutex);
    failNextDestroyCallback = true;
}

extern "C" bool mock_delivery_complete_held_start(int callbackResult, const char* message)
{
    return completeHeldLifecycleCallback(
        deferredStartCallback, callbackResult, message, message ? std::strlen(message) : 0);
}

extern "C" bool mock_delivery_complete_held_stop(int callbackResult, const char* message)
{
    return completeHeldLifecycleCallback(
        deferredStopCallback, callbackResult, message, message ? std::strlen(message) : 0);
}

extern "C" bool mock_delivery_complete_held_create(int callbackResult, const char* message)
{
    return completeHeldLifecycleCallback(
        deferredCreateCallback, callbackResult, message, message ? std::strlen(message) : 0);
}

extern "C" bool mock_delivery_complete_held_destroy(int callbackResult, const char* message)
{
    const bool completed = completeHeldLifecycleCallback(
        deferredDestroyCallback, callbackResult, message, message ? std::strlen(message) : 0);
    if (!completed) return false;
    if (callbackResult == RET_OK) {
        std::lock_guard<std::mutex> lock(eventListenerMutex);
        eventListeners.clear();
        nextEventListenerId = 1;
    }
    return true;
}

extern "C" bool mock_delivery_complete_held_start_raw(int callbackResult,
                                                        const char* payload,
                                                        std::size_t payloadSize)
{
    return completeHeldLifecycleCallback(
        deferredStartCallback, callbackResult, payload, payloadSize, false);
}

extern "C" bool mock_delivery_has_held_start()
{
    std::lock_guard<std::mutex> lock(deferredLifecycleCallbackMutex);
    return deferredStartCallback.has_value();
}

extern "C" bool mock_delivery_has_held_stop()
{
    std::lock_guard<std::mutex> lock(deferredLifecycleCallbackMutex);
    return deferredStopCallback.has_value();
}

extern "C" bool mock_delivery_has_held_create()
{
    std::lock_guard<std::mutex> lock(deferredLifecycleCallbackMutex);
    return deferredCreateCallback.has_value();
}

extern "C" bool mock_delivery_has_held_destroy()
{
    std::lock_guard<std::mutex> lock(deferredLifecycleCallbackMutex);
    return deferredDestroyCallback.has_value();
}

extern "C" void mock_delivery_reset_held_callbacks()
{
    std::lock_guard<std::mutex> lock(deferredLifecycleCallbackMutex);
    deferredCreateCallback.reset();
    deferredStartCallback.reset();
    deferredStopCallback.reset();
    deferredDestroyCallback.reset();
    holdNextCreateCallback = false;
    holdNextStartCallback = false;
    holdNextStopCallback = false;
    holdNextDestroyCallback = false;
    failNextDestroyCallback = false;
}

extern "C" void mock_delivery_fail_event_listener_registration_at(std::size_t attempt)
{
    std::lock_guard<std::mutex> lock(eventListenerMutex);
    eventListenerRegistrationAttempt = 0;
    eventListenerRegistrationFailureAt = attempt;
}

extern "C" void mock_delivery_fail_event_listener_removal_at(std::size_t attempt)
{
    std::lock_guard<std::mutex> lock(eventListenerMutex);
    eventListenerRemovalAttempt = 0;
    eventListenerRemovalFailureAt = attempt;
    failAllEventListenerRemovals = false;
}

extern "C" void mock_delivery_fail_all_event_listener_removals()
{
    std::lock_guard<std::mutex> lock(eventListenerMutex);
    eventListenerRemovalAttempt = 0;
    eventListenerRemovalFailureAt = 0;
    failAllEventListenerRemovals = true;
}

extern "C" std::size_t mock_delivery_event_listener_count()
{
    std::lock_guard<std::mutex> lock(eventListenerMutex);
    return eventListeners.size();
}

extern "C" void mock_delivery_reset_event_listeners()
{
    std::lock_guard<std::mutex> lock(eventListenerMutex);
    eventListeners.clear();
    nextEventListenerId = 1;
    eventListenerRegistrationAttempt = 0;
    eventListenerRegistrationFailureAt = 0;
    eventListenerRemovalAttempt = 0;
    eventListenerRemovalFailureAt = 0;
    failAllEventListenerRemovals = false;
}

extern "C" bool mock_delivery_has_event_listener(const char* eventName)
{
    if (!eventName) return false;

    std::lock_guard<std::mutex> lock(eventListenerMutex);
    for (const auto& entry : eventListeners) {
        if (entry.second.eventName == eventName) return true;
    }
    return false;
}

extern "C" const char* mock_delivery_last_create_config()
{
    static thread_local std::string config;
    std::lock_guard<std::mutex> lock(eventListenerMutex);
    config = lastCreateConfig;
    return config.c_str();
}

extern "C" bool mock_delivery_emit_event(int callerResult, const char* eventJson)
{
    const std::string event = eventJson ? eventJson : "";
    const char* eventName = eventNameForPayload(event);
    if (!eventName) return false;

    std::vector<EventListener> listeners;
    {
        std::lock_guard<std::mutex> lock(eventListenerMutex);
        for (const auto& entry : eventListeners) {
            if (entry.second.eventName == eventName && entry.second.callback) {
                listeners.push_back(entry.second);
            }
        }
    }

    if (listeners.empty()) return false;
    {
        std::unique_lock<std::mutex> lock(heldEventCallbackMutex);
        if (holdNextEventBeforeCallback) {
            holdNextEventBeforeCallback = false;
            eventCallbackHeld = true;
            heldEventCallbackChanged.notify_all();
            heldEventCallbackChanged.wait(lock, [] {
                return releaseHeldEventCallback;
            });
            eventCallbackHeld = false;
        }
    }
    for (const auto& listener : listeners) {
        listener.callback(callerResult, event.c_str(), event.size(), listener.userData);
    }
    return true;
}

extern "C" void mock_delivery_hold_next_event_before_callback()
{
    std::lock_guard<std::mutex> lock(heldEventCallbackMutex);
    holdNextEventBeforeCallback = true;
    eventCallbackHeld = false;
    releaseHeldEventCallback = false;
}

extern "C" bool mock_delivery_wait_for_held_event_before_callback()
{
    std::unique_lock<std::mutex> lock(heldEventCallbackMutex);
    return heldEventCallbackChanged.wait_for(lock, std::chrono::seconds(1), [] {
        return eventCallbackHeld;
    });
}

extern "C" void mock_delivery_release_held_event_before_callback()
{
    std::lock_guard<std::mutex> lock(heldEventCallbackMutex);
    releaseHeldEventCallback = true;
    heldEventCallbackChanged.notify_all();
}

extern "C" void mock_delivery_set_raw_store_query_response(const char* payload,
                                                            std::size_t payloadSize)
{
    std::lock_guard<std::mutex> lock(rawStoreQueryResponseMutex);
    rawStoreQueryResponse = payload ? std::string(payload, payloadSize) : std::string();
}

extern "C" void mock_delivery_clear_raw_store_query_response()
{
    std::lock_guard<std::mutex> lock(rawStoreQueryResponseMutex);
    rawStoreQueryResponse.reset();
}
