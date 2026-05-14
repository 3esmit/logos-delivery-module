# Infra dashboard — collecting node info via `logoscore`

Reference for the [Logos fleet dashboard](https://github.com/status-im/infra-logos/issues/4): how to scrape node identity, version, and Prometheus metrics from a Logos node that runs as `logoscore` + `delivery_module` in a Docker container. **No REST API is exposed** — all data comes through the `logoscore` CLI, which talks to the daemon over a local Unix socket inside the container.

Companion to [`run-locally.md`](./run-locally.md) (covers the build/run pipeline this doc assumes).

## Required change to the infra role

The current [`infra-role-logos-node`](https://github.com/status-im/infra-role-logos-node) entrypoint runs `logoscore` in **inline mode** (`--load-modules X -c module.method(...)`). There is no daemon socket in that mode, so `logoscore call ...` from `docker exec` cannot work. The role currently works around this by grepping `docker logs` for the peer ID / ENR / mix key (`tasks/delivery/query.yml` — every step is marked `FIXME: Use API when available!`).

**To enable API-driven queries, switch the entrypoint to daemon mode.** Replace `templates/entrypoint.sh.j2` with:

```bash
#!/usr/bin/env bash
set -e

# Start the daemon in the background
./logos/bin/logoscore -D -m ./modules &
DAEMON_PID=$!

# Wait for the local socket to come up, then load + start the modules
until ./logos/bin/logoscore status --json >/dev/null 2>&1; do sleep 1; done

{% for m in logos_node_modules %}
./logos/bin/logoscore load-module {{ m }}
{% endfor %}
{% for cmd in logos_node_module_commands %}
./logos/bin/logoscore call {{ cmd | replace('.', ' ', 1) | replace('(', ' ') | replace(')', '') }}
{% endfor %}

# Hand PID 1 over to the daemon so docker stop works
wait $DAEMON_PID
```

(The `-c module.method(args)` → `call module method args` rewrite is a literal string transform; you may prefer to express the commands directly as `logoscore call` lines in the role's defaults.)

With that change, `~/.logoscore/{daemon,client}` exists inside the container and `docker exec logos-node /app/logos/bin/logoscore ...` works for everything below.

## Outputs needed by the dashboard

| Dashboard field        | Command (run inside the container)                                  | Source / notes |
|------------------------|---------------------------------------------------------------------|----------------|
| Logos Core version     | `logoscore status --json` → `.daemon.version`                       | Version of the `logoscore` binary itself. |
| Loaded modules         | `logoscore status --json` → `.modules[]`                            | Each entry: `name`, `status` (`loaded` / `not_loaded` / `crashed`). Per-module version is **not** in the status payload today — see "Known gaps". |
| Delivery node version  | `logoscore call delivery_module getNodeInfo Version --json`         | Returns `liblogosdelivery` git version. May currently return `"n/a"` — see "Known gaps". |
| Peer ID                | `logoscore call delivery_module getNodeInfo MyPeerId --json`        | libp2p peer id. Replaces the `docker logs | grep listenAddresses` hack. |
| ENR                    | `logoscore call delivery_module getNodeInfo MyENR --json`           | discv5 ENR URI. Replaces the `docker logs | grep 'discoverable ENR'` hack. |
| Multiaddresses         | `logoscore call delivery_module getNodeInfo MyMultiaddresses --json` | Comma-separated listen addrs. |
| Prometheus metrics     | `logoscore call delivery_module getNodeInfo Metrics --json`         | Full Prometheus text exposition. Includes `libp2p_peers` (connected peer count) and all waku/libp2p counters. |

`getAvailableNodeInfoIDs` returns the canonical list at runtime — currently `[Version, Metrics, MyMultiaddresses, MyENR, MyPeerId]`.

## Response shape

Every `logoscore --json` response wraps the value:

```json
{ "status": "ok",
  "module": "delivery_module",
  "method": "getNodeInfo",
  "result": { "success": true, "error": null, "value": "<payload>" } }
```

The actual value lives at `.result.value`. Errors surface as `.status == "error"` (CLI-level) or `.result.success == false` (module-level).

## Drop-in replacement for `tasks/delivery/query.yml`

```yaml
---
- name: Get peer ID from delivery_module
  shell: >
    docker exec {{ logos_node_cont_name }}
    /app/logos/bin/logoscore --json call delivery_module getNodeInfo MyPeerId
    | jq -r '.result.value'
  register: logos_node_peer_id_raw
  changed_when: false
  failed_when: false

- name: Get ENR from delivery_module
  shell: >
    docker exec {{ logos_node_cont_name }}
    /app/logos/bin/logoscore --json call delivery_module getNodeInfo MyENR
    | jq -r '.result.value'
  register: logos_node_enr_raw
  changed_when: false
  failed_when: false

- name: Get listen multiaddresses from delivery_module
  shell: >
    docker exec {{ logos_node_cont_name }}
    /app/logos/bin/logoscore --json call delivery_module getNodeInfo MyMultiaddresses
    | jq -r '.result.value'
  register: logos_node_multiaddr_raw
  changed_when: false
  failed_when: false

- name: Set delivery node facts
  set_fact:
    logos_node_peer_id:   '{{ logos_node_peer_id_raw.stdout   | default("none", true) }}'
    logos_node_enr:       '{{ logos_node_enr_raw.stdout       | default("none", true) }}'
    logos_node_multiaddr: '{{ logos_node_multiaddr_raw.stdout | default("none", true) }}'
```

The mix-public-key extraction has no equivalent `getNodeInfo` ID yet (`Mix` is not in `NodeInfoId` upstream); keep the log-grep for that one until the module adds it.

## Scrape script for the fleet dashboard

The dashboard collector (Python script in `infra-sites/ansible/roles/fleets-dash-*`) can hit each node's docker daemon over SSH and run:

```bash
#!/usr/bin/env bash
set -euo pipefail
EXEC="docker exec logos-node /app/logos/bin/logoscore --json"

core_version=$($EXEC status | jq -r '.daemon.version')
modules=$(    $EXEC status | jq -c '.modules')

node_version=$($EXEC call delivery_module getNodeInfo Version          | jq -r '.result.value')
peer_id=$(    $EXEC call delivery_module getNodeInfo MyPeerId          | jq -r '.result.value')
enr=$(        $EXEC call delivery_module getNodeInfo MyENR             | jq -r '.result.value')
maddrs=$(     $EXEC call delivery_module getNodeInfo MyMultiaddresses  | jq -r '.result.value')

# Prometheus exposition — write to a textfile collector for node_exporter to pick up
$EXEC call delivery_module getNodeInfo Metrics | jq -r '.result.value' \
  > /var/lib/node_exporter/textfile/delivery.prom
```

Same shape the existing fleet-dash collectors (`fleets-dash-waku`, `fleets-dash-nimbus`) use — they produce a per-node JSON blob and render it client-side.

## Remote scraping (alternative to `docker exec`)

If the collector cannot SSH+exec into each host, expose the daemon's RPC over TCP+TLS instead:

1. Add a TLS listener to the entrypoint:

   ```bash
   ./logos/bin/logoscore -D -m ./modules \
       --module-transport core_service=tcp_ssl,host=0.0.0.0,port=6443,cert=/conf/cert.pem,key=/conf/key.pem
   ```

2. Expose the port in `docker-compose.yml`:

   ```yaml
   ports:
     - "6443:6443/tcp"
   ```

3. Issue a token on each node and copy it to the collector host:

   ```bash
   docker exec logos-node /app/logos/bin/logoscore issue-token --name dashboard
   # → /root/.logoscore/daemon/tokens/dashboard.json inside the container
   ```

4. On the collector, dial the node:

   ```bash
   LOGOSCORE_TOKEN=$(jq -r .token /etc/logoscore/dashboard.json) \
   LOGOSCORE_CLIENT_TCP_HOST=node.fleet.logos.co \
   LOGOSCORE_CLIENT_TCP_PORT=6443 \
   logoscore --json call delivery_module getNodeInfo MyPeerId
   ```

`docker exec` is simpler if the collector already has SSH to each host; TCP+TLS is needed if you want a single collector with no SSH path to the fleet.

## Known gaps

- **Per-module versions** are not exposed by `logoscore status` / `list-modules`. The manifest knows the version (`manifest.json["version"]`), and each plugin implements `version()`, but neither is routed to the CLI today. Workaround: read the manifest directly inside the container (`docker exec logos-node cat /app/modules/delivery_module/manifest.json | jq -r .version`) or pull from `getNodeInfo Version` for the delivery node version specifically.
- **`getNodeInfo Version` may return `"n/a"`** when `liblogosdelivery` was built without git version info baked in. Tracked upstream — treat it as best-effort.
- **No Prometheus HTTP endpoint.** Logos Core does not host an HTTP server. The dashboard must pull metrics via `getNodeInfo Metrics` and re-expose them (textfile collector / push gateway / scrape-by-proxy). Background: [logos-liblogos#118](https://github.com/logos-co/logos-liblogos/issues/118), [logos-liblogos#120](https://github.com/logos-co/logos-liblogos/issues/120).
- **Mix public key** has no `getNodeInfo` ID upstream yet — keep the existing log-grep until it's added.
