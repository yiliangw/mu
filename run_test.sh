#!/bin/bash
set -e

# ============================================================
# mu crash-consensus 3-node throughput test
# Runs on node1 (leader), node2, node3 (followers)
# Requires: RDMA devices on all nodes
# ============================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEMO_BIN="$SCRIPT_DIR/crash-consensus/demo/without_conan/build/src/main-st"
REGISTRY="10.0.0.1:9999"
MEMC_PORT=9999
PAYLOAD=64
OUTSTANDING=8
STARTED_MEMC=false

if [ ! -f "$DEMO_BIN" ]; then
  echo "ERROR: Demo binary not found at $DEMO_BIN"
  echo "Run the build steps in SETUP_GUIDE.md first."
  exit 1
fi

cleanup() {
  echo "Cleaning up..."
  ssh node2 "pkill -f main-st" 2>/dev/null || true
  ssh node3 "pkill -f main-st" 2>/dev/null || true
  pkill -f main-st 2>/dev/null || true
  if $STARTED_MEMC; then
    echo "Stopping memcached (port $MEMC_PORT)..."
    pkill -f "memcached -p $MEMC_PORT" 2>/dev/null || true
  fi
}
trap cleanup EXIT

# Start memcached registry if not already running
if echo "version" | nc -q 1 localhost $MEMC_PORT &>/dev/null; then
  echo "Memcached already running on port $MEMC_PORT."
else
  echo "Starting memcached on port $MEMC_PORT..."
  memcached -p $MEMC_PORT -d
  sleep 1
  STARTED_MEMC=true
fi

# Flush memcached registry
echo "Flushing memcached registry..."
echo "flush_all" | nc -q 1 localhost $MEMC_PORT >/dev/null

# Start followers
echo "Starting follower on node3 (id=3)..."
ssh node3 "cd $(dirname $DEMO_BIN) && DORY_REGISTRY_IP=$REGISTRY ./main-st 3 $PAYLOAD $OUTSTANDING" &>/tmp/mu_node3.log &
PID3=$!

echo "Starting follower on node2 (id=2)..."
ssh node2 "cd $(dirname $DEMO_BIN) && DORY_REGISTRY_IP=$REGISTRY ./main-st 2 $PAYLOAD $OUTSTANDING" &>/tmp/mu_node2.log &
PID2=$!

sleep 2

# Start leader on node1
echo "Starting leader on node1 (id=1)..."
echo "---"
cd "$(dirname $DEMO_BIN)" && DORY_REGISTRY_IP=$REGISTRY ./main-st 1 $PAYLOAD $OUTSTANDING 2>&1 | \
  grep -E "^(USING|Replicated|Started|Error|Wait)"

echo "---"
echo "Done. Logs: /tmp/mu_node1.log, /tmp/mu_node2.log, /tmp/mu_node3.log"
