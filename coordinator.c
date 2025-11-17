#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <rpc/rpc.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "commit.h"

#define LOG_FILE "txn.log"
#define MAX_PARTICIPANTS 16
#define MAX_HOST_LEN 256
// RPC 타임아웃 5초
#define TIMEOUT_SEC 5 
#define MAX_TXNS 10 // 최대 복구 가능 트랜잭션 수

// --- 전역 변수 및 구조체 정의 ---
typedef struct {
    char host[MAX_HOST_LEN];
    unsigned long prog_number;
} Participant;

typedef struct {
    int id;
    unsigned long prog_number;
    char coord_host[256];
    int fail_after_prepare;
    int fail_after_commit;
} Config;

typedef struct {
    int txn_id;
    char state[32];
} TxnRecord;

Config cfg;
Participant participants[MAX_PARTICIPANTS];
int participant_count = 0;
// RPC 타임아웃 구조체 초기화.
static struct timeval TIMEOUT = {TIMEOUT_SEC, 0};
int next_txn_id = 1; // 트랜잭션 ID 관리
int initial_txn_id = 1; // main에서 복구 전 ID를 저장하기 위한 변수

// conf_file 전역 변수 선언
char conf_file[256] = "participants.conf";

// --- 함수 선언 ---
void print_usage(const char *prog);
void parse_args(int argc, char *argv[], Config *cfgp);
void load_participants(const char *filename);
void write_log(int txn_id, const char *state);
CLIENT *connect_to_participant(int i);
void notify_participants(int txn_id, int decision);
TxnRecord *read_all_txn_states(int *record_count);
PrepareResult *prepare_rpc(int txn_id, CLIENT *clnt);
int *commit_rpc(int txn_id, CLIENT *clnt);
int *abort_rpc(int txn_id, CLIENT *clnt);
int *status_rpc(int txn_id, CLIENT *clnt);


/* ---------- Failure Injection / Logging ---------- */
void maybe_fail(const char *phase) {
    if ((strcmp(phase, "after_prepare") == 0 && cfg.fail_after_prepare) ||
        (strcmp(phase, "after_commit") == 0 && cfg.fail_after_commit)) {
        fprintf(stderr, "[FAILURE] Simulating crash during %s\n", phase);
        exit(99);
    }
}
void write_log(int txn_id, const char *state) {
    FILE *f = fopen(LOG_FILE, "a+");
    if (!f) { perror("fopen"); exit(1); }
    fprintf(f, "%d %s\n", txn_id, state);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

/* ---------- Utility: Log File Reading for Recovery ---------- */
TxnRecord *read_all_txn_states(int *record_count) {
    FILE *f = fopen(LOG_FILE, "r");
    if (!f) { *record_count = 0; return NULL; }

    TxnRecord *records = calloc(MAX_TXNS, sizeof(TxnRecord));
    if (!records) { perror("calloc"); fclose(f); exit(1); }

    *record_count = 0;
    char line[256];
    int id;
    char state[32];

    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%d %31s", &id, state) == 2 && id > 0 && id <= MAX_TXNS) {
            records[id-1].txn_id = id;
            strncpy(records[id-1].state, state, 31);
            records[id-1].state[31] = '\0';
            if (id > *record_count) *record_count = id;
        }
    }
    fclose(f);
    return records;
}

/* ---------- CLI Parsing / Participant Loading ---------- */

void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "--id <n>\n"
        "--prog <hex|dec>\n"
        "--coord-host <name>\n"
        "--conf <filename>    (participant list)\n"
        "--fail-after-prepare\n"
        "--fail-after-commit\n"
        "-h,--help\n",
        prog);
}

void parse_args(int argc, char *argv[], Config *cfgp) {
    memset(cfgp, 0, sizeof(*cfgp));
    strcpy(cfgp->coord_host, "localhost");
    cfgp->prog_number = 0; 

    static struct option long_opts[] = {
        {"id", required_argument, 0, 'i'},
        {"prog", required_argument, 0, 'p'},
        {"coord-host", required_argument, 0, 'c'},
        {"conf", required_argument, 0, 'f'},
        {"fail-after-prepare", no_argument, 0, 1},
        {"fail-after-commit", no_argument, 0, 2},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };

    int opt, index = 0;
    while ((opt = getopt_long(argc, argv, "i:p:c:f:h", long_opts, &index)) != -1) {
        switch (opt) {
            case 'i': cfgp->id = atoi(optarg); break;
            case 'p': cfgp->prog_number = strtoul(optarg, NULL, 0); break;
            case 'c': strncpy(cfgp->coord_host, optarg, sizeof(cfgp->coord_host)-1); break;
            case 'f': strncpy(conf_file, optarg, sizeof(conf_file)-1); break;
            case 1: cfgp->fail_after_prepare = 1; break;
            case 2: cfgp->fail_after_commit = 1; break;
            case 'h':
            default: print_usage(argv[0]); exit(opt=='h'?0:1);
        }
    }
}

void load_participants(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) { perror("fopen participants.conf"); exit(1); }
    participant_count = 0;

    while (fscanf(f, "%s %lx", participants[participant_count].host,
                      &participants[participant_count].prog_number) == 2) {
        participant_count++;
        if (participant_count >= MAX_PARTICIPANTS) break;
    }
    fclose(f);

    if (participant_count == 0) {
        fprintf(stderr, "[ERROR] No participants found in %s\n", filename);
        exit(1);
    }

    // cfg.prog_number가 0이면 첫 번째 participant prog_number로 채움
    if (cfg.prog_number == 0) {
        cfg.prog_number = participants[0].prog_number;
    }

    printf("Loaded %d participants from %s.\n", participant_count, filename);
}

/* ---------- RPC Calls ---------- */
PrepareResult *prepare_rpc(int txn_id, CLIENT *clnt) {
    TxnID arg;
    arg.txn_id = txn_id;
    return prepare_1(arg, clnt);
}
int *commit_rpc(int txn_id, CLIENT *clnt) {
    TxnID arg;
    arg.txn_id = txn_id;
    return commit_1(arg, clnt);
}
int *abort_rpc(int txn_id, CLIENT *clnt) {
    TxnID arg;
    arg.txn_id = txn_id;
    return abort_1(arg, clnt);
}
int *status_rpc(int txn_id, CLIENT *clnt) {
    TxnID arg;
    arg.txn_id = txn_id;
    return status_1(arg, clnt);
}

/* ---------- Connection Helper ---------- */
CLIENT *connect_to_participant(int i) {
    int attempts = 0; const int max_attempts = 10; const int retry_delay_sec = 1;
    CLIENT *clnt = NULL;

    while (clnt == NULL && attempts < max_attempts) {
        if (attempts > 0) {
            fprintf(stderr, "[RETRY] P%d connection failed. Retrying in %d sec (Attempt %d/%d)...\n",
                             i+1, retry_delay_sec, attempts + 1, max_attempts);
            sleep(retry_delay_sec);
        }
        clnt = clnt_create(participants[i].host, participants[i].prog_number, COMMIT_VERS, "udp");
        if (clnt) {
             clnt_control(clnt, CLSET_TIMEOUT, (char *)&TIMEOUT);
        }
        attempts++;
    }

    if (!clnt) {
        fprintf(stderr, "[ERROR] Connect FAILED to P%d (Prog: 0x%lx) after %d attempts. RPC Error: %s\n",
                         i+1, participants[i].prog_number, max_attempts, clnt_spcreateerror("clnt_create"));
    } else {
        fprintf(stderr, "[DEBUG] Connect SUCCESS to P%d (Prog: 0x%lx) after %d attempt(s)\n",
                         i+1, participants[i].prog_number, attempts);
    }
    return clnt;
}

/* ---------- Notify Helper ---------- */
void notify_participants(int txn_id, int decision) {
    CLIENT *clnts[MAX_PARTICIPANTS] = {0};
    int i;
    for (i = 0; i < participant_count; i++) {
        clnts[i] = connect_to_participant(i);
        if (!clnts[i]) {
            fprintf(stderr, "[WARNING] Cannot notify P%d of decision. Recovery needed.\n", i+1);
            continue;
        }

        if (decision) {
            // 명세: maybe_fail("after_commit")은 COMMIT 통지 루프 안에 있어야 함.
            maybe_fail("after_commit"); // COMMIT 통지 직전 또는 직후 (여기서는 통지 직전)
            commit_rpc(txn_id, clnts[i]);
        } else {
            // ABORT는 충돌 주입 없음
            abort_rpc(txn_id, clnts[i]);
        }
        clnt_destroy(clnts[i]);
    }
}

/* ---------- Recovery Logic ---------- */
void run_recovery() {
    int max_id = 0;
    TxnRecord *records = read_all_txn_states(&max_id);
    int i;

    printf("Starting Coordinator Recovery: Scanning %d transaction logs...\n", max_id);

    if (!records) return;

    for (i = 0; i < max_id; i++) {
        int txn_id = records[i].txn_id;
        const char *state = records[i].state;

        if (txn_id == 0) continue;

        if (strstr(state, "DECISION") != NULL && strcmp(state, "COMPLETE") != 0) {
            int decision = strcmp(state, "DECISION_COMMIT") == 0;
            printf("[RECOVERY] Txn %d: Found DECISION (%s) but no COMPLETE. Resending...\n", txn_id, state);

            notify_participants(txn_id, decision);
            write_log(txn_id, "COMPLETE");
            printf("[RECOVERY] Txn %d: Recovered successfully.\n", txn_id);
        } else if (strcmp(state, "START") == 0) {
            printf("[RECOVERY] Txn %d: Found START but no DECISION. Deciding ABORT...\n", txn_id);

            int decision = 0;
            write_log(txn_id, "DECISION_ABORT");
            notify_participants(txn_id, decision);
            write_log(txn_id, "COMPLETE");
            printf("[RECOVERY] Txn %d: Aborted and recovered successfully.\n", txn_id);
        }

        if (txn_id >= next_txn_id) {
            next_txn_id = txn_id + 1;
        }
    }
    free(records);
    printf("Recovery finished. Next transaction ID: %d\n", next_txn_id);
}

/* ---------- Transaction Handling (일반 실행) ---------- */
void handle_transaction(int txn_id) {
    int decision = 1; // 1: COMMIT, 0: ABORT
    CLIENT *clnts[MAX_PARTICIPANTS] = {0};
    PrepareResult *res;
    int i;

    // 참가자 연결은 Phase 1 시작 전에 한 번만 시도 (원본 코드 유지)
    for (i = 0; i < participant_count; i++) {
        clnts[i] = connect_to_participant(i);
        // 연결 실패 시 ABORT 결정은 하지만, PREPARE를 보내기 전에 fail_fast 하지 않음
        if (!clnts[i]) {
            decision = 0;
            fprintf(stderr, "[TXN_ERROR] P%d not connected. DECISION=ABORT.\n", i+1);
        }
    }

    write_log(txn_id, "START");

    // Phase 1: Prepare
    for (i = 0; i < participant_count; i++) {
        if (!clnts[i]) {
            // 이미 연결 실패로 decision=0이 되었고, 다음 참가자로 넘어감
            continue; 
        }

        // PREPARE RPC 호출
        res = prepare_rpc(txn_id, clnts[i]);
        
        // ⚠️ 명세: send PREPARE 직후 maybe_fail("after_prepare")
        maybe_fail("after_prepare"); 

        // 명세: if no reply or VOTE_ABORT received: decision = ABORT, break
        if (!res || res->ok == 0) {
            if (!res) {
                fprintf(stderr, "[TXN_ERROR] P%d (0x%lx) failed to respond to PREPARE (Timeout/RPC error)\n",
                                 i+1, participants[i].prog_number);
            } else {
                fprintf(stderr, "[TXN_ABORT] P%d (0x%lx) voted NO (Result: %s). DECISION=ABORT.\n",
                                 i+1, participants[i].prog_number, res->info);
            }

            // ABORT 결정 및 루프 종료 (명세 준수)
            decision = 0;
            break; 
        }
    }

    // Phase 1 투표 결과에 따른 결정 로깅
    write_log(txn_id, decision ? "DECISION_COMMIT" : "DECISION_ABORT");

    // Phase 2: Commit/Abort - 결정에 따라 모든 참가자에게 통지합니다.
    // ⚠️ maybe_fail("after_commit")은 notify_participants 내에서 참가자별로 호출됨.
    notify_participants(txn_id, decision);

    write_log(txn_id, "COMPLETE");
    printf("Transaction %d completed with decision = %s\n", txn_id, decision ? "COMMIT" : "ABORT");

    for (i = 0; i < participant_count; i++)
        if (clnts[i]) clnt_destroy(clnts[i]);
}

int main(int argc, char **argv) {
    parse_args(argc, argv, &cfg);
    load_participants(conf_file);

    initial_txn_id = next_txn_id;

    run_recovery();

    if (next_txn_id > initial_txn_id) {
        printf("[INFO] Coordinator finished recovery of Txn %d. Not starting a new transaction.\n", initial_txn_id);
    } else {
        printf("Starting new transaction %d...\n", next_txn_id);
        handle_transaction(next_txn_id);
    }

    printf("Coordinator finished.\n");
    return 0;
}
