/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "Copyright (c) 2007, 2008 Tokutek Inc.  All rights reserved."

/* rollback and rollforward routines. */

#include <stdlib.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log_header.h"
#include "log-internal.h"
#include "cachetable.h"
#include "key.h"

int toku_commit_fcreate (TXNID xid __attribute__((__unused__)),
			 BYTESTRING bs_fname __attribute__((__unused__)),
			 TOKUTXN    txn       __attribute__((__unused__))) {
    return 0;
}

int toku_rollback_fcreate (TXNID xid __attribute__((__unused__)),
			   BYTESTRING bs_fname,
			   TOKUTXN    txn       __attribute__((__unused__))) {
    char *fname = fixup_fname(&bs_fname);
    char *directory = txn->logger->directory;
    int  full_len=strlen(fname)+strlen(directory)+2;
    char full_fname[full_len];
    int l = snprintf(full_fname,full_len, "%s/%s", directory, fname);
    assert(l<=full_len);
    int r = unlink(full_fname);
    assert(r==0);
    free(fname);
    return 0;
}

int toku_commit_cmdinsert (TXNID xid, FILENUM filenum, BYTESTRING key,BYTESTRING data,TOKUTXN txn) {
    CACHEFILE cf;
    //printf("%s:%d committing insert %s %s\n", __FILE__, __LINE__, key.data, data.data);
    int r = toku_cachefile_of_filenum(txn->logger->ct, filenum, &cf);
    assert(r==0);
    DBT key_dbt,data_dbt;
    BRT_CMD_S brtcmd = { BRT_COMMIT_BOTH, xid,
			 .u.id={toku_fill_dbt(&key_dbt,  key.data,  key.len),
				toku_fill_dbt(&data_dbt, data.data, data.len)}};
    r = toku_cachefile_root_put_cmd(cf, &brtcmd, toku_txn_logger(txn));
    if (r!=0) return r;
    return toku_cachefile_close(&cf, toku_txn_logger(txn));
}

int toku_rollback_cmdinsert (TXNID xid, FILENUM filenum, BYTESTRING key,BYTESTRING data,TOKUTXN txn) {
    CACHEFILE cf;
    int r = toku_cachefile_of_filenum(txn->logger->ct, filenum, &cf);
    assert(r==0);
    //printf("%s:%d aborting insert %s %s\n", __FILE__, __LINE__, key.data, data.data);
    DBT key_dbt,data_dbt;
    BRT_CMD_S brtcmd = { BRT_ABORT_BOTH, xid,
			 .u.id={toku_fill_dbt(&key_dbt,  key.data,  key.len),
				toku_fill_dbt(&data_dbt, data.data, data.len)}};
    r = toku_cachefile_root_put_cmd(cf, &brtcmd, toku_txn_logger(txn));
    if (r!=0) return r;
    return toku_cachefile_close(&cf, toku_txn_logger(txn));
}

int toku_commit_cmddeleteboth (TXNID xid, FILENUM filenum, BYTESTRING key,BYTESTRING data,TOKUTXN txn) {
    return toku_commit_cmdinsert(xid, filenum, key, data, txn);
}

int toku_rollback_cmddeleteboth (TXNID xid, FILENUM filenum, BYTESTRING key,BYTESTRING data,TOKUTXN txn) {
    return toku_rollback_cmdinsert(xid, filenum, key, data, txn);
}

int toku_commit_cmddelete (TXNID xid, FILENUM filenum, BYTESTRING key,TOKUTXN txn) {
    CACHEFILE cf;
    int r = toku_cachefile_of_filenum(txn->logger->ct, filenum, &cf);
    assert(r==0);
    //printf("%s:%d aborting delete %s %s\n", __FILE__, __LINE__, key.data, data.data);
    DBT key_dbt,data_dbt;
    BRT_CMD_S brtcmd = { BRT_COMMIT_ANY, xid,
			 .u.id={toku_fill_dbt(&key_dbt,  key.data,  key.len),
				toku_init_dbt(&data_dbt)}};
    r = toku_cachefile_root_put_cmd(cf, &brtcmd, toku_txn_logger(txn));
    if (r!=0) return r;
    return toku_cachefile_close(&cf, toku_txn_logger(txn));
}

int toku_rollback_cmddelete (TXNID xid, FILENUM filenum, BYTESTRING key,TOKUTXN txn) {
    CACHEFILE cf;
    int r = toku_cachefile_of_filenum(txn->logger->ct, filenum, &cf);
    assert(r==0);
    //printf("%s:%d aborting delete %s %s\n", __FILE__, __LINE__, key.data, data.data);
    DBT key_dbt,data_dbt;
    BRT_CMD_S brtcmd = { BRT_ABORT_ANY, xid,
			 .u.id={toku_fill_dbt(&key_dbt,  key.data,  key.len),
				toku_init_dbt(&data_dbt)}};
    r = toku_cachefile_root_put_cmd(cf, &brtcmd, toku_txn_logger(txn));
    if (r!=0) return r;
    return toku_cachefile_close(&cf, toku_txn_logger(txn));
}

int toku_commit_fileentries (int fd, off_t filesize, TOKUTXN txn) {
    while (filesize>0) {
        int r;
        struct roll_entry *item;
        r = toku_read_rollback_backwards(fd, filesize, &item, &filesize);
        if (r!=0) { return r; }
        r = toku_commit_rollback_item(txn, item);
        if (r!=0) { return r; }
    }
    return 0;
}

int toku_rollback_fileentries (int fd, off_t filesize, TOKUTXN txn) {
    while (filesize>0) {
        int r;
        struct roll_entry *item;
        r = toku_read_rollback_backwards(fd, filesize, &item, &filesize);
        if (r!=0) { return r; }
        r = toku_abort_rollback_item(txn, item);
        if (r!=0) { return r; }
    }
    return 0;
}

int toku_commit_rollinclude (BYTESTRING bs,TOKUTXN txn) {
    int r;
    char *fname = fixup_fname(&bs);
    int fd = open(fname, O_RDONLY);
    assert(fd>=0);
    struct stat statbuf;
    r = fstat(fd, &statbuf);
    assert(r==0);
    r = toku_commit_fileentries(fd, statbuf.st_size, txn);
    assert(r==0);
    r = close(fd);
    assert(r==0);
    unlink(fname);
    free(fname);
    return 0;
}

int toku_rollback_rollinclude (BYTESTRING bs,TOKUTXN txn) {
    int r;
    char *fname = fixup_fname(&bs);
    int fd = open(fname, O_RDONLY);
    assert(fd>=0);
    struct stat statbuf;
    r = fstat(fd, &statbuf);
    assert(r==0);
    r = toku_rollback_fileentries(fd, statbuf.st_size, txn);
    assert(r==0);
    r = close(fd);
    assert(r==0);
    unlink(fname);
    free(fname);
    return 0;
}
