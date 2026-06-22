# delivery_module RPC-reply wedge — diagnostic probes

A throwaway harness to pin down **why** the two-node tests in PR #51 fail.

## What we already know (from the CI run)

Delivery works: node B receives the message; the sender sees `messagePropagated`.
But the synchronous `send` RPC call **times out at the client (20s)** even though
the module executes it server-side in ~2ms and keeps executing later calls
(`Subscribe completed … success`). So the module's **RPC reply channel wedges** —
replies stop reaching the CLI — and every later call to that node times out.

The wedge starts in the `send` window, which changes three things at once:
1. events are emitted (`messageReceived` loopback + `messagePropagated`),
2. a `watchModuleEvents` watcher is attached for the first time,
3. the loopback `messageReceived` carries a `vector<uint8_t>` payload.

No single one is yet proven. These probes remove one factor each.

## The probes

| Probe | Setup | Pass means |
|---|---|---|
| `test_probe_a_send_without_watcher` | sender self-subscribes, sends, **no watcher** | emission alone is harmless → a watcher transmission is needed to wedge |
| `test_probe_b_watcher_without_send` | watcher attached, **no event emitted** | the subscription alone is harmless → an event must actually flow |
| `test_probe_c_no_self_subscribe` | watcher attached, sender **not** subscribed → only `messagePropagated` reaches the watcher (no loopback) | `messageReceived(vector<uint8_t>)` is implicated |

Each probe runs on a **fresh** two-node pair, so a wedge can't bleed into the next.

### Reading the matrix

```
A wedges               → event emission ALONE wedges (watcher irrelevant)
A ok, B wedges         → watcher subscription ALONE wedges (no event needed)
A ok, B ok, C wedges   → messagePropagated transmission to a watcher wedges
A ok, B ok, C ok       → the loopback messageReceived (vector<uint8_t>) is the wedge
```

## Running

Same prerequisites as the parent suite (see `../README.md`) — **needs Linux or a
Linux remote builder**; it will not run on macOS arm64.

```bash
# from tests/e2e, with LOGOS_MODULES_DIR + LOGOSCORE_IMAGE set and the CLI on PATH
E2E_PROBES=1 LOGOSCORE_PY_FORWARD_OUTPUT=1 pytest -v probes/
```

Add `E2E_QT_DEBUG=1` to inject `QT_LOGGING_RULES=qt.remoteobjects.*=true` into the
daemon containers, so the transport break is visible in the captured `docker logs`
(`$E2E_LOG_DIR`, default `/tmp`) rather than inferred from behaviour. Override the
rules with `E2E_QT_LOGGING_RULES=...`.

The probes are excluded from the default `pytest` run; `E2E_PROBES=1` opts them in.
