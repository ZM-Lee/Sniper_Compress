#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${ROOT}/build"
ENABLE_TRT="${ENABLE_TRT:-0}"
DAHENG_ROOT="${DAHENG_ROOT:-}"

args=(-S "${ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release)
if [[ "${ENABLE_TRT}" == "1" ]]; then
  args+=(-DCOMPRESS_ENABLE_TENSORRT=ON)
fi
if [[ -n "${DAHENG_ROOT}" ]]; then
  args+=(-DDAHENG_ROOT="${DAHENG_ROOT}")
fi

cmake "${args[@]}"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

mkdir -p "${ROOT}/bin"
cp -a "${BUILD_DIR}/compress" "${ROOT}/bin/"
cp -a "${BUILD_DIR}/librmcc_sniper.so" "${ROOT}/bin/"
if [[ -x "${BUILD_DIR}/camera_capture" ]]; then
  cp -a "${BUILD_DIR}/camera_capture" "${ROOT}/bin/"
else
  echo "WARN: camera_capture was not built. Set DAHENG_ROOT to Daheng Galaxy SDK root and rebuild."
fi
