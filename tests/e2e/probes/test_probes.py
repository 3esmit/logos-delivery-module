"""Three probes that separate the candidate causes of the delivery_module
RPC-reply wedge investigated in PR #51.

Established from the CI run: a `send` issued while an event-watcher is attached
makes the sender node's RPC replies stop arriving (20s callModuleMethod
timeouts) for that node and every later call to it — even though the method
runs server-side in ~2ms and the message is delivered. The original failing
window confounds three factors: (1) events are emitted, (2) a watcher is
attached, (3) the loopback `messageReceived` carries a vector<uint8_t> payload.

Each probe removes one factor and asserts the channel stays alive (a cheap
read-only `getNodeInfo` returns). Read the results as a matrix:

  A wedges                      → event emission ALONE wedges; watcher irrelevant.
  A ok, B wedges                → watcher subscription ALONE wedges; no event needed.
  A ok, B ok, C wedges          → messagePropagated transmission to a watcher wedges.
  A ok, B ok, C ok              → the loopback messageReceived (vector<uint8_t>)
                                  transmission is the wedge (it's the only factor
                                  C drops vs the original failing test).
"""

from __future__ import annotations

import time

import pytest
from logos_integration_test_framework import subscribe
from logoscore.errors import MethodError

from libs.constants import CONTENT_TOPIC
from libs.helpers import MODULE, SUBSCRIBE_GRACE_S, call_ok, wait_for_event

pytestmark = pytest.mark.two_node

PROBE_TIMEOUT_S = 15.0
PROPAGATED_TIMEOUT_S = 40.0


def _assert_alive(node, where: str) -> None:
    try:
        call_ok(node.client, "getNodeInfo", "MyMultiaddresses", timeout=PROBE_TIMEOUT_S)
    except MethodError as e:
        pytest.fail(f"{node.label} RPC reply channel WEDGED {where}: {e}")


def test_probe_a_send_without_watcher(fresh_pair):
    """Emit events (incl. loopback messageReceived) with NO watcher attached.
    Pass ⇒ emission alone is harmless; the wedge needs a watcher transmission."""
    node_a, node_b = fresh_pair
    call_ok(node_a.client, "subscribe", CONTENT_TOPIC)
    call_ok(node_b.client, "subscribe", CONTENT_TOPIC)
    time.sleep(SUBSCRIBE_GRACE_S)

    try:
        request_id = call_ok(node_a.client, "send", CONTENT_TOPIC, "probe-a", timeout=PROBE_TIMEOUT_S)
    except MethodError as e:
        pytest.fail(f"send WEDGED with no watcher attached ⇒ event emission alone wedges: {e}")
    assert request_id, "send returned an empty requestId"

    _assert_alive(node_a, "after a send with no watcher attached")


def test_probe_b_watcher_without_send(fresh_pair):
    """Attach a watcher but emit no event. Pass ⇒ the subscription alone is
    harmless; an event must actually flow to wedge the channel."""
    node_a, _node_b = fresh_pair
    with subscribe(node_a.client, MODULE, "messagePropagated"):
        time.sleep(SUBSCRIBE_GRACE_S)
        _assert_alive(node_a, "with a watcher attached but no event emitted")


def test_probe_c_no_self_subscribe(fresh_pair):
    """Send WITH a watcher, but the sender is NOT subscribed to its own topic,
    so no loopback messageReceived fires — only messagePropagated reaches the
    watcher. Pass ⇒ messageReceived(vector<uint8_t>) is implicated; wedge here
    ⇒ messagePropagated transmission is, which is more fundamental."""
    node_a, node_b = fresh_pair
    call_ok(node_b.client, "subscribe", CONTENT_TOPIC)

    with subscribe(node_a.client, MODULE, "messagePropagated") as w:
        time.sleep(SUBSCRIBE_GRACE_S)
        try:
            request_id = call_ok(node_a.client, "send", CONTENT_TOPIC, "probe-c", timeout=PROBE_TIMEOUT_S)
        except MethodError as e:
            pytest.fail(
                "send WEDGED without a loopback messageReceived ⇒ messagePropagated "
                f"transmission to the watcher is the cause, not the vector<uint8_t> payload: {e}"
            )
        assert request_id, "send returned an empty requestId"
        wait_for_event(w, "messagePropagated", timeout=PROPAGATED_TIMEOUT_S)

    _assert_alive(node_a, "after a no-loopback send")
