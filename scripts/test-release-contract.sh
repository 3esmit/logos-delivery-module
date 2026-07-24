#!/usr/bin/env bash

set -euo pipefail

metadata_path="metadata.json"
release_workflow=".github/workflows/release.yml"
version="$(jq -er '.version | strings | select(length > 0)' "$metadata_path")"

test "$(jq -r '.name' "$metadata_path")" = "delivery_module"
grep -Fq "std::string moduleVersion = \"${version}\";" src/delivery_module_plugin.cpp
grep -Fq "## [${version}]" CHANGELOG.md
grep -Fq 'test "$GITHUB_REF" = "refs/heads/master"' "$release_workflow"
grep -Fq 'uses: 3esmit/logos-modules-release-action/.github/workflows/release.yml@670e3d8b704207da88947d3417767fa34a9c5e28' "$release_workflow"
grep -Fq 'variants: linux-amd64,darwin-arm64' "$release_workflow"
grep -Fq 'require_all_variants: true' "$release_workflow"
grep -Fq 'dispatch_rebuild_index: false' "$release_workflow"
grep -Fq 'prerelease: true' "$release_workflow"
