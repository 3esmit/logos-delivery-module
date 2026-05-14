# Run the delivery node locally

Headless delivery node = `logoscore` daemon + `delivery_module`. No GUI, no HTTP server, all interaction via the `logoscore` CLI client.

## Prerequisites

- Nix with flakes enabled (`experimental-features = nix-command flakes`)
- macOS (aarch64/x86_64) or Linux (aarch64/x86_64)

## Build

1. Build the delivery module into a runnable layout:

   ```bash
   git clone https://github.com/logos-co/logos-delivery-module.git
   cd logos-delivery-module
   nix build '.#install'
   ```

   Produces `./result/modules/delivery_module/{delivery_module_plugin.dylib, manifest.json, ...}` — a directory tree shaped like a modules registry, which is what `logoscore -m` expects.

2. Build the `logoscore` CLI (daemon + client):

   ```bash
   git clone https://github.com/logos-co/logos-logoscore-cli.git
   cd logos-logoscore-cli
   nix build
   ```

   Produces `./result/bin/logoscore`.

## Run — daemon mode (recommended for the dashboard workflow)

1. Start the daemon, pointing it at the modules dir produced above:

   ```bash
   ./result/bin/logoscore -D -m /path/to/logos-delivery-module/result/modules
   ```

2. From another terminal, sanity check:

   ```bash
   ./result/bin/logoscore status --json
   ./result/bin/logoscore list-modules --json
   ```

On first start the daemon writes `~/.logoscore/{daemon,client}/` and auto-issues a local-only auth token, so same-host client commands work without extra setup.

## Boot the delivery node

1. Load the plugin:

   ```bash
   logoscore load-module delivery_module --json
   ```

2. Write a minimal config (`Edge` mode on the `logos.dev` preset is enough for read-only queries):

   ```json
   {"logLevel":"INFO","mode":"Edge","preset":"logos.dev"}
   ```

   Save as `node.json`.

3. Create and start the node:

   ```bash
   logoscore call delivery_module createNode @node.json --json
   logoscore call delivery_module start --json
   ```

The `@file` syntax reads the parameter from a file — required for `createNode` because the JSON config contains characters the shell would otherwise eat.

Other `createNode` keys (cluster, tcpPort, entryNodes, etc.) are documented in `src/delivery_module_plugin.h` and `README.md`. Use `mode: "Core"` for relay-participating nodes; `Edge` is sufficient when you only need observability.

## Run — inline mode (current infra deployment)

`infra-role-logos-node`'s entrypoint runs:

```bash
./logos/bin/logoscore \
    -m ./modules \
    --load-modules delivery_module \
    -c 'delivery_module.createNode(@/conf/waku_config.json)' \
    -c 'delivery_module.start()'
```

That's **inline mode** — a single process that runs the `-c` calls then stays alive. No daemon socket is opened, so `logoscore call ...` from another shell will fail. This is why the current `tasks/delivery/query.yml` resorts to grepping `docker logs`.

To unblock dashboard scraping (see [`infra-dashboard.md`](./infra-dashboard.md)), add `-D` to the entrypoint and drop `--load-modules` / `-c` (load + start happen as client calls after boot). See "Docker deployment" below.

## Docker deployment

Two paths, depending on what you're verifying.

### Production image (Linux amd64 hosts)

Source: [`status-im/infra-role-logos-node`](https://github.com/status-im/infra-role-logos-node) → image `harbor.status.im/logos-co/logos-node:deploy-logos-dev` deployed via `docker-compose.yml`. The image's stock `CMD` is already `./logos/bin/logoscore -D -m ./modules` — daemon mode is the default. The infra role currently *overrides* this with an inline-mode entrypoint; for the dashboard workflow, don't override it.

> **Apple silicon caveat (verified 2026-05-14):** this image is published only for `linux/amd64`. On Apple silicon (`linux/arm64/v8`), Docker Desktop pulls it anyway and emits
>
> > `the requested image's platform (linux/amd64) does not match the detected host platform (linux/arm64/v8) and no specific platform was requested`
>
> then runs it under Rosetta emulation. Under that emulation `load-module` crashes with `boost::asio: Bad file descriptor` (reproduces with the stock image and zero overrides). The image works fine on native amd64 Linux. For local dev on Apple silicon, use the "Build natively" path below.

To reproduce on amd64 Linux: replace the `build: .` line in [`docker-compose.yml`](./docker-compose.yml) with `image: harbor.status.im/logos-co/logos-node:deploy-logos-dev`, drop the `Dockerfile`, then follow steps 2–3 below.

### Build natively (works on macOS / Apple silicon)

[`docker-compose.yml`](./docker-compose.yml) uses [`logos-co/logos-docker`](https://github.com/logos-co/logos-docker) as a remote build context — Compose clones the repo and builds the image locally for the host architecture, no `Dockerfile` copy in this directory. The image installs `logoscore` plus `delivery_module`, `storage_module`, and `blockchain_module` via `lgpm`, and exposes `logoscore` on `PATH`.

1. Bring it up (first build runs Nix + AppImage extraction; allow ~15–30 min):

   ```bash
   docker compose up -d --build
   docker exec logos-node logoscore status --json
   ```

2. Bootstrap the delivery node (one-shot, from the host):

   ```bash
   docker exec logos-node logoscore load-module delivery_module --json
   docker exec logos-node logoscore call delivery_module createNode @/conf/logos-dev.json --json
   docker exec logos-node logoscore call delivery_module start --json
   ```

## Shut down

```bash
logoscore call delivery_module stop --json
logoscore unload-module delivery_module
logoscore stop                              # stops the daemon
```

## Troubleshooting

- **`status` says `not_running` after `-D`** — the daemon writes its log to stdout; redirect it (`logoscore -D -m ... > daemon.log 2>&1 &`) and tail the file. Look for `Module loaded:` and `Logoscore daemon started`.
- **`load-module` fails with `MODULE_NOT_FOUND`** — `-m` points at a directory that contains module subdirs, not at a single module. Check `ls $MODULES_DIR/delivery_module/manifest.json` resolves.
- **`createNode` hangs** — first call downloads/connects to bootstrap peers; allow ~30s on a cold start.
- **`logoscore call` inside the container says `not_configured` / `not_running`** — the entrypoint is inline mode, not daemon mode. The CLI client needs a daemon to dial; see "Docker deployment" above.
