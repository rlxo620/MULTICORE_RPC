struct TxnID {
        int txn_id;
};
struct PrepareResult {
        int ok; /* 1 = YES (vote-commit), 0 = NO (vote-abort) */
        string info<256>;
};
program COMMIT_PROG {
        version COMMIT_VERS {
                PrepareResult PREPARE(TxnID) = 1;
                int COMMIT(TxnID) = 2;
                int ABORT(TxnID) = 3;
                int STATUS(TxnID) = 4;
        } = 1;
} = 0x20000001;
