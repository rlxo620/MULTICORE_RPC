#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <rpc/rpc.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "commit.h"

#define INFO_MSG_SIZE 256

static char log_file[256];

typedef struct {
    int id;
    unsigned long prog_number;
    char coord_host[256];
    int fail_on_prepare;
    int fail_after_prepare;
    int fail_on_commit;
    int fail_on_abort;
    int fail_after_commit;
} Config;

static Config cfg;

/* ---------- Logging helpers ---------- */
void write_log(int txn_id, const char *state, const char *vote) {
    FILE *f = fopen(log_file, "a+");
    if (!f) { perror("fopen"); exit(1); }

    // Log "PREPARED YES/NO" or other simple states
    if (strcmp(state, "PREPARED") == 0 && vote != NULL) {
        fprintf(f, "%d %s %s\n", txn_id, state, vote);
    }
    else {
        fprintf(f, "%d %s\n", txn_id, state);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

char *read_last_state(int txn_id) {
    FILE *f = fopen(log_file, "r");
    if (!f) return NULL;
    static char last_state[INFO_MSG_SIZE];
    last_state[0] = '\0';
    char line[INFO_MSG_SIZE];
    char state[INFO_MSG_SIZE];
    char vote[INFO_MSG_SIZE];
    int id;

    // Find the last recorded state for the transaction ID
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%d %255s %255s", &id, state, vote) >= 2 && id == txn_id) {
            strncpy(last_state, state, sizeof(last_state) - 1);
            last_state[sizeof(last_state) - 1] = '\0';
        }
    }

    fclose(f);
    return strlen(last_state) ? last_state : NULL;
}

/* ---------- Failure injection helper ---------- */
void maybe_fail(const char *phase) {
    
    bool should_fail = false;

    if ((strcmp(phase, "prepare") == 0 && cfg.fail_on_prepare) ||
        (strcmp(phase, "after_prepare") == 0 && cfg.fail_after_prepare) ||
        (strcmp(phase, "commit") == 0 && cfg.fail_on_commit) ||
        (strcmp(phase, "abort") == 0 && cfg.fail_on_abort))
    {
        should_fail = true;
    }

    if (should_fail)
    {
        fprintf(stderr, "[FAILURE] P%d Simulating crash during %s\n", cfg.id, phase);
        exit(99);
    }
}

/* ---------- RPC handlers ---------- */
PrepareResult *prepare_1_svc(TxnID arg, struct svc_req *rqstp) {
    static PrepareResult result;
    static char info_buf[INFO_MSG_SIZE];
    result.info = info_buf;
    info_buf[0] = '\0';

    // 1. maybe_fail("prepare")
    maybe_fail("prepare");

    fprintf(stderr, "[DEBUG] P%d Received PREPARE for Txn %d\n", cfg.id, arg.txn_id);

    // 2. Check for previous ABORT decision
    char *prev = read_last_state(arg.txn_id);
    if (prev && (strcmp(prev, "ABORT") == 0 || strcmp(prev, "ABORTED") == 0)) {
        fprintf(stderr, "[DEBUG] P%d Voted ABORT (NO) due to previous ABORT log.\n", cfg.id);
        result.ok = 0;
        snprintf(result.info, sizeof(info_buf), "Voted ABORT (Previous log)");
        return &result;
    }

    // 3. If can_commit(data) == TRUE: (No fail_on_prepare flag)
    if (!cfg.fail_on_prepare) {

        // Log PREPARED YES
        write_log(arg.txn_id, "PREPARED", "YES");

        // maybe_fail("after_prepare")
        maybe_fail("after_prepare");

        // return VOTE_COMMIT
        result.ok = 1;
        snprintf(result.info, sizeof(info_buf), "Prepared");
        return &result;
    }
    // 4. Else (can't commit / fail_on_prepare is set): VOTE_ABORT
    else {
        // VOTE_ABORT 시 로그 기록을 생략합니다 (최종 ABORT 통지 시에만 로깅).

        // return VOTE_ABORT
        result.ok = 0;
        snprintf(result.info, sizeof(info_buf), "Voted ABORT (Can't commit/Fail flag)");
        return &result;
    }
}

int *commit_1_svc(TxnID arg, struct svc_req *rqstp) {
    static int ack = 1;

    // maybe_fail("commit")
    maybe_fail("commit");

    // write_log("COMMIT", transaction_id)
    write_log(arg.txn_id, "COMMITTED", NULL); 
    return &ack;
}

int *abort_1_svc(TxnID arg, struct svc_req *rqstp) {
    static int ack = 1;

    // maybe_fail("abort")
    maybe_fail("abort");

    // write_log("ABORT", transaction_id) - 명세에 맞춰 "ABORT" 사용
    write_log(arg.txn_id, "ABORT", NULL); 

    return &ack;
}

int *status_1_svc(TxnID arg, struct svc_req *rqstp) {
    static int status = 0;
    char *prev = read_last_state(arg.txn_id);
    if (!prev) status = 0;
    else if (strcmp(prev, "COMMITTED") == 0) status = 1;
    else if (strcmp(prev, "PREPARED") == 0) status = 2; 
    else status = 0;
    return &status;
}

/* ---------- Command-line parsing ---------- */
void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "Options:\n"
        "  --id <n>\n"
        "  --prog <hex|dec>\n"
        "  --coord-host <name>\n"
        "  --fail-on-prepare\n"
        "  --fail-after-prepare\n"
        "  --fail-on-commit\n"
        "  --fail-on-abort\n"
        "  --fail-after-commit\n"
        "  -h, --help\n",
        prog);
}

void parse_args(int argc, char *argv[], Config *cfgp) {
    memset(cfgp, 0, sizeof(*cfgp));
    strcpy(cfgp->coord_host, "localhost");

    static struct option long_opts[] = {
        {"id", required_argument, 0, 'i'},
        {"prog", required_argument, 0, 'p'},
        {"coord-host", required_argument, 0, 'c'},
        {"fail-on-prepare", no_argument, 0, 1},
        {"fail-after-prepare", no_argument, 0, 2},
        {"fail-on-commit", no_argument, 0, 3},
        {"fail-after-commit", no_argument, 0, 4},
        {"fail-on-abort", no_argument, 0, 5},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };

    int opt, index = 0;
    while ((opt = getopt_long(argc, argv, "i:p:c:h", long_opts, &index)) != -1) {
        switch (opt) {
            case 'i':
                cfgp->id = atoi(optarg);
                break;
            case 'p':
                cfgp->prog_number = strtoul(optarg, NULL, 0);
                break;
            case 'c':
                strncpy(cfgp->coord_host, optarg, sizeof(cfgp->coord_host)-1);
                cfgp->coord_host[sizeof(cfgp->coord_host)-1] = '\0';
                break;
            case 1: cfgp->fail_on_prepare = 1; break;
            case 2: cfgp->fail_after_prepare = 1; break;
            case 3: cfgp->fail_on_commit = 1; break;
            case 4: cfgp->fail_after_commit = 1; break;
            case 5: cfgp->fail_on_abort = 1; break;
            case 'h':
            default:
                print_usage(argv[0]);
                exit(opt == 'h' ? 0 : 1);
        }
    }

    if (cfgp->prog_number == 0) {
        fprintf(stderr, "[ERROR] --prog <number> must be provided.\n");
        exit(1);
    }

    snprintf(log_file, sizeof(log_file), "txn_%d.log", cfgp->id);
}

/* ---------- RPC dispatch glue ---------- */
#ifndef SIG_PF
#define SIG_PF void(*)(int)
#endif

static PrepareResult *_prepare_1(TxnID *argp, struct svc_req *rqstp) { return prepare_1_svc(*argp, rqstp); }
static int *_commit_1(TxnID *argp, struct svc_req *rqstp) { return commit_1_svc(*argp, rqstp); }
static int *_abort_1(TxnID *argp, struct svc_req *rqstp) { return abort_1_svc(*argp, rqstp); }
static int *_status_1(TxnID *argp, struct svc_req *rqstp) { return status_1_svc(*argp, rqstp); }

static void
commit_prog_1(struct svc_req *rqstp, register SVCXPRT *transp)
{
    union {
        TxnID prepare_1_arg;
        TxnID commit_1_arg;
        TxnID abort_1_arg;
        TxnID status_1_arg;
    } argument;
    char *result;
    xdrproc_t _xdr_argument, _xdr_result;
    char *(*local)(char *, struct svc_req *);

    switch (rqstp->rq_proc) {
    case NULLPROC:
        (void) svc_sendreply (transp, (xdrproc_t) xdr_void, (char *)NULL);
        return;
    case PREPARE:
        _xdr_argument = (xdrproc_t) xdr_TxnID; _xdr_result = (xdrproc_t) xdr_PrepareResult; local = (char *(*)(char *, struct svc_req *)) _prepare_1; break;
    case COMMIT:
        _xdr_argument = (xdrproc_t) xdr_TxnID; _xdr_result = (xdrproc_t) xdr_int; local = (char *(*)(char *, struct svc_req *)) _commit_1; break;
    case ABORT:
        _xdr_argument = (xdrproc_t) xdr_TxnID; _xdr_result = (xdrproc_t) xdr_int; local = (char *(*)(char *, struct svc_req *)) _abort_1; break;
    case STATUS:
        _xdr_argument = (xdrproc_t) xdr_TxnID; _xdr_result = (xdrproc_t) xdr_int; local = (char *(*)(char *, struct svc_req *)) _status_1; break;
    default:
        svcerr_noproc (transp); return;
    }

    memset ((char *)&argument, 0, sizeof (argument));
    if (!svc_getargs (transp, (xdrproc_t) _xdr_argument, (caddr_t) &argument)) { svcerr_decode (transp); return; }
    result = (*local)((char *)&argument, rqstp);
    if (result != NULL && !svc_sendreply(transp, (xdrproc_t) _xdr_result, result)) { svcerr_systemerr (transp); }
    if (!svc_freeargs (transp, (xdrproc_t) _xdr_argument, (caddr_t) &argument)) { fprintf (stderr, "%s", "unable to free arguments"); exit (1); }
    return;
}

int main(int argc, char **argv) {
    parse_args(argc, argv, &cfg);

    register SVCXPRT *transp;
    pmap_unset(cfg.prog_number, COMMIT_VERS);

    {
        FILE *f = fopen(log_file, "a+");
        if (f) fclose(f);
        else fprintf(stderr, "[WARN] P%d could not create log file '%s'\n", cfg.id, log_file);
    }

    transp = svcudp_create(RPC_ANYSOCK);
    if (!transp) { fprintf(stderr, "cannot create udp service.\n"); exit(1); }
    if (!svc_register(transp, cfg.prog_number, COMMIT_VERS, commit_prog_1, IPPROTO_UDP)) {
        fprintf(stderr, "unable to register (0x%lx, COMMIT_VERS, udp).\n", cfg.prog_number);
        exit(1);
    }

    transp = svctcp_create(RPC_ANYSOCK, 0, 0);
    if (!transp) { fprintf(stderr, "cannot create tcp service.\n"); exit(1); }
    if (!svc_register(transp, cfg.prog_number, COMMIT_VERS, commit_prog_1, IPPROTO_TCP)) {
        fprintf(stderr, "unable to register (0x%lx, COMMIT_VERS, tcp).\n", cfg.prog_number);
        exit(1);
    }

    printf("Participant %d (Prog: 0x%lx) running. Log file: %s\n",
           cfg.id, cfg.prog_number, log_file);

    svc_run();
    fprintf(stderr, "svc_run returned unexpectedly\n");
    exit(1);
}
