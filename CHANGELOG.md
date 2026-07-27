# Changelog

## [0.1.8] - Alpha

### Added

- Expose read-only `getConnectedPeersInfo()` metadata so callers can discover
  connected peers and their advertised protocols before selecting a Store
  provider.

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
