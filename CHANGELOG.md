# Changelog

## [0.2.0] - Alpha

### Added

- Reliable Channels operations and event handling, plus current Store-query
  behavior from the Delivery module upstream.

### Changed

- Module metadata is the authoritative module-version source; the legacy
  `version()` module API is removed.

## [0.1.10] - Alpha

### Added

- Support the V1 `destroy` lifecycle action for stopped Delivery contexts,
  including correlated completion events and safe reinitialization.

### Fixed

- Supply the required callback when releasing a Delivery FFI context so module
  unload performs teardown instead of being rejected by the library.
- Bundle the Delivery revision that terminally closes REST and metrics listeners
  before releasing a destroyed FFI context.

## [0.1.9] - Alpha

### Fixed

- Preserve binary `messageReceived()` payloads across the portable cdylib event
  bridge.

## [0.1.8] - Alpha

### Added

- Expose read-only `getConnectedPeersInfo()` metadata so callers can discover
  connected peers and their advertised protocols before selecting a Store
  provider.

### Fixed

- Package the shutdown-safe Delivery library revision so stopping a managed
  Delivery node does not crash the host process.

## [0.1.7] - Alpha

### Fixed

- Keep V1 Delivery stop/restart safe by retaining persistency, stopping peer
  manager workers before transport teardown, and retaining REST routes across
  listener restart.

## [0.1.6] - Alpha

### Added

- Versioned `nodeStatus()`, `nodeAction(QString)`, and `nodeChanged(QString)`
  lifecycle contract for Delivery nodes.

### Fixed

- Return V1 initialization acknowledgements immediately while lifecycle work
  completes asynchronously and reports one terminal state event.

## [0.1.5] - Alpha

### Added

- Source-owned alpha releases for portable Linux and Apple-silicon macOS packages.

### Fixed

- Package the merged runtime-path correction under a distinct Delivery module version.
