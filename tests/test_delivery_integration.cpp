// Integration tests for DeliveryModulePlugin - uses the REAL liblogosdelivery library.
// No mocking. These tests start an actual delivery node and exercise the API
// as shown in examples/simple.cpp.
//
// Requires liblogosdelivery to be available in ../lib at build time.
// Skipped automatically when liblogosdelivery is not found.

#include <logos_test.h>
#include "delivery_module_plugin.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

// ---------------------------------------------------------------------------
// EventWaiter - captures events emitted via the plugin signal.
// Installed as a mock LogosAPI client via initLegacy() so that emitEvent()
// reaches the waiter.
// ---------------------------------------------------------------------------

// Minimal config - no preset, no network peers, Edge mode with relay+sharding
// so subscribe/send can be exercised without connecting to any external nodes.
static const char* kMinimalConfig = R"({
  "logLevel": "INFO",
  "mode": "Edge",
  "relay": true,
  "numShardsInNetwork": 8
})";

static const char* kTestTopic = "/test/2/delivery-integration/proto";

static const int DEFAULT_TIMEOUT_MS = 30000;

// ---------------------------------------------------------------------------
// Shared plugin instance - restarted before each test group.
// ---------------------------------------------------------------------------

static DeliveryModulePlugin* g_plugin = nullptr;

static void ensureStarted() {
    if (g_plugin) {
        g_plugin->stop();
        delete g_plugin;
        g_plugin = nullptr;
    }

    g_plugin = new DeliveryModulePlugin();

    if (!g_plugin->createNode(kMinimalConfig).success) {
        delete g_plugin;
        g_plugin = nullptr;
        throw LogosTestFailure("Integration: failed to createNode.");
    }

    if (!g_plugin->start().success) {
        delete g_plugin;
        g_plugin = nullptr;
        throw LogosTestFailure("Integration: failed to start node.");
    }
}

// ---------------------------------------------------------------------------
// Tests - lifecycle
// ---------------------------------------------------------------------------

LOGOS_TEST(integration_createNode) {
    DeliveryModulePlugin plugin;
    LOGOS_ASSERT_TRUE(plugin.createNode(kMinimalConfig).success);
    plugin.stop();
}

LOGOS_TEST(integration_createNode_with_logos_dev_preset) {
    DeliveryModulePlugin plugin;
    LOGOS_ASSERT_TRUE(plugin.createNode(R"({"logLevel":"DEBUG","mode":"Core","preset":"logos.dev"})").success);
    plugin.stop();
}

LOGOS_TEST(integration_start_stop) {
    DeliveryModulePlugin plugin;
    LOGOS_ASSERT_TRUE(plugin.createNode(kMinimalConfig).success);
    LOGOS_ASSERT_TRUE(plugin.start().success);
    LOGOS_ASSERT_TRUE(plugin.stop().success);
}

// ---------------------------------------------------------------------------
// Tests - queries (mirror the simple.cpp info loop)
// ---------------------------------------------------------------------------

LOGOS_TEST(integration_getAvailableConfigs_returns_non_empty) {
    ensureStarted();

    LogosResult result = g_plugin->getAvailableConfigs();
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_FALSE(result.getString().isEmpty());
}

LOGOS_TEST(integration_getAvailableNodeInfoIDs_returns_non_empty) {
    ensureStarted();

    LogosResult result = g_plugin->getAvailableNodeInfoIDs();
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_FALSE(result.getString().isEmpty());
}

LOGOS_TEST(integration_getNodeInfo_returns_value_for_each_id) {
    ensureStarted();

    LogosResult idsResult = g_plugin->getAvailableNodeInfoIDs();
    LOGOS_ASSERT_TRUE(idsResult.success);

    QString nodeInfoIDs = idsResult.getString();
    LOGOS_ASSERT_FALSE(nodeInfoIDs.isEmpty());

    // IDs are returned as "@[ID1,ID2,...]" - strip the "@[" prefix and "]" suffix.
    if (nodeInfoIDs.startsWith("@[") && nodeInfoIDs.endsWith("]")) {
        nodeInfoIDs = nodeInfoIDs.mid(2, nodeInfoIDs.length() - 3);
    }

    QStringList ids = nodeInfoIDs.split(",", Qt::SkipEmptyParts);
    LOGOS_ASSERT_GT(static_cast<int>(ids.size()), 0);

    for (const QString& id : ids) {
        QString trimmedId = id.trimmed();
        LogosResult infoResult = g_plugin->getNodeInfo(trimmedId);
        LOGOS_ASSERT_TRUE(infoResult.success);
        LOGOS_ASSERT_FALSE(infoResult.getString().isEmpty());
    }
}

// ---------------------------------------------------------------------------
// Tests - pub/sub (as in simple.cpp)
// ---------------------------------------------------------------------------

LOGOS_TEST(integration_subscribe_succeeds) {
    ensureStarted();
    LOGOS_ASSERT_TRUE(g_plugin->subscribe(kTestTopic).success);
}

LOGOS_TEST(integration_subscribe_unsubscribe) {
    ensureStarted();

    LOGOS_ASSERT_TRUE(g_plugin->subscribe(kTestTopic).success);
    LOGOS_ASSERT_TRUE(g_plugin->unsubscribe(kTestTopic).success);
}

// ---------------------------------------------------------------------------
// Tests - send (as in simple.cpp interactive loop)
// ---------------------------------------------------------------------------

LOGOS_TEST(integration_send_returns_success_with_request_id) {
    ensureStarted();

    LOGOS_ASSERT_TRUE(g_plugin->subscribe(kTestTopic).success);

    LogosResult result = g_plugin->send(kTestTopic, "hello from integration test");

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_FALSE(result.getString().isEmpty());
}
