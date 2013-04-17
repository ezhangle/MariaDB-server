/* -*- mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
#ident "Copyright (c) 2007-2010 Tokutek Inc.  All rights reserved."
#ident "The technology is licensed by the Massachusetts Institute of Technology, Rutgers State University of New Jersey, and the Research Foundation of State University of New York at Stony Brook under United States of America Serial No. 11/760379 and to the patents and/or patent applications resulting from it."

#include "includes.h"

static void
poll_txn_progress_function(TOKUTXN txn, uint8_t is_commit, uint8_t stall_for_checkpoint) {
    if (txn->progress_poll_fun) {
        TOKU_TXN_PROGRESS_S progress = {
            .entries_total     = txn->num_rollentries,
            .entries_processed = txn->num_rollentries_processed,
            .is_commit = is_commit,
            .stalled_on_checkpoint = stall_for_checkpoint};
        txn->progress_poll_fun(&progress, txn->progress_poll_fun_extra);
    }
}

int toku_commit_rollback_item (TOKUTXN txn, struct roll_entry *item, YIELDF yield, void*yieldv, LSN lsn) {
    int r=0;
    rolltype_dispatch_assign(item, toku_commit_, r, txn, yield, yieldv, lsn);
    txn->num_rollentries_processed++;
    if (txn->num_rollentries_processed % 1024 == 0) {
        poll_txn_progress_function(txn, TRUE, FALSE);
    }
    return r;
}

int toku_abort_rollback_item (TOKUTXN txn, struct roll_entry *item, YIELDF yield, void*yieldv, LSN lsn) {
    int r=0;
    rolltype_dispatch_assign(item, toku_rollback_, r, txn, yield, yieldv, lsn);
    txn->num_rollentries_processed++;
    if (txn->num_rollentries_processed % 1024 == 0) {
        poll_txn_progress_function(txn, FALSE, FALSE);
    }
    return r;
}

static inline int
txn_has_current_rollback_log(TOKUTXN txn) {
    return txn->current_rollback.b != ROLLBACK_NONE.b;
}

static inline int
txn_has_spilled_rollback_logs(TOKUTXN txn) {
    return txn->spilled_rollback_tail.b != ROLLBACK_NONE.b;
}

static void rollback_unpin_remove_callback(CACHEKEY* cachekey, BOOL for_checkpoint, void* extra) {
    FT h = extra;
    toku_free_blocknum(
        h->blocktable, 
        cachekey,
        h,
        for_checkpoint
        );
}


void toku_rollback_log_unpin_and_remove(TOKUTXN txn, ROLLBACK_LOG_NODE log) {
    int r;
    CACHEFILE cf = txn->logger->rollback_cachefile;
    FT h = toku_cachefile_get_userdata(cf);
    r = toku_cachetable_unpin_and_remove (cf, log->blocknum, rollback_unpin_remove_callback, h);
    assert(r == 0);
}

static int
apply_txn (TOKUTXN txn, YIELDF yield, void*yieldv, LSN lsn,
                apply_rollback_item func) {
    int r = 0;
    // do the commit/abort calls and free everything
    // we do the commit/abort calls in reverse order too.
    struct roll_entry *item;
    //printf("%s:%d abort\n", __FILE__, __LINE__);
    int count=0;

    BLOCKNUM next_log      = ROLLBACK_NONE;
    uint32_t next_log_hash = 0;

    BOOL is_current = FALSE;
    if (txn_has_current_rollback_log(txn)) {
        next_log      = txn->current_rollback;
        next_log_hash = txn->current_rollback_hash;
        is_current = TRUE;
    }
    else if (txn_has_spilled_rollback_logs(txn)) {
        next_log      = txn->spilled_rollback_tail;
        next_log_hash = txn->spilled_rollback_tail_hash;
    }

    uint64_t last_sequence = txn->num_rollback_nodes;
    BOOL found_head = FALSE;
    while (next_log.b != ROLLBACK_NONE.b) {
        ROLLBACK_LOG_NODE log;
        //pin log
        toku_get_and_pin_rollback_log(txn, next_log, next_log_hash, &log);
        toku_rollback_verify_contents(log, txn->txnid64, last_sequence - 1);

        toku_maybe_prefetch_previous_rollback_log(txn, log);
        
        last_sequence = log->sequence;
        if (func) {
            while ((item=log->newest_logentry)) {
                log->newest_logentry = item->prev;
                r = func(txn, item, yield, yieldv, lsn);
                if (r!=0) return r;
                count++;
                // We occassionally yield here to prevent transactions
                // from hogging the log.  This yield will allow other
                // threads to grab the ydb lock.  However, we don't
                // want any transaction doing more than one log
                // operation to always yield the ydb lock, as it must
                // wait for the ydb lock to be released to proceed.
                if (count % 8 == 0) {
                    yield(NULL, NULL, yieldv);
                }
            }
        }
        if (next_log.b == txn->spilled_rollback_head.b) {
            assert(!found_head);
            found_head = TRUE;
            assert(log->sequence == 0);
        }
        next_log      = log->previous;
        next_log_hash = log->previous_hash;
        {
            //Clean up transaction structure to prevent
            //toku_txn_close from double-freeing
            if (is_current) {
                txn->current_rollback      = ROLLBACK_NONE;
                txn->current_rollback_hash = 0;
                is_current = FALSE;
            }
            else {
                txn->spilled_rollback_tail      = next_log;
                txn->spilled_rollback_tail_hash = next_log_hash;
            }
            if (found_head) {
                assert(next_log.b == ROLLBACK_NONE.b);
                txn->spilled_rollback_head      = next_log;
                txn->spilled_rollback_head_hash = next_log_hash;
            }
        }
        toku_rollback_log_unpin_and_remove(txn, log);
    }
    return r;
}

int
toku_find_xid_by_xid (OMTVALUE v, void *xidv) {
    TXNID xid = (TXNID) v;
    TXNID xidfind = (TXNID) xidv;
    if (xid<xidfind) return -1;
    if (xid>xidfind) return +1;
    return 0;
}

int
toku_find_pair_by_xid (OMTVALUE v, void *xidv) {
    XID_PAIR pair = v;
    TXNID xidfind = (TXNID)xidv;
    if (pair->xid1<xidfind) return -1;
    if (pair->xid1>xidfind) return +1;
    return 0;
}

void *toku_malloc_in_rollback(ROLLBACK_LOG_NODE log, size_t size) {
    return malloc_in_memarena(log->rollentry_arena, size);
}

void *toku_memdup_in_rollback(ROLLBACK_LOG_NODE log, const void *v, size_t len) {
    void *r=toku_malloc_in_rollback(log, len);
    memcpy(r,v,len);
    return r;
}

static int note_ft_used_in_txns_parent(OMTVALUE hv, u_int32_t UU(index), void*txnv) {
    TOKUTXN child  = txnv;
    TOKUTXN parent = child->parent;
    FT h = hv;
    int r = toku_txn_note_ft(parent, h);
    if (r==0 &&
        h->txnid_that_created_or_locked_when_empty == toku_txn_get_txnid(child)) {
        //Pass magic "no rollback needed" flag to parent.
        h->txnid_that_created_or_locked_when_empty = toku_txn_get_txnid(parent);
    }
    if (r==0 &&
        h->txnid_that_suppressed_recovery_logs == toku_txn_get_txnid(child)) {
        //Pass magic "no recovery needed" flag to parent.
        h->txnid_that_suppressed_recovery_logs = toku_txn_get_txnid(parent);
    }
    return r;
}

//Commit each entry in the rollback log.
//If the transaction has a parent, it just promotes its information to its parent.
int toku_rollback_commit(TOKUTXN txn, YIELDF yield, void*yieldv, LSN lsn) {
    int r=0;
    if (txn->parent!=0) {
        // First we must put a rollinclude entry into the parent if we spilled

        if (txn_has_spilled_rollback_logs(txn)) {
            uint64_t num_nodes = txn->num_rollback_nodes;
            if (txn_has_current_rollback_log(txn)) {
                num_nodes--; //Don't count the in-progress rollback log.
            }
            r = toku_logger_save_rollback_rollinclude(txn->parent, txn->txnid64, num_nodes,
                                                      txn->spilled_rollback_head, txn->spilled_rollback_head_hash,
                                                      txn->spilled_rollback_tail, txn->spilled_rollback_tail_hash);
            if (r!=0) return r;
            //Remove ownership from child.
            txn->spilled_rollback_head      = ROLLBACK_NONE; 
            txn->spilled_rollback_head_hash = 0; 
            txn->spilled_rollback_tail      = ROLLBACK_NONE; 
            txn->spilled_rollback_tail_hash = 0; 
        }
        // if we're commiting a child rollback, put its entries into the parent
        // by pinning both child and parent and then linking the child log entry
        // list to the end of the parent log entry list.
        if (txn_has_current_rollback_log(txn)) {
            //Pin parent log
            ROLLBACK_LOG_NODE parent_log;
            toku_get_and_pin_rollback_log_for_new_entry(txn->parent, &parent_log);

            //Pin child log
            ROLLBACK_LOG_NODE child_log;
            toku_get_and_pin_rollback_log(txn, txn->current_rollback, 
                    txn->current_rollback_hash, &child_log);
            toku_rollback_verify_contents(child_log, txn->txnid64, txn->num_rollback_nodes - 1);

            // Append the list to the front of the parent.
            if (child_log->oldest_logentry) {
                // There are some entries, so link them in.
                child_log->oldest_logentry->prev = parent_log->newest_logentry;
                if (!parent_log->oldest_logentry) {
                    parent_log->oldest_logentry = child_log->oldest_logentry;
                }
                parent_log->newest_logentry = child_log->newest_logentry;
                parent_log->rollentry_resident_bytecount += child_log->rollentry_resident_bytecount;
                txn->parent->rollentry_raw_count         += txn->rollentry_raw_count;
                child_log->rollentry_resident_bytecount = 0;
            }
            if (parent_log->oldest_logentry==NULL) {
                parent_log->oldest_logentry = child_log->oldest_logentry;
            }
            child_log->newest_logentry = child_log->oldest_logentry = 0;
            // Put all the memarena data into the parent.
            if (memarena_total_size_in_use(child_log->rollentry_arena) > 0) {
                // If there are no bytes to move, then just leave things alone, and let the memory be reclaimed on txn is closed.
                memarena_move_buffers(parent_log->rollentry_arena, child_log->rollentry_arena);
            }
            toku_rollback_log_unpin_and_remove(txn, child_log);
            txn->current_rollback = ROLLBACK_NONE;
            txn->current_rollback_hash = 0;

            toku_maybe_spill_rollbacks(txn->parent, parent_log);
            toku_rollback_log_unpin(txn->parent, parent_log);
            assert(r == 0);
        }

        // Note the open brts, the omts must be merged
        r = toku_omt_iterate(txn->open_fts, note_ft_used_in_txns_parent, txn);
        assert(r==0);

        // Merge the list of headers that must be checkpointed before commit
        if (txn->checkpoint_needed_before_commit) {
            txn->parent->checkpoint_needed_before_commit = TRUE;
        }

        //If this transaction needs an fsync (if it commits)
        //save that in the parent.  Since the commit really happens in the root txn.
        txn->parent->force_fsync_on_commit |= txn->force_fsync_on_commit;
        txn->parent->num_rollentries       += txn->num_rollentries;
    } else {
        r = apply_txn(txn, yield, yieldv, lsn, toku_commit_rollback_item);
        assert(r==0);
    }

    return r;
}

int toku_rollback_abort(TOKUTXN txn, YIELDF yield, void*yieldv, LSN lsn) {
    int r;
    r = apply_txn(txn, yield, yieldv, lsn, toku_abort_rollback_item);
    assert(r==0);
    return r;
}
                         
static inline PAIR_ATTR make_rollback_pair_attr(long size) { 
    PAIR_ATTR result={
     .size = size, 
     .nonleaf_size = 0, 
     .leaf_size = 0, 
     .rollback_size = size, 
     .cache_pressure_size = 0,
     .is_valid = TRUE
    }; 
    return result; 
}


// Write something out.  Keep trying even if partial writes occur.
// On error: Return negative with errno set.
// On success return nbytes.

static PAIR_ATTR
rollback_memory_size(ROLLBACK_LOG_NODE log) {
    size_t size = sizeof(*log);
    size += memarena_total_memory_size(log->rollentry_arena);
    return make_rollback_pair_attr(size);
}

// Cleanup the rollback memory
static void
rollback_log_destroy(ROLLBACK_LOG_NODE log) {
    memarena_close(&log->rollentry_arena);
    toku_free(log);
}

static void rollback_flush_callback (CACHEFILE cachefile, int fd, BLOCKNUM logname,
                                          void *rollback_v,  void** UU(disk_data), void *extraargs, PAIR_ATTR size, PAIR_ATTR* new_size,
                                          BOOL write_me, BOOL keep_me, BOOL for_checkpoint, BOOL UU(is_clone)) {
    int r;
    ROLLBACK_LOG_NODE  log = rollback_v;
    FT h   = extraargs;

    assert(h->cf == cachefile);
    assert(log->blocknum.b==logname.b);

    if (write_me && !h->panic) {
        int n_workitems, n_threads; 
        toku_cachefile_get_workqueue_load(cachefile, &n_workitems, &n_threads);

        r = toku_serialize_rollback_log_to(fd, log->blocknum, log, h, n_workitems, n_threads, for_checkpoint);
        if (r) {
            if (h->panic==0) {
                char *e = strerror(r);
                int   l = 200 + strlen(e);
                char s[l];
                h->panic=r;
                snprintf(s, l-1, "While writing data to disk, error %d (%s)", r, e);
                h->panic_string = toku_strdup(s);
            }
        }
    }
    *new_size = size;
    if (!keep_me) {
        rollback_log_destroy(log);
    }
}

static int rollback_fetch_callback (CACHEFILE cachefile, int fd, BLOCKNUM logname, u_int32_t fullhash,
                                 void **rollback_pv,  void** UU(disk_data), PAIR_ATTR *sizep, int * UU(dirtyp), void *extraargs) {
    int r;
    FT h = extraargs;
    assert(h->cf == cachefile);

    ROLLBACK_LOG_NODE *result = (ROLLBACK_LOG_NODE*)rollback_pv;
    r = toku_deserialize_rollback_log_from(fd, logname, fullhash, result, h);
    if (r==0) {
        *sizep = rollback_memory_size(*result);
    }
    return r;
}

static void rollback_pe_est_callback(
    void* rollback_v, 
    void* UU(disk_data),
    long* bytes_freed_estimate, 
    enum partial_eviction_cost *cost, 
    void* UU(write_extraargs)
    )
{
    assert(rollback_v != NULL);
    *bytes_freed_estimate = 0;
    *cost = PE_CHEAP;
}

// callback for partially evicting a cachetable entry
static int rollback_pe_callback (
    void *rollback_v, 
    PAIR_ATTR UU(old_attr), 
    PAIR_ATTR* new_attr, 
    void* UU(extraargs)
    ) 
{
    assert(rollback_v != NULL);
    *new_attr = old_attr;
    return 0;
}

// partial fetch is never required for a rollback log node
static BOOL rollback_pf_req_callback(void* UU(ftnode_pv), void* UU(read_extraargs)) {
    return FALSE;
}

// a rollback node should never be partial fetched, 
// because we always say it is not required.
// (pf req callback always returns false)
static int rollback_pf_callback(void* UU(ftnode_pv),  void* UU(disk_data), void* UU(read_extraargs), int UU(fd), PAIR_ATTR* UU(sizep)) {
    assert(FALSE);
    return 0;
}

// the cleaner thread should never choose a rollback node for cleaning
static int rollback_cleaner_callback (
    void* UU(ftnode_pv),
    BLOCKNUM UU(blocknum),
    u_int32_t UU(fullhash),
    void* UU(extraargs)
    )
{
    assert(FALSE);
    return 0;
}

static inline CACHETABLE_WRITE_CALLBACK get_write_callbacks_for_rollback_log(FT h) {
    CACHETABLE_WRITE_CALLBACK wc;
    wc.flush_callback = rollback_flush_callback;
    wc.pe_est_callback = rollback_pe_est_callback;
    wc.pe_callback = rollback_pe_callback;
    wc.cleaner_callback = rollback_cleaner_callback;
    wc.clone_callback = NULL;
    wc.write_extraargs = h;
    return wc;
}

// create and pin a new rollback log node. chain it to the other rollback nodes
// by providing a previous blocknum/ hash and assigning the new rollback log
// node the next sequence number
static void rollback_log_create (TOKUTXN txn, BLOCKNUM previous, uint32_t previous_hash, ROLLBACK_LOG_NODE *result) {
    ROLLBACK_LOG_NODE MALLOC(log);
    assert(log);

    int r;
    CACHEFILE cf = txn->logger->rollback_cachefile;
    FT h = toku_cachefile_get_userdata(cf);

    log->layout_version                = FT_LAYOUT_VERSION;
    log->layout_version_original       = FT_LAYOUT_VERSION;
    log->layout_version_read_from_disk = FT_LAYOUT_VERSION;
    log->dirty = TRUE;
    log->txnid = txn->txnid64;
    log->sequence = txn->num_rollback_nodes++;
    toku_allocate_blocknum(h->blocktable, &log->blocknum, h);
    log->hash   = toku_cachetable_hash(cf, log->blocknum);
    log->previous      = previous;
    log->previous_hash = previous_hash;
    log->oldest_logentry = NULL;
    log->newest_logentry = NULL;
    log->rollentry_arena = memarena_create();
    log->rollentry_resident_bytecount = 0;

    *result = log;
    r = toku_cachetable_put(cf, log->blocknum, log->hash,
                          log, rollback_memory_size(log),
                          get_write_callbacks_for_rollback_log(h));
    assert(r == 0);
    txn->current_rollback      = log->blocknum;
    txn->current_rollback_hash = log->hash;
}

void toku_rollback_log_unpin(TOKUTXN txn, ROLLBACK_LOG_NODE log) {
    int r;
    CACHEFILE cf = txn->logger->rollback_cachefile;
    r = toku_cachetable_unpin(cf, log->blocknum, log->hash,
                              (enum cachetable_dirty)log->dirty, rollback_memory_size(log));
    assert(r == 0);
}

//Requires: log is pinned
//          log is current
//After:
//  Maybe there is no current after (if it spilled)
void toku_maybe_spill_rollbacks(TOKUTXN txn, ROLLBACK_LOG_NODE log) {
    if (log->rollentry_resident_bytecount > txn->logger->write_block_size) {
        assert(log->blocknum.b == txn->current_rollback.b);
        //spill
        if (!txn_has_spilled_rollback_logs(txn)) {
            //First spilled.  Copy to head.
            txn->spilled_rollback_head      = txn->current_rollback;
            txn->spilled_rollback_head_hash = txn->current_rollback_hash;
        }
        //Unconditionally copy to tail.  Old tail does not need to be cached anymore.
        txn->spilled_rollback_tail      = txn->current_rollback;
        txn->spilled_rollback_tail_hash = txn->current_rollback_hash;

        txn->current_rollback      = ROLLBACK_NONE;
        txn->current_rollback_hash = 0;
    }
}

static int find_filenum (OMTVALUE v, void *hv) {
    FT h = v;
    FT hfind = hv;
    FILENUM fnum     = toku_cachefile_filenum(h->cf);
    FILENUM fnumfind = toku_cachefile_filenum(hfind->cf);
    if (fnum.fileid<fnumfind.fileid) return -1;
    if (fnum.fileid>fnumfind.fileid) return +1;
    return 0;
}

//Notify a transaction that it has touched a brt.
int toku_txn_note_ft (TOKUTXN txn, FT h) {
    BOOL ref_added = toku_ft_maybe_add_txn_ref(h, txn);
    // Insert reference to brt into transaction
    if (ref_added) {
        int r = toku_omt_insert(txn->open_fts, h, find_filenum, h, 0);
        assert(r==0);
    }
    return 0;
}

// Return the number of bytes that went into the rollback data structure (the uncompressed count if there is compression)
int toku_logger_txn_rollback_raw_count(TOKUTXN txn, u_int64_t *raw_count)
{
    *raw_count = txn->rollentry_raw_count;
    return 0;
}

void toku_maybe_prefetch_previous_rollback_log(TOKUTXN txn, ROLLBACK_LOG_NODE log) {
    //Currently processing 'log'.  Prefetch the next (previous) log node.

    BLOCKNUM name = log->previous;
    int r = 0;
    if (name.b != ROLLBACK_NONE.b) {
        uint32_t hash = log->previous_hash;
        CACHEFILE cf = txn->logger->rollback_cachefile;
        FT h = toku_cachefile_get_userdata(cf);
        BOOL doing_prefetch = FALSE;
        r = toku_cachefile_prefetch(cf, name, hash,
                                    get_write_callbacks_for_rollback_log(h),
                                    rollback_fetch_callback,
                                    rollback_pf_req_callback,
                                    rollback_pf_callback,
                                    h,
                                    &doing_prefetch);
        assert(r == 0);
    }
}

void toku_rollback_verify_contents(ROLLBACK_LOG_NODE log, 
        TXNID txnid, uint64_t sequence)
{
    assert(log->txnid == txnid);
    assert(log->sequence == sequence);
}

void toku_get_and_pin_rollback_log(TOKUTXN txn, BLOCKNUM blocknum, uint32_t hash, ROLLBACK_LOG_NODE *log) {
    void * value;
    CACHEFILE cf = txn->logger->rollback_cachefile;
    FT h = toku_cachefile_get_userdata(cf);
    int r = toku_cachetable_get_and_pin(cf, blocknum, hash,
                                        &value, NULL,
                                        get_write_callbacks_for_rollback_log(h),
                                        rollback_fetch_callback,
                                        rollback_pf_req_callback,
                                        rollback_pf_callback,
                                        TRUE, // may_modify_value
                                        h
                                        );
    assert(r == 0);
    ROLLBACK_LOG_NODE pinned_log = value;
    assert(pinned_log->blocknum.b == blocknum.b);
    *log = pinned_log;
}

void toku_get_and_pin_rollback_log_for_new_entry (TOKUTXN txn, ROLLBACK_LOG_NODE *log) {
    ROLLBACK_LOG_NODE pinned_log;
    invariant(txn->state == TOKUTXN_LIVE); // #3258
    if (txn_has_current_rollback_log(txn)) {
        toku_get_and_pin_rollback_log(txn, txn->current_rollback, txn->current_rollback_hash, &pinned_log);
        toku_rollback_verify_contents(pinned_log, txn->txnid64, txn->num_rollback_nodes - 1);
    } else {
        // create a new log for this transaction to use. 
        // this call asserts success internally
        rollback_log_create(txn, txn->spilled_rollback_tail, txn->spilled_rollback_tail_hash, &pinned_log);
    }
    assert(pinned_log->txnid == txn->txnid64);
    assert(pinned_log->blocknum.b != ROLLBACK_NONE.b);        
    *log = pinned_log;
}

