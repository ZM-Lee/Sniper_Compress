#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "${ROOT}/config/sender.env"

usage() {
  cat <<EOF
Usage: ./scripts/run_camera_sender.sh [options]

Options:
  --preset qvrf192x2x24|qvrf192x1x24_lowlat|qvrf448x6x8
  --frames N                 0 means forever
  --serial-port PORT
  --mqtt-host HOST           Send via MQTT instead of serial
  --mqtt-port PORT
  --mqtt-topic TOPIC
  --mqtt-client-id ID
  --mqtt-qos N
  --tx-device DEV            OpenVINO device, e.g. GPU.0 or CPU
  --tx-ga-backend openvino|tensorrt
  --tx-trt-engine PATH
  --camera-config PATH
  --camera-index N
  --camera-roi-mode fixed|max-square
  --roi-size N               Fixed camera hardware ROI size
  --compress-roi-mode MODE   resample|can
  --compress-roi-size N      Override mode-selected compression ROI size
  --can-interface IFACE      SocketCAN interface; empty disables CAN
  --can-cmd-id ID            Direction command CAN frame id
  --camera-fps FPS
  --exposure-us N
  --prebuffer-chunks N
  --tail-flush-chunks N
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --preset) PRESET="$2"; shift 2 ;;
    --frames) FRAMES="$2"; shift 2 ;;
    --serial-port|--port) SERIAL_PORT="$2"; shift 2 ;;
    --baudrate) BAUDRATE="$2"; shift 2 ;;
    --mqtt-host) MQTT_HOST="$2"; shift 2 ;;
    --mqtt-port) MQTT_PORT="$2"; shift 2 ;;
    --mqtt-topic) MQTT_TOPIC="$2"; shift 2 ;;
    --mqtt-client-id) MQTT_CLIENT_ID="$2"; shift 2 ;;
    --mqtt-qos) MQTT_QOS="$2"; shift 2 ;;
    --tx-device) TX_DEVICE="$2"; shift 2 ;;
    --tx-ga-backend) TX_GA_BACKEND="$2"; shift 2 ;;
    --tx-trt-engine) TX_TRT_ENGINE="$2"; shift 2 ;;
    --tx-trt-device) TX_TRT_DEVICE="$2"; shift 2 ;;
    --camera-config) CAMERA_CONFIG="$2"; shift 2 ;;
    --camera-index) CAMERA_INDEX="$2"; shift 2 ;;
    --camera-roi-mode|--roi-mode) CAMERA_ROI_MODE="$2"; shift 2 ;;
    --roi-size) ROI_SIZE="$2"; CAMERA_ROI_MODE=fixed; shift 2 ;;
    --compress-roi-mode) COMPRESS_ROI_MODE="$2"; shift 2 ;;
    --compress-roi-size) COMPRESS_ROI_SIZE="$2"; shift 2 ;;
    --can-interface) CAN_INTERFACE="$2"; shift 2 ;;
    --can-cmd-id) CAN_CMD_ID="$2"; shift 2 ;;
    --no-can) CAN_INTERFACE=""; shift ;;
    --camera-fps) CAMERA_FPS="$2"; shift 2 ;;
    --exposure-us) EXPOSURE_US="$2"; shift 2 ;;
    --prebuffer-chunks) PREBUFFER_CHUNKS="$2"; shift 2 ;;
    --tail-flush-chunks) TAIL_FLUSH_CHUNKS="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

if [[ -z "${MQTT_CLIENT_ID}" || "${MQTT_CLIENT_ID}" == "auto" ]]; then
  mqtt_node="${HOSTNAME:-nuc}"
  MQTT_CLIENT_ID="rmcc-${mqtt_node%%.*}-$$"
fi

case "${PRESET}" in
  qvrf192x1x24_lowlat)
    FPS=24; CODEC_SIZE=192; DISPLAY_SIZE=384; CHUNKS_PER_FRAME=1; CHUNK_RATE_HZ=24; MAX_QUEUE_CHUNKS=16
    ;;
  qvrf192x2x24)
    FPS=24; CODEC_SIZE=192; DISPLAY_SIZE=384; CHUNKS_PER_FRAME=2; CHUNK_RATE_HZ=48; MAX_QUEUE_CHUNKS=16
    ;;
  qvrf448x6x8)
    FPS=8; CODEC_SIZE=448; DISPLAY_SIZE=896; CHUNKS_PER_FRAME=6; CHUNK_RATE_HZ=48; MAX_QUEUE_CHUNKS=16
    ;;
  *)
    echo "Unknown preset: ${PRESET}" >&2
    usage
    exit 1
    ;;
esac

case "${COMPRESS_ROI_MODE}" in
  resample)
    [[ -z "${COMPRESS_ROI_SIZE}" || "${COMPRESS_ROI_SIZE}" == "auto" ]] && COMPRESS_ROI_SIZE="${ROI_SIZE}"
    CAN_ROI_ENABLED=0
    ;;
  can)
    [[ -z "${COMPRESS_ROI_SIZE}" || "${COMPRESS_ROI_SIZE}" == "auto" ]] && COMPRESS_ROI_SIZE="${CODEC_SIZE}"
    CAN_ROI_ENABLED=1
    ;;
  *)
    echo "Unknown compression ROI mode: ${COMPRESS_ROI_MODE} (expected resample or can)" >&2
    exit 1
    ;;
esac

if [[ "${TX_TRT_ENGINE}" != /* && -n "${TX_TRT_ENGINE}" ]]; then
  TX_TRT_ENGINE="${ROOT}/${TX_TRT_ENGINE}"
fi
if [[ "${CAMERA_CONFIG}" != /* && -n "${CAMERA_CONFIG}" ]]; then
  CAMERA_CONFIG="${ROOT}/${CAMERA_CONFIG}"
fi

CAM_BIN="${ROOT}/bin/camera_capture"
TX_BIN="${ROOT}/bin/compress"
if [[ ! -x "${CAM_BIN}" || ! -x "${TX_BIN}" ]]; then
  echo "ERROR: missing binaries. Run ./install.sh or ./scripts/build_sender.sh" >&2
  exit 1
fi
if [[ "${TX_GA_BACKEND}" == "tensorrt" && ! -f "${TX_TRT_ENGINE}" ]]; then
  echo "ERROR: missing TensorRT g_a engine: ${TX_TRT_ENGINE}" >&2
  exit 1
fi

OV_LIBS=""
if [[ -d "${ROOT}/.venv/lib" ]]; then
  pyver="$(ls "${ROOT}/.venv/lib" | grep '^python3\\.' | head -1 || true)"
  [[ -n "${pyver}" ]] && OV_LIBS="${ROOT}/.venv/lib/${pyver}/site-packages/openvino/libs"
fi
export LD_LIBRARY_PATH="${OV_LIBS}:${ROOT}/bin:${LD_LIBRARY_PATH:-}"
export RM_QVRF_CPP_SENDER=1
export RM_COMPRESS_ROOT="${ROOT}"

echo "=== RM QVRF Camera Sender ==="
echo "preset=${PRESET} codec=${CODEC_SIZE} fps=${FPS} chunks/frame=${CHUNKS_PER_FRAME} chunk_rate=${CHUNK_RATE_HZ}"
if [[ -n "${MQTT_HOST}" ]]; then
  echo "mqtt=${MQTT_HOST}:${MQTT_PORT} topic=${MQTT_TOPIC} client_id=${MQTT_CLIENT_ID} qos=${MQTT_QOS} frames=${FRAMES}"
else
  echo "serial=${SERIAL_PORT}@${BAUDRATE} frames=${FRAMES}"
fi
echo "camera=daheng index:${CAMERA_INDEX} config:${CAMERA_CONFIG} roi_mode:${CAMERA_ROI_MODE} roi:${ROI_SIZE} fps:${CAMERA_FPS} exposure:${EXPOSURE_US}us"
echo "compression_mode=${COMPRESS_ROI_MODE} roi=${COMPRESS_ROI_SIZE} can=$([[ "${CAN_ROI_ENABLED}" == "1" ]] && echo "${CAN_INTERFACE:-disabled}" || echo disabled) cmd_id=${CAN_CMD_ID} step=10px"
echo "g_a=${TX_GA_BACKEND} device=${TX_DEVICE} trt_engine=${TX_TRT_ENGINE:-none}"

rm -f "/dev/shm${SHM_NAME}" 2>/dev/null || true

CAM_CMD=("${CAM_BIN}" --device-index "${CAMERA_INDEX}" --camera-config "${CAMERA_CONFIG}" --roi-mode "${CAMERA_ROI_MODE}" --fps "${CAMERA_FPS}" --exposure-us "${EXPOSURE_US}" --shm-name "${SHM_NAME}" --slots 4)
if [[ "${CAMERA_ROI_MODE}" == "fixed" ]]; then
  CAM_CMD+=(--roi-size "${ROI_SIZE}")
fi

"${CAM_CMD[@]}" &
CAM_PID=$!
cleanup() {
  set +e
  kill "${TX_PID:-}" "${CAM_PID:-}" 2>/dev/null || true
  wait "${TX_PID:-}" "${CAM_PID:-}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

for i in $(seq 1 15); do
  [[ -e "/dev/shm${SHM_NAME}" ]] && break
  if ! kill -0 "${CAM_PID}" 2>/dev/null; then
    echo "ERROR: camera exited during startup" >&2
    exit 1
  fi
  sleep 1
done
if [[ ! -e "/dev/shm${SHM_NAME}" ]]; then
  echo "ERROR: camera shared memory not found: /dev/shm${SHM_NAME}" >&2
  exit 1
fi

TX_CMD=(
  "${TX_BIN}"
  --shm-input --shm-name "${SHM_NAME}"
  --codec msssim_qvrf --qvrf-cpp-sender
  -d "${TX_DEVICE}"
  --tx-ga-backend "${TX_GA_BACKEND}"
  --fps "${FPS}"
  --codec-size "${CODEC_SIZE}"
  --roi-size "${COMPRESS_ROI_SIZE}"
  --chunks-per-frame "${CHUNKS_PER_FRAME}"
  --fec-data-chunks 0
  --prebuffer-chunks "${PREBUFFER_CHUNKS}"
  --tail-flush-chunks "${TAIL_FLUSH_CHUNKS}"
  --chunk-rate-hz "${CHUNK_RATE_HZ}"
  --max-queue-chunks "${MAX_QUEUE_CHUNKS}"
  --chunk-order "${CHUNK_ORDER}"
  --profile
)
if [[ "${CAN_ROI_ENABLED}" == "1" && -n "${CAN_INTERFACE}" ]]; then
  TX_CMD+=(--can-interface "${CAN_INTERFACE}" --can-cmd-id "${CAN_CMD_ID}")
fi
if [[ -n "${MQTT_HOST}" ]]; then
  TX_CMD+=(--mqtt-host "${MQTT_HOST}" --mqtt-port "${MQTT_PORT}" --mqtt-topic "${MQTT_TOPIC}" --mqtt-client-id "${MQTT_CLIENT_ID}" --mqtt-qos "${MQTT_QOS}")
else
  TX_CMD+=(-p "${SERIAL_PORT}" -b "${BAUDRATE}" -r 1 --serial-wait)
fi
if [[ "${FRAMES}" != "0" ]]; then
  TX_CMD+=(-n "${FRAMES}")
fi
if [[ "${TX_GA_BACKEND}" == "tensorrt" ]]; then
  TX_CMD+=(--tx-trt-engine "${TX_TRT_ENGINE}" --tx-trt-device "${TX_TRT_DEVICE}")
fi

"${TX_CMD[@]}" &
TX_PID=$!
wait "${TX_PID}"
