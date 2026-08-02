// Shared test-observable state for the stubbed logos_events: methods.
// Lets tests check that start()/stop() emitted their completion events
// (nodeStarted / nodeStopped).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace delivery_test_events {

struct NodeLifecycleEvent {
    bool success = false;
    std::string message;
    int64_t timestamp = 0;
    bool fired = false;  // set true once the event has been emitted at least once
};

struct MessageReceivedEvent {
    std::string messageHash;
    std::string contentTopic;
    std::vector<uint8_t> payload;
    int64_t timestamp = 0;
    bool fired = false;
};

extern NodeLifecycleEvent g_lastNodeStarted;
extern NodeLifecycleEvent g_lastNodeStopped;

void resetNodeLifecycleEvents();
void resetMessageReceivedEvent();
MessageReceivedEvent lastMessageReceivedEvent();
std::size_t nodeChangedEventCount();
std::string nodeChangedEventAt(std::size_t index);
std::string lastNodeChangedEvent();

} // namespace delivery_test_events
