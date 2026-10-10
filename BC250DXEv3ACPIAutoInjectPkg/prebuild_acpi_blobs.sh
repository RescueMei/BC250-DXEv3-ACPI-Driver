#!/usr/bin/env bash
#
# ACPI pre-build hook (run on the host by build.conf PREBUILD_CMD).
#
# The ACPI generator needs iasl, which is provided by the edk2 build container,
# so this hook runs the generator inside that container from the host.  It is
# invoked with the repository root as the working directory.
#
# Env: CONTAINER_ENGINE (podman), EDK2_IMAGE.
set -euo pipefail

REPO_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
CONTAINER_ENGINE="${CONTAINER_ENGINE:-podman}"
EDK2_IMAGE="${EDK2_IMAGE:-ghcr.io/tianocore/containers/fedora-41-dev:latest}"

if ! command -v "$CONTAINER_ENGINE" >/dev/null 2>&1; then
  echo "error: container engine not found: $CONTAINER_ENGINE" >&2
  exit 1
fi

"$CONTAINER_ENGINE" run --rm \
  -v "$REPO_DIR:$REPO_DIR:Z" \
  -w "$REPO_DIR" \
  "$EDK2_IMAGE" \
  python3 BC250DXEv3ACPIAutoInjectPkg/prepare_acpi_blobs.py
