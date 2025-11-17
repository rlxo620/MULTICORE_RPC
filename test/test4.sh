#!/bin/bash
# recovery_test.sh
LOG_DIR="./logs/recovery_test"
mkdir -p $LOG_DIR

# --- 이전 로그 파일 정리 (필수) ---
rm -f txn.log txn_*.log 

echo "Starting participants..."
./participant --id 1 --prog 0x20000001 > $LOG_DIR/p1.log 2>&1 &
P1=$!
./participant --id 2 --prog 0x20000002 > $LOG_DIR/p2.log 2>&1 &
P2=$!
./participant --id 3 --prog 0x20000003 > $LOG_DIR/p3.log 2>&1 &
P3=$!

sleep 1 # wait for participants to register

echo "Starting coordinator (Crash After Commit)..."
./coordinator --id 0 --prog 0x20000000 --fail-after-commit > $LOG_DIR/coordinator.log 2>&1 &
COORD=$!

sleep 10

kill -9 $COORD 2>/dev/null

echo "Restarting crashed coordinator (Recovery)..."
./coordinator --id 0 --prog 0x20000000 >> $LOG_DIR/coordinator.log 2>&1 &

wait $P1 $P2 $P3

echo "Recovery test finished. Check logs in $LOG_DIR"
