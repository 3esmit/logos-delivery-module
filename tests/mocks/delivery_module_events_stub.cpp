// Stub implementations for logos_events: methods.
// In the real build, the codegen generates delivery_module_events.cpp with
// bodies that route through LogosModuleContext::emitEventImpl_. For unit tests,
// the codegen doesn't run so we provide stubs here.
//
// The node-lifecycle events (nodeStarted / nodeStopped) record their last
// payload in process-global slots (see delivery_module_events_stub.h) so tests
// can assert that start()/stop() emit them. messageReceived is captured too,
// allowing the FFI event decoder to be tested without a real delivery node.

#include "delivery_module_plugin.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "delivery_module_events_stub.h"

namespace delivery_test_events {
NodeLifecycleEvent g_lastNodeStarted{};
NodeLifecycleEvent g_lastNodeStopped{};
namespace {
std::mutex nodeChangedEventsMutex;
std::vector<std::string> nodeChangedEvents;
std::mutex messageReceivedEventMutex;
MessageReceivedEvent g_lastMessageReceived;
std::mutex heldMessageReceivedMutex;
std::condition_variable heldMessageReceivedChanged;
bool holdNextMessageReceived = false;
bool messageReceivedHeld = false;
bool releaseHeldMessageReceived = false;
}

void resetNodeLifecycleEvents()
{
    std::lock_guard<std::mutex> lock(nodeChangedEventsMutex);
    g_lastNodeStarted = NodeLifecycleEvent{};
    g_lastNodeStopped = NodeLifecycleEvent{};
    nodeChangedEvents.clear();
}

void resetMessageReceivedEvent()
{
    std::lock_guard<std::mutex> lock(messageReceivedEventMutex);
    g_lastMessageReceived = MessageReceivedEvent{};
}

void holdNextMessageReceivedEvent()
{
    std::lock_guard<std::mutex> lock(heldMessageReceivedMutex);
    holdNextMessageReceived = true;
    messageReceivedHeld = false;
    releaseHeldMessageReceived = false;
}

bool waitForHeldMessageReceivedEvent()
{
    std::unique_lock<std::mutex> lock(heldMessageReceivedMutex);
    return heldMessageReceivedChanged.wait_for(lock, std::chrono::seconds(1), [] {
        return messageReceivedHeld;
    });
}

void releaseHeldMessageReceivedEvent()
{
    std::lock_guard<std::mutex> lock(heldMessageReceivedMutex);
    releaseHeldMessageReceived = true;
    heldMessageReceivedChanged.notify_all();
}

MessageReceivedEvent lastMessageReceivedEvent()
{
    std::lock_guard<std::mutex> lock(messageReceivedEventMutex);
    return g_lastMessageReceived;
}

void recordMessageReceivedEvent(const std::string& messageHash,
                                const std::string& contentTopic,
                                const std::vector<uint8_t>& payload,
                                int64_t timestamp)
{
    {
        std::unique_lock<std::mutex> lock(heldMessageReceivedMutex);
        if (holdNextMessageReceived) {
            holdNextMessageReceived = false;
            messageReceivedHeld = true;
            heldMessageReceivedChanged.notify_all();
            heldMessageReceivedChanged.wait(lock, [] {
                return releaseHeldMessageReceived;
            });
            messageReceivedHeld = false;
        }
    }
    std::lock_guard<std::mutex> lock(messageReceivedEventMutex);
    g_lastMessageReceived = {
        messageHash,
        contentTopic,
        payload,
        timestamp,
        true,
    };
}

std::size_t nodeChangedEventCount()
{
    std::lock_guard<std::mutex> lock(nodeChangedEventsMutex);
    return nodeChangedEvents.size();
}

std::string nodeChangedEventAt(std::size_t index)
{
    std::lock_guard<std::mutex> lock(nodeChangedEventsMutex);
    return index < nodeChangedEvents.size() ? nodeChangedEvents[index] : std::string();
}

std::string lastNodeChangedEvent()
{
    std::lock_guard<std::mutex> lock(nodeChangedEventsMutex);
    return nodeChangedEvents.empty() ? std::string() : nodeChangedEvents.back();
}

void recordNodeChangedEvent(const std::string& event)
{
    std::lock_guard<std::mutex> lock(nodeChangedEventsMutex);
    nodeChangedEvents.push_back(event);
}
} // namespace delivery_test_events

void DeliveryModuleImpl::nodeChanged(const std::string& event) {
    delivery_test_events::recordNodeChangedEvent(event);
}

void DeliveryModuleImpl::messageSent(const std::string&, const std::string&, int64_t) {}
void DeliveryModuleImpl::messageError(const std::string&, const std::string&, const std::string&, int64_t) {}
void DeliveryModuleImpl::messagePropagated(const std::string&, const std::string&, int64_t) {}
void DeliveryModuleImpl::messageReceived(const std::string& messageHash,
                                         const std::string& contentTopic,
                                         const std::vector<uint8_t>& payload,
                                         int64_t timestamp)
{
    delivery_test_events::recordMessageReceivedEvent(messageHash, contentTopic, payload, timestamp);
}
void DeliveryModuleImpl::connectionStateChanged(const std::string&, int64_t) {}
void DeliveryModuleImpl::channelMessageReceived(const std::string&, const std::string&, const std::vector<uint8_t>&, int64_t) {}
void DeliveryModuleImpl::channelMessageSent(const std::string&, const std::string&, int64_t) {}
void DeliveryModuleImpl::channelMessageError(const std::string&, const std::string&, const std::string&, int64_t) {}

void DeliveryModuleImpl::nodeStarted(bool success, const std::string& message, int64_t timestamp) {
    delivery_test_events::g_lastNodeStarted = {success, message, timestamp, true};
}
void DeliveryModuleImpl::nodeStopped(bool success, const std::string& message, int64_t timestamp) {
    delivery_test_events::g_lastNodeStopped = {success, message, timestamp, true};
}
