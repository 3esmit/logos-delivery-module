#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <new>
#include <semaphore>
#include <string>
#include <unordered_map>
#include <utility>

#include <logos_result.h>

extern "C" {
#include <liblogosdelivery.h>
}

namespace {
using DeliveryCallback = void (*)(int, const char*, size_t, void*);

struct CallbackPayload {
    int callerRet{RET_ERR};
    std::string message;
};

struct CAbiCallbackBridge {
    DeliveryCallback callback;
    void* userData;
    std::atomic<unsigned int> references{2};
    std::atomic<bool> callbackInvoked{false};

    CAbiCallbackBridge(DeliveryCallback callback_, void* userData_)
        : callback(callback_), userData(userData_)
    {
    }
};

void releaseCAbiCallbackBridge(CAbiCallbackBridge* bridge)
{
    if (bridge && bridge->references.fetch_sub(1) == 1) {
        delete bridge;
    }
}

void forwardCAbiReply(int callerRet,
                      const char* reply,
                      const char* errorMessage,
                      void* userData)
{
    auto* bridge = static_cast<CAbiCallbackBridge*>(userData);
    if (!bridge || !bridge->callback) {
        releaseCAbiCallbackBridge(bridge);
        return;
    }

    bridge->callbackInvoked.store(true);
    const char* message = callerRet == RET_OK ? reply : errorMessage;
    const std::size_t messageLength = message ? std::char_traits<char>::length(message) : 0;
    bridge->callback(callerRet, message, messageLength, bridge->userData);

    // RET_STALE_WARN is non-terminal and keeps the bridge alive for the final
    // callback. The current C ABI uses RET_OK/RET_ERR for terminal replies.
    if (callerRet != RET_STALE_WARN) {
        releaseCAbiCallbackBridge(bridge);
    }
}

void forwardScalarReply(int callerRet, char* reply, size_t len, void* userData)
{
    auto* bridge = static_cast<CAbiCallbackBridge*>(userData);
    if (!bridge || !bridge->callback) {
        releaseCAbiCallbackBridge(bridge);
        return;
    }

    bridge->callbackInvoked.store(true);
    bridge->callback(callerRet, reply, len, bridge->userData);
    if (callerRet != RET_STALE_WARN) {
        releaseCAbiCallbackBridge(bridge);
    }
}

template <typename ReplyFn, typename Request, typename... BoundArgs>
auto bindApiCall(int (*func)(void*, ReplyFn, void*, const Request*),
                 void* callbackCtx,
                 BoundArgs&&... boundArgs)
{
    return [func, callbackCtx, ... args = std::forward<BoundArgs>(boundArgs)](
               DeliveryCallback callback, void* userData) {
        auto* bridge = new (std::nothrow) CAbiCallbackBridge(callback, userData);
        if (!bridge) {
            callback(RET_ERR, "out of memory", sizeof("out of memory") - 1, userData);
            return RET_ERR;
        }

        Request request{args...};
        const int dispatchResult = func(callbackCtx,
                                        forwardCAbiReply,
                                        bridge,
                                        &request);
        const bool callbackInvoked = bridge->callbackInvoked.load();
        releaseCAbiCallbackBridge(bridge);
        if (dispatchResult != RET_OK) {
            if (!callbackInvoked) {
                releaseCAbiCallbackBridge(bridge);
            }
        }
        return dispatchResult;
    };
}

template <typename ScalarFn>
auto bindApiCall(int (*func)(void*, ScalarFn, void*), void* callbackCtx)
{
    return [func, callbackCtx](DeliveryCallback callback, void* userData) {
        auto* bridge = new (std::nothrow) CAbiCallbackBridge(callback, userData);
        if (!bridge) {
            callback(RET_ERR, "out of memory", sizeof("out of memory") - 1, userData);
            return RET_ERR;
        }

        const int dispatchResult = func(callbackCtx, forwardScalarReply, bridge);
        const bool callbackInvoked = bridge->callbackInvoked.load();
        releaseCAbiCallbackBridge(bridge);
        if (dispatchResult != RET_OK) {
            if (!callbackInvoked) {
                releaseCAbiCallbackBridge(bridge);
            }
        }
        return dispatchResult;
    };
}

template <typename Context>
auto bindApiCall(int (*func)(Context*), void* callbackCtx)
{
    return [func, callbackCtx](DeliveryCallback, void*) {
        return func(static_cast<Context*>(callbackCtx));
    };
}

template <typename BoundInvoke>
StdLogosResult callApiRetVoid(const std::string& operationName, std::chrono::seconds timeout, BoundInvoke&& invoke)
{
    struct CallbackContext {
        std::binary_semaphore sem{0};
        CallbackPayload payload;
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
        if (callerRet == RET_STALE_WARN) {
            return;
        }

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

        callbackCtx->payload.callerRet = callerRet;
        if (msg && len > 0) {
            callbackCtx->payload.message = std::string(msg, len);
        }
        callbackCtx->sem.release();
    };

    int startResult = invoke(callback, callbackKey);
    if (startResult != RET_OK) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts.erase(callbackKey);
        return {false, {}, "failed to initiate " + operationName};
    }

    if (!callbackCtx->sem.try_acquire_for(timeout)) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts.erase(callbackKey);
        return {false, {}, operationName + " callback timeout"};
    }

    if (callbackCtx->payload.callerRet != RET_OK) {
        std::string message = callbackCtx->payload.message.empty()
            ? operationName + " failed"
            : callbackCtx->payload.message;
        return {false, {}, message};
    }

    return {true, {}};
}

template <typename BoundInvoke>
StdLogosResult callApiRetValue(
    const std::string& operationName,
    std::chrono::milliseconds timeout,
    BoundInvoke&& invoke)
{
    struct CallbackContext {
        std::binary_semaphore sem{0};
        CallbackPayload payload;
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
        if (callerRet == RET_STALE_WARN) {
            return;
        }

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

        callbackCtx->payload.callerRet = callerRet;
        if (msg && len > 0) {
            callbackCtx->payload.message = std::string(msg, len);
        }
        callbackCtx->sem.release();
    };

    int startResult = invoke(callback, callbackKey);
    if (startResult != RET_OK) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts.erase(callbackKey);
        return {false, {}, "failed to initiate " + operationName};
    }

    if (!callbackCtx->sem.try_acquire_for(timeout)) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts.erase(callbackKey);
        return {false, {}, operationName + " callback timeout"};
    }

    if (callbackCtx->payload.callerRet != RET_OK) {
        std::string message = callbackCtx->payload.message.empty()
            ? operationName + " failed"
            : callbackCtx->payload.message;
        return {false, {}, message};
    }

    return {true, callbackCtx->payload.message};
}
} // namespace
