 #!/bin/bash
 # Test Case 1: Normal commit (all vote YES)
 LOG_DIR="./logs/test1"
 mkdir -p $LOG_DIR
 echo "Starting participants..."
 ./participant --id 1 --prog 0x20000001 > $LOG_DIR/participant1.log 2>&1 &
 P1=$!
 ./participant --id 2 --prog 0x20000002 > $LOG_DIR/participant2.log 2>&1 &
 P2=$!
 ./participant --id 3 --prog 0x20000003 > $LOG_DIR/participant3.log 2>&1 &
 P3=$!
 sleep 5 # give participants time to register
 echo "Starting coordinator..."
 ./coordinator --conf participants.conf > $LOG_DIR/coordinator.log 2>&1 &
 wait $P1 $P2 $P3
 echo "Test Case 1 finished. Logs in $LOG_DIR"
