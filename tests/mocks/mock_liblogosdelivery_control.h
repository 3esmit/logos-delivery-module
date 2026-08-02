// Controls for deferred lifecycle callbacks in mock_liblogosdelivery.cpp.
//
// The production FFI completes start/stop asynchronously. Unit tests use
// these controls to hold one completion callback so they can verify the V1
// acknowledgement, idempotency, and terminal-event contract independently.
#pragma once

#include <cstddef>

extern "C" {

void mock_delivery_hold_next_start();
void mock_delivery_hold_next_stop();
void mock_delivery_hold_next_create();
void mock_delivery_hold_next_destroy();
void mock_delivery_fail_next_destroy_callback();
bool mock_delivery_complete_held_start(int callback_result, const char* message);
bool mock_delivery_complete_held_stop(int callback_result, const char* message);
bool mock_delivery_complete_held_create(int callback_result, const char* message);
bool mock_delivery_complete_held_destroy(int callback_result, const char* message);
// Delivers unencoded transport bytes for the malformed-CBOR lifecycle case.
bool mock_delivery_complete_held_start_raw(int callback_result,
                                           const char* payload,
                                           std::size_t payload_size);
bool mock_delivery_has_held_start();
bool mock_delivery_has_held_stop();
bool mock_delivery_has_held_create();
bool mock_delivery_has_held_destroy();
void mock_delivery_reset_held_callbacks();

// Test-only access to the configuration and per-event listener registry that
// the module hands to the C FFI. The returned config pointer remains valid
// until the next call on this test thread.
const char* mock_delivery_last_create_config();
void mock_delivery_fail_event_listener_registration_at(std::size_t attempt);
void mock_delivery_fail_event_listener_removal_at(std::size_t attempt);
void mock_delivery_fail_all_event_listener_removals();
std::size_t mock_delivery_event_listener_count();
void mock_delivery_reset_event_listeners();
bool mock_delivery_has_event_listener(const char* event_name);
bool mock_delivery_emit_event(int caller_result, const char* event_json);
// Pauses one already-dispatched event immediately before it enters the module
// callback. This exercises a callback queued across module teardown.
void mock_delivery_hold_next_event_before_callback();
bool mock_delivery_wait_for_held_event_before_callback();
void mock_delivery_release_held_event_before_callback();

// Sets raw response bytes for the next store-query callback. This bypasses
// mock-side RET_OK CBOR encoding so tests can exercise the wire decoder.
void mock_delivery_set_raw_store_query_response(const char* payload,
                                                 std::size_t payload_size);
void mock_delivery_clear_raw_store_query_response();

}
