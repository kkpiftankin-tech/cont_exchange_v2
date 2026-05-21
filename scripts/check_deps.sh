#!/usr/bin/env bash
set -euo pipefail

missing=0

check_cmd() {
  local cmd="$1"
  if command -v "${cmd}" >/dev/null 2>&1; then
    echo "[ok] command '${cmd}' is available"
  else
    echo "[fail] command '${cmd}' is not installed"
    missing=1
  fi
}

check_pkg() {
  local pkg="$1"
  if pkg-config --exists "${pkg}"; then
    echo "[ok] pkg-config package '${pkg}' is available"
  else
    echo "[fail] pkg-config package '${pkg}' is not installed"
    missing=1
  fi
}

echo "Checking required CLI tools..."
check_cmd cmake
check_cmd pkg-config
check_cmd protoc

echo "Checking required pkg-config packages..."
check_pkg rdkafka++
check_pkg libcurl
check_pkg protobuf
check_pkg grpc++

if [[ "${missing}" -ne 0 ]]; then
  echo
  echo "Dependency check failed."
  echo "Please install: protobuf-compiler libprotobuf-dev libgrpc++-dev librdkafka-dev libcurl4-openssl-dev"
  exit 1
fi

echo "All dependency checks passed."
