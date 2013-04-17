/* -*- mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
#ident "Copyright (c) 2007-2011 Tokutek Inc.  All rights reserved."
#ident "The technology is licensed by the Massachusetts Institute of Technology, Rutgers State University of New Jersey, and the Research Foundation of State University of New York at Stony Brook under United States of America Serial No. 11/760379 and to the patents and/or patent applications resulting from it."

#include "includes.h"

static const int log_format_version=TOKU_LOG_VERSION;

static int open_logfile (TOKULOGGER logger);
static int toku_logger_write_buffer (TOKULOGGER logger, LSN *fsynced_lsn);
static int delete_logfile(TOKULOGGER logger, long long index, uint32_t version);
static void grab_output(TOKULOGGER logger, LSN *fsynced_lsn);
static void release_output(TOKULOGGER logger, LSN fsynced_lsn);

static void toku_print_bytes (FILE *outf, u_int32_t len, char *data) {
    fprintf(outf, "\"");
    u_int32_t i;
    for (i=0; i<len; i++) {
	switch (data[i]) {
	case '"':  fprintf(outf, "\\\""); break;
	case '\\': fprintf(outf, "\\\\"); break;
	case '\n': fprintf(outf, "\\n");  break;
	default:
	    if (isprint(data[i])) fprintf(outf, "%c", data[i]);
	    else fprintf(outf, "\\%03o", (unsigned char)(data[i]));
	}
    }
    fprintf(outf, "\"");
}

static BOOL is_a_logfile_any_version (const char *name, uint64_t *number_result, uint32_t *version_of_log) {
    BOOL rval = TRUE;
    uint64_t result;
    int n;
    int r;
    uint32_t version;
    r = sscanf(name, "log%"SCNu64".tokulog%"SCNu32"%n", &result, &version, &n);
    if (r!=2 || name[n]!='\0' || version <= TOKU_LOG_VERSION_1) {
        //Version 1 does NOT append 'version' to end of '.tokulog'
        version = TOKU_LOG_VERSION_1;
        r = sscanf(name, "log%"SCNu64".tokulog%n", &result, &n);
        if (r!=1 || name[n]!='\0') {
            rval = FALSE;
        }
    }
    if (rval) {
        *number_result  = result;
        *version_of_log = version;
    }

    return rval;
}

// added for #2424, improved for #2521
static BOOL is_a_logfile (const char *name, long long *number_result) {
    BOOL rval;
    uint64_t result;
    uint32_t version;
    rval = is_a_logfile_any_version(name, &result, &version);
    if (rval && version != TOKU_LOG_VERSION)
        rval = FALSE;
    if (rval)
        *number_result = result;
    return rval;
}


int toku_logger_create (TOKULOGGER *resultp) {
    int r;
    TOKULOGGER MALLOC(result);
    if (result==0) return errno;
    result->is_open=FALSE;
    result->is_panicked=FALSE;
    result->panic_errno = 0;
    result->write_log_files = TRUE;
    result->trim_log_files = TRUE;
    result->directory=0;
    result->remove_finalize_callback = NULL;
    // fd is uninitialized on purpose
    // ct is uninitialized on purpose
    result->lg_max = 100<<20; // 100MB default
    // lsn is uninitialized
    toku_mutex_init(&result->txn_list_lock, NULL);
    r = toku_omt_create(&result->live_txns); if (r!=0) goto panic;
    r = toku_omt_create(&result->live_root_txns); if (r!=0) goto panic;
    r = toku_omt_create(&result->snapshot_txnids); if (r!=0) goto panic;
    r = toku_omt_create(&result->live_list_reverse); if (r!=0) goto panic;
    result->inbuf  = (struct logbuf) {0, LOGGER_MIN_BUF_SIZE, toku_xmalloc(LOGGER_MIN_BUF_SIZE), ZERO_LSN};
    result->outbuf = (struct logbuf) {0, LOGGER_MIN_BUF_SIZE, toku_xmalloc(LOGGER_MIN_BUF_SIZE), ZERO_LSN};
    // written_lsn is uninitialized
    // fsynced_lsn is uninitialized
    result->last_completed_checkpoint_lsn = ZERO_LSN;
    // next_log_file_number is uninitialized
    // n_in_file is uninitialized
    result->write_block_size = FT_DEFAULT_NODE_SIZE; // default logging size is the same as the default brt block size
    result->oldest_living_xid = TXNID_NONE_LIVING;
    result->oldest_living_starttime = 0;
    toku_logfilemgr_create(&result->logfilemgr);
    *resultp=result;
    ml_init(&result->input_lock);
    toku_mutex_init(&result->output_condition_lock, NULL);
    toku_cond_init(&result->output_condition,       NULL);
    result->input_lock_ctr = 0;
    result->output_condition_lock_ctr = 0;
    result->swap_ctr = 0;
    result->rollback_cachefile = NULL;
    result->output_is_available = TRUE;
    toku_list_init(&result->prepared_txns);
    toku_list_init(&result->prepared_and_returned_txns);
    return 0;

 panic:
    toku_logger_panic(result, r);
    return r;
}

static int fsync_logdir(TOKULOGGER logger) {
    return toku_fsync_dirfd_without_accounting(logger->dir);
}

static int open_logdir(TOKULOGGER logger, const char *directory) {
    if (toku_os_is_absolute_name(directory)) {
        logger->directory = toku_strdup(directory);
    } else {
        char *cwd = getcwd(NULL, 0);
        if (cwd == NULL)
            return -1;
        char *new_log_dir = toku_malloc(strlen(cwd) + strlen(directory) + 2);
        if (new_log_dir == NULL) {
            toku_free(cwd);
            return -2;
        }
        sprintf(new_log_dir, "%s/%s", cwd, directory);
        toku_free(cwd);
        logger->directory = new_log_dir;
    }
    if (logger->directory==0) return errno;

    logger->dir = opendir(logger->directory);
    if ( logger->dir == NULL ) return -1;
    return 0;
}

static int close_logdir(TOKULOGGER logger) {
    return closedir(logger->dir);
}

int toku_logger_open (const char *directory, TOKULOGGER logger) {
    if (logger->is_open) return EINVAL;
    if (logger->is_panicked) return EINVAL;

    int r;
    r = toku_logfilemgr_init(logger->logfilemgr, directory);
    if ( r!=0 ) 
        return r;
    logger->lsn = toku_logfilemgr_get_last_lsn(logger->logfilemgr);
    logger->written_lsn = logger->lsn;
    logger->fsynced_lsn = logger->lsn;
    logger->inbuf.max_lsn_in_buf  = logger->lsn;
    logger->outbuf.max_lsn_in_buf = logger->lsn;

    // open directory, save pointer for fsyncing t:2445
    r = open_logdir(logger, directory);
    if (r!=0) return r;

    long long nexti;
    r = toku_logger_find_next_unused_log_file(logger->directory, &nexti);
    if (r!=0) return r;

    logger->next_log_file_number = nexti;
    open_logfile(logger);

    logger->is_open = TRUE;
    return 0;
}

bool toku_logger_rollback_is_open (TOKULOGGER logger) {
    return logger->rollback_cachefile != NULL;
}

int
toku_logger_open_rollback(TOKULOGGER logger, CACHETABLE cachetable, BOOL create) {
    assert(logger->is_open);
    assert(!logger->is_panicked);
    assert(!logger->rollback_cachefile);
    
    int r;
    FT_HANDLE t = NULL;   // Note, there is no DB associated with this BRT.

    r = toku_ft_handle_create(&t);
    assert_zero(r);
    r = toku_ft_handle_open(t, ROLLBACK_CACHEFILE_NAME, create, create, cachetable, NULL_TXN);
    assert_zero(r);
    logger->rollback_cachefile = t->ft->cf;
    //Verify it is empty
    assert(!t->ft->panic);
    //Must have no data blocks (rollback logs or otherwise).
    toku_block_verify_no_data_blocks_except_root_unlocked(t->ft->blocktable, t->ft->h->root_blocknum);
    BOOL is_empty;
    is_empty = toku_ft_is_empty_fast(t);
    assert(is_empty);
    return r;
}


//  Requires: Rollback cachefile can only be closed immediately after a checkpoint,
//            so it will always be clean (!h->dirty) when about to be closed.
//            Rollback log can only be closed when there are no open transactions,
//            so it will always be empty (no data blocks) when about to be closed.
int
toku_logger_close_rollback(TOKULOGGER logger, BOOL recovery_failed) {
    int r = 0;
    CACHEFILE cf = logger->rollback_cachefile;  // stored in logger at rollback cachefile open
    if (!logger->is_panicked && cf) {
        FT_HANDLE ft_to_close;
        {   //Find "brt"
            FT ft = toku_cachefile_get_userdata(cf);
            if (!ft->panic && recovery_failed) {
                r = toku_ft_set_panic(ft, EINVAL, "Recovery failed");
                assert_zero(r);
            }
            //Verify it is safe to close it.
            if (!ft->panic) { //If paniced, it is safe to close.
                assert(!ft->h->dirty);  //Must not be dirty.
                //Must have no data blocks (rollback logs or otherwise).
                toku_block_verify_no_data_blocks_except_root_unlocked(ft->blocktable, ft->h->root_blocknum);
            }
            assert(!ft->h->dirty);
            ft_to_close = toku_ft_get_some_existing_ft_handle(ft);
            assert(ft_to_close);
            {
                BOOL is_empty;
                is_empty = toku_ft_is_empty_fast(ft_to_close);
                assert(is_empty);
            }
            assert(!ft->h->dirty); // it should not have been dirtied by the toku_ft_is_empty test.
        }

        r = toku_ft_handle_close(ft_to_close, FALSE, ZERO_LSN);
        //Set as dealt with already.
        logger->rollback_cachefile = NULL;
    }
    return r;
}

// No locks held on entry
// No locks held on exit.
// No locks are needed, since you cannot legally close the log concurrently with doing anything else.
int toku_logger_close(TOKULOGGER *loggerp) {
    TOKULOGGER logger = *loggerp;
    if (logger->is_panicked) return EINVAL;
    int r = 0;
    if (!logger->is_open) goto is_closed;
    ml_lock(&logger->input_lock);
    logger->input_lock_ctr++;
    LSN fsynced_lsn;
    grab_output(logger, &fsynced_lsn);
    r = toku_logger_write_buffer(logger, &fsynced_lsn);           if (r!=0) goto panic; //Releases the input lock
    if (logger->fd!=-1) {
        if ( logger->write_log_files ) {
            r = toku_file_fsync_without_accounting(logger->fd);   if (r!=0) { r=errno; goto panic; }
        }
	r = close(logger->fd);                                    if (r!=0) { r=errno; goto panic; }
    }
    r = close_logdir(logger);  if (r!=0) { r=errno; goto panic; }
    logger->fd=-1;
    release_output(logger, fsynced_lsn);

 is_closed:
    toku_free(logger->inbuf.buf);
    toku_free(logger->outbuf.buf);
    // before destroying locks they must be left in the unlocked state.
    ml_destroy(&logger->input_lock);
    toku_mutex_destroy(&logger->output_condition_lock);
    toku_cond_destroy(&logger->output_condition);
    logger->is_panicked=TRUE; // Just in case this might help.
    if (logger->directory) toku_free(logger->directory);
    toku_omt_destroy(&logger->live_txns);
    toku_omt_destroy(&logger->live_root_txns);
    toku_omt_destroy(&logger->snapshot_txnids);
    toku_omt_destroy(&logger->live_list_reverse);
    toku_logfilemgr_destroy(&logger->logfilemgr);
    toku_free(logger);
    *loggerp=0;
    return r;
 panic:
    toku_logger_panic(logger, r);
    return r;
}

int toku_logger_shutdown(TOKULOGGER logger) {
    int r = 0;
    if (logger->is_open) {
        if (toku_omt_size(logger->live_txns) == 0) {
            int r2 = toku_log_shutdown(logger, NULL, TRUE, 0);
            if (!r) r = r2;
        }
    }
    return r;
}

static int close_and_open_logfile (TOKULOGGER logger, LSN *fsynced_lsn)
// Effect: close the current file, and open the next one.
// Entry: This thread has permission to modify the output.
// Exit:  This thread has permission to modify the output.
{
    int r;
    if (logger->write_log_files) {
        r = toku_file_fsync_without_accounting(logger->fd);                 if (r!=0) return errno;
	*fsynced_lsn = logger->written_lsn;
        toku_logfilemgr_update_last_lsn(logger->logfilemgr, logger->written_lsn);          // fixes t:2294
    }
    r = close(logger->fd);                               if (r!=0) return errno;
    return open_logfile(logger);
}

static int
max_int (int a, int b)
{
    if (a>b) return a;
    return b;
}

// ***********************************************************
// output mutex/condition manipulation routines
// ***********************************************************

static void
wait_till_output_available (TOKULOGGER logger)
// Effect: Wait until output becomes available.
// Implementation hint: Use a pthread_cond_wait.
// Entry: Holds the output_condition_lock (but not the inlock)
// Exit: Holds the output_condition_lock and logger->output_is_available
// 
{
    while (!logger->output_is_available) {
        toku_cond_wait(&logger->output_condition, &logger->output_condition_lock);
    }
}

static void
grab_output(TOKULOGGER logger, LSN *fsynced_lsn)
// Effect: Wait until output becomes available and get permission to modify output.
// Entry: Holds no lock (including not holding the input lock, since we never hold both at once).
// Exit:  Hold permission to modify output (but none of the locks).
{
    toku_mutex_lock(&logger->output_condition_lock);
    logger->output_condition_lock_ctr++;
    wait_till_output_available(logger);
    logger->output_is_available = FALSE;
    if (fsynced_lsn) {
	*fsynced_lsn = logger->fsynced_lsn;
    }
    logger->output_condition_lock_ctr++;
    toku_mutex_unlock(&logger->output_condition_lock);
}

static BOOL
wait_till_output_already_written_or_output_buffer_available (TOKULOGGER logger, LSN lsn, LSN *fsynced_lsn)
// Effect: Wait until either the output is available or the lsn has been written.
//  Return true iff the lsn has been written.
//  If returning true, then on exit we don't hold output permission.
//  If returning false, then on exit we do hold output permission.
// Entry: Hold no locks.
// Exit: Hold the output permission if returns false.
{
    BOOL result;
    toku_mutex_lock(&logger->output_condition_lock);
    logger->output_condition_lock_ctr++;
    while (1) {
	if (logger->fsynced_lsn.lsn >= lsn.lsn) { // we can look at the fsynced lsn since we have the lock.
	    result = TRUE;
	    break;
	}
	if (logger->output_is_available) {
	    logger->output_is_available = FALSE;
	    result = FALSE;
	    break;
	}
	// otherwise wait for a good time to look again.
	toku_cond_wait(&logger->output_condition, &logger->output_condition_lock);
    }
    *fsynced_lsn = logger->fsynced_lsn;
    logger->output_condition_lock_ctr++;
    toku_mutex_unlock(&logger->output_condition_lock);
    return result;
}

static void
release_output (TOKULOGGER logger, LSN fsynced_lsn)
// Effect: Release output permission.
// Entry: Holds output permissions, but no locks.
// Exit: Holds neither locks nor output permission.
{
    toku_mutex_lock(&logger->output_condition_lock);
    logger->output_condition_lock_ctr++;
    logger->output_is_available = TRUE;
    if (logger->fsynced_lsn.lsn < fsynced_lsn.lsn) {
	logger->fsynced_lsn = fsynced_lsn;
    }
    toku_cond_broadcast(&logger->output_condition);
    logger->output_condition_lock_ctr++;
    toku_mutex_unlock(&logger->output_condition_lock);
}
    
static void
swap_inbuf_outbuf (TOKULOGGER logger)
// Effect: Swap the inbuf and outbuf
// Entry and exit: Hold the input lock and permission to modify output.
{
    struct logbuf tmp = logger->inbuf;
    logger->inbuf = logger->outbuf;
    logger->outbuf = tmp;
    assert(logger->inbuf.n_in_buf == 0);
    logger->swap_ctr++;
}

static void
write_outbuf_to_logfile (TOKULOGGER logger, LSN *fsynced_lsn)
// Effect:  Write the contents of outbuf to logfile.  Don't necessarily fsync (but it might, in which case fynced_lsn is updated).
//  If the logfile gets too big, open the next one (that's the case where an fsync might happen).
// Entry and exit: Holds permission to modify output (and doesn't let it go, so it's ok to also hold the inlock).
{
    if (logger->outbuf.n_in_buf>0) {
	toku_os_full_write(logger->fd, logger->outbuf.buf, logger->outbuf.n_in_buf);
	assert(logger->outbuf.max_lsn_in_buf.lsn > logger->written_lsn.lsn); // since there is something in the buffer, its LSN must be bigger than what's previously written.
	logger->written_lsn = logger->outbuf.max_lsn_in_buf;
	logger->n_in_file += logger->outbuf.n_in_buf;
	logger->outbuf.n_in_buf = 0;
    }
    // If the file got too big, then open a new file.
    if (logger->n_in_file > logger->lg_max) {
	int r = close_and_open_logfile(logger, fsynced_lsn);
	assert_zero(r);
    }
}

int
toku_logger_make_space_in_inbuf (TOKULOGGER logger, int n_bytes_needed)
// Entry: Holds the inlock
// Exit:  Holds the inlock
// Effect: Upon exit, the inlock is held and there are at least n_bytes_needed in the buffer.
//  May release the inlock (and then reacquire it), so this is not atomic.
//  May obtain the output lock and output permission (but if it does so, it will have released the inlock, since we don't hold both locks at once).
//   (But may hold output permission and inlock at the same time.)
// Implementation hint: Makes space in the inbuf, possibly by writing the inbuf to disk or increasing the size of the inbuf.  There might not be an fsync.
// Arguments:  logger:         the logger (side effects)
//             n_bytes_needed: how many bytes to make space for.
{
    if (logger->inbuf.n_in_buf + n_bytes_needed <= LOGGER_MIN_BUF_SIZE) return 0;
    logger->input_lock_ctr++;
    ml_unlock(&logger->input_lock);
    LSN fsynced_lsn;
    grab_output(logger, &fsynced_lsn);

    ml_lock(&logger->input_lock);
    logger->input_lock_ctr++;
    // Some other thread may have written the log out while we didn't have the lock.  If we have space now, then be happy.
    if (logger->inbuf.n_in_buf + n_bytes_needed <= LOGGER_MIN_BUF_SIZE) {
	release_output(logger, fsynced_lsn);
	return 0;
    }
    if (logger->inbuf.n_in_buf > 0) {
	// There isn't enough space, and there is something in the buffer, so write the inbuf.
	swap_inbuf_outbuf(logger);

	// Don't release the inlock in this case, because we don't want to get starved.
	write_outbuf_to_logfile(logger, &fsynced_lsn);
    }
    // the inbuf is empty.  Make it big enough (just in case it is somehow smaller than a single log entry).
    if (n_bytes_needed > logger->inbuf.buf_size) {
	assert(n_bytes_needed < (1<<30)); // it seems unlikely to work if a logentry gets that big.
	int new_size = max_int(logger->inbuf.buf_size * 2, n_bytes_needed); // make it at least twice as big, and big enough for n_bytes
	assert(new_size < (1<<30));
	XREALLOC_N(new_size, logger->inbuf.buf);
	logger->inbuf.buf_size = new_size;
    }
    release_output(logger, fsynced_lsn);
    return 0;
}

int toku_logger_fsync (TOKULOGGER logger)
// Effect: This is the exported fsync used by ydb.c for env_log_flush.  Group commit doesn't have to work.
// Entry: Holds no locks
// Exit: Holds no locks
// Implementation note:  Acquire the output condition lock, then the output permission, then release the output condition lock, then get the input lock.
// Then release everything.
// 
{
    int r;
    if (logger->is_panicked) return EINVAL;
    ml_lock(&logger->input_lock);
    logger->input_lock_ctr++;
    r = toku_logger_maybe_fsync(logger, logger->inbuf.max_lsn_in_buf, TRUE);
    if (r!=0) {
	toku_logger_panic(logger, r);
    }
    return r;
}

int
toku_logger_fsync_if_lsn_not_fsynced (TOKULOGGER logger, LSN lsn) {
    int r = 0;
    if (logger->is_panicked) r = EINVAL;
    else if (logger->write_log_files) {
        ml_lock(&logger->input_lock);
        logger->input_lock_ctr++;
        r = toku_logger_maybe_fsync(logger, lsn, TRUE);
        if (r!=0) {
            toku_logger_panic(logger, r);
        }
    }
    return r;
}

void toku_logger_panic (TOKULOGGER logger, int err) {
    logger->panic_errno=err;
    logger->is_panicked=TRUE;
}
int toku_logger_panicked(TOKULOGGER logger) {
    if (logger==0) return 0;
    return logger->is_panicked;
}
int toku_logger_is_open(TOKULOGGER logger) {
    if (logger==0) return 0;
    return logger->is_open;
}

void toku_logger_set_cachetable (TOKULOGGER logger, CACHETABLE ct) {
    logger->ct = ct;
}

int toku_logger_set_lg_max(TOKULOGGER logger, u_int32_t lg_max) {
    if (logger==0) return EINVAL; // no logger
    if (logger->is_panicked) return EINVAL;
    if (logger->is_open) return EINVAL;
    if (lg_max>(1<<30)) return EINVAL; // too big
    logger->lg_max = lg_max;
    return 0;
}
int toku_logger_get_lg_max(TOKULOGGER logger, u_int32_t *lg_maxp) {
    if (logger==0) return EINVAL; // no logger
    if (logger->is_panicked) return EINVAL;
    *lg_maxp = logger->lg_max;
    return 0;
}

int toku_logger_set_lg_bsize(TOKULOGGER logger, u_int32_t bsize) {
    if (logger==0) return EINVAL; // no logger
    if (logger->is_panicked) return EINVAL;
    if (logger->is_open) return EINVAL;
    if (bsize<=0 || bsize>(1<<30)) return EINVAL;
    logger->write_block_size = bsize;
    return 0;
}

int toku_logger_find_next_unused_log_file(const char *directory, long long *result)
// This is called during logger initialalization, and no locks are required.
{
    DIR *d=opendir(directory);
    long long maxf=-1; *result = maxf;
    struct dirent *de;
    if (d==0) return errno;
    while ((de=readdir(d))) {
	if (de==0) return errno;
        long long thisl;
        if ( is_a_logfile(de->d_name, &thisl) ) {
            if ((long long)thisl > maxf) maxf = thisl;
        }
    }
    *result=maxf+1;
    int r = closedir(d);
    return r;
}

// TODO: Put this in portability layer when ready
// in: file pathname that may have a dirname prefix
// return: file leaf name
static char * fileleafname(char *pathname) {
    const char delimiter = '/';
    char *leafname = strrchr(pathname, delimiter);
    if (leafname)
	leafname++;
    else
	leafname = pathname;
    return leafname;
}

static int logfilenamecompare (const void *ap, const void *bp) {
    char *a=*(char**)ap;
    char *a_leafname = fileleafname(a);
    char *b=*(char**)bp;
    char * b_leafname = fileleafname(b);
    int rval;
    BOOL valid;
    uint64_t num_a = 0;  // placate compiler
    uint64_t num_b = 0;
    uint32_t ver_a = 0;
    uint32_t ver_b = 0;
    valid = is_a_logfile_any_version(a_leafname, &num_a, &ver_a);
    invariant(valid);
    valid = is_a_logfile_any_version(b_leafname, &num_b, &ver_b);
    invariant(valid);
    if (ver_a < ver_b) rval = -1;
    else if (ver_a > ver_b) rval = +1;
    else if (num_a < num_b) rval = -1;
    else if (num_a > num_b) rval = +1;
    else rval = 0;
    return rval;
}

// Return the log files in sorted order
// Return a null_terminated array of strings, and also return the number of strings in the array.
// Requires: Race conditions must be dealt with by caller.  Either call during initialization or grab the output permission.
int toku_logger_find_logfiles (const char *directory, char ***resultp, int *n_logfiles)
{
    int result_limit=2;
    int n_results=0;
    char **MALLOC_N(result_limit, result);
    assert(result!= NULL);
    struct dirent *de;
    DIR *d=opendir(directory);
    if (d==0) {
        toku_free(result);
        return errno;
    }
    int dirnamelen = strlen(directory);
    while ((de=readdir(d))) {
	uint64_t thisl;
        uint32_t version_ignore;
        if ( !(is_a_logfile_any_version(de->d_name, &thisl, &version_ignore)) ) continue; //#2424: Skip over files that don't match the exact logfile template 
	if (n_results+1>=result_limit) {
	    result_limit*=2;
	    result = toku_realloc(result, result_limit*sizeof(*result));
            // should we try to recover here?
            assert(result!=NULL);
	}
	int fnamelen = dirnamelen + strlen(de->d_name) + 2; // One for the slash and one for the trailing NUL.
	char *fname = toku_malloc(fnamelen);
        assert(fname!=NULL);
	snprintf(fname, fnamelen, "%s/%s", directory, de->d_name);
	result[n_results++] = fname;
    }
    // Return them in increasing order.  Set width to allow for newer log file names ("xxx.tokulog13")
    // which are one character longer than old log file names ("xxx.tokulog2").  The comparison function
    // won't look beyond the terminating NUL, so an extra character in the comparison string doesn't matter.
    // Allow room for terminating NUL after "xxx.tokulog13" even if result[0] is of form "xxx.tokulog2."
    int width = sizeof(result[0]+2);  
    qsort(result, n_results, width, logfilenamecompare);
    *resultp    = result;
    *n_logfiles = n_results;
    result[n_results]=0; // make a trailing null
    return d ? closedir(d) : 0;
}

static int open_logfile (TOKULOGGER logger)
// Entry and Exit: This thread has permission to modify the output.
{
    int fnamelen = strlen(logger->directory)+50;
    char fname[fnamelen];
    snprintf(fname, fnamelen, "%s/log%012lld.tokulog%d", logger->directory, logger->next_log_file_number, TOKU_LOG_VERSION);
    long long index = logger->next_log_file_number;
    if (logger->write_log_files) {
        logger->fd = open(fname, O_CREAT+O_WRONLY+O_TRUNC+O_EXCL+O_BINARY, S_IRWXU);     
        if (logger->fd==-1) return errno;
        int r = fsync_logdir(logger);   if (r!=0) return r; // t:2445
        logger->next_log_file_number++;
    } else {
        logger->fd = open(DEV_NULL_FILE, O_WRONLY+O_BINARY);
        // printf("%s: %s %d\n", __FUNCTION__, DEV_NULL_FILE, logger->fd); fflush(stdout);
        if (logger->fd==-1) return errno;
    }
    toku_os_full_write(logger->fd, "tokulogg", 8);
    int version_l = toku_htonl(log_format_version); //version MUST be in network byte order regardless of disk order
    toku_os_full_write(logger->fd, &version_l, 4);
    if ( logger->write_log_files ) {
        TOKULOGFILEINFO lf_info = toku_malloc(sizeof(struct toku_logfile_info));
        if (lf_info == NULL) 
            return ENOMEM;
        lf_info->index = index;
        lf_info->maxlsn = logger->written_lsn; 
        lf_info->version = TOKU_LOG_VERSION;
        toku_logfilemgr_add_logfile_info(logger->logfilemgr, lf_info);
    }
    logger->fsynced_lsn = logger->written_lsn;
    logger->n_in_file = 12;
    return 0;
}

static int delete_logfile(TOKULOGGER logger, long long index, uint32_t version)
// Entry and Exit: This thread has permission to modify the output.
{
    int fnamelen = strlen(logger->directory)+50;
    char fname[fnamelen];
    snprintf(fname, fnamelen, "%s/log%012lld.tokulog%d", logger->directory, index, version);
    int r = remove(fname);
    return r;
}

int toku_logger_maybe_trim_log(TOKULOGGER logger, LSN trim_lsn)
// On entry and exit: No logger locks held.
// Acquires and releases output permission.
{
    int r=0;
    LSN fsynced_lsn;
    grab_output(logger, &fsynced_lsn);
    TOKULOGFILEMGR lfm = logger->logfilemgr;
    int n_logfiles = toku_logfilemgr_num_logfiles(lfm);

    TOKULOGFILEINFO lf_info = NULL;
    
    if ( logger->write_log_files && logger->trim_log_files) {
        while ( n_logfiles > 1 ) { // don't delete current logfile
            uint32_t log_version;
            lf_info = toku_logfilemgr_get_oldest_logfile_info(lfm);
            log_version = lf_info->version;
            if ( lf_info->maxlsn.lsn >= trim_lsn.lsn ) {
                // file contains an open LSN, can't delete this or any newer log files
                break;
            }
            // need to save copy - toku_logfilemgr_delete_oldest_logfile_info free's the lf_info
            long index = lf_info->index;
            toku_logfilemgr_delete_oldest_logfile_info(lfm);
            n_logfiles--;
            r = delete_logfile(logger, index, log_version);
            if (r!=0) {
                break;
            }
        }
    }
    release_output(logger, fsynced_lsn);
    return r;
}

void toku_logger_write_log_files (TOKULOGGER logger, BOOL write_log_files)
// Called only during initialization (or just after recovery), so no locks are needed.
{
    logger->write_log_files = write_log_files;
}

void toku_logger_trim_log_files (TOKULOGGER logger, BOOL trim_log_files)
// Called only during initialization, so no locks are needed.
{
    logger->trim_log_files = trim_log_files;
}

int toku_logger_maybe_fsync (TOKULOGGER logger, LSN lsn, int do_fsync)
// Effect: If fsync is nonzero, then make sure that the log is flushed and synced at least up to lsn.
// Entry: Holds input lock.  The log entry has already been written to the input buffer.
// Exit:  Holds no locks.
// The input lock may be released and then reacquired.  Thus this function does not run atomically with respect to other threads.
{
    int r;
    if (do_fsync) {
	// reacquire the locks (acquire output permission first)
	logger->input_lock_ctr++;
	ml_unlock(&logger->input_lock);
	LSN  fsynced_lsn;
	BOOL already_done = wait_till_output_already_written_or_output_buffer_available(logger, lsn, &fsynced_lsn);
	if (already_done) return 0;

	// otherwise we now own the output permission, and our lsn isn't outputed.

	ml_lock(&logger->input_lock);
	logger->input_lock_ctr++;
    
	swap_inbuf_outbuf(logger);

	logger->input_lock_ctr++;
	ml_unlock(&logger->input_lock); // release the input lock now, so other threads can fill the inbuf.  (Thus enabling group commit.)

	write_outbuf_to_logfile(logger, &fsynced_lsn);
	if (fsynced_lsn.lsn < lsn.lsn) {
	    // it may have gotten fsynced by the write_outbuf_to_logfile.
	    r = toku_file_fsync_without_accounting(logger->fd);
	    if (r!=0) {
		toku_logger_panic(logger, r);
		return r;
	    }
	    assert(fsynced_lsn.lsn <= logger->written_lsn.lsn);
	    fsynced_lsn = logger->written_lsn;
	}
	// the last lsn is only accessed while holding output permission or else when the log file is old.
	if ( logger->write_log_files )
	    toku_logfilemgr_update_last_lsn(logger->logfilemgr, logger->written_lsn);
	release_output(logger, fsynced_lsn);
    } else {
	logger->input_lock_ctr++;
	ml_unlock(&logger->input_lock);
    }
    return 0;
}

static int
toku_logger_write_buffer (TOKULOGGER logger, LSN *fsynced_lsn) 
// Entry:  Holds the input lock and permission to modify output.
// Exit:   Holds only the permission to modify output.
// Effect:  Write the buffers to the output.  If DO_FSYNC is true, then fsync.
// Note: Only called during single-threaded activity from toku_logger_restart, so locks aren't really needed.
{
    swap_inbuf_outbuf(logger);
    logger->input_lock_ctr++;
    ml_unlock(&logger->input_lock);
    write_outbuf_to_logfile(logger, fsynced_lsn);
    if (logger->write_log_files) {
	int r = toku_file_fsync_without_accounting(logger->fd);
	if (r!=0) {
	    toku_logger_panic(logger, r);
	    return r;
	}
	toku_logfilemgr_update_last_lsn(logger->logfilemgr, logger->written_lsn);  // t:2294
    }
    return 0;
}

int toku_logger_restart(TOKULOGGER logger, LSN lastlsn)
// Entry and exit: Holds no locks (this is called only during single-threaded activity, such as initial start).
{
    int r;

    // flush out the log buffer
    LSN fsynced_lsn;
    grab_output(logger, &fsynced_lsn);
    ml_lock(&logger->input_lock);
    logger->input_lock_ctr++;
    r = toku_logger_write_buffer(logger, &fsynced_lsn); assert(r == 0);

    // close the log file
    if ( logger->write_log_files) { // fsyncs don't work to /dev/null
        r = toku_file_fsync_without_accounting(logger->fd); 
        if ( r!=0 ) {
            toku_logger_panic(logger, r);
            return r;
        }
    }
    r = close(logger->fd);                              assert(r == 0);
    logger->fd = -1;

    // reset the LSN's to the lastlsn when the logger was opened
    logger->lsn = logger->written_lsn = logger->fsynced_lsn = lastlsn;
    logger->write_log_files = TRUE;
    logger->trim_log_files = TRUE;

    // open a new log file
    r = open_logfile(logger);
    release_output(logger, fsynced_lsn);
    return r;
}

// fname is the iname
int toku_logger_log_fcreate (TOKUTXN txn, const char *fname, FILENUM filenum, u_int32_t mode, u_int32_t treeflags, u_int32_t nodesize, u_int32_t basementnodesize, enum toku_compression_method compression_method) {
    if (txn==0) return 0;
    if (txn->logger->is_panicked) return EINVAL;
    BYTESTRING bs_fname = { .len=strlen(fname), .data = (char *) fname };
    // fsync log on fcreate
    int r = toku_log_fcreate (txn->logger, (LSN*)0, 1, toku_txn_get_txnid(txn), filenum, bs_fname, mode, treeflags, nodesize, basementnodesize, compression_method);
    return r;
}


// fname is the iname 
int toku_logger_log_fdelete (TOKUTXN txn, const char *fname) {
    if (txn==0) return 0;
    if (txn->logger->is_panicked) return EINVAL;
    BYTESTRING bs = { .len=strlen(fname), .data = (char *) fname };
    //No fsync.
    int r = toku_log_fdelete (txn->logger, (LSN*)0, 0, toku_txn_get_txnid(txn), bs);
    return r;
}



/* fopen isn't really an action.  It's just for bookkeeping.  We need to know the filename that goes with a filenum. */
int toku_logger_log_fopen (TOKUTXN txn, const char * fname, FILENUM filenum, uint32_t treeflags) {
    if (txn==0) return 0;
    if (txn->logger->is_panicked) return EINVAL;
    BYTESTRING bs;
    bs.len = strlen(fname);
    bs.data = (char*)fname;
    return toku_log_fopen (txn->logger, (LSN*)0, 0, bs, filenum, treeflags);
}

static int toku_fread_u_int8_t_nocrclen (FILE *f, u_int8_t *v) {
    int vi=fgetc(f);
    if (vi==EOF) return -1;
    u_int8_t vc=(u_int8_t)vi;
    *v = vc;
    return 0;
}

int toku_fread_u_int8_t (FILE *f, u_int8_t *v, struct x1764 *mm, u_int32_t *len) {
    int vi=fgetc(f);
    if (vi==EOF) return -1;
    u_int8_t vc=(u_int8_t)vi;
    x1764_add(mm, &vc, 1);
    (*len)++;
    *v = vc;
    return 0;
}

int toku_fread_u_int32_t_nocrclen (FILE *f, u_int32_t *v) {
    u_int32_t result;
    u_int8_t *cp = (u_int8_t*)&result;
    int r;
    r = toku_fread_u_int8_t_nocrclen (f, cp+0); if (r!=0) return r;
    r = toku_fread_u_int8_t_nocrclen (f, cp+1); if (r!=0) return r;
    r = toku_fread_u_int8_t_nocrclen (f, cp+2); if (r!=0) return r;
    r = toku_fread_u_int8_t_nocrclen (f, cp+3); if (r!=0) return r;
    *v = toku_dtoh32(result);

    return 0;
}
int toku_fread_u_int32_t (FILE *f, u_int32_t *v, struct x1764 *checksum, u_int32_t *len) {
    u_int32_t result;
    u_int8_t *cp = (u_int8_t*)&result;
    int r;
    r = toku_fread_u_int8_t (f, cp+0, checksum, len); if(r!=0) return r;
    r = toku_fread_u_int8_t (f, cp+1, checksum, len); if(r!=0) return r;
    r = toku_fread_u_int8_t (f, cp+2, checksum, len); if(r!=0) return r;
    r = toku_fread_u_int8_t (f, cp+3, checksum, len); if(r!=0) return r;
    *v = toku_dtoh32(result);
    return 0;
}

int toku_fread_u_int64_t (FILE *f, u_int64_t *v, struct x1764 *checksum, u_int32_t *len) {
    u_int32_t v1,v2;
    int r;
    r=toku_fread_u_int32_t(f, &v1, checksum, len);    if (r!=0) return r;
    r=toku_fread_u_int32_t(f, &v2, checksum, len);    if (r!=0) return r;
    *v = (((u_int64_t)v1)<<32 ) | ((u_int64_t)v2);
    return 0;
}

int toku_fread_BOOL (FILE *f, BOOL *v, struct x1764 *mm, u_int32_t *len) {
    u_int8_t iv;
    int r = toku_fread_u_int8_t(f, &iv, mm, len);
    *v = (iv!=0);
    return r;
}

int toku_fread_LSN     (FILE *f, LSN *lsn, struct x1764 *checksum, u_int32_t *len) {
    return toku_fread_u_int64_t (f, &lsn->lsn, checksum, len);
}

int toku_fread_BLOCKNUM (FILE *f, BLOCKNUM *b, struct x1764 *checksum, u_int32_t *len) {
    return toku_fread_u_int64_t (f, (u_int64_t*)&b->b, checksum, len);
}

int toku_fread_FILENUM (FILE *f, FILENUM *filenum, struct x1764 *checksum, u_int32_t *len) {
    return toku_fread_u_int32_t (f, &filenum->fileid, checksum, len);
}

int toku_fread_TXNID   (FILE *f, TXNID *txnid, struct x1764 *checksum, u_int32_t *len) {
    return toku_fread_u_int64_t (f, txnid, checksum, len);
}

int toku_fread_XIDP    (FILE *f, XIDP *xidp, struct x1764 *checksum, u_int32_t *len) {
    // These reads are verbose because XA defined the fields as "long", but we use 4 bytes, 1 byte and 1 byte respectively.
    TOKU_XA_XID *XMALLOC(xid);
    {
        u_int32_t formatID;
        toku_fread_u_int32_t(f, &formatID,     checksum, len);
        xid->formatID = formatID;
    }
    {
        u_int8_t gtrid_length;
        toku_fread_u_int8_t (f, &gtrid_length, checksum, len);
        xid->gtrid_length = gtrid_length;
    }
    {
        u_int8_t bqual_length;
        toku_fread_u_int8_t (f, &bqual_length, checksum, len);
        xid->bqual_length = bqual_length;
    }
    for (int i=0; i< xid->gtrid_length + xid->bqual_length; i++) {
        u_int8_t byte;
	int r = toku_fread_u_int8_t(f, &byte, checksum, len);
        xid->data[i] = byte;
	if (r!=0) return r;
    }
    *xidp = xid;
    return 0;
}

// fills in the bs with malloced data.
int toku_fread_BYTESTRING (FILE *f, BYTESTRING *bs, struct x1764 *checksum, u_int32_t *len) {
    int r=toku_fread_u_int32_t(f, (u_int32_t*)&bs->len, checksum, len);
    if (r!=0) return r;
    bs->data = toku_malloc(bs->len);
    u_int32_t i;
    for (i=0; i<bs->len; i++) {
	r=toku_fread_u_int8_t(f, (u_int8_t*)&bs->data[i], checksum, len);
	if (r!=0) {
	    toku_free(bs->data);
	    bs->data=0;
	    return r;
	}
    }
    return 0;
}

// fills in the fs with malloced data.
int toku_fread_FILENUMS (FILE *f, FILENUMS *fs, struct x1764 *checksum, u_int32_t *len) {
    int r=toku_fread_u_int32_t(f, (u_int32_t*)&fs->num, checksum, len);
    if (r!=0) return r;
    fs->filenums = toku_malloc(fs->num * sizeof(FILENUM));
    u_int32_t i;
    for (i=0; i<fs->num; i++) {
	r=toku_fread_FILENUM (f, &fs->filenums[i], checksum, len);
	if (r!=0) {
	    toku_free(fs->filenums);
	    fs->filenums=0;
	    return r;
	}
    }
    return 0;
}

int toku_logprint_LSN (FILE *outf, FILE *inf, const char *fieldname, struct x1764 *checksum, u_int32_t *len, const char *format __attribute__((__unused__))) {
    LSN v;
    int r = toku_fread_LSN(inf, &v, checksum, len);
    if (r!=0) return r;
    fprintf(outf, " %s=%" PRIu64, fieldname, v.lsn);
    return 0;
}

int toku_logprint_TXNID (FILE *outf, FILE *inf, const char *fieldname, struct x1764 *checksum, u_int32_t *len, const char *format __attribute__((__unused__))) {
    TXNID v;
    int r = toku_fread_TXNID(inf, &v, checksum, len);
    if (r!=0) return r;
    fprintf(outf, " %s=%" PRIu64, fieldname, v);
    return 0;
}

int toku_logprint_XIDP (FILE *outf, FILE *inf, const char *fieldname, struct x1764 *checksum, u_int32_t *len, const char *format __attribute__((__unused__))) {
    XIDP vp;
    int r = toku_fread_XIDP(inf, &vp, checksum, len);
    if (r!=0) return r;
    fprintf(outf, "%s={formatID=0x%lx gtrid_length=%ld bqual_length=%ld data=", fieldname, vp->formatID, vp->gtrid_length, vp->bqual_length);
    toku_print_bytes(outf, vp->gtrid_length + vp->bqual_length, vp->data);
    fprintf(outf, "}");
    toku_free(vp);
    return 0;
}

int toku_logprint_u_int8_t (FILE *outf, FILE *inf, const char *fieldname, struct x1764 *checksum, u_int32_t *len, const char *format) {
    u_int8_t v;
    int r = toku_fread_u_int8_t(inf, &v, checksum, len);
    if (r!=0) return r;
    fprintf(outf, " %s=%d", fieldname, v);
    if (format) fprintf(outf, format, v);
    else if (v=='\'') fprintf(outf, "('\'')");
    else if (isprint(v)) fprintf(outf, "('%c')", v);
    else {}/*nothing*/
    return 0;
}

int toku_logprint_u_int32_t (FILE *outf, FILE *inf, const char *fieldname, struct x1764 *checksum, u_int32_t *len, const char *format) {
    u_int32_t v;
    int r = toku_fread_u_int32_t(inf, &v, checksum, len);
    if (r!=0) return r;
    fprintf(outf, " %s=", fieldname);
    fprintf(outf, format ? format : "%d", v);
    return 0;
}

int toku_logprint_u_int64_t (FILE *outf, FILE *inf, const char *fieldname, struct x1764 *checksum, u_int32_t *len, const char *format) {
    u_int64_t v;
    int r = toku_fread_u_int64_t(inf, &v, checksum, len);
    if (r!=0) return r;
    fprintf(outf, " %s=", fieldname);
    fprintf(outf, format ? format : "%"PRId64, v);
    return 0;
}

int toku_logprint_BOOL (FILE *outf, FILE *inf, const char *fieldname, struct x1764 *checksum, u_int32_t *len, const char *format __attribute__((__unused__))) {
    BOOL v;
    int r = toku_fread_BOOL(inf, &v, checksum, len);
    if (r!=0) return r;
    fprintf(outf, " %s=%s", fieldname, v ? "TRUE" : "FALSE");
    return 0;

}

void toku_print_BYTESTRING (FILE *outf, u_int32_t len, char *data) {
    fprintf(outf, "{len=%u data=", len);
    toku_print_bytes(outf, len, data);
    fprintf(outf, "}");

}

int toku_logprint_BYTESTRING (FILE *outf, FILE *inf, const char *fieldname, struct x1764 *checksum, u_int32_t *len, const char *format __attribute__((__unused__))) {
    BYTESTRING bs;
    int r = toku_fread_BYTESTRING(inf, &bs, checksum, len);
    if (r!=0) return r;
    fprintf(outf, " %s=", fieldname);
    toku_print_BYTESTRING(outf, bs.len, bs.data);
    toku_free(bs.data);
    return 0;
}

int toku_logprint_BLOCKNUM (FILE *outf, FILE *inf, const char *fieldname, struct x1764 *checksum, u_int32_t *len, const char *format) {
    return toku_logprint_u_int64_t(outf, inf, fieldname, checksum, len, format);

}

int toku_logprint_FILENUM (FILE *outf, FILE *inf, const char *fieldname, struct x1764 *checksum, u_int32_t *len, const char *format) {
    return toku_logprint_u_int32_t(outf, inf, fieldname, checksum, len, format);

}

static void
toku_print_FILENUMS (FILE *outf, u_int32_t num, FILENUM *filenums) {
    fprintf(outf, "{num=%u filenums=\"", num);
    u_int32_t i;
    for (i=0; i<num; i++) {
        if (i>0)
            fprintf(outf, ",");
        fprintf(outf, "0x%"PRIx32, filenums[i].fileid);
    }
    fprintf(outf, "\"}");

}

int toku_logprint_FILENUMS (FILE *outf, FILE *inf, const char *fieldname, struct x1764 *checksum, u_int32_t *len, const char *format __attribute__((__unused__))) {
    FILENUMS bs;
    int r = toku_fread_FILENUMS(inf, &bs, checksum, len);
    if (r!=0) return r;
    fprintf(outf, " %s=", fieldname);
    toku_print_FILENUMS(outf, bs.num, bs.filenums);
    toku_free(bs.filenums);
    return 0;
}

int toku_read_and_print_logmagic (FILE *f, u_int32_t *versionp) {
    {
	char magic[8];
	int r=fread(magic, 1, 8, f);
	if (r!=8) {
	    return DB_BADFORMAT;
	}
	if (memcmp(magic, "tokulogg", 8)!=0) {
	    return DB_BADFORMAT;
	}
    }
    {
	int version;
    	int r=fread(&version, 1, 4, f);
	if (r!=4) {
	    return DB_BADFORMAT;
	}
	printf("tokulog v.%u\n", toku_ntohl(version));
        //version MUST be in network order regardless of disk order
	*versionp=toku_ntohl(version);
    }
    return 0;
}

int toku_read_logmagic (FILE *f, u_int32_t *versionp) {
    {
	char magic[8];
	int r=fread(magic, 1, 8, f);
	if (r!=8) {
	    return DB_BADFORMAT;
	}
	if (memcmp(magic, "tokulogg", 8)!=0) {
	    return DB_BADFORMAT;
	}
    }
    {
	int version;
    	int r=fread(&version, 1, 4, f);
	if (r!=4) {
	    return DB_BADFORMAT;
	}
	*versionp=toku_ntohl(version);
    }
    return 0;
}

TXNID toku_txn_get_root_txnid (TOKUTXN txn) {
    if (txn==0) return 0;
    TOKUTXN root = txn;
    while (root->parent) {
        root = root->parent;
    }
    return root->txnid64;
}

TXNID toku_txn_get_txnid (TOKUTXN txn) {
    if (txn==0) return 0;
    else return txn->txnid64;
}

LSN toku_logger_last_lsn(TOKULOGGER logger) {
    return logger->lsn;
}

TOKULOGGER toku_txn_logger (TOKUTXN txn) {
    return txn ? txn->logger : 0;
}

//Heaviside function to search through an OMT by a TXNID
static int
find_by_xid (OMTVALUE v, void *txnidv) {
    TOKUTXN txn = v;
    TXNID   txnidfind = *(TXNID*)txnidv;
    if (txn->txnid64<txnidfind) return -1;
    if (txn->txnid64>txnidfind) return +1;
    return 0;
}

BOOL is_txnid_live(TOKULOGGER logger, TXNID txnid) {
    assert(logger);
    TOKUTXN result = NULL;
    int rval = toku_txnid2txn(logger, txnid, &result);
    assert(rval == 0);
    return (result != NULL);
}

int toku_txnid2txn (TOKULOGGER logger, TXNID txnid, TOKUTXN *result) {
    if (logger==NULL) return -1;

    OMTVALUE txnfound;
    int rval;
    int r = toku_omt_find_zero(logger->live_txns, find_by_xid, &txnid, &txnfound, NULL);
    if (r==0) {
        TOKUTXN txn = txnfound;
        assert(txn->txnid64==txnid);
        *result = txn;
        rval = 0;
    }
    else {
        assert(r==DB_NOTFOUND);
        // If there is no txn, then we treat it as the null txn.
        *result = NULL;
        rval    = 0;
    }
    return rval;
}

// Find the earliest LSN in a log.  No locks are needed.
static int peek_at_log (TOKULOGGER logger, char* filename, LSN *first_lsn) {
    int fd = open(filename, O_RDONLY+O_BINARY);
    if (fd<0) {
        if (logger->write_log_files) printf("couldn't open: %s\n", strerror(errno));
        return errno;
    }
    enum { SKIP = 12+1+4 }; // read the 12 byte header, the first cmd, and the first len
    unsigned char header[SKIP+8];
    int r = read(fd, header, SKIP+8);
    if (r!=SKIP+8) return 0; // cannot determine that it's archivable, so we'll assume no.  If a later-log is archivable is then this one will be too.

    u_int64_t lsn;
    {
        struct rbuf rb;
        rb.buf   = header+SKIP;
        rb.size  = 8;
        rb.ndone = 0;
        lsn = rbuf_ulonglong(&rb);
    }

    r=close(fd);
    if (r!=0) { return 0; }

    first_lsn->lsn=lsn;
    return 0;
}

// Return a malloc'd array of malloc'd strings which are the filenames that can be archived.
// Output permission are obtained briefly so we can get a list of the log files without conflicting.
int toku_logger_log_archive (TOKULOGGER logger, char ***logs_p, int flags) {
    if (flags!=0) return EINVAL; // don't know what to do.
    int all_n_logs;
    int i;
    char **all_logs;
    int n_logfiles;
    LSN fsynced_lsn;
    grab_output(logger, &fsynced_lsn);
    int r = toku_logger_find_logfiles (logger->directory, &all_logs, &n_logfiles);
    release_output(logger, fsynced_lsn);
    if (r!=0) return r;

    for (i=0; all_logs[i]; i++);
    all_n_logs=i;
    // get them into increasing order
    qsort(all_logs, all_n_logs, sizeof(all_logs[0]), logfilenamecompare);

    LSN save_lsn = logger->last_completed_checkpoint_lsn;

    // Now starting at the last one, look for archivable ones.
    // Count the total number of bytes, because we have to return a single big array.  (That's the BDB interface.  Bleah...)
    LSN earliest_lsn_in_logfile={(unsigned long long)(-1LL)};
    r = peek_at_log(logger, all_logs[all_n_logs-1], &earliest_lsn_in_logfile); // try to find the lsn that's in the most recent log
    if (earliest_lsn_in_logfile.lsn <= save_lsn.lsn) {
	i=all_n_logs-1;
    } else {
	for (i=all_n_logs-2; i>=0; i--) { // start at all_n_logs-2 because we never archive the most recent log
	    r = peek_at_log(logger, all_logs[i], &earliest_lsn_in_logfile);
	    if (r!=0) continue; // In case of error, just keep going
	
	    if (earliest_lsn_in_logfile.lsn <= save_lsn.lsn) {
		break;
	    }
	}
    }

    // all log files up to, but but not including, i can be archived.
    int n_to_archive=i;
    int count_bytes=0;
    for (i=0; i<n_to_archive; i++) {
	count_bytes+=1+strlen(all_logs[i]);
    }
    char **result;
    if (i==0) {
	result=0;
    } else {
	result = toku_malloc((1+n_to_archive)*sizeof(*result) + count_bytes);
	char  *base = (char*)(result+1+n_to_archive);
	for (i=0; i<n_to_archive; i++) {
	    int len=1+strlen(all_logs[i]);
	    result[i]=base;
	    memcpy(base, all_logs[i], len);
	    base+=len;
	}
	result[n_to_archive]=0;
    }
    for (i=0; all_logs[i]; i++) {
	toku_free(all_logs[i]);
    }
    toku_free(all_logs);
    *logs_p = result;
    return 0;
}


TOKUTXN toku_logger_txn_parent (TOKUTXN txn) {
    return txn->parent;
}

void toku_logger_note_checkpoint(TOKULOGGER logger, LSN lsn) {
    logger->last_completed_checkpoint_lsn = lsn;
}

TXNID toku_logger_get_oldest_living_xid(TOKULOGGER logger, time_t * oldest_living_starttime) {
    TXNID rval = 0;
    if (logger) {
        rval = logger->oldest_living_xid;
	if (oldest_living_starttime)
	    *oldest_living_starttime = logger->oldest_living_starttime;
    }
    return rval;
}

LSN
toku_logger_get_next_lsn(TOKULOGGER logger) {
    return logger->lsn;
}

//////////////////////////////////////////////////////////////////////////////////////
// remove_finalize_callback is set when environment is created so that when 
// a file removal is committed (or a file creation is aborted), the brt
// layer can call the ydb-layer callback to clean up the lock tree.


// called from toku_env_open()
void 
toku_logger_set_remove_finalize_callback(TOKULOGGER logger, void (*funcp)(DICTIONARY_ID, void *), void * extra) {
    logger->remove_finalize_callback = funcp;
    logger->remove_finalize_callback_extra = extra;
}

// called when a transaction that deleted a file is committed, or
// when a transaction that created a file is aborted.
// During recovery, there is no ydb layer, so no callback exists.
void
toku_logger_call_remove_finalize_callback(TOKULOGGER logger, DICTIONARY_ID dict_id) {
    if (logger->remove_finalize_callback)
        logger->remove_finalize_callback(dict_id, logger->remove_finalize_callback_extra);
}

///////////////////////////////////////////////////////////////////////////////////
// Engine status
//
// Status is intended for display to humans to help understand system behavior.
// It does not need to be perfectly thread-safe.

static LOGGER_STATUS_S logger_status;

#define STATUS_INIT(k,t,l) { \
	logger_status.status[k].keyname = #k; \
	logger_status.status[k].type    = t;  \
	logger_status.status[k].legend  = "logger: " l; \
    }

static void
status_init(void) {
    // Note, this function initializes the keyname, type, and legend fields.
    // Value fields are initialized to zero by compiler.
    STATUS_INIT(LOGGER_NEXT_LSN,     UINT64,  "next LSN");
    STATUS_INIT(LOGGER_ILOCK_CTR,    UINT64,  "ilock count");
    STATUS_INIT(LOGGER_OLOCK_CTR,    UINT64,  "olock count");
    STATUS_INIT(LOGGER_SWAP_CTR,     UINT64,  "swap count");
    STATUS_INIT(LOGGER_PANICKED,     UINT64,  "panic");
    STATUS_INIT(LOGGER_PANIC_ERRNO,  UINT64,  "panic errno");
    logger_status.initialized = true;
}
#undef STATUS_INIT

#define STATUS_VALUE(x) logger_status.status[x].value.num

void
toku_logger_get_status(TOKULOGGER logger, LOGGER_STATUS statp) {
    if (!logger_status.initialized)
	status_init();
    if (logger) {
        STATUS_VALUE(LOGGER_NEXT_LSN)    = logger->lsn.lsn;
	STATUS_VALUE(LOGGER_ILOCK_CTR)   = logger->input_lock_ctr;
	STATUS_VALUE(LOGGER_OLOCK_CTR)   = logger->output_condition_lock_ctr;
	STATUS_VALUE(LOGGER_SWAP_CTR)    = logger->swap_ctr;
	STATUS_VALUE(LOGGER_PANICKED)    = logger->is_panicked;
	STATUS_VALUE(LOGGER_PANIC_ERRNO) = logger->panic_errno;
    }
    *statp = logger_status;
}



//////////////////////////////////////////////////////////////////////////////////////////////////////
// Used for upgrade: 
// if any valid log files exist in log_dir, then
//   set *found_any_logs to TRUE and set *version_found to version number of latest log
int
toku_get_version_of_logs_on_disk(const char *log_dir, BOOL *found_any_logs, uint32_t *version_found) {
    BOOL found = FALSE;
    uint32_t highest_version = 0;
    int r = 0;

    struct dirent *de;
    DIR *d=opendir(log_dir);
    if (d==NULL) {
        r = errno;
    }
    else {
        // Examine every file in the directory and find highest version
        while ((de=readdir(d))) {
            uint32_t this_log_version;
            uint64_t this_log_number;
            BOOL is_log = is_a_logfile_any_version(de->d_name, &this_log_number, &this_log_version);
            if (is_log) {
                if (!found) {  // first log file found
		    found = TRUE;
		    highest_version = this_log_version;
                }
		else
		    highest_version = highest_version > this_log_version ? highest_version : this_log_version;
            }
        }
    }
    {
        int r2 = closedir(d);
        if (r==0) r = r2;
    }
    if (r==0) {
        *found_any_logs = found;
        if (found)
            *version_found = highest_version;
    }
    return r;
}

#undef STATUS_VALUE