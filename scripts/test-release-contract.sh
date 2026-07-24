#!/usr/bin/env bash

set -euo pipefail

metadata_path="metadata.json"
release_workflow=".github/workflows/release.yml"
version="$(jq -er '.version | strings | select(length > 0)' "$metadata_path")"

test "$(jq -r '.name' "$metadata_path")" = "delivery_module"
grep -Fq "std::string moduleVersion = \"${version}\";" src/delivery_module_plugin.cpp
grep -Fq "## [${version}]" CHANGELOG.md
grep -Fq 'test "$GITHUB_REF" = "refs/heads/master"' "$release_workflow"
grep -Fq 'uses: 3esmit/logos-modules-release-action/.github/workflows/release.yml@7e09ddf90bf33ce29ef3505609332175a6cd06c9' "$release_workflow"
grep -Fq 'variants: linux-amd64,darwin-arm64' "$release_workflow"
grep -Fq 'require_all_variants: true' "$release_workflow"
grep -Fq 'dispatch_rebuild_index: false' "$release_workflow"
grep -Fq 'prerelease: true' "$release_workflow"
