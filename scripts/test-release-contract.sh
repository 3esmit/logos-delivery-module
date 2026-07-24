#!/usr/bin/env bash

set -euo pipefail

metadata_path="metadata.json"
release_workflow=".github/workflows/release.yml"
version="$(jq -er '.version | strings | select(length > 0)' "$metadata_path")"

test "$(jq -r '.name' "$metadata_path")" = "delivery_module"
grep -Fq "std::string moduleVersion = \"${version}\";" src/delivery_module_plugin.cpp
grep -Fq "## [${version}]" CHANGELOG.md
grep -Fq 'test "$GITHUB_REF" = "refs/heads/master"' "$release_workflow"
grep -Fq 'uses: 3esmit/logos-modules-release-action/.github/workflows/release.yml@81f506530c56e8757e6d99ee7f9d4c092e74411c' "$release_workflow"
grep -Fq 'variants: linux-amd64,darwin-arm64' "$release_workflow"
grep -Fq 'require_all_variants: true' "$release_workflow"
grep -Fq 'dispatch_rebuild_index: false' "$release_workflow"
grep -Fq 'prerelease: true' "$release_workflow"
