// Controls for deferred lifecycle callbacks in mock_liblogosdelivery.cpp.
//
// The production FFI completes start/stop asynchronously. Unit tests use
// these controls to hold one completion callback so they can verify the V1
// acknowledgement, idempotency, and terminal-event contract independently.
#pragma once

extern "C" {

void mock_delivery_hold_next_start();
void mock_delivery_hold_next_stop();
void mock_delivery_hold_next_create();
void mock_delivery_hold_next_destroy();
bool mock_delivery_complete_held_start(int callback_result, const char* message);
bool mock_delivery_complete_held_stop(int callback_result, const char* message);
bool mock_delivery_complete_held_create(int callback_result, const char* message);
bool mock_delivery_complete_held_destroy(int callback_result, const char* message);
bool mock_delivery_has_held_create();
bool mock_delivery_has_held_destroy();
void mock_delivery_reset_held_callbacks();

}
