"""Networking + waku-cluster constants for the two-daemon e2e fixtures."""

from __future__ import annotations

# A private /16 distinct from anything chat-module's e2e uses, so both suites
# can run on the same docker host without subnet clashes.
NETWORK_SUBNET = "172.31.0.0/16"

# Isolated single-shard cluster: with numShardsInNetwork=1 every content topic
# maps to shard 0, so both nodes share one pubsub topic regardless of the topic
# string. clusterId mirrors logos-delivery-interop-tests' DEFAULT_CLUSTER_ID — a
# string, the form proven against this liblogosdelivery build.
CLUSTER_ID = "198"
NUM_SHARDS_IN_NETWORK = 1

# Waku TCP port each delivery container binds for libp2p. Each daemon runs in
# its own container (separate network namespace), so the value is reused across
# nodes without collision. discv5 is off — peering is via staticnodes.
NODE_TCP_PORT = 60000

# One content topic for the whole suite (→ shard 0 under single-shard sharding).
CONTENT_TOPIC = "/test/1/logos-delivery-e2e/proto"
