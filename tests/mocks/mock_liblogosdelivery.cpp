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
#include <cstring>
#include <mutex>
#include <optional>

#define RET_OK  0
#define RET_ERR 1

typedef void (*logosdelivery_callback)(int callerRet, const char* msg, size_t len, void* userData);

// Sentinel address used as a fake non-null delivery context.
static char s_fakeCtx = 0;

struct DeferredLifecycleCallback {
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

// Helper: invoke callback with RET_OK and the string configured in the mock store.
static void invokeOk(const char* funcName, logosdelivery_callback cb, void* userData) {
    if (!cb) return;
    const char* msg = LogosCMockStore::instance().getReturnString(funcName);
    cb(RET_OK, msg ? msg : "", msg ? strlen(msg) : 0, userData);
}

extern "C" {

void* logosdelivery_create_node(const char* /*cfg*/, logosdelivery_callback cb, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_create_node");
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

void logosdelivery_set_event_callback(void* /*ctx*/, logosdelivery_callback /*cb*/, void* /*userData*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_set_event_callback");
}

int logosdelivery_destroy(void* /*ctx*/, logosdelivery_callback cb, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_destroy");
    int dispatch = LOGOS_CMOCK_RETURN(int, "logosdelivery_destroy");
    if (dispatch == RET_OK) {
        std::lock_guard<std::mutex> lock(deferredLifecycleCallbackMutex);
        if (holdNextDestroyCallback) {
            holdNextDestroyCallback = false;
            deferredDestroyCallback = DeferredLifecycleCallback{cb, userData};
        } else {
            invokeOk("logosdelivery_destroy", cb, userData);
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
        const char* response = LogosCMockStore::instance().getReturnString("waku_store_query");
        if (cb) {
            cb(callbackResult, response ? response : "", response ? strlen(response) : 0, userData);
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
        if (cb) {
            cb(callbackResult, response ? response : "", response ? strlen(response) : 0,
               userData);
        }
    }
    return dispatch;
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

extern "C" bool mock_delivery_complete_held_start(int callbackResult, const char* message)
{
    std::optional<DeferredLifecycleCallback> deferred;
    {
        std::lock_guard<std::mutex> lock(deferredLifecycleCallbackMutex);
        if (!deferredStartCallback) return false;
        deferred = std::move(deferredStartCallback);
        deferredStartCallback.reset();
    }
    if (!deferred->callback) return false;
    const std::size_t length = message ? std::strlen(message) : 0;
    deferred->callback(callbackResult, message ? message : "", length, deferred->userData);
    return true;
}

extern "C" bool mock_delivery_complete_held_stop(int callbackResult, const char* message)
{
    std::optional<DeferredLifecycleCallback> deferred;
    {
        std::lock_guard<std::mutex> lock(deferredLifecycleCallbackMutex);
        if (!deferredStopCallback) return false;
        deferred = std::move(deferredStopCallback);
        deferredStopCallback.reset();
    }
    if (!deferred->callback) return false;
    const std::size_t length = message ? std::strlen(message) : 0;
    deferred->callback(callbackResult, message ? message : "", length, deferred->userData);
    return true;
}

extern "C" bool mock_delivery_complete_held_create(int callbackResult, const char* message)
{
    std::optional<DeferredLifecycleCallback> deferred;
    {
        std::lock_guard<std::mutex> lock(deferredLifecycleCallbackMutex);
        if (!deferredCreateCallback) return false;
        deferred = std::move(deferredCreateCallback);
        deferredCreateCallback.reset();
    }
    if (!deferred->callback) return false;
    const std::size_t length = message ? std::strlen(message) : 0;
    deferred->callback(callbackResult, message ? message : "", length, deferred->userData);
    return true;
}

extern "C" bool mock_delivery_complete_held_destroy(int callbackResult, const char* message)
{
    std::optional<DeferredLifecycleCallback> deferred;
    {
        std::lock_guard<std::mutex> lock(deferredLifecycleCallbackMutex);
        if (!deferredDestroyCallback) return false;
        deferred = std::move(deferredDestroyCallback);
        deferredDestroyCallback.reset();
    }
    if (!deferred->callback) return false;
    const std::size_t length = message ? std::strlen(message) : 0;
    deferred->callback(callbackResult, message ? message : "", length, deferred->userData);
    return true;
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
}
