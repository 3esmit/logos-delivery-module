"""Single-daemon e2e checks.

These prove delivery_module loads into a real logoscore daemon and answers over
RPC — the codegen + transport + StdLogosResult path the in-process C++ tests
don't exercise. Each uses a fresh daemon (createNode is once-per-context).
"""

from __future__ import annotations

from libs.constants import CONTENT_TOPIC
from libs.helpers import MODULE, call_ok, call_result, make_delivery_config


def test_module_loads_and_lifecycle(solo_daemon):
    client = solo_daemon.client()
    client.load_module(MODULE)
    call_ok(client, "createNode", make_delivery_config(), timeout=90.0)
    call_ok(client, "start", timeout=90.0)
    call_ok(client, "stop", timeout=90.0)


def test_createNode_twice_rejected(solo_daemon):
    client = solo_daemon.client()
    client.load_module(MODULE)
    call_ok(client, "createNode", make_delivery_config(), timeout=90.0)

    second = call_result(client, "createNode", make_delivery_config(), timeout=90.0)
    assert second.get("success") is False, f"expected the second createNode to be rejected, got {second!r}"
    assert second.get("error"), "rejection carried no error message"


def test_query_and_subscribe_methods(solo_daemon):
    """Every post-start RPC method round-trips: the queries, plus subscribe +
    unsubscribe (the only public methods otherwise unexercised e2e)."""
    client = solo_daemon.client()
    client.load_module(MODULE)
    call_ok(client, "createNode", make_delivery_config(), timeout=90.0)
    call_ok(client, "start", timeout=90.0)

    assert call_ok(client, "getAvailableConfigs"), "getAvailableConfigs returned empty"
    assert call_ok(client, "getAvailableNodeInfoIDs"), "getAvailableNodeInfoIDs returned empty"

    # Each advertised id resolves; value may legitimately be empty for an
    # unconfigured feature, so only require the lookup to succeed.
    for info_id in ("Version", "MyPeerId", "MyMultiaddresses"):
        call_ok(client, "getNodeInfo", info_id)

    call_ok(client, "subscribe", CONTENT_TOPIC)
    call_ok(client, "unsubscribe", CONTENT_TOPIC)

    # version() returns a plain string, not a StdLogosResult.
    version = client.call(MODULE, "version")
    if isinstance(version, dict):
        version = version.get("value", "")
    assert isinstance(version, str) and version, f"unexpected version: {version!r}"
