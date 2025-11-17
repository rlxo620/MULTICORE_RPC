# proj3
## CODE DESIGN

### coordinator.c

#### 1. maybe_fail
로그 출력하고 exit
#### 2. write_log
txn_id와 state를 LOG_FILE(txn.log)에 출력 fflush와 fsync를 이용해 바로 디스크에 써지게 함
#### 3. read_all_txn_states
LOG_FILE(txn.log)을 한줄 한줄 읽으며 txn_id와 state 반환
#### 4. parse_args
명령어를 받아 getopt_long을 통해서 각 정보를 Config 구조체에 저장
#### 5. load_participants
각 participant의 정보를 받아 저장
#### 6. *_rpc
stub wrapping 함수
#### 7. connect_to_participant
10번까지 시도하며 연결을 시도하고 연결되면 timeout 정함
#### 8. notify_participant
participant들에게 commit이나 abort를 보냄 단 commit 을 보내기 전에 fail-after-commit이면 보내지 않고 exit함
#### 9. run_recovery
read_all_txn_states를 읽음. decision 있는데 completion 없는 경우 notify_participant를 통해 participant들에게 결과를 전송하고 COMPLETE 로그 출력.
START 만 있고 DECISION COMPLETION 둘다 없으면 ABORT로 결정하고 COMPLETE로그 남기고 다른 PARTICIPANT들에게 전달 마지막으로 transaction id 증가
#### 10. hadnle_transaction
START 로그를 기록하고 PARTICIPANT_COUNT만큼 for문을 돌며 연결 시도하고 prepare_rpc 진행 만약 after_prepare 가 명령어에 있으면 이 시점에서 exit됨. prepare에서 abort 신호를 받게 된다면 DECISION_ABORT 기록 아니면 DECISION_COMPLETE 기록 후에 notify_participants 호출해 PARTICIPANT들에게 결과 전달 (after_commit이 있다면 notify_participants 함수에서 처리함) 후 COMPLETE출력하며 마무리 
#### 11. main
argument parsing 후 load participant와 run_recovery 호출 만약 recovery 할게 없다면 아무것도 안할것임. 이후 next_txn_id 와 initial_txn_id를 비교해 복구 로직인지 아닌지 구분함. (recovery logic을 들어갈 때마다 next_txn_id 가 올라가고 복구가 진행되면 recovery logic을 두번 들어갈 것 이기 때문에 next_txn_id는 2가 되어 initial_txn_id 인 1보다 커져 recovery만 진행) 복구로직이 아니라면 handle_transaction 수행 복구로직이라면 handle_transaction 미수행.

--------------------------------------

### paricipant.c

#### 1. write_log
prepared상태이고 vote 할 권리가 주어지면 fsync를 통해 기록
#### 2. read_last_stae
마지막 상태가 abort 였다면 vote_abort해야 되기 때문에 필요 
#### 3. maybe_fail
fail on prepare fail, fail after prepare, fail on commit fail on abort 등을 처리
#### 4. prepare_1_svc 
이전에 기록했던게 ABORT였으면 abort 반환. 만약 fail_on_prepare가 아니라면 VOTE_COMMIT으로 투표하고 로그에 기록. 여기서 fail_after_prepare라면 그대로 exit함. fail_on_prepare라면 VOTE_ABORT하고 로그 출력
#### 5. commit_1_svc 
commit log 기록 fail on commmit 있으면 그냥 exit
#### 6. abort_1_svc 
abort log 기록 fail on abort 있으면 그냥 exit
#### 7. status_1_svc 
status 확인하고 반환
#### 8. parse_args
participant 명령어 argument parsing후 Config구조체에 정보 기록
#### 9. commit_prog_1
commit_svc.c 에 정의되어 있긴 하나 participnat.c를 통해 main함수를 써야하고 거기서 commit_prog_1을 사용해야 하기 때문에 다시 정의
#### 10. main
먼저 명령어를 parsing하고 pmap_unset을 통해 해당 prog_numbe가 등록되어있으면 제거 진행 이후 svc_register를 통해 udp와 tcp 모두 등록하고 svc_run을 진행하며 rpc 시작

----------------------------------------

## BUILD && Execution Instruction

### 1. participant, coordinator, commi_svc.c commit_clnt.c commit.h 생성

    make

### 2. commit_svc.c의 main 제거
parsing 하는 부분이 필요하기 때문에 participant.c의 main을 써야하고 commit_svc.c 에서 main 지워야 함

### 3. participant 재생성

    make participant

### 4. test 진행
#### test1

    ./test/test1.sh

#### test2

    ./test/test2.sh    

#### test3

    ./test/test3.sh

#### test4

    ./test/test4.sh

### 5. result 확인
fauilure injection 등의 log는 logs/test를 통해 확인 가능
state의 경우 현재 디렉토리 내에 txn.log txn_1.log txn_2.log txn_3.log를 통해 확인 가능
