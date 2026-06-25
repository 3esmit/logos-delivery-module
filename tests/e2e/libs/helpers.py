"""Drive `delivery_module` over the logoscore RPC client.

Unlike chat_module, delivery_module's public methods are SYNCHRONOUS: each
returns a StdLogosResult `{success, value, error}` that `client.call` hands back
directly — there are no per-call result events. Only message delivery is async,
surfacing as the typed events messageReceived / messagePropagated / messageSent
/ messageError / connectionStateChanged.
"""

from __future__ import annotations

import json
import logging
import re
import subprocess
from dataclasses import dataclass
from typing import Any, Optional

from libs.constants import CLUSTER_ID, NODE_TCP_PORT, NUM_SHARDS_IN_NETWORK

logger = logging.getLogger(__name__)

MODULE = "delivery_module"

SUBSCRIBE_GRACE_S = 0.5


def make_delivery_config(*, tcp_port: int = NODE_TCP_PORT, static_peers: Optional[list[str]] = None) -> str:
    """JSON config for `createNode` — relay-only, single-shard, discovery-off,
    matching the two-node relay setup proven by logos-delivery-interop-tests."""
    cfg: dict[str, Any] = {
        "logLevel": "DEBUG",
        "listenAddress": "0.0.0.0",
        "tcpPort": tcp_port,
        "clusterId": CLUSTER_ID,
        "numShardsInNetwork": NUM_SHARDS_IN_NETWORK,
        "relay": True,
        "store": False,
        "filter": False,
        "lightpush": False,
        "peerExchange": False,
        "discv5Discovery": False,
        "reliabilityEnabled": True,
    }
    if static_peers:
        cfg["staticnodes"] = static_peers
    return json.dumps(cfg)


def call_result(client, method: str, *args: Any, timeout: Optional[float] = None) -> dict:
    """Call a delivery method and return its StdLogosResult dict. No assertion."""
    result = client.call(MODULE, method, *args, timeout=timeout)
    if not isinstance(result, dict):
        raise AssertionError(f"{method} returned non-dict result: {result!r}")
    return result


def call_ok(client, method: str, *args: Any, timeout: Optional[float] = None) -> Any:
    """Call a delivery method, assert StdLogosResult.success, return its value."""
    result = call_result(client, method, *args, timeout=timeout)
    if not result.get("success"):
        raise AssertionError(f"{method}({args!r}) failed: {result.get('error')!r}")
    return result.get("value")


@dataclass
class DeliveryNode:
    client: Any
    daemon: Any
    label: str


def setup_delivery_node(daemon, config_json: str, label: str) -> DeliveryNode:
    """load_module → createNode → start. Returns a started node.

    createNode/start get a generous timeout: cold-starting liblogosdelivery can
    take tens of seconds, and the module itself waits up to 30s on the FFI callback.
    """
    client = daemon.client()
    client.load_module(MODULE)
    call_ok(client, "createNode", config_json, timeout=90.0)
    call_ok(client, "start", timeout=90.0)
    return DeliveryNode(client=client, daemon=daemon, label=label)


def node_multiaddr(node: DeliveryNode) -> str:
    """The node's libp2p multiaddr with the container IP substituted in.

    MyMultiaddresses advertises the configured listen address (0.0.0.0), which a
    peer can't dial; rewrite the /ip4/ segment to the container's address on the
    shared docker network so the peer reaches it container-to-container.
    """
    raw = call_ok(node.client, "getNodeInfo", "MyMultiaddresses")
    addr = _first_multiaddr(raw)
    ip = _container_ip(node.daemon)
    routable = re.sub(r"/ip4/[^/]+/", f"/ip4/{ip}/", addr, count=1)
    if "/ip4/" not in routable:
        raise AssertionError(f"multiaddr has no /ip4/ segment to rewrite: {addr!r}")
    return routable


def _first_multiaddr(raw: str) -> str:
    addr = (raw or "").strip().strip("@[]")
    for part in re.split(r"[,\n]", addr):
        part = part.strip()
        if part.startswith("/"):
            return part
    raise AssertionError(f"no multiaddr in MyMultiaddresses: {raw!r}")


def _container_ip(daemon) -> str:
    fmt = '{{(index .NetworkSettings.Networks "' + str(daemon.network) + '").IPAddress}}'
    r = subprocess.run(
        ["docker", "inspect", "-f", fmt, daemon.container_id],
        capture_output=True, text=True,
    )
    ip = r.stdout.strip()
    if r.returncode != 0 or not ip:
        raise AssertionError(
            f"could not resolve container IP for {daemon.container_name}: {r.stderr.strip()!r}"
        )
    return ip


def wait_for_event(waiter, event_name: str, *, timeout: float, predicate=None):
    """Return the next event named `event_name` (subscribe() doesn't filter by
    event name, only by module, so re-filter here). Raises on timeout."""
    def _match(e):
        if e.get("event") != event_name:
            return False
        return predicate(e) if predicate else True

    return waiter.next(predicate=_match, timeout=timeout)


def parse_event(event: dict) -> dict:
    """The event's positional args as a dict {arg0, arg1, ...}.

    The logoscore CLI serializes a typed event as
    {"event": <name>, "data": {"arg0": <1st arg>, "arg1": <2nd arg>, ...}}
    (logos-logoscore-cli client.cpp watchModuleEvents), so each codegen event arg
    lands at argN by position. The vector<uint8_t> payload arg of messageReceived
    is the one whose serialization is still unverified.
    """
    data = event.get("data", {})
    return data if isinstance(data, dict) else {}


def event_arg(event: dict, index: int) -> Any:
    """Value of positional arg `index`, or None if absent."""
    return parse_event(event).get(f"arg{index}")


def event_request_id(event: dict) -> Optional[str]:
    """requestId of a messageSent/messagePropagated/messageError event (arg 0)."""
    rid = event_arg(event, 0)
    return str(rid) if rid is not None else None


def event_content_topic(event: dict) -> Optional[str]:
    """contentTopic of a messageReceived event (arg 1)."""
    ct = event_arg(event, 1)
    return str(ct) if ct is not None else None
