// Stub implementations for logos_events: methods.
// In the real build, the codegen generates delivery_module_events.cpp with
// bodies that route through LogosModuleContext::emitEventImpl_. For unit tests,
// the codegen doesn't run so we provide stubs here.
//
// The node-lifecycle events (nodeStarted / nodeStopped) record their last
// payload in process-global slots (see delivery_module_events_stub.h) so tests
// can assert that start()/stop() emit them. The message/connection events stay
// no-ops.

#include "delivery_module_plugin.h"

#include <mutex>
#include <vector>

#include "delivery_module_events_stub.h"

namespace delivery_test_events {
NodeLifecycleEvent g_lastNodeStarted{};
NodeLifecycleEvent g_lastNodeStopped{};
namespace {
std::mutex nodeChangedEventsMutex;
std::vector<std::string> nodeChangedEvents;
}

void resetNodeLifecycleEvents()
{
    std::lock_guard<std::mutex> lock(nodeChangedEventsMutex);
    g_lastNodeStarted = NodeLifecycleEvent{};
    g_lastNodeStopped = NodeLifecycleEvent{};
    nodeChangedEvents.clear();
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
void DeliveryModuleImpl::messageReceived(const std::string&, const std::string&, const std::vector<uint8_t>&, int64_t) {}
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
