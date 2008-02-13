/* -*- mode: C; c-basic-offset: 4 -*- */
#ident "Copyright (c) 2007 Tokutek Inc.  All rights reserved."

#include <string.h>
#include <db.h>
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>

#include "test.h"

// DIR is defined in the Makefile

int dbtcmp(DBT *dbt1, DBT *dbt2) {
    int r;
    
    r = dbt1->size - dbt2->size;  if (r) return r;
    return memcmp(dbt1->data, dbt2->data, dbt1->size);
}

DB *db;
DB_TXN* txns[(int)256];
DB_ENV* dbenv;
DBC*    cursors[(int)256];

void put(BOOL success, char txn, int _key, int _data) {
    assert(txns[(int)txn]);

    int r;
    DBT key;
    DBT data;
    
    r = db->put(db, txns[(int)txn],
                    dbt_init(&key, &_key, sizeof(int)),
                    dbt_init(&data, &_data, sizeof(int)),
                    DB_YESOVERWRITE);

    if (success)    CKERR(r);
    else            CKERR2s(r, DB_LOCK_DEADLOCK, DB_LOCK_NOTGRANTED);
}

void cget(BOOL success, BOOL find, char txn, int _key, int _data, 
          int _key_expect, int _data_expect, u_int32_t flags) {
    assert(txns[(int)txn] && cursors[(int)txn]);

    int r;
    DBT key;
    DBT data;
    
    r = cursors[(int)txn]->c_get(cursors[(int)txn],
                                 dbt_init(&key,  &_key,  sizeof(int)),
                                 dbt_init(&data, &_data, sizeof(int)),
                                 flags);
    if (success) {
        if (find) {
            CKERR(r);
            assert(*(int *)key.data  == _key_expect);
            assert(*(int *)data.data == _data_expect);
        }
        else        CKERR2(r, DB_NOTFOUND);
    }
    else            CKERR2s(r, DB_LOCK_DEADLOCK, DB_LOCK_NOTGRANTED);
}

void init_txn(char name) {
    int r;
    assert(!txns[(int)name]);
    r = dbenv->txn_begin(dbenv, NULL, &txns[(int)name], DB_TXN_NOWAIT);
        CKERR(r);
    assert(txns[(int)name]);
}

void init_dbc(char name) {
    int r;

    assert(!cursors[(int)name] && txns[(int)name]);
    r = db->cursor(db, txns[(int)name], &cursors[(int)name], 0);
        CKERR(r);
    assert(cursors[(int)name]);
}

void commit_txn(char name) {
    int r;
    assert(txns[(int)name] && !cursors[(int)name]);

    r = txns[(int)name]->commit(txns[(int)name], 0);
        CKERR(r);
    txns[(int)name] = NULL;
}

void abort_txn(char name) {
    int r;
    assert(txns[(int)name] && !cursors[(int)name]);

    r = txns[(int)name]->abort(txns[(int)name]);
        CKERR(r);
    txns[(int)name] = NULL;
}

void close_dbc(char name) {
    int r;

    assert(cursors[(int)name]);
    r = cursors[(int)name]->c_close(cursors[(int)name]);
        CKERR(r);
    cursors[(int)name] = NULL;
}

void early_commit(char name) {
    assert(cursors[(int)name] && txns[(int)name]);
    close_dbc(name);
    commit_txn(name);
}

void early_abort(char name) {
    assert(cursors[(int)name] && txns[(int)name]);
    close_dbc(name);
    abort_txn(name);
}

void setup_dbs(u_int32_t dup_flags) {
    int r;

    system("rm -rf " DIR);
    mkdir(DIR, 0777);
    dbenv   = NULL;
    db      = NULL;
    /* Open/create primary */
    r = db_env_create(&dbenv, 0);
        CKERR(r);
    u_int32_t env_txn_flags  = DB_INIT_TXN | DB_INIT_LOCK;
    u_int32_t env_open_flags = DB_CREATE | DB_PRIVATE | DB_INIT_MPOOL;
	r = dbenv->open(dbenv, DIR, env_open_flags | env_txn_flags, 0600);
        CKERR(r);
    
    r = db_create(&db, dbenv, 0);
        CKERR(r);
    if (dup_flags) {
        r = db->set_flags(db, dup_flags);
            CKERR(r);
    }
    r = db->set_bt_compare( db, int_dbt_cmp);
    CKERR(r);
    r = db->set_dup_compare(db, int_dbt_cmp);
    CKERR(r);

    char a;
    for (a = 'a'; a <= 'z'; a++) init_txn(a);
    init_txn('\0');
    r = db->open(db, txns[(int)'\0'], "foobar.db", NULL, DB_BTREE, DB_CREATE, 0600);
        CKERR(r);
    commit_txn('\0');
    for (a = 'a'; a <= 'z'; a++) init_dbc(a);
}

void close_dbs(void) {
    char a;
    for (a = 'a'; a <= 'z'; a++) {
        if (cursors[(int)a]) close_dbc(a);
        if (txns[(int)a])    commit_txn(a);
    }

    int r;
    r = db->close(db, 0);
        CKERR(r);
    db      = NULL;
    r = dbenv->close(dbenv, 0);
        CKERR(r);
    dbenv   = NULL;
}


void test_abort(u_int32_t dup_flags) {
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    put(TRUE, 'a', 1, 1);
    early_abort('a');
    cget(TRUE, FALSE, 'b', 1, 1, 0, 0, DB_SET);
    close_dbs();
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    cget(TRUE, FALSE, 'a', 1, 1, 0, 0, DB_SET);
    cget(TRUE, FALSE, 'b', 1, 1, 0, 0, DB_SET);
    put(FALSE, 'a', 1, 1);
    early_commit('b');
    put(TRUE, 'a', 1, 1);
    cget(TRUE, TRUE, 'a', 1, 1, 1, 1, DB_SET);
    cget(TRUE, FALSE, 'a', 2, 1, 1, 1, DB_SET);
    cget(FALSE, TRUE, 'c', 1, 1, 0, 0, DB_SET);
    early_abort('a');
    cget(TRUE, FALSE, 'c', 1, 1, 0, 0, DB_SET);
    close_dbs();
    /* ********************************************************************** */
}

void test_both(u_int32_t dup_flags, u_int32_t db_flags) {
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    cget(TRUE, FALSE, 'a', 1, 1, 0, 0, db_flags);
    close_dbs();
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    cget(TRUE, FALSE, 'a', 1, 1, 0, 0, db_flags);
    cget(TRUE, FALSE, 'a', 2, 1, 0, 0, db_flags);
    close_dbs();
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    cget(TRUE, FALSE, 'a', 1, 1, 0, 0, db_flags);
    cget(TRUE, FALSE, 'a', 1, 1, 0, 0, db_flags);
    close_dbs();
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    cget(TRUE, FALSE, 'a', 1, 1, 0, 0, db_flags);
    cget(TRUE, FALSE, 'b', 2, 1, 0, 0, db_flags);
    close_dbs();
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    cget(TRUE, FALSE, 'a', 1, 1, 0, 0, db_flags);
    cget(TRUE, FALSE, 'b', 1, 1, 0, 0, db_flags);
    close_dbs();
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    cget(TRUE, FALSE, 'a', 1, 1, 0, 0, db_flags);
    cget(TRUE, FALSE, 'b', 1, 1, 0, 0, db_flags);
    put(FALSE, 'a', 1, 1);
    early_commit('b');
    put(TRUE, 'a', 1, 1);
    cget(TRUE, TRUE, 'a', 1, 1, 1, 1, db_flags);
    cget(TRUE, FALSE, 'a', 2, 1, 0, 0, db_flags);
    cget(FALSE, TRUE, 'c', 1, 1, 0, 0, db_flags);
    early_commit('a');
    cget(TRUE, TRUE, 'c', 1, 1, 1, 1, db_flags);
    close_dbs();
}


void test_last(u_int32_t dup_flags) {
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    cget(TRUE, FALSE, 'a', 0, 0, 0, 0, DB_LAST);
    put(FALSE, 'b', 2, 1);
    put(TRUE, 'a', 2, 1);
    cget(TRUE, TRUE, 'a', 0, 0, 2, 1, DB_LAST);
    early_commit('a');
    put(TRUE, 'b', 2, 1);
    close_dbs();
    /* ****************************************** */
    setup_dbs(dup_flags);
    put(TRUE, 'a', 1, 1);
    cget(TRUE, TRUE, 'a', 0, 0, 1, 1, DB_LAST);
    put(FALSE, 'b', 2, 1);
    put(TRUE, 'b', -1, 1);
    cget(TRUE, TRUE, 'a', 0, 0, 1, 1, DB_LAST);
    close_dbs();
    /* ****************************************** */
    setup_dbs(dup_flags);
    put(TRUE, 'a', 1, 1);
    put(TRUE, 'a', 3, 1);
    put(TRUE, 'a', 6, 1);
    cget(TRUE, TRUE, 'a', 0, 0, 6, 1, DB_LAST);
    put(TRUE, 'b', 2, 1);
    put(TRUE, 'b', 4, 1);
    put(FALSE, 'b', 7, 1);
    put(TRUE, 'b', -1, 1);
    close_dbs();
    /* ****************************************** */
    setup_dbs(dup_flags);
    put(TRUE, 'a', 1, 1);
    cget(TRUE, TRUE, 'a', 0, 0, 1, 1, DB_LAST);
    put(dup_flags != 0, 'b', 1, 0);
    close_dbs();
}

void test_first(u_int32_t dup_flags) {
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    cget(TRUE, FALSE, 'a', 0, 0, 0, 0, DB_FIRST);
    put(FALSE, 'b', 2, 1);
    put(TRUE, 'a', 2, 1);
    cget(TRUE, TRUE, 'a', 0, 0, 2, 1, DB_FIRST);
    early_commit('a');
    put(TRUE, 'b', 2, 1);
    close_dbs();
    /* ****************************************** */
    setup_dbs(dup_flags);
    put(TRUE, 'a', 1, 1);
    cget(TRUE, TRUE, 'a', 0, 0, 1, 1, DB_FIRST);
    put(TRUE, 'b', 2, 1);
    put(FALSE, 'b', -1, 1);
    cget(TRUE, TRUE, 'a', 0, 0, 1, 1, DB_FIRST);
    close_dbs();
    /* ****************************************** */
    setup_dbs(dup_flags);
    put(TRUE, 'a', 1, 1);
    put(TRUE, 'a', 3, 1);
    put(TRUE, 'a', 6, 1);
    cget(TRUE, TRUE, 'a', 0, 0, 1, 1, DB_FIRST);
    put(TRUE, 'b', 2, 1);
    put(TRUE, 'b', 4, 1);
    put(TRUE, 'b', 7, 1);
    put(FALSE, 'b', -1, 1);
    close_dbs();
    /* ****************************************** */
    setup_dbs(dup_flags);
    put(TRUE, 'a', 1, 1);
    cget(TRUE, TRUE, 'a', 0, 0, 1, 1, DB_FIRST);
    put(dup_flags != 0, 'b', 1, 2);
    close_dbs();
}

void test_set_range(u_int32_t dup_flags) {
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    cget(TRUE, FALSE, 'a', 1, 1, 0, 0, DB_SET_RANGE);
    close_dbs();
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    cget(TRUE, FALSE, 'a', 1, 1, 0, 0, DB_SET_RANGE);
    cget(TRUE, FALSE, 'a', 2, 1, 0, 0, DB_SET_RANGE);
    close_dbs();
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    cget(TRUE, FALSE, 'a', 1, 1, 0, 0, DB_SET_RANGE);
    cget(TRUE, FALSE, 'a', 1, 1, 0, 0, DB_SET_RANGE);
    close_dbs();
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    cget(TRUE, FALSE, 'a', 1, 1, 0, 0, DB_SET_RANGE);
    cget(TRUE, FALSE, 'b', 2, 1, 0, 0, DB_SET_RANGE);
    close_dbs();
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    cget(TRUE, FALSE, 'a', 1, 1, 0, 0, DB_SET_RANGE);
    cget(TRUE, FALSE, 'b', 1, 1, 0, 0, DB_SET_RANGE);
    close_dbs();
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    cget(TRUE, FALSE, 'a', 1, 1, 0, 0, DB_SET_RANGE);
    cget(TRUE, FALSE, 'b', 5, 5, 0, 0, DB_SET_RANGE);
    put(FALSE, 'a', 7, 6);
    put(FALSE, 'a', 5, 5);
    put(TRUE,  'a', 4, 4);
    put(TRUE,  'b', -1, 4);
    put(FALSE,  'b', 2, 4);
    put(FALSE, 'a', 5, 4);
    early_commit('b');
    put(TRUE, 'a', 7, 6);
    put(TRUE, 'a', 5, 5);
    put(TRUE,  'a', 4, 4);
    put(TRUE, 'a', 5, 4);
    cget(TRUE, TRUE, 'a', 1, 1, 4, 4, DB_SET_RANGE);
    cget(TRUE, TRUE, 'a', 2, 1, 4, 4, DB_SET_RANGE);
    cget(FALSE, TRUE, 'c', 6, 6, 7, 6, DB_SET_RANGE);
    early_commit('a');
    cget(TRUE, TRUE, 'c', 6, 6, 7, 6, DB_SET_RANGE);
    close_dbs();
}

void test_both_range(u_int32_t dup_flags) {
    if (dup_flags == 0) {
      test_both(dup_flags, DB_GET_BOTH_RANGE);
      return;
    }
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    cget(TRUE, FALSE, 'a', 1, 1, 0, 0, DB_GET_BOTH_RANGE);
    close_dbs();
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    cget(TRUE, FALSE, 'a', 1, 1, 0, 0, DB_GET_BOTH_RANGE);
    cget(TRUE, FALSE, 'a', 2, 1, 0, 0, DB_GET_BOTH_RANGE);
    close_dbs();
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    cget(TRUE, FALSE, 'a', 1, 1, 0, 0, DB_GET_BOTH_RANGE);
    cget(TRUE, FALSE, 'a', 1, 1, 0, 0, DB_GET_BOTH_RANGE);
    close_dbs();
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    cget(TRUE, FALSE, 'a', 1, 1, 0, 0, DB_GET_BOTH_RANGE);
    cget(TRUE, FALSE, 'b', 2, 1, 0, 0, DB_GET_BOTH_RANGE);
    close_dbs();
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    cget(TRUE, FALSE, 'a', 1, 1, 0, 0, DB_GET_BOTH_RANGE);
    cget(TRUE, FALSE, 'b', 1, 1, 0, 0, DB_GET_BOTH_RANGE);
    close_dbs();
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    cget(TRUE, FALSE, 'a', 1, 1, 0, 0, DB_GET_BOTH_RANGE);
    cget(TRUE, FALSE, 'b', 5, 5, 0, 0, DB_GET_BOTH_RANGE);
    put(TRUE, 'a', 5, 0);
    put(FALSE, 'a', 5, 5);
    put(FALSE, 'a', 5, 6);
    put(TRUE,  'a', 6, 0);
    put(TRUE,  'b', 1, 0);
    early_commit('b');
    put(TRUE, 'a', 5, 0);
    put(TRUE, 'a', 5, 5);
    put(TRUE, 'a', 5, 6);
    put(TRUE,  'a', 6, 0);
    cget(TRUE, FALSE, 'a', 1, 1, 4, 4, DB_GET_BOTH_RANGE);
    cget(TRUE,  TRUE, 'a', 1, 0, 1, 0, DB_GET_BOTH_RANGE);
    cget(FALSE, TRUE, 'c', 5, 5, 5, 5, DB_GET_BOTH_RANGE);
    early_commit('a');
    cget(TRUE, TRUE, 'c', 5, 5, 5, 5, DB_GET_BOTH_RANGE);
    close_dbs();
}

void test_next(u_int32_t dup_flags, u_int32_t next_type) {
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    put(TRUE,  'a', 2, 1);
    put(TRUE,  'a', 5, 1);
    cget(TRUE, TRUE, 'a', 0, 0, 2, 1, next_type);
    put(FALSE, 'b', 2, 1);
    put(TRUE,  'b', 4, 1);
    put(FALSE, 'b', -1, 1);
    cget(FALSE, TRUE, 'a', 0, 0, 4, 1, next_type);
    early_commit('b');
/* We need to keep going from here
    cget(TRUE,  TRUE, 'a', 0, 0, 4, 1, next_type);
    cget(TRUE,  TRUE, 'a', 0, 0, 5, 1, next_type);
*/
    close_dbs();
    /* ****************************************** */
    setup_dbs(dup_flags);
    put(TRUE, 'a', 1, 1);
    put(TRUE, 'a', 3, 1);
    put(TRUE, 'a', 6, 1);
    cget(TRUE, TRUE, 'a', 0, 0, 1, 1, next_type);
    cget(TRUE, TRUE, 'a', 0, 0, 3, 1, next_type);
    put(FALSE, 'b', 2, 1);
    put(TRUE,  'b', 4, 1);
    put(TRUE,  'b', 7, 1);
    put(FALSE, 'b', -1, 1);
    close_dbs();
}

void test(u_int32_t dup_flags) {
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    close_dbs();
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    early_abort('a');
    close_dbs();
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    early_commit('a');
    close_dbs();
    /* ********************************************************************** */
    setup_dbs(dup_flags);
    put(TRUE, 'a', 1, 1);
    close_dbs();
    /* ********************************************************************** */
    test_both(dup_flags, DB_SET);
    test_both(dup_flags, DB_GET_BOTH);
    /* ********************************************************************** */
    test_first(dup_flags);
    /* ********************************************************************** */
    test_last(dup_flags);
    /* ********************************************************************** */
    test_set_range(dup_flags);
    /* ********************************************************************** */
    test_both_range(dup_flags);
    /* ********************************************************************** */
    test_next(dup_flags, DB_NEXT);
    test_next(dup_flags, DB_NEXT_NODUP);
}


int main(int argc, const char* argv[]) {
    parse_args(argc, argv);
#if defined(USE_BDB)
    if (verbose) {
	printf("Warning: " __FILE__" does not work in BDB.\n");
    }
    return 0;
#endif
    test(0);
    test(DB_DUP | DB_DUPSORT);
    /*
    test_abort(0);
    test_abort(DB_DUP | DB_DUPSORT);
    */
    return 0;
}
