#!/usr/bin/env bash
#
# Execute the delivery-module doc-tests end-to-end and regenerate their Markdown.
#
# There is one spec:
#   delivery-module-runtime.test.yaml — packages this module as an .lgx, installs
#       it with lgpm, and drives it through a headless logoscore daemon.
#
# The runner is the shared `doctest` CLI
# (https://github.com/logos-co/logos-doctest), invoked directly via its flake.
# Each spec runs into ./outputs/ via --output-dir; `doctest generate` renders the
# .md; `doctest clean` then strips build artifacts, keeping only the .md.
#
# To run against a local logos-doctest checkout instead of the published flake,
# set DOCTEST, e.g.:  DOCTEST="nix run path:../../logos-doctest --" ./run.sh
#
set -euo pipefail

# Run from this doctests/ directory regardless of where the script is invoked from.
cd "$(dirname "$0")"

# The doctest CLI. Override by exporting DOCTEST (space-separated command).
read -r -a DOCTEST <<< "${DOCTEST:-nix run github:logos-co/logos-doctest --}"
OUTPUT_DIR="./outputs"

# The runtime spec packages this maintained fork at the selected commit. Use the
# logos-co binary cache for the lighter logoscore/lgpm/lgpd builds. Skip with
# SKIP_CACHIX=1.
if [ "${SKIP_CACHIX:-0}" != "1" ]; then
  echo "==> Enabling logos-co binary cache (set SKIP_CACHIX=1 to skip)"
  if command -v cachix >/dev/null 2>&1; then
    cachix use logos-co
  else
    nix run nixpkgs#cachix -- use logos-co
  fi
fi

# Optional: verify this checkout still builds as the dev .lgx consumed by the
# runtime test (slow; pulls logos-delivery). The public `#lgx` alias is the
# portable distribution package in this maintained fork.
# The doctest itself does not require this — CI runs it on every PR.
if [ "${VERIFY_BUILD:-0}" = "1" ]; then
  REPO_ROOT="$(git -C .. rev-parse --show-toplevel)"
  echo "==> VERIFY_BUILD=1: building path:${REPO_ROOT}#lgx-dev"
  nix build "path:${REPO_ROOT}#lgx-dev" -L
fi

echo "==> Clearing previous ${OUTPUT_DIR}/"
# A prior run copies module artifacts out of the read-only nix store, so the
# directories land read-only (r-x) too. `rm -rf` can't delete files inside a
# directory it can't write to, so restore write permission first.
if [ -e "${OUTPUT_DIR}" ]; then
  chmod -R u+w "${OUTPUT_DIR}" 2>/dev/null || true
fi
rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}"

# Run each spec into ./outputs/ separately. --output-dir is single-spec, but
# passing it once per spec makes the runner write each spec's artifacts beside
# the generated .md.
for spec in *.test.yaml; do
  name="$(basename "${spec%.test.yaml}")"
  echo "==> Running ${spec} into ${OUTPUT_DIR}/"
  # Pin the package URL to this checkout's commit unless the caller supplied a
  # different ref (for example, a release tag).
  release_ref="${RELEASE_FOR:-$(git -C .. rev-parse HEAD)}"
  release_args=(--release-for "logos-delivery-module=${release_ref}")
  "${DOCTEST[@]}" run "${spec}" \
    --verbose \
    --continue-on-fail \
    "${release_args[@]}" \
    --output-dir "${OUTPUT_DIR}/"

  echo "==> Generating ${OUTPUT_DIR}/${name}.md"
  "${DOCTEST[@]}" generate "${spec}" \
    "${release_args[@]}" \
    -o "${OUTPUT_DIR}/${name}.md"
done

echo "==> Cleaning build artifacts from ${OUTPUT_DIR}/ (keeps .md)"
"${DOCTEST[@]}" clean "${OUTPUT_DIR}" --verbose

echo "==> Done. Rendered docs are in ${OUTPUT_DIR}/"
