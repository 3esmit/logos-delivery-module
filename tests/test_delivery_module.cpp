// Unit tests for DeliveryModuleImpl.
// All liblogosdelivery C functions are mocked at link time via mock_liblogosdelivery.cpp.
// Mocks invoke callbacks synchronously so the semaphore inside api_call_handler.h
// is released before try_acquire_for starts waiting.

#include <climits>
#include <chrono>
#include <limits>
#include <thread>

#include <liblogosdelivery.h>
#include <logos_test.h>
#include <nlohmann/json.hpp>
#include "delivery_module_plugin.h"
#include "mocks/delivery_module_events_stub.h"
#include "mocks/mock_liblogosdelivery_control.h"

using nlohmann::json;

namespace {

constexpr const char* kLifecycleCommandSchema =
    "logos.managed_node_lifecycle.command";

json lifecycleSnapshot(DeliveryModuleImpl& impl)
{
    return json::parse(impl.nodeStatus());
}

json lifecycleCommand(const std::string& operationId,
                      const std::string& action,
                      const json& parameters = json::object())
{
    return {
        {"schema", kLifecycleCommandSchema},
        {"version", 1},
        {"operation_id", operationId},
        {"action", action},
        {"parameters", parameters},
    };
}

json lifecycleCommandWithExpected(const std::string& operationId,
                                  const std::string& action,
                                  const json& snapshot,
                                  const json& parameters = json::object())
{
    json command = lifecycleCommand(operationId, action, parameters);
    command["expected"] = {
        {"instance_id", snapshot.at("instance_id")},
        {"epoch", snapshot.at("epoch")},
        {"sequence", snapshot.at("sequence")},
    };
    return command;
}

json lastNodeChangedEvent()
{
    return json::parse(delivery_test_events::lastNodeChangedEvent());
}

bool waitForLifecycleState(DeliveryModuleImpl& impl,
                           const std::string& expectedState,
                           std::size_t expectedEventCount = 0)
{
    for (int attempt = 0; attempt < 100; ++attempt) {
        const json status = lifecycleSnapshot(impl);
        if (status.at("state").get<std::string>() == expectedState
            && delivery_test_events::nodeChangedEventCount() >= expectedEventCount) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool waitForHeldCreateCallback()
{
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (mock_delivery_has_held_create()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool waitForHeldDestroyCallback()
{
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (mock_delivery_has_held_destroy()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Helper: create an impl that has a valid delivery context (createNode called).
// ---------------------------------------------------------------------------
static DeliveryModuleImpl* createInitializedImpl(LogosTestContext& t) {
    t.mockCFunction("logosdelivery_create_node").returns(1);
    auto* impl = new DeliveryModuleImpl();
    LOGOS_ASSERT_TRUE(impl->createNode(R"({"logLevel":"INFO"})").success);
    return impl;
}

// createNode

LOGOS_TEST(createNode_succeeds_when_ffi_returns_non_null_context) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_create_node").returns(1);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(R"({"logLevel":"INFO"})").success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_create_node"));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_set_event_callback"));
}

LOGOS_TEST(createNode_fails_when_ffi_returns_null) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_create_node").returns(0);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_FALSE(impl.createNode(R"({"logLevel":"INFO"})").success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_create_node"));
}

LOGOS_TEST(createNode_tracks_call_count) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_create_node").returns(1);

    DeliveryModuleImpl impl;
    impl.createNode(R"({"logLevel":"INFO"})");
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_create_node"), 1);
}

LOGOS_TEST(createNode_succeeds_with_logos_dev_preset_config) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_create_node").returns(1);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(R"({"logLevel":"DEBUG","mode":"Core","preset":"logos.dev"})").success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_create_node"));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_set_event_callback"));
}

// start

LOGOS_TEST(start_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    LOGOS_ASSERT_FALSE(impl.start().success);
}

LOGOS_TEST(start_succeeds_after_createNode) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT_TRUE(impl->start().success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_start_node"));

    delete impl;
}

LOGOS_TEST(start_calls_ffi_start_node) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    impl->start();
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_start_node"), 1);

    delete impl;
}

// stop

LOGOS_TEST(stop_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    LOGOS_ASSERT_FALSE(impl.stop().success);
}

LOGOS_TEST(stop_succeeds_after_createNode) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT_TRUE(impl->stop().success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_stop_node"));

    delete impl;
}

// start()/stop() report completion via events

LOGOS_TEST(start_emits_node_started_event) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_events::resetNodeLifecycleEvents();
    auto* impl = createInitializedImpl(t);

    // Dispatch succeeds; the mock fires the completion callback synchronously,
    // so the nodeStarted event is observable right after start() returns.
    LOGOS_ASSERT_TRUE(impl->start().success);
    LOGOS_ASSERT_TRUE(delivery_test_events::g_lastNodeStarted.fired);
    LOGOS_ASSERT_TRUE(delivery_test_events::g_lastNodeStarted.success);

    delete impl;
}

LOGOS_TEST(stop_emits_node_stopped_event) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_events::resetNodeLifecycleEvents();
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT_TRUE(impl->stop().success);
    LOGOS_ASSERT_TRUE(delivery_test_events::g_lastNodeStopped.fired);
    LOGOS_ASSERT_TRUE(delivery_test_events::g_lastNodeStopped.success);

    delete impl;
}

LOGOS_TEST(start_returns_false_when_dispatch_fails) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_events::resetNodeLifecycleEvents();
    auto* impl = createInitializedImpl(t);

    // A non-zero dispatch code means the library refused to start; start()
    // reports failure immediately and NO completion event is emitted.
    t.mockCFunction("logosdelivery_start_node").returns(1);
    LOGOS_ASSERT_FALSE(impl->start().success);
    LOGOS_ASSERT_FALSE(delivery_test_events::g_lastNodeStarted.fired);

    delete impl;
}

LOGOS_TEST(stop_returns_false_when_dispatch_fails) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_events::resetNodeLifecycleEvents();
    auto* impl = createInitializedImpl(t);

    t.mockCFunction("logosdelivery_stop_node").returns(1);
    LOGOS_ASSERT_FALSE(impl->stop().success);
    LOGOS_ASSERT_FALSE(delivery_test_events::g_lastNodeStopped.fired);

    delete impl;
}

// V1 managed-node lifecycle contract

LOGOS_TEST(nodeStatus_reports_uninitialized_v1_lifecycle_contract) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;

    const json status = lifecycleSnapshot(impl);
    LOGOS_ASSERT_EQ(status.at("schema").get<std::string>(),
                    std::string("logos.managed_node_lifecycle.snapshot"));
    LOGOS_ASSERT_EQ(status.at("version").get<int>(), 1);
    LOGOS_ASSERT_EQ(status.at("scope").at("kind").get<std::string>(),
                    std::string("messaging"));
    LOGOS_ASSERT_EQ(status.at("state").get<std::string>(),
                    std::string("uninitialized"));
    LOGOS_ASSERT_EQ(status.at("health").get<std::string>(), std::string("unknown"));
    LOGOS_ASSERT_EQ(status.at("supported_actions").size(), std::size_t{1});
    LOGOS_ASSERT_EQ(status.at("supported_actions").at(0).get<std::string>(),
                    std::string("initialize"));
    LOGOS_ASSERT_TRUE(status.at("pending_operation").is_null());
    LOGOS_ASSERT_TRUE(status.at("last_completed_operation").is_null());
}

LOGOS_TEST(nodeAction_initializes_with_v1_acknowledgement_and_events) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_create_node").returns(1);
    delivery_test_events::resetNodeLifecycleEvents();
    mock_delivery_reset_held_callbacks();
    mock_delivery_hold_next_create();

    DeliveryModuleImpl impl;
    const json command = lifecycleCommand(
        "delivery-initialize-1", "initialize",
        {{"config", R"({"logLevel":"INFO"})"}});
    const auto startedAt = std::chrono::steady_clock::now();
    const json acknowledgement = json::parse(impl.nodeAction(command.dump()));
    const auto acknowledgementElapsed = std::chrono::steady_clock::now() - startedAt;

    LOGOS_ASSERT_EQ(acknowledgement.at("schema").get<std::string>(),
                    std::string("logos.managed_node_lifecycle.ack"));
    LOGOS_ASSERT_TRUE(acknowledgement.at("accepted").get<bool>());
    LOGOS_ASSERT_FALSE(acknowledgement.at("duplicate").get<bool>());
    LOGOS_ASSERT_EQ(acknowledgement.at("operation_id").get<std::string>(),
                    std::string("delivery-initialize-1"));
    LOGOS_ASSERT_EQ(acknowledgement.at("state").get<std::string>(),
                    std::string("initializing"));
    LOGOS_ASSERT_TRUE(acknowledgementElapsed < std::chrono::seconds(1));
    LOGOS_ASSERT_TRUE(waitForHeldCreateCallback());
    const json pending = lifecycleSnapshot(impl);
    LOGOS_ASSERT_EQ(pending.at("state").get<std::string>(), std::string("initializing"));
    LOGOS_ASSERT_EQ(pending.at("pending_operation").at("operation_id").get<std::string>(),
                    std::string("delivery-initialize-1"));
    LOGOS_ASSERT_TRUE(mock_delivery_complete_held_create(RET_OK, "created"));
    LOGOS_ASSERT_TRUE(waitForLifecycleState(impl, "stopped", 2));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_create_node"));

    const json status = lifecycleSnapshot(impl);
    LOGOS_ASSERT_EQ(status.at("state").get<std::string>(), std::string("stopped"));
    LOGOS_ASSERT_EQ(status.at("epoch").get<std::uint64_t>(), std::uint64_t{1});
    LOGOS_ASSERT_EQ(status.at("last_completed_operation").at("operation_id").get<std::string>(),
                    std::string("delivery-initialize-1"));
    LOGOS_ASSERT_EQ(delivery_test_events::nodeChangedEventCount(), std::size_t{2});
    LOGOS_ASSERT_EQ(json::parse(delivery_test_events::nodeChangedEventAt(0))
                            .at("phase").get<std::string>(),
                    std::string("accepted"));
    const json settled = lastNodeChangedEvent();
    LOGOS_ASSERT_EQ(settled.at("phase").get<std::string>(), std::string("settled"));
    LOGOS_ASSERT_EQ(settled.at("outcome").get<std::string>(), std::string("succeeded"));
    LOGOS_ASSERT_EQ(settled.at("status").at("state").get<std::string>(),
                    std::string("stopped"));
    mock_delivery_reset_held_callbacks();
}

LOGOS_TEST(nodeAction_start_is_idempotent_while_callback_is_pending) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);
    delivery_test_events::resetNodeLifecycleEvents();
    mock_delivery_reset_held_callbacks();

    const json before = lifecycleSnapshot(*impl);
    const json command = lifecycleCommandWithExpected(
        "delivery-start-1", "start", before);
    mock_delivery_hold_next_start();
    const json acknowledgement = json::parse(impl->nodeAction(command.dump()));

    LOGOS_ASSERT_TRUE(acknowledgement.at("accepted").get<bool>());
    LOGOS_ASSERT_FALSE(acknowledgement.at("duplicate").get<bool>());
    LOGOS_ASSERT_EQ(acknowledgement.at("state").get<std::string>(), std::string("starting"));
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_start_node"), 1);
    LOGOS_ASSERT_EQ(delivery_test_events::nodeChangedEventCount(), std::size_t{1});

    const json duplicate = json::parse(impl->nodeAction(command.dump()));
    LOGOS_ASSERT_TRUE(duplicate.at("accepted").get<bool>());
    LOGOS_ASSERT_TRUE(duplicate.at("duplicate").get<bool>());
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_start_node"), 1);
    LOGOS_ASSERT_EQ(delivery_test_events::nodeChangedEventCount(), std::size_t{1});

    json reusedOperationId = command;
    reusedOperationId["expected"]["sequence"] =
        reusedOperationId.at("expected").at("sequence").get<std::uint64_t>() + 1;
    const json operationIdConflict = json::parse(impl->nodeAction(reusedOperationId.dump()));
    LOGOS_ASSERT_FALSE(operationIdConflict.at("accepted").get<bool>());
    LOGOS_ASSERT_EQ(operationIdConflict.at("error").at("code").get<std::string>(),
                    std::string("operation_id_conflict"));
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_start_node"), 1);
    LOGOS_ASSERT_EQ(delivery_test_events::nodeChangedEventCount(), std::size_t{1});

    const json pendingStatus = lifecycleSnapshot(*impl);
    LOGOS_ASSERT_EQ(pendingStatus.at("state").get<std::string>(), std::string("starting"));
    LOGOS_ASSERT_EQ(pendingStatus.at("pending_operation").at("operation_id").get<std::string>(),
                    std::string("delivery-start-1"));

    const json conflicting = json::parse(impl->nodeAction(
        lifecycleCommand("delivery-start-2", "start").dump()));
    LOGOS_ASSERT_FALSE(conflicting.at("accepted").get<bool>());
    LOGOS_ASSERT_EQ(conflicting.at("error").at("code").get<std::string>(),
                    std::string("operation_in_progress"));
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_start_node"), 1);

    LOGOS_ASSERT_TRUE(mock_delivery_complete_held_start(RET_OK, "started"));
    const json runningStatus = lifecycleSnapshot(*impl);
    LOGOS_ASSERT_EQ(runningStatus.at("state").get<std::string>(), std::string("running"));
    LOGOS_ASSERT_TRUE(runningStatus.at("pending_operation").is_null());
    LOGOS_ASSERT_TRUE(delivery_test_events::g_lastNodeStarted.fired);
    LOGOS_ASSERT_TRUE(delivery_test_events::g_lastNodeStarted.success);
    const json settled = lastNodeChangedEvent();
    LOGOS_ASSERT_EQ(settled.at("operation_id").get<std::string>(),
                    std::string("delivery-start-1"));
    LOGOS_ASSERT_EQ(settled.at("phase").get<std::string>(), std::string("settled"));
    LOGOS_ASSERT_EQ(settled.at("outcome").get<std::string>(), std::string("succeeded"));

    mock_delivery_reset_held_callbacks();
    delete impl;
}

LOGOS_TEST(nodeAction_stop_reports_stopping_then_settles_stopped) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);
    LOGOS_ASSERT_TRUE(impl->start().success);
    delivery_test_events::resetNodeLifecycleEvents();
    mock_delivery_reset_held_callbacks();

    const json before = lifecycleSnapshot(*impl);
    mock_delivery_hold_next_stop();
    const json acknowledgement = json::parse(impl->nodeAction(
        lifecycleCommandWithExpected("delivery-stop-1", "stop", before).dump()));
    LOGOS_ASSERT_TRUE(acknowledgement.at("accepted").get<bool>());
    LOGOS_ASSERT_EQ(acknowledgement.at("state").get<std::string>(), std::string("stopping"));
    LOGOS_ASSERT_EQ(lifecycleSnapshot(*impl).at("state").get<std::string>(),
                    std::string("stopping"));
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_stop_node"), 1);

    LOGOS_ASSERT_TRUE(mock_delivery_complete_held_stop(RET_OK, "stopped"));
    const json stopped = lifecycleSnapshot(*impl);
    LOGOS_ASSERT_EQ(stopped.at("state").get<std::string>(), std::string("stopped"));
    LOGOS_ASSERT_TRUE(stopped.at("pending_operation").is_null());
    LOGOS_ASSERT_TRUE(delivery_test_events::g_lastNodeStopped.fired);
    LOGOS_ASSERT_TRUE(delivery_test_events::g_lastNodeStopped.success);
    const json settled = lastNodeChangedEvent();
    LOGOS_ASSERT_EQ(settled.at("operation_id").get<std::string>(),
                    std::string("delivery-stop-1"));
    LOGOS_ASSERT_EQ(settled.at("outcome").get<std::string>(), std::string("succeeded"));

    mock_delivery_reset_held_callbacks();
    delete impl;
}

LOGOS_TEST(nodeAction_destroy_releases_context_and_allows_reinitialization) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_create_node").returns(1);
    delivery_test_events::resetNodeLifecycleEvents();
    mock_delivery_reset_held_callbacks();

    DeliveryModuleImpl impl;
    const json initialize = lifecycleCommand(
        "delivery-initialize-before-destroy", "initialize",
        {{"config", R"({"logLevel":"INFO"})"}});
    LOGOS_ASSERT_TRUE(json::parse(impl.nodeAction(initialize.dump()))
                          .at("accepted").get<bool>());
    LOGOS_ASSERT_TRUE(waitForLifecycleState(impl, "stopped", 2));
    const json stopped = lifecycleSnapshot(impl);
    const auto stoppedActions = stopped.at("supported_actions").get<std::vector<std::string>>();
    LOGOS_ASSERT_EQ(stoppedActions.size(), std::size_t{2});
    LOGOS_ASSERT_EQ(stoppedActions.at(0), std::string("start"));
    LOGOS_ASSERT_EQ(stoppedActions.at(1), std::string("destroy"));

    delivery_test_events::resetNodeLifecycleEvents();
    mock_delivery_hold_next_destroy();
    const json destroy = lifecycleCommandWithExpected(
        "delivery-destroy-1", "destroy", stopped);
    const auto startedAt = std::chrono::steady_clock::now();
    const json acknowledgement = json::parse(impl.nodeAction(destroy.dump()));
    const auto acknowledgementElapsed = std::chrono::steady_clock::now() - startedAt;

    LOGOS_ASSERT_TRUE(acknowledgement.at("accepted").get<bool>());
    LOGOS_ASSERT_FALSE(acknowledgement.at("duplicate").get<bool>());
    LOGOS_ASSERT_EQ(acknowledgement.at("state").get<std::string>(), std::string("destroying"));
    LOGOS_ASSERT_TRUE(acknowledgementElapsed < std::chrono::seconds(1));
    LOGOS_ASSERT_TRUE(waitForHeldDestroyCallback());
    const json destroying = lifecycleSnapshot(impl);
    LOGOS_ASSERT_EQ(destroying.at("state").get<std::string>(), std::string("destroying"));
    LOGOS_ASSERT_EQ(destroying.at("pending_operation").at("operation_id").get<std::string>(),
                    std::string("delivery-destroy-1"));
    LOGOS_ASSERT_TRUE(destroying.at("supported_actions").empty());
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_destroy"), 1);

    const json duplicate = json::parse(impl.nodeAction(destroy.dump()));
    LOGOS_ASSERT_TRUE(duplicate.at("accepted").get<bool>());
    LOGOS_ASSERT_TRUE(duplicate.at("duplicate").get<bool>());
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_destroy"), 1);

    LOGOS_ASSERT_TRUE(mock_delivery_complete_held_destroy(RET_OK, "destroyed"));
    LOGOS_ASSERT_TRUE(waitForLifecycleState(impl, "uninitialized", 2));
    const json uninitialized = lifecycleSnapshot(impl);
    LOGOS_ASSERT_EQ(uninitialized.at("state").get<std::string>(), std::string("uninitialized"));
    LOGOS_ASSERT_EQ(uninitialized.at("supported_actions").size(), std::size_t{1});
    LOGOS_ASSERT_EQ(uninitialized.at("supported_actions").at(0).get<std::string>(),
                    std::string("initialize"));
    LOGOS_ASSERT_TRUE(uninitialized.at("pending_operation").is_null());
    LOGOS_ASSERT_EQ(uninitialized.at("last_completed_operation").at("operation_id")
                        .get<std::string>(),
                    std::string("delivery-destroy-1"));
    LOGOS_ASSERT_EQ(uninitialized.at("last_completed_operation").at("outcome")
                        .get<std::string>(),
                    std::string("succeeded"));
    LOGOS_ASSERT_EQ(delivery_test_events::nodeChangedEventCount(), std::size_t{2});
    const json settled = lastNodeChangedEvent();
    LOGOS_ASSERT_EQ(settled.at("phase").get<std::string>(), std::string("settled"));
    LOGOS_ASSERT_EQ(settled.at("outcome").get<std::string>(), std::string("succeeded"));
    LOGOS_ASSERT_EQ(settled.at("status").at("state").get<std::string>(),
                    std::string("uninitialized"));

    delivery_test_events::resetNodeLifecycleEvents();
    const json reinitialize = lifecycleCommand(
        "delivery-reinitialize-after-destroy", "initialize",
        {{"config", R"({"logLevel":"INFO"})"}});
    LOGOS_ASSERT_TRUE(json::parse(impl.nodeAction(reinitialize.dump()))
                          .at("accepted").get<bool>());
    LOGOS_ASSERT_TRUE(waitForLifecycleState(impl, "stopped", 2));
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_create_node"), 2);
    mock_delivery_reset_held_callbacks();
}

LOGOS_TEST(nodeAction_destroy_failure_restores_stopped_state) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);
    delivery_test_events::resetNodeLifecycleEvents();
    mock_delivery_reset_held_callbacks();

    const json stopped = lifecycleSnapshot(*impl);
    mock_delivery_hold_next_destroy();
    const json acknowledgement = json::parse(impl->nodeAction(
        lifecycleCommandWithExpected("delivery-destroy-failure", "destroy", stopped).dump()));
    LOGOS_ASSERT_TRUE(acknowledgement.at("accepted").get<bool>());
    LOGOS_ASSERT_TRUE(waitForHeldDestroyCallback());
    LOGOS_ASSERT_TRUE(mock_delivery_complete_held_destroy(
        RET_ERR, "destroy callback included secret=not-for-lifecycle-event"));
    LOGOS_ASSERT_TRUE(waitForLifecycleState(*impl, "stopped", 2));

    const json status = lifecycleSnapshot(*impl);
    LOGOS_ASSERT_EQ(status.at("state").get<std::string>(), std::string("stopped"));
    LOGOS_ASSERT_EQ(status.at("last_error").at("code").get<std::string>(),
                    std::string("destroy_failed"));
    LOGOS_ASSERT_EQ(status.at("last_error").at("message").get<std::string>(),
                    std::string("Delivery destruction failed."));
    LOGOS_ASSERT_FALSE(lastNodeChangedEvent().dump().find("secret=") != std::string::npos);
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_set_event_callback"), 2);

    mock_delivery_reset_held_callbacks();
    delete impl;
}

LOGOS_TEST(nodeAction_destroy_is_a_noop_for_an_uninitialized_context) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_events::resetNodeLifecycleEvents();
    DeliveryModuleImpl impl;

    const json acknowledgement = json::parse(impl.nodeAction(
        lifecycleCommand("delivery-destroy-uninitialized", "destroy").dump()));
    LOGOS_ASSERT_TRUE(acknowledgement.at("accepted").get<bool>());
    LOGOS_ASSERT_FALSE(acknowledgement.at("duplicate").get<bool>());
    LOGOS_ASSERT_TRUE(acknowledgement.at("error").is_null());
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_destroy"), 0);
    LOGOS_ASSERT_TRUE(waitForLifecycleState(impl, "uninitialized", 2));
    const json settled = lastNodeChangedEvent();
    LOGOS_ASSERT_EQ(settled.at("phase").get<std::string>(), std::string("settled"));
    LOGOS_ASSERT_EQ(settled.at("outcome").get<std::string>(), std::string("no_op"));
}

LOGOS_TEST(nodeAction_rejects_invalid_envelopes_without_side_effects) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    delivery_test_events::resetNodeLifecycleEvents();

    const json oversizedVersion = {
        {"schema", kLifecycleCommandSchema},
        {"version", std::numeric_limits<std::uint64_t>::max()},
        {"operation_id", "delivery-invalid-1"},
        {"action", "start"},
    };
    const json acknowledgement = json::parse(impl.nodeAction(oversizedVersion.dump()));

    LOGOS_ASSERT_FALSE(acknowledgement.at("accepted").get<bool>());
    LOGOS_ASSERT_TRUE(acknowledgement.at("operation_id").is_null());
    LOGOS_ASSERT_EQ(acknowledgement.at("error").at("code").get<std::string>(),
                    std::string("invalid_request"));
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_create_node"), 0);
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_start_node"), 0);
    LOGOS_ASSERT_EQ(delivery_test_events::nodeChangedEventCount(), std::size_t{0});
}

LOGOS_TEST(nodeAction_rejects_stale_expected_snapshot_without_dispatch) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);
    delivery_test_events::resetNodeLifecycleEvents();

    json stale = lifecycleSnapshot(*impl);
    stale["sequence"] = stale.at("sequence").get<std::uint64_t>() + 1;
    const json acknowledgement = json::parse(impl->nodeAction(
        lifecycleCommandWithExpected("delivery-start-stale", "start", stale).dump()));

    LOGOS_ASSERT_FALSE(acknowledgement.at("accepted").get<bool>());
    LOGOS_ASSERT_EQ(acknowledgement.at("error").at("code").get<std::string>(),
                    std::string("state_mismatch"));
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_start_node"), 0);
    LOGOS_ASSERT_EQ(delivery_test_events::nodeChangedEventCount(), std::size_t{1});
    const json rejected = lastNodeChangedEvent();
    LOGOS_ASSERT_EQ(rejected.at("phase").get<std::string>(), std::string("settled"));
    LOGOS_ASSERT_EQ(rejected.at("outcome").get<std::string>(), std::string("rejected"));

    delete impl;
}

LOGOS_TEST(nodeAction_sanitizes_start_callback_failure) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);
    delivery_test_events::resetNodeLifecycleEvents();
    mock_delivery_reset_held_callbacks();

    mock_delivery_hold_next_start();
    const json acknowledgement = json::parse(impl->nodeAction(
        lifecycleCommand("delivery-start-failure", "start").dump()));
    LOGOS_ASSERT_TRUE(acknowledgement.at("accepted").get<bool>());
    LOGOS_ASSERT_TRUE(mock_delivery_complete_held_start(
        RET_ERR, "delivery callback included secret=not-for-lifecycle-event"));

    const json status = lifecycleSnapshot(*impl);
    LOGOS_ASSERT_EQ(status.at("state").get<std::string>(), std::string("stopped"));
    LOGOS_ASSERT_EQ(status.at("last_error").at("code").get<std::string>(),
                    std::string("start_failed"));
    LOGOS_ASSERT_EQ(status.at("last_error").at("message").get<std::string>(),
                    std::string("Delivery start failed."));
    LOGOS_ASSERT_TRUE(delivery_test_events::g_lastNodeStarted.fired);
    LOGOS_ASSERT_FALSE(delivery_test_events::g_lastNodeStarted.success);
    LOGOS_ASSERT_FALSE(delivery_test_events::lastNodeChangedEvent().find("secret=")
                       != std::string::npos);

    mock_delivery_reset_held_callbacks();
    delete impl;
}

LOGOS_TEST(legacy_lifecycle_methods_keep_completion_events_and_update_v1_status) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_create_node").returns(1);
    delivery_test_events::resetNodeLifecycleEvents();

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(R"({"logLevel":"INFO"})").success);
    LOGOS_ASSERT_EQ(lifecycleSnapshot(impl).at("state").get<std::string>(),
                    std::string("stopped"));
    LOGOS_ASSERT_TRUE(impl.start().success);
    LOGOS_ASSERT_TRUE(delivery_test_events::g_lastNodeStarted.fired);
    LOGOS_ASSERT_EQ(lifecycleSnapshot(impl).at("state").get<std::string>(),
                    std::string("running"));
    LOGOS_ASSERT_TRUE(impl.stop().success);
    LOGOS_ASSERT_TRUE(delivery_test_events::g_lastNodeStopped.fired);
    LOGOS_ASSERT_EQ(lifecycleSnapshot(impl).at("state").get<std::string>(),
                    std::string("stopped"));
    LOGOS_ASSERT_TRUE(delivery_test_events::nodeChangedEventCount() > 0);
}

// send

LOGOS_TEST(send_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;

    std::vector<uint8_t> payload{'h','e','l','l','o'};
    StdLogosResult result = impl.send("/test/1/delivery/proto", payload);
    LOGOS_ASSERT_FALSE(result.success);
}

LOGOS_TEST(send_succeeds_and_returns_request_id) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    t.mockCFunction("logosdelivery_send").returns("req-id-abc123");
    std::vector<uint8_t> payload{'h','e','l','l','o',' ','w','o','r','l','d'};
    StdLogosResult result = impl->send("/test/1/delivery/proto", payload);

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string("req-id-abc123"));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_send"));

    delete impl;
}

LOGOS_TEST(send_calls_ffi_with_byte_array_payload) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    t.mockCFunction("logosdelivery_send").returns("req-id-xyz");
    std::vector<uint8_t> payload{'t','e','s','t','-','p','a','y','l','o','a','d'};
    StdLogosResult result = impl->send("/test/1/delivery/proto", payload);

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_send"), 1);

    delete impl;
}

LOGOS_TEST(send_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    DeliveryModuleImpl implNoCtx;
    std::vector<uint8_t> payload{'p','a','y','l','o','a','d'};
    StdLogosResult failResult = implNoCtx.send("/topic", payload);
    LOGOS_ASSERT_FALSE(failResult.success);
    LOGOS_ASSERT_FALSE(failResult.error.empty());

    delete impl;
}

// subscribe

LOGOS_TEST(subscribe_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    LOGOS_ASSERT_FALSE(impl.subscribe("/test/1/delivery/proto").success);
}

LOGOS_TEST(subscribe_succeeds_with_context) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT_TRUE(impl->subscribe("/test/1/delivery/proto").success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_subscribe"));

    delete impl;
}

// unsubscribe

LOGOS_TEST(unsubscribe_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    LOGOS_ASSERT_FALSE(impl.unsubscribe("/test/1/delivery/proto").success);
}

LOGOS_TEST(unsubscribe_succeeds_with_context) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT_TRUE(impl->unsubscribe("/test/1/delivery/proto").success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_unsubscribe"));

    delete impl;
}

// Store query

LOGOS_TEST(storeQuery_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;

    LOGOS_ASSERT_FALSE(impl.storeQuery("{}", "/ip4/127.0.0.1/tcp/8645", 5000).success);
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("waku_store_query"));
}

LOGOS_TEST(storeQuery_rejects_missing_provider_or_timeout) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT_FALSE(impl->storeQuery("", "/ip4/127.0.0.1/tcp/8645/p2p/16Uuu2HBmAcHvhLqQKwSSbX6BG5JLWUDRcaLVrehUVqpw7fz1hbYc", 5000).success);
    LOGOS_ASSERT_FALSE(impl->storeQuery("{}", "", 5000).success);
    LOGOS_ASSERT_FALSE(impl->storeQuery("{}", "/ip4/127.0.0.1/tcp/8645/p2p/16Uuu2HBmAcHvhLqQKwSSbX6BG5JLWUDRcaLVrehUVqpw7fz1hbYc", 0).success);
    LOGOS_ASSERT_FALSE(impl->storeQuery("{}", "/ip4/127.0.0.1/tcp/8645/p2p/16Uuu2HBmAcHvhLqQKwSSbX6BG5JLWUDRcaLVrehUVqpw7fz1hbYc", static_cast<int64_t>(INT_MAX) + 1).success);
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("waku_store_query"));

    delete impl;
}

LOGOS_TEST(storeQuery_returns_store_response_json) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);
    const char* response = R"({"requestId":"query-1","statusCode":200,"statusDesc":"OK","messages":[]})";
    t.mockCFunction("waku_store_query").returns(response);

    StdLogosResult result = impl->storeQuery(
        R"({"requestId":"query-1","includeData":true,"paginationForward":true})",
        "/ip4/127.0.0.1/tcp/8645/p2p/16Uuu2HBmAcHvhLqQKwSSbX6BG5JLWUDRcaLVrehUVqpw7fz1hbYc",
        5000);

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string(response));
    LOGOS_ASSERT(t.cFunctionCalled("waku_store_query"));

    delete impl;
}

LOGOS_TEST(storeQuery_reports_dispatch_failure) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);
    t.mockCFunction("waku_store_query_dispatch").returns(1);

    StdLogosResult result = impl->storeQuery(
        R"({"requestId":"query-1","includeData":true,"paginationForward":true})",
        "/ip4/127.0.0.1/tcp/8645/p2p/16Uuu2HBmAcHvhLqQKwSSbX6BG5JLWUDRcaLVrehUVqpw7fz1hbYc",
        5000);

    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_FALSE(result.error.empty());
    LOGOS_ASSERT(t.cFunctionCalled("waku_store_query"));

    delete impl;
}

LOGOS_TEST(storeQuery_reports_callback_failure) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);
    t.mockCFunction("waku_store_query").returns("Store provider rejected the request");
    t.mockCFunction("waku_store_query_callback_result").returns(RET_ERR);

    StdLogosResult result = impl->storeQuery(
        R"({"requestId":"query-1","includeData":true,"paginationForward":true})",
        "/ip4/127.0.0.1/tcp/8645/p2p/16Uuu2HBmAcHvhLqQKwSSbX6BG5JLWUDRcaLVrehUVqpw7fz1hbYc",
        5000);

    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_CONTAINS(result.error, "Store provider rejected the request");
    LOGOS_ASSERT(t.cFunctionCalled("waku_store_query"));

    delete impl;
}

// getConnectedPeersInfo

LOGOS_TEST(getConnectedPeersInfo_returns_raw_peer_metadata_json) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);
    const char* peers = R"({"16Uiu2HAmExample":{"protocols":["/vac/waku/store-query/3.0.0"],"addresses":["/ip4/203.0.113.1/tcp/30303"]}})";
    t.mockCFunction("waku_get_connected_peers_info").returns(peers);

    StdLogosResult result = impl->getConnectedPeersInfo();

    LOGOS_ASSERT_TRUE(result.success);
    const json parsed = json::parse(result.value.get<std::string>());
    LOGOS_ASSERT_TRUE(parsed.is_object());
    LOGOS_ASSERT_EQ(parsed.at("16Uiu2HAmExample").at("protocols").at(0).get<std::string>(),
                    std::string("/vac/waku/store-query/3.0.0"));
    LOGOS_ASSERT_EQ(parsed.at("16Uiu2HAmExample").at("addresses").at(0).get<std::string>(),
                    std::string("/ip4/203.0.113.1/tcp/30303"));
    LOGOS_ASSERT(t.cFunctionCalled("waku_get_connected_peers_info"));

    delete impl;
}

LOGOS_TEST(getConnectedPeersInfo_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;

    StdLogosResult result = impl.getConnectedPeersInfo();

    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_EQ(result.error, std::string("Context not initialized"));
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("waku_get_connected_peers_info"));
}

LOGOS_TEST(getConnectedPeersInfo_reports_dispatch_failure) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);
    t.mockCFunction("waku_get_connected_peers_info_dispatch").returns(1);

    StdLogosResult result = impl->getConnectedPeersInfo();

    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_EQ(result.error, std::string("failed to initiate get_connected_peers_info"));
    LOGOS_ASSERT(t.cFunctionCalled("waku_get_connected_peers_info"));

    delete impl;
}

LOGOS_TEST(getConnectedPeersInfo_reports_callback_failure) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);
    t.mockCFunction("waku_get_connected_peers_info_callback_result").returns(1);
    t.mockCFunction("waku_get_connected_peers_info").returns("connected peers unavailable");

    StdLogosResult result = impl->getConnectedPeersInfo();

    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_EQ(result.error, std::string("connected peers unavailable"));
    LOGOS_ASSERT(t.cFunctionCalled("waku_get_connected_peers_info"));

    delete impl;
}

// getAvailableNodeInfoIDs

LOGOS_TEST(getAvailableNodeInfoIDs_returns_mocked_string) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    t.mockCFunction("logosdelivery_get_available_node_info_ids").returns("@[Version,PeerID]");
    StdLogosResult result = impl->getAvailableNodeInfoIDs();

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_get_available_node_info_ids"));
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string("@[Version,PeerID]"));

    delete impl;
}

LOGOS_TEST(getAvailableNodeInfoIDs_returns_empty_on_ffi_failure) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    StdLogosResult result = impl.getAvailableNodeInfoIDs();
    LOGOS_ASSERT_FALSE(result.success);
}

// getNodeInfo

LOGOS_TEST(getNodeInfo_returns_mocked_value_for_attribute) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    t.mockCFunction("logosdelivery_get_node_info").returns("v1.2.3");
    StdLogosResult result = impl->getNodeInfo("Version");

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string("v1.2.3"));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_get_node_info"));

    delete impl;
}

// getAvailableConfigs

LOGOS_TEST(getAvailableConfigs_returns_mocked_json) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    t.mockCFunction("logosdelivery_get_available_configs").returns(R"([{"key":"mode","type":"string"}])");
    StdLogosResult result = impl->getAvailableConfigs();

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_get_available_configs"));

    delete impl;
}

LOGOS_TEST(getAvailableConfigs_returns_empty_on_ffi_failure) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    StdLogosResult result = impl.getAvailableConfigs();
    LOGOS_ASSERT_FALSE(result.success);
}

// collectOpenMetricsText

LOGOS_TEST(collectOpenMetricsText_returns_empty_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;

    LOGOS_ASSERT_EQ(impl.collectOpenMetricsText(), std::string(""));
    // No context -> we must not even attempt the FFI read.
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("logosdelivery_get_node_info"));
}

LOGOS_TEST(collectOpenMetricsText_returns_metrics_text_verbatim) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    const char* promText =
        "# HELP waku_node_messages_total number of messages\n"
        "# TYPE waku_node_messages_total counter\n"
        "waku_node_messages_total{shard=\"0\"} 42\n";
    t.mockCFunction("logosdelivery_get_node_info").returns(promText);

    // The module is a pure passthrough: the openmetrics scraper does the parsing.
    LOGOS_ASSERT_EQ(impl->collectOpenMetricsText(), std::string(promText));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_get_node_info"));

    delete impl;
}

// module name

LOGOS_TEST(name_returns_delivery_module) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    LOGOS_ASSERT_EQ(impl.name(), std::string("delivery_module"));
}
