"""Diagnostic probe harness for the delivery_module RPC-reply wedge (PR #51).

Each probe gets a FRESH two-node pair (function scope) so a wedge in one probe
can't pollute the next — unlike the module-scoped node_a/node_b in the parent
suite, where the first wedged node makes every later test on it time out.

Set E2E_QT_DEBUG=1 to inject Qt RemoteObjects framework logging into the daemon
containers (QT_LOGGING_RULES), so the transport-level break shows up in the
captured `docker logs` instead of being inferred from behaviour.
"""

from __future__ import annotations

import os
import subprocess
import time
import uuid
from collections.abc import Iterator
from contextlib import ExitStack
from pathlib import Path

import pytest
import logoscore.docker_daemon as _docker_daemon
from logoscore import LogoscoreDockerDaemon

from libs.helpers import DeliveryNode, make_delivery_config, node_multiaddr, setup_delivery_node

MESH_STABILIZATION_S = 12.0


@pytest.fixture(scope="session", autouse=True)
def _qt_remoteobjects_logging() -> Iterator[None]:
    if not os.environ.get("E2E_QT_DEBUG"):
        yield
        return
    rules = os.environ.get("E2E_QT_LOGGING_RULES", "qt.remoteobjects.*=true")
    real_run = _docker_daemon.subprocess.run

    def _run(cmd, *args, **kwargs):
        if isinstance(cmd, (list, tuple)) and list(cmd[:3]) == ["docker", "run", "-d"]:
            cmd = [*cmd[:3], "-e", f"QT_LOGGING_RULES={rules}",
                   "-e", "QT_FORCE_STDERR_LOGGING=1", *cmd[3:]]
        return real_run(cmd, *args, **kwargs)

    _docker_daemon.subprocess.run = _run
    try:
        yield
    finally:
        _docker_daemon.subprocess.run = real_run


def _save_logs(container_name: str) -> None:
    log_dir = Path(os.environ.get("E2E_LOG_DIR", "/tmp"))
    log_dir.mkdir(parents=True, exist_ok=True)
    r = subprocess.run(["docker", "logs", container_name], capture_output=True, text=True)
    (log_dir / f"{container_name}.log").write_text((r.stdout or "") + (r.stderr or ""))


@pytest.fixture
def fresh_pair(
    _e2e_env_or_skip: tuple[str, Path],
    shared_docker_network: str,
) -> Iterator[tuple[DeliveryNode, DeliveryNode]]:
    """Two freshly-created, started, directly-peered delivery nodes, torn down
    after the test. A dials nothing; B dials A via staticnodes."""
    image, modules_dir = _e2e_env_or_skip

    with ExitStack() as stack:
        def _make(label: str, **cfg) -> DeliveryNode:
            container_name = f"logoscore-probe-{label.lower()}-{uuid.uuid4().hex[:8]}"
            daemon = LogoscoreDockerDaemon(
                image=image,
                modules_dir=modules_dir,
                container_name=container_name,
                network=shared_docker_network,
                startup_timeout=60.0,
                extra_args=["--verbose"],
            )
            stack.enter_context(daemon)
            stack.callback(_save_logs, container_name)
            return setup_delivery_node(daemon, make_delivery_config(**cfg), label)

        node_a = _make("A")
        node_b = _make("B", static_peers=[node_multiaddr(node_a)])
        time.sleep(MESH_STABILIZATION_S)
        yield node_a, node_b
