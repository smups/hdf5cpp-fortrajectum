/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.                                               *
 * Copyright by the Board of Trustees of the University of Illinois.         *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of HDF5.  The full HDF5 copyright notice, including     *
 * terms governing use, modification, and redistribution, is contained in    *
 * the COPYING file, which can be found at the root of the source code       *
 * distribution tree, or in https://support.hdfgroup.org/ftp/HDF5/releases.  *
 * If you do not have access to either file, you may request a copy from     *
 * help@hdfgroup.org.                                                        *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Programmer:  Richard Warren
 *              Wednesday, July 1, 2020
 *
 * Purpose:     This is part of a parallel subfiling I/O driver.
 *
 */

#include "H5FDsubfiling.h"

/***********/
/* Headers */
/***********/
#include "H5CXprivate.h" /* API Contexts                             */
#include "H5Dprivate.h"  /* Datasets                                 */
#include "H5Eprivate.h"  /* Error handling                           */
#include "H5Iprivate.h"  /* IDs                                      */
#include "H5Ipublic.h"   /* IDs                                      */
#include "H5MMprivate.h" /* Memory management                        */
#include "H5Pprivate.h"  /* Property lists                           */
#include "H5private.h"   /* Generic Functions                        */

/*
=========================================
Private functions
=========================================
*/

/*
--------------------------------------------------------------------------
sf_context_limit   -- How many contexts can be recorded (default = 4)
sf_context_entries -- The number of contexts that are currently recorded.
sf_context_cache   -- Storage for contexts
--------------------------------------------------------------------------
*/
// static size_t                 twoGIG_LIMIT = (1 << 30);
static size_t               sf_context_limit  = 16;
static subfiling_context_t *sf_context_cache  = NULL;
static size_t               sf_topology_limit = 4;
static sf_topology_t *      sf_topology_cache = NULL;
static app_layout_t *       sf_app_layout     = NULL;

static file_map_to_context_t *sf_open_file_map = NULL;
static int                    sf_file_map_size = 0;
#define DEFAULT_MAP_ENTRIES 8

/*
---------------------------------------
 Recording subfiling related statistics
---------------------------------------
 */
static stat_record_t subfiling_stats[TOTAL_STAT_COUNT];
#define SF_WRITE_OPS       (subfiling_stats[WRITE_STAT].op_count)
#define SF_WRITE_TIME      (subfiling_stats[WRITE_STAT].total / (double)subfiling_stats[WRITE_STAT].op_count)
#define SF_WRITE_WAIT_TIME (subfiling_stats[WRITE_WAIT].total / (double)subfiling_stats[WRITE_WAIT].op_count)
#define SF_READ_OPS        (subfiling_stats[READ_STAT].op_count)
#define SF_READ_TIME       (subfiling_stats[READ_STAT].total / (double)subfiling_stats[READ_STAT].op_count)
#define SF_READ_WAIT_TIME  (subfiling_stats[READ_WAIT].total / (double)subfiling_stats[READ_WAIT].op_count)
#define SF_QUEUE_DELAYS    (subfiling_stats[QUEUE_STAT].total)

static void
maybe_initialize_statistics(void)
{
    memset(subfiling_stats, 0, sizeof(subfiling_stats));
}

static void clear_fid_map_entry(uint64_t sf_fid);

/*
=========================================
Public functions
=========================================
*/

/*
-------------------------------------------------------------------------
  Programmer:  Richard Warren
  Purpose:     Return a pointer to the requested storage object.
               There are only 2 object types: TOPOLOGY or CONTEXT
               structures.  An object_id contains the object type
               in upper 32 bits and an index value in the lower 32 bits.
               Storage for an object is allocated as required.

               Topologies are static, i.e. for any one IO Concentrator
               allocation strategy, the results should always be the
               same.
               FIXME: The one exception to this being the 1 IOC per
               N MPI ranks. The value of N can be changed on a per-file
               basis, so we need address that at some point.

               Contexts are 1 per open file.  If only one file is open
               at a time, then we will only use a single context cache
               entry.
  Errors:      returns NULL if input SF_OBJ_TYPE is unrecognized or
               a memory allocation error.

  Revision History -- Initial implementation
-------------------------------------------------------------------------
*/
void *
get__subfiling_object(int64_t object_id)
{
    int obj_type = (int)((object_id >> 32) & 0x0FFFF);
    /* We don't require a large indexing space
     * 16 bits should be enough..
     */
    size_t index = (object_id & 0x0FFFF);
    if (obj_type == SF_TOPOLOGY) {
        /* We will likely only cache a single topology
         * which is that of the original parallel application.
         * In that context, we will identify the number of
         * nodes along with the number of MPI ranks on a node.
         */
        if (sf_topology_cache == NULL) {
            sf_topology_cache = (sf_topology_t *)calloc(sf_topology_limit, sizeof(sf_topology_t));
            assert(sf_topology_cache != NULL);
        }
        if (index < sf_topology_limit) {
            return (void *)&sf_topology_cache[index];
        }
        else {
            HDputs("Illegal toplogy object index");
        }
    }
    else if (obj_type == SF_CONTEXT) {
        /* Contexts provide information principally about
         * the application and how the data layout is managed
         * over some number of sub-files.  The important
         * parameters are the number of subfiles (or in the
         * context of IOCs, the MPI ranks and counts of the
         * processes which host an IO Concentrator.  We
         * also provide a map of IOC rank to MPI rank
         * to facilitate the communication of IO requests.
         */
        if (sf_context_cache == NULL) {
            sf_context_cache = (subfiling_context_t *)calloc(sf_context_limit, sizeof(subfiling_context_t));
            assert(sf_context_cache != NULL);
        }
        if (index == sf_context_limit) {
            sf_context_limit *= 2;
            sf_context_cache = (subfiling_context_t *)realloc(sf_context_cache,
                                                              sf_context_limit * sizeof(subfiling_context_t));
            assert(sf_context_cache != NULL);
        }
        else {
            return (void *)&sf_context_cache[index];
        }
    }
    else {
        printf("get__subfiling_object: UNKNOWN Subfiling object type id = 0x%lx\n", object_id);
    }
    return NULL;
} /* end get__subfiling_object() */

/*-------------------------------------------------------------------------
 * Function:    UTILITY FUNCTIONS:
 *              delete_subfiling_context - removes a context entry in the
 *                                         object cache.  Free communicators
 *                                         and zero other structure fields.
 *
 * Return:      none
 * Errors:      none
 *
 * Programmer:  Richard Warren
 *
 * Changes:     Initial Version/None.
 *
 *-------------------------------------------------------------------------
 */
void
delete_subfiling_context(hid_t context_id)
{
    subfiling_context_t *sf_context = get__subfiling_object(context_id);
    if (sf_context) {
        if (sf_context->topology->n_io_concentrators > 1) {
            if (sf_context->sf_group_comm != MPI_COMM_NULL) {
                MPI_Comm_free(&sf_context->sf_group_comm);
            }
            if (sf_context->sf_intercomm != MPI_COMM_NULL) {
                MPI_Comm_free(&sf_context->sf_intercomm);
            }
        }
        /* free(sf_context); */
    }

    return;
}

/*
======================================================
Public vars (for subfiling) and functions
We probably need a function to set and clear this
======================================================
*/
int sf_verbose_flag    = 0;
int sf_open_file_count = 0;

/*-------------------------------------------------------------------------
 * Function:    Public/Client set_verbose_flag
 *
 * Purpose:     For debugging purposes, I allow a verbose setting to
 *              have printing of relevent information into an IOC specific
 *              file that is opened as a result of enabling the flag
 *              and closed when the verbose setting is disabled.
 *
 * Return:      None
 * Errors:      None
 *
 * Programmer:  Richard Warren
 *
 * Changes:     Initial Version/None.
 *-------------------------------------------------------------------------
 */
void
set_verbose_flag(int subfile_rank, int new_value)
{
#ifndef NDEBUG
    sf_verbose_flag = (int)(new_value & 0x0FF);
    if (sf_verbose_flag) {
        char logname[64];
        sprintf(logname, "ioc_%d.log", subfile_rank);
        if (sf_open_file_count > 1)
            sf_logfile = fopen(logname, "a+");
        else
            sf_logfile = fopen(logname, "w+");
    }
    else if (sf_logfile) {
        fclose(sf_logfile);
        sf_logfile = NULL;
    }

#endif
    return;
}

/*-------------------------------------------------------------------------
 * Function:    record_fid_to_subfile
 *
 * Purpose:     Every opened HDF5 file will have (if utilizing subfiling)
 *              a subfiling context associated with it. It is important that
 *              the HDF5 file index is a constant rather than utilizing a
 *              posix file handle since files can be opened multiple times
 *              and with each file open, a new file handle will be assigned.
 *              Note that in such a case, the actual filesystem id will be
 *              retained.
 *
 *              We utilize that filesystem id (ino_t inode) so that
 *              irrespective of what process opens a common file, the
 *              subfiling system will generate a consistent context for this
 *              file across all parallel ranks.
 *
 *              This function simply records the filesystem handle to
 *              subfiling context mapping.
 *
 * Return:      SUCCEED or FAIL.
 * Errors:      FAILs ONLY if storage for the mapping entry cannot
 *              be allocated.
 *
 * Programmer:  Richard Warren
 *              7/17/2020
 *
 * Changes:     Initial Version/None.
 *
 *-------------------------------------------------------------------------
 */
static herr_t
record_fid_to_subfile(uint64_t fid, hid_t subfile_context_id, int *next_index)
{
    herr_t status = SUCCEED;
    int    index;
    if (sf_file_map_size == 0) {
        int i;
        sf_open_file_map =
            (file_map_to_context_t *)malloc((size_t)DEFAULT_MAP_ENTRIES * sizeof(file_map_to_context_t));
        if (sf_open_file_map == NULL) {
            perror("malloc");
            return FAIL;
        }
        sf_file_map_size = DEFAULT_MAP_ENTRIES;
        for (i = 0; i < sf_file_map_size; i++) {
            sf_open_file_map[i].h5_file_id    = (uint64_t)H5I_INVALID_HID;
            sf_open_file_map[i].sf_context_id = 0;
        }
    }
    for (index = 0; index < sf_file_map_size; index++) {
        if (sf_open_file_map[index].h5_file_id == (uint64_t)H5I_INVALID_HID) {
            sf_open_file_map[index].h5_file_id    = fid;
            sf_open_file_map[index].sf_context_id = subfile_context_id;

            if (next_index) {
                *next_index = index;
            }
            return status;
        }
    }
    if (index == sf_file_map_size) {
        int i;
        sf_open_file_map =
            realloc(sf_open_file_map, ((size_t)(sf_file_map_size * 2) * sizeof(file_map_to_context_t)));
        if (sf_open_file_map == NULL) {
            perror("realloc");
            return FAIL;
        }
        sf_file_map_size *= 2;
        for (i = index; i < sf_file_map_size; i++) {
            sf_open_file_map[i].h5_file_id = (uint64_t)H5I_INVALID_HID;
        }

        if (next_index) {
            *next_index = index;
        }

        sf_open_file_map[index].h5_file_id      = fid;
        sf_open_file_map[index++].sf_context_id = subfile_context_id;
    }
    return status;
} /* end record_fid_to_subfile() */

/*-------------------------------------------------------------------------
 * Function:    Internal open_subfile_with_context
 *
 * Purpose:     While we cannot know a priori, whether an HDF client will
 *              need to access data across the entirety of a file, e.g.
 *              an individual MPI rank may read or write only small
 *              segments of the entire file space; this function sends
 *              a file OPEN_OP to every IO concentrator.
 *
 *              Prior to opening any subfiles, the H5FDopen will have
 *              created an HDF5 file with the user specified naming.
 *              A path prefix will be selected and is available as
 *              an input argument.
 *
 *              The opened HDF5 file handle will contain device and
 *              inode values, these being constant for all processes
 *              opening the shared file.  The inode value is utilized
 *              as a key value and is associated with the sf_context
 *              which we recieve as one of the input arguments.
 *
 *              IO Concentrator threads will be initialized on MPI ranks
 *              which have been identified via application toplogy
 *              discovery.  The number and mapping of IOC to MPI_rank
 *              is part of the sf_context->topology structure.
 *
 * Return:      Success (0) or Faiure (non-zero)
 * Errors:      If MPI operations fail for some reason.
 *
 * Programmer:  Richard Warren
 *              7/17/2020
 *
 * Changes:     Initial Version/None.
 *-------------------------------------------------------------------------
 */
int
open_subfile_with_context(subfiling_context_t *sf_context, uint64_t fid, int flags)
{
    int    ret;
    int    g_errors = 0;
    int    l_errors = 0;
    double start_t  = MPI_Wtime();
    assert(sf_context != NULL);

#ifdef VERBOSE
    printf("[%s %d]: context_id=%ld\n", __func__, sf_context->topology->app_layout->world_rank,
           sf_context->sf_context_id);
#endif

    /*
     * Save the HDF5 file id (fid) to subfile context mapping.
     * There shouldn't be any issue, but check the status and
     * return if there was a problem.
     */

    ret = record_fid_to_subfile(fid, sf_context->sf_context_id, NULL);
    if (ret != SUCCEED) {
        printf("[%d - %s] Error mapping hdf5 file to a subfiling context\n",
               sf_context->topology->app_layout->world_rank, __func__);
        return -1;
    }

    if (sf_context->topology->rank_is_ioc) {
        sf_work_request_t msg = {{flags, (int64_t)fid, sf_context->sf_context_id},
                                 OPEN_OP,
                                 sf_context->topology->app_layout->world_rank,
                                 sf_context->topology->subfile_rank,
                                 sf_context->sf_context_id,
                                 start_t,
                                 NULL,
                                 0};

        if (flags & O_CREAT) {
            sf_context->sf_fid = -2;
        }

        l_errors = subfiling_open_file(&msg, sf_context->topology->subfile_rank, flags);
    }

    g_errors = l_errors;

    MPI_Allreduce(&l_errors, &g_errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (g_errors) {
        printf("[%s %d]: error count = %d l_errors=%d\n", __func__,
               sf_context->topology->app_layout->world_rank, g_errors, l_errors);
        fflush(stdout);
    }
    return g_errors;
} /* end open_subfile_with_context() */

/*-------------------------------------------------------------------------
 * Function:    Internal close__subfiles
 *
 * Purpose:     When closing and HDF5 file, we need to close any associated
 *              subfiles as well.  This function cycles through all known
 *              IO Concentrators to send a file CLOSE_OP command.
 *
 *              This function is collective across all MPI ranks which
 *              have opened HDF5 file which associated with the provided
 *              sf_context.  Once the request has been issued by all
 *              ranks, the subfile at each IOC will be closed and an
 *              completion ACK will be received.
 *
 *              Once the subfiles are closed, we initiate a teardown of
 *              the IOC and associated thread_pool threads.
 *
 * Return:      Success (0) or Faiure (non-zero)
 * Errors:      If MPI operations fail for some reason.
 *
 * Programmer:  Richard Warren
 *              7/17/2020
 *
 * Changes:     Initial Version/None.
 *-------------------------------------------------------------------------
 */
static int
close__subfiles(subfiling_context_t *sf_context, uint64_t fid)
{
    int    global_errors = 0, errors = 0;
    int    file_open_count;
    int    subfile_fid = 0;
    double t0 = 0.0, t1 = 0.0, t2 = 0.0;
    double t_main_exit = 0.0, t_finalize_threads = 0.0;

    HDassert((sf_context != NULL));
    t0 = MPI_Wtime();

#if MPI_VERSION >= 3 && MPI_SUBVERSION >= 1
    MPI_Request b_req      = MPI_REQUEST_NULL;
    int         mpi_status = MPI_Ibarrier(MPI_COMM_WORLD, &b_req);
    if (mpi_status == MPI_SUCCESS) {
        int completed = 0;
        while (!completed) {
            useconds_t t_delay = 5;
            usleep(t_delay);
            mpi_status = MPI_Test(&b_req, &completed, MPI_STATUS_IGNORE);
            if (mpi_status != MPI_SUCCESS)
                completed = 1;
        }
    }
#else
    MPI_Barrier(MPI_COMM_WORLD);
#endif

    /* We make the subfile close operation collective.
     * Otherwise, there may be a race condition between
     * our closing the subfiles and the user application
     * moving ahead and possibly re-opening a file.
     *
     * If we can, we utilize an async barrier which gives
     * us the opportunity to reduce the CPU load due to
     * MPI spinning while waiting for the barrier to
     * complete.  This is especially important if there
     * is heavy thread utilization due to subfiling
     * activities, i.e. the thread pool might be
     * extremely busy servicing IO requests from all
     * HDF5 application ranks.
     */
    /* The map from fid to context can now be cleared */
    clear_fid_map_entry(fid);

    if (sf_context->topology->rank_is_ioc) {
        file_open_count = atomic_load(&sf_file_open_count);
        atomic_fetch_sub(&sf_file_open_count, 1);

        /* If there's only a single file that is
         * currently open, we can shutdown the IO concentrator
         * as part of the file close.
         */
        if (file_open_count == 1) {
            /* Shutdown the main IOC thread */
            H5FD_ioc_set_shutdown_flag(1);
            /* Allow ioc_main to exit.*/
            usleep(20);

            t1 = MPI_Wtime();
            H5FD_ioc_wait_thread_main();
            t2          = MPI_Wtime();
            t1          = t2;
            t_main_exit = t2 - t1;
            H5FD_ioc_finalize_threads();
            t2 = MPI_Wtime();
        }

        t_finalize_threads = t2 - t1;

        if ((subfile_fid = sf_context->sf_fid) > 0) {
            if (HDclose(subfile_fid) < 0) {
                perror("close(subfile_fid)");
                errors++;
            }
            else {
                sf_context->sf_fid = -1;
            }
        }

#ifndef NDEBUG
        /* FIXME: If we've had multiple files open, our statistics
         * will be messed up!
         */
        if (sf_verbose_flag) {
            t1 = t2;
            if (sf_logfile != NULL) {
                fprintf(sf_logfile, "[%d] main_exit=%lf, finalize_threads=%lf\n", sf_context->sf_group_rank,
                        t_main_exit, t_finalize_threads);
                if (SF_WRITE_OPS > 0)
                    fprintf(sf_logfile,
                            "[%d] pwrite perf: wrt_ops=%ld wait=%lf pwrite=%lf IOC_shutdown = %lf seconds\n",
                            sf_context->sf_group_rank, SF_WRITE_OPS, SF_WRITE_WAIT_TIME, SF_WRITE_TIME,
                            (t1 - t0));
                if (SF_READ_OPS > 0)
                    fprintf(sf_logfile,
                            "[%d] pread perf: read_ops=%ld wait=%lf pread=%lf IOC_shutdown = %lf seconds\n",
                            sf_context->sf_group_rank, SF_READ_OPS, SF_READ_WAIT_TIME, SF_READ_TIME,
                            (t1 - t0));

                fprintf(sf_logfile, "[%d] Avg queue time=%lf seconds\n", sf_context->sf_group_rank,
                        SF_QUEUE_DELAYS / (double)(SF_WRITE_OPS + SF_READ_OPS));

                fflush(sf_logfile);

                fclose(sf_logfile);
                sf_logfile = NULL;
            }
        }
        if (sf_context->h5_filename) {
            free(sf_context->h5_filename);
            sf_context->h5_filename = NULL;
        }
        if (sf_context->subfile_prefix) {
            free(sf_context->subfile_prefix);
            sf_context->subfile_prefix = NULL;
        }
#endif
    }

    MPI_Allreduce(&errors, &global_errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

#ifndef NDEBUG
    if (sf_verbose_flag) {
        if (client_log != NULL) {
            fclose(client_log);
            client_log = NULL;
        }
    }
#endif
    return global_errors;
} /* end close__subfiles() */

#define MIN_RETRIES 10
/*
======================================================
File functions

The pread and pwrite posix functions are described as
being thread safe.
======================================================
*/

int
sf_read_data(int fd, int64_t file_offset, void *data_buffer, int64_t data_size, int subfile_rank)
{
    int        ret     = 0;
    int        retries = MIN_RETRIES;
    useconds_t delay   = 100;
    ssize_t    bytes_read;
    ssize_t    bytes_remaining = (ssize_t)data_size;
    char *     this_buffer     = data_buffer;

    while (bytes_remaining) {
        if ((bytes_read = (ssize_t)pread(fd, this_buffer, (size_t)bytes_remaining, file_offset)) < 0) {

            perror("pread failed!");
            HDprintf("[ioc(%d) %s] pread(fd, buf, bytes_remaining=%ld, "
                     "file_offset =%ld)\n",
                     subfile_rank, __func__, bytes_remaining, file_offset);
            HDfflush(stdout);
            return -1;
        }
        else if (bytes_read > 0) {
            /* reset retry params */
            retries = MIN_RETRIES;
            delay   = 100;
            bytes_remaining -= bytes_read;
#ifdef VERBOSE
            printf("[ioc(%d) %s]: read %ld bytes, remaining=%ld, file_offset=%ld\n", subfile_rank, __func__,
                   bytes_read, bytes_remaining, file_offset);
            fflush(stdout);
#endif
            this_buffer += bytes_read;
            file_offset += bytes_read;
        }
        else {
            if (retries == 0) {
#ifdef VERBOSE
                printf("[ioc(%d) %s] TIMEOUT: file_offset=%ld, data_size=%ld\n", subfile_rank, __func__,
                       file_offset, data_size);
                printf("[ioc(%d) %s] ERROR! read of 0 bytes == eof!\n", subfile_rank, __func__);

                fflush(stdout);
#endif
                return -2;
            }
            retries--;
            usleep(delay);
            delay *= 2;
        }
    }
    return ret;
} /* end sf_read_data() */

int
sf_write_data(int fd, int64_t file_offset, void *data_buffer, int64_t data_size, int subfile_rank)
{
    int     ret             = 0;
    char *  this_data       = (char *)data_buffer;
    ssize_t bytes_remaining = (ssize_t)data_size;
    ssize_t written         = 0;
    while (bytes_remaining) {
        if ((written = pwrite(fd, this_data, (size_t)bytes_remaining, file_offset)) < 0) {
            struct stat statbuf;
            perror("pwrite failed!");
            fstat(fd, &statbuf);
            HDprintf("[ioc(%d) %s] pwrite(fd, data, bytes_remaining=%ld, "
                     "file_offset=%ld), fd=%d, st_size=%ld\n",
                     subfile_rank, __func__, bytes_remaining, file_offset, fd, statbuf.st_size);
            HDfflush(stdout);
            return -1;
        }
        else {
            bytes_remaining -= written;
#ifdef VERBOSE
            printf("[ioc(%d) %s]: wrote %ld bytes, remaining=%ld, file_offset=%ld\n", subfile_rank, __func__,
                   written, bytes_remaining, file_offset);
            fflush(stdout);
#endif
            this_data += written;
            file_offset += written;
        }
    }
    /* We don't usually use this for each file write.  We usually do the file
     * flush as part of file close operation.
     */
#ifdef SUBFILE_REQUIRE_FLUSH
    fdatasync(fd);
#endif
    return ret;
} /* end sf_write_data() */

/*
 * ---------------------------------------------------
 * Topology discovery related functions for choosing
 * IO Concentrator (IOC) ranks.
 * Currently, the default approach for assigning an IOC
 * is select the lowest MPI rank on each node.
 *
 * The approach collectively generates N tuples
 * consisting of the MPI rank and hostid. This
 * collection is then sorted by hostid and scanned
 * to identify the IOC ranks.
 *
 * As time permits, addition assignment methods will
 * be implemented, e.g. 1-per-Nranks or via a config
 * option.  Additional selection methodologies can
 * be included as users get more experience using the
 * subfiling implementation.
 * ---------------------------------------------------
 */

/*-------------------------------------------------------------------------
 * Function:    compare_hostid
 *
 * Purpose:     qsort sorting function.
 *              Compares tuples of 'layout_t'. The sorting is based on
 *              the long hostid values.
 *
 * Return:      result of: (hostid1 > hostid2)
 *
 * Programmer:  Richard Warren
 *              7/17/2020
 *
 * Changes:     Initial Version/None.
 *
 *-------------------------------------------------------------------------
 */
static int
compare_hostid(const void *h1, const void *h2)
{
    const layout_t *host1 = (const layout_t *)h1;
    const layout_t *host2 = (const layout_t *)h2;
    return (host1->hostid > host2->hostid);
}

/*-------------------------------------------------------------------------
 * Function:    gather_topology_info
 *
 * Purpose:     Collectively generate a sorted collection of hostid+mpi_rank
 *              tuples.  The result is returned in the 'topology' field
 *              of the sf_topology_t structure.
 *
 * Return:      Sorted array of hostid/mpi_rank tuples.
 * Errors:      MPI_Abort if memory cannot be allocated.
 *
 * Programmer:  Richard Warren
 *              7/17/2020
 *
 * Changes:     Initial Version/None.
 *
 *-------------------------------------------------------------------------
 */
static void
gather_topology_info(sf_topology_t *info)
{
    int           sf_world_size;
    int           sf_world_rank;
    app_layout_t *app_layout = NULL;

    HDassert(info != NULL);
    HDassert((app_layout = info->app_layout) != NULL);

    sf_world_size = app_layout->world_size;
    sf_world_rank = app_layout->world_rank;

    if (app_layout->layout)
        return;

    if (1) {
        long     hostid = gethostid();
        layout_t my_hostinfo;
        app_layout->layout = (layout_t *)calloc((size_t)sf_world_size + 1, sizeof(layout_t));
        if (app_layout->layout == NULL) {
            perror("calloc failure!");
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        app_layout->hostid = hostid;

        my_hostinfo.rank                  = sf_world_rank;
        my_hostinfo.hostid                = hostid;
        app_layout->layout[sf_world_rank] = my_hostinfo;
        if (sf_world_size > 1) {
            if (MPI_Allgather(&my_hostinfo, 2, MPI_LONG, app_layout->layout, 2, MPI_LONG, MPI_COMM_WORLD) ==
                MPI_SUCCESS) {
                qsort(app_layout->layout, (size_t)sf_world_size, sizeof(layout_t), compare_hostid);
            }
        }
    }
} /* end gather_topology_info() */

/*-------------------------------------------------------------------------
 * Function:    count_nodes
 *
 * Purpose:     Initializes the sorted collection of hostid+mpi_rank
 *              tuples.  After initialization, the collection is scanned
 *              to determine the number of unique hostid entries.  This
 *              value will determine the number of actual IO concentrators
 *              that available to the application.  A side effect is to
 *              identify the 'node_index' of the current process.
 *
 * Return:      The number of unique hostid's (nodes).
 * Errors:      MPI_Abort if memory cannot be allocated.
 *
 * Programmer:  Richard Warren
 *              7/17/2020
 *
 * Changes:     Initial Version/None.
 *
 *-------------------------------------------------------------------------
 */
static int
count_nodes(sf_topology_t *info, int my_rank)
{
    int           k, node_count, hostid_index = -1;
    app_layout_t *app_layout = NULL;
    long          nextid;

    HDassert(info != NULL);
    HDassert((app_layout = info->app_layout) != NULL);

    if (app_layout->layout == NULL) {
        gather_topology_info(info);
    }

    if (app_layout->node_ranks == NULL) {
        app_layout->node_ranks = (int *)calloc((size_t)(app_layout->world_size + 1), sizeof(int));
    }

    HDassert(app_layout->node_ranks != NULL);

    nextid = app_layout->layout[0].hostid;
    /* Possibly record my hostid_index */
    if (app_layout->layout[0].rank == my_rank) {
        hostid_index = 0;
    }

    app_layout->node_ranks[0] = 0; /* Add index */
    node_count                = 1;

    /* Recall that the topology array has been sorted! */
    for (k = 1; k < app_layout->world_size; k++) {
        /* Possibly record my hostid_index */
        if (app_layout->layout[k].rank == my_rank)
            hostid_index = k;
        if (app_layout->layout[k].hostid != nextid) {
            nextid = app_layout->layout[k].hostid;
            /* Record the index of new hostid */
            app_layout->node_ranks[node_count++] = k;
        }
    }

    /* Mark the end of the node_ranks */
    app_layout->node_ranks[node_count] = app_layout->world_size;
    /* Save the index where we first located my hostid */
    app_layout->node_index        = hostid_index;
    return app_layout->node_count = node_count;
} /* end count_nodes() */

/*-------------------------------------------------------------------------
 * Function:    identify_ioc_ranks
 *
 * Purpose:     We've already identified the number of unique nodes and
 *              have a sorted list layout_t structures.  Under normal
 *              conditions, we only utilize a single IOC per node. Under
 *              that circumstance, we only need to fill the io_concentrator
 *              vector from the node_ranks array (which contains the index
 *              into the layout array of lowest MPI rank on each node) into
 *              the io_concentrator vector;
 *              Otherwise, while determining the number of local_peers per
 *              node, we can also select one or more additional IOCs.
 *
 *              As a side effect, we fill the 'ioc_concentrator' vector
 *              and set the 'rank_is_ioc' flag to TRUE if our rank is
 *              identified as owning an IO Concentrator (IOC).
 *
 *-------------------------------------------------------------------------
 */

static int
identify_ioc_ranks(int node_count, int iocs_per_node, sf_topology_t *info)
{
    int           n;
    int           total_ioc_count = 0;
    app_layout_t *app_layout      = NULL;
    HDassert(info != NULL);
    HDassert((app_layout = info->app_layout) != NULL);

    for (n = 0; n < node_count; n++) {
        int k;
        int node_index                           = app_layout->node_ranks[n];
        int local_peer_count                     = app_layout->node_ranks[n + 1] - app_layout->node_ranks[n];
        info->io_concentrator[total_ioc_count++] = (int)(app_layout->layout[node_index++].rank);

        if (app_layout->layout[node_index - 1].rank == app_layout->world_rank) {
            info->subfile_rank = total_ioc_count - 1;
            info->rank_is_ioc  = TRUE;
        }

        for (k = 1; k < iocs_per_node; k++) {
            if (k < local_peer_count) {
                if (app_layout->layout[node_index].rank == app_layout->world_rank) {
                    info->rank_is_ioc  = TRUE;
                    info->subfile_rank = total_ioc_count;
                }
                info->io_concentrator[total_ioc_count++] = (int)(app_layout->layout[node_index++].rank);
            }
        }
    }

    info->n_io_concentrators = total_ioc_count;
    return total_ioc_count;
} /* end identify_ioc_ranks() */

static inline void
assign_ioc_ranks(int *io_concentrator, int ioc_count, int rank_multiple, sf_topology_t *app_topology)
{
    app_layout_t *app_layout = NULL;
    /* Validate that the input pointers are not NULL */
    HDassert(io_concentrator);
    HDassert(app_topology);
    HDassert((app_layout = app_topology->app_layout) != NULL);
    /* fill the io_concentrator values based on the application layout */
    if (io_concentrator) {
        int k, ioc_next, ioc_index;
        for (k = 0, ioc_next = 0; ioc_next < ioc_count; ioc_next++) {
            ioc_index                 = rank_multiple * k++;
            io_concentrator[ioc_next] = (int)(app_layout->layout[ioc_index].rank);
            if (io_concentrator[ioc_next] == app_layout->world_rank)
                app_topology->rank_is_ioc = TRUE;
        }
        app_topology->n_io_concentrators = ioc_count;
    }
} /* end assign_ioc_ranks() */

/*-------------------------------------------------------------------------
 * Function:    fid_map_to_context
 *
 * Purpose:     This is a basic lookup function which returns the subfiling
 *              context id associated with the specified file->inode.
 *
 * Return:      The Subfiling context ID if it exists.
 * Errors:      H5I_INVALID_HID if the inode to context map is not found.
 *
 * Programmer:  Richard Warren
 *              7/17/2020
 *
 * Changes:     Initial Version/None.
 *
 *-------------------------------------------------------------------------
 */
hid_t
fid_map_to_context(uint64_t sf_fid)
{
    if (sf_open_file_map) {
        int i;
        for (i = 0; i < sf_file_map_size; i++) {
            hid_t sf_context_id = sf_open_file_map[i].sf_context_id;
            if (sf_open_file_map[i].h5_file_id == sf_fid) {
                return sf_context_id;
            }
        }
    }
    return H5I_INVALID_HID;
} /* end fid_map_to_context() */

/*-------------------------------------------------------------------------
 * Function:    clear_fid_map_entry
 *
 * Purpose:     Remove the map entry associated with the file->inode.
 *              This is done at file close.
 *
 * Return:      None
 * Errors:      Cannot fail.
 *
 * Programmer:  Richard Warren
 *              7/17/2020
 *
 * Changes:     Initial Version/None.
 *
 *-------------------------------------------------------------------------
 */
static void
clear_fid_map_entry(uint64_t sf_fid)
{
    if (sf_open_file_map) {
        int i;
        for (i = 0; i < sf_file_map_size; i++) {
            if (sf_open_file_map[i].h5_file_id == sf_fid) {
                sf_open_file_map[i].h5_file_id    = (uint64_t)H5I_INVALID_HID;
                sf_open_file_map[i].sf_context_id = 0;
                return;
            }
        }
    }
} /* end clear_fid_map_entry() */

/*-------------------------------------------------------------------------
 * Function:    active_map_entries
 *
 * Purpose:     Count the number of entries that have valid h5_file_id
 *              values.
 *
 * Return:      The number of active map entries (can be zero).
 * Errors:      Cannot fail.
 *
 * Programmer:  Richard Warren
 *              7/17/2020
 *
 * Changes:     Initial Version/None.
 *
 *-------------------------------------------------------------------------
 */
int
active_map_entries(void)
{
    int i, map_entries = 0;
    for (i = 0; i < sf_file_map_size; i++) {
        if (sf_open_file_map[i].h5_file_id != (uint64_t)H5I_INVALID_HID) {
            map_entries++;
        }
    }
    return map_entries;
} /* end active_map_entries() */

/*-------------------------------------------------------------------------
 * Function:    H5FD__determine_ioc_count
 *
 * Purpose:     Once a sorted collection of hostid/mpi_rank tuples has been
 *              created and the number of unique hostids (nodes) has
 *              been determined, we may modify this "default" value for
 *              the number of IO Concentrators for this application.
 *
 *              The default of one(1) IO concentrator per node can be
 *              changed (principally for testing) by environment variable.
 *              if IOC_COUNT_PER_NODE is defined, then that integer value
 *              is utilized as a mulitiplier to modify the set of
 *              IO Concentrator ranks.
 *
 *              The cached results will be replicated within the
 *              subfiling_context_t structure and is utilized as a map from
 *              io concentrator rank to MPI communicator rank for message
 *              sends and receives.
 *
 * Return:      The number of IO Concentrator ranks. We also cache
 *              the MPI ranks in the 'io_concentrator' vector variable.
 *              The length of this vector is cached as 'n_io_concentrators'.
 * Errors:      MPI_Abort if memory cannot be allocated.
 *
 * Programmer:  Richard Warren
 *              7/17/2020
 *
 * Changes:     - Initial Version/None.
 *              - Updated the API to allow a variety of methods for
 *                determining the number and MPI ranks that will have
 *                IO Concentrators.  The default approach will define
 *                a single IOC per node.
 *
 *-------------------------------------------------------------------------
 */
int
H5FD__determine_ioc_count(int world_size, int world_rank, ioc_selection_t ioc_select_method,
                          char *ioc_select_option, sf_topology_t **thisapp)
{
    int             ioc_count     = 0;
    ioc_selection_t ioc_selection = ioc_selection_options;
    sf_topology_t * app_topology  = NULL;

    HDassert(thisapp != NULL);

    if (!ioc_count || (ioc_selection != ioc_select_method)) {
        int   rank_multiple   = 0;
        int   iocs_per_node   = 1;
        char *envValue        = NULL;
        int * io_concentrator = NULL;

        if ((app_topology = *thisapp) == NULL) {
            app_topology = (sf_topology_t *)calloc(1, sizeof(sf_topology_t));
            HDassert(app_topology != NULL);
        }
        if (sf_app_layout == NULL) {
            sf_app_layout = (app_layout_t *)calloc(1, sizeof(app_layout_t));
            HDassert(sf_app_layout != NULL);
        }
        /* Once the application layout has been filled once, any additional
         * file open operations won't be required to gather that information.
         */
        app_topology->app_layout  = sf_app_layout;
        sf_app_layout->world_size = world_size;
        sf_app_layout->world_rank = world_rank;
        if (app_topology->io_concentrator == NULL) {
            app_topology->io_concentrator = io_concentrator =
                (int *)HDmalloc(((size_t)world_size * sizeof(int)));
        }
        assert(io_concentrator != NULL);
        app_topology->selection_type = ioc_selection = ioc_select_method;

        if (ioc_select_method == SELECT_IOC_WITH_CONFIG) {
            HDputs("SELECT_IOC_WITH_CONFIG: not supported yet...");
            ioc_select_method = SELECT_IOC_ONE_PER_NODE;
            goto next;
        }
        if (ioc_select_method == SELECT_IOC_TOTAL) {
            if (ioc_select_option) {
                int checkValue = atoi(ioc_select_option);
                if ((checkValue <= 0) || (checkValue >= world_size)) {
                    ioc_select_method = SELECT_IOC_ONE_PER_NODE;
                    goto next;
                }

                ioc_count     = checkValue;
                rank_multiple = (world_size / checkValue);
                assign_ioc_ranks(io_concentrator, ioc_count, rank_multiple, app_topology);
                *thisapp = app_topology;
            }
            else {
                HDputs("Missing option argument!");
                ioc_select_method = SELECT_IOC_ONE_PER_NODE;
                goto next;
            }
        }
        if (ioc_select_method == SELECT_IOC_EVERY_NTH_RANK) {
            /* This is similar to the previous method (SELECT_IOC_TOTAL)
             * in that the user chooses a rank multiple rather than an
             * absolute number of IO Concentrators.  Unlike the former,
             * we always start our selection with rank zero (0) and
             * the apply the stride to identify other IOCs.
             */
            if (ioc_select_option) {
                int checkValue = atoi(ioc_select_option);
                if (checkValue == 0) { /* Error */
                    ioc_select_method = SELECT_IOC_ONE_PER_NODE;
                    goto next;
                }
                rank_multiple = checkValue;
                ioc_count     = (world_size / rank_multiple);

                if ((world_size % rank_multiple) != 0) {
                    ioc_count++;
                }

                assign_ioc_ranks(io_concentrator, ioc_count, rank_multiple, app_topology);
                *thisapp = app_topology;
            }
            else {
                HDputs("Missing option argument!");
                ioc_select_method = SELECT_IOC_ONE_PER_NODE;
            }
        }

next:

        if (ioc_select_method == SELECT_IOC_ONE_PER_NODE) {
            app_topology->selection_type = ioc_select_method;
            ioc_count                    = count_nodes(app_topology, world_rank);

            if ((envValue = HDgetenv("H5_IOC_COUNT_PER_NODE")) != NULL) {
                int value_check = atoi(envValue);
                if (value_check > 0) {
                    iocs_per_node = value_check;
                }
            }
            ioc_count = identify_ioc_ranks(ioc_count, iocs_per_node, app_topology);
        }
        if (ioc_count > 0) {
            app_topology->n_io_concentrators = ioc_count;
            /* Create a vector of "potential" file descriptors
             * which can be indexed by the IOC id.
             */
            app_topology->subfile_fd = (int *)HDcalloc((size_t)ioc_count, sizeof(int));
            if (app_topology->subfile_fd == NULL) {
                HDputs("Failed to allocate vector of subfile fds");
            }
            *thisapp = app_topology;
        }
    }
    else {
        HDputs("Unable to create app_toplogy");
    }

    return ioc_count;
} /* end H5FD__determine_ioc_count() */

/*
-------------------------------------------------------------------------
  Programmer:  Richard Warren
  Purpose:     Return a character string which represents either the
               default selection method: SELECT_IOC_ONE_PER_NODE; or
               if the user has selected a method via the environment
               variable (H5_IOC_SELECTION_CRITERIA), we return that
               along with any optional qualifier with for that method.

  Errors:      None.

  Revision History -- Initial implementation
-------------------------------------------------------------------------
*/
char *
get_ioc_selection_criteria(ioc_selection_t *selection)
{
    char *optValue = NULL;
    char *envValue = HDgetenv("H5_IOC_SELECTION_CRITERIA");

    /* For non-default options, the environment variable
     * should have the following form:  integer:[integer|string]
     * In particular, EveryNthRank == 1:64 or every 64 ranks assign an IOC
     * or WithConfig == 2:/<full_path_to_config_file>
     */
    if (envValue && (optValue = strchr(envValue, ':'))) {
        *optValue++ = 0;
    }
    if (envValue) {
        int checkValue = atoi(envValue);
        if ((checkValue < 0) || (checkValue >= ioc_selection_options)) {
            *selection = SELECT_IOC_ONE_PER_NODE;
            return NULL;
        }
        else {
            *selection = (ioc_selection_t)checkValue;
            return optValue;
        }
    }
    *selection = SELECT_IOC_ONE_PER_NODE;
    return NULL;
} /* end get_ioc_selection_criteria() */

/*-------------------------------------------------------------------------
 * Function:    H5FD__init_subfile_context
 *
 * Purpose:     Called as part of the HDF5 file + subfiling opening.
 *              This initializes the subfiling context and associates
 *              this context with the specific HDF5 file.
 *
 * Return:      Success (0) or Faiure (-1)
 * Errors:      If MPI operations fail for some reason.
 *
 * Programmer:  Richard Warren
 *              7/17/2020
 *
 * Changes:     Initial Version/None.
 *-------------------------------------------------------------------------
 */
int
H5FD__init_subfile_context(sf_topology_t *thisApp, int n_iocs, int world_rank,
                           subfiling_context_t *newContext)
{
    MPI_Comm sf_msg_comm  = MPI_COMM_NULL;
    MPI_Comm sf_data_comm = MPI_COMM_NULL;

    assert(newContext != NULL);
    if (newContext->topology == NULL) {
        int   status;
        char *envValue = NULL;

        newContext->topology       = thisApp;
        newContext->sf_msg_comm    = MPI_COMM_NULL;
        newContext->sf_data_comm   = MPI_COMM_NULL;
        newContext->sf_group_comm  = MPI_COMM_NULL;
        newContext->sf_intercomm   = MPI_COMM_NULL;
        newContext->sf_stripe_size = H5FD_DEFAULT_STRIPE_DEPTH;
        newContext->sf_write_count = 0;
        newContext->sf_read_count  = 0;
        newContext->sf_eof         = 0;

        if ((envValue = HDgetenv("H5_IOC_STRIPE_SIZE")) != NULL) {
            long value_check = atol(envValue);
            if (value_check > 0) {
                newContext->sf_stripe_size = (int64_t)value_check;
            }
        }
        if ((envValue = HDgetenv("H5_IOC_SUBFILE_PREFIX")) != NULL) {
            char temp[PATH_MAX];
            sprintf(temp, "%s", envValue);
            newContext->subfile_prefix = strdup(temp);
            /* sf_subfile_prefix = strdup(temp); */
        }

        newContext->sf_blocksize_per_stripe = newContext->sf_stripe_size * n_iocs;
        if (sf_msg_comm == MPI_COMM_NULL) {
            status = MPI_Comm_dup(MPI_COMM_WORLD, &newContext->sf_msg_comm);
            if (status != MPI_SUCCESS)
                goto err_exit;
            status = MPI_Comm_set_errhandler(newContext->sf_msg_comm, MPI_ERRORS_RETURN);
            if (status != MPI_SUCCESS)
                goto err_exit;
            sf_msg_comm = newContext->sf_msg_comm;
        }
        if (sf_data_comm == MPI_COMM_NULL) {
            status = MPI_Comm_dup(MPI_COMM_WORLD, &newContext->sf_data_comm);
            if (status != MPI_SUCCESS)
                goto err_exit;
            status = MPI_Comm_set_errhandler(newContext->sf_data_comm, MPI_ERRORS_RETURN);
            if (status != MPI_SUCCESS)
                goto err_exit;
            sf_data_comm = newContext->sf_data_comm;
        }
        if (n_iocs > 1) {
            status =
                MPI_Comm_split(MPI_COMM_WORLD, thisApp->rank_is_ioc, world_rank, &newContext->sf_group_comm);

            if (status != MPI_SUCCESS)
                goto err_exit;
            status = MPI_Comm_size(newContext->sf_group_comm, &newContext->sf_group_size);
            if (status != MPI_SUCCESS)
                goto err_exit;
            status = MPI_Comm_rank(newContext->sf_group_comm, &newContext->sf_group_rank);
            if (status != MPI_SUCCESS)
                goto err_exit;
            /*
             * There may be additional functionality we need for the IOCs...
             * If so, then can probably initialize those things here!
             */
        }
        else {
            newContext->sf_group_size = 1;
            newContext->sf_group_rank = 0;
        }
    }
    return 0;

err_exit:
    return -1;
} /* end H5FD__init_subfile_context() */

/*
-------------------------------------------------------------------------
  Programmer:  Richard Warren
  Purpose:     Called as part of a file open operation, we initialize a
               subfiling context which includes the application topology
               along with other relevant info such as the MPI objects
               (communicators) for communicating with IO concentrators.
               We also identify which MPI ranks will have IOC threads
               started on them.

               We return a context ID via the 'sf_context' variable.

  Errors:      returns an error if we detect any initialization errors,
               including malloc failures or any resource allocation
               problems.

  Revision History -- Initial implementation
-------------------------------------------------------------------------
*/
herr_t
H5FDsubfiling_init(ioc_selection_t ioc_select_method, char *ioc_select_option, int64_t *sf_context)
{
    herr_t               ret_value = SUCCEED;
    int                  ioc_count;
    int                  world_rank, world_size;
    sf_topology_t *      thisApp    = NULL;
    int                  file_index = active_map_entries();
    int64_t              tag        = SF_CONTEXT;
    int64_t              context_id = ((tag << 32) | file_index);
    subfiling_context_t *newContext = (subfiling_context_t *)get__subfiling_object(context_id);
    char *               envValue   = NULL;

    FUNC_ENTER_API(FAIL)
    H5TRACE3("e", "IO*s*!", ioc_select_method, ioc_select_option, sf_context);

    if (MPI_Comm_size(MPI_COMM_WORLD, &world_size) != MPI_SUCCESS) {
        HDputs("MPI_Comm_size returned an error");
        ret_value = FAIL;
        goto done;
    }
    if (MPI_Comm_rank(MPI_COMM_WORLD, &world_rank) != MPI_SUCCESS) {
        HDputs("MPI_Comm_rank returned an error");
        ret_value = FAIL;
        goto done;
    }

    /* Compute the number an distribution map of the set of IO Concentrators */
    if ((ioc_count = H5FD__determine_ioc_count(world_size, world_rank, ioc_select_method, ioc_select_option,
                                               &thisApp)) <= 0) {
        HDputs("Unable to register subfiling topology!");
        ret_value = FAIL;
        goto done;
    }

    newContext->sf_context_id = context_id;

    /* Maybe set the verbose flag for more debugging info */
    envValue = HDgetenv("H5_SF_VERBOSE_FLAG");
    if (envValue != NULL) {
        int check_value = atoi(envValue);
        if (check_value > 0)
            sf_verbose_flag = 1;
    }

    /* Maybe open client-side log files */
    if (sf_verbose_flag) {
        manage_client_logfile(world_rank, sf_verbose_flag);
    }

    if (H5FD__init_subfile_context(thisApp, ioc_count, world_rank, newContext) != SUCCEED) {
        HDputs("Unable to initialize a subfiling context!");
        ret_value = FAIL;
        goto done;
    }

    if (context_id < 0) {
        ret_value = FAIL;
        goto done;
    }

    newContext->sf_base_addr = 0;
    if (newContext->topology->rank_is_ioc) {
        newContext->sf_base_addr = (int64_t)(newContext->topology->subfile_rank * newContext->sf_stripe_size);
    }
    *sf_context = context_id;

done:

    FUNC_LEAVE_API(ret_value)
    return ret_value;
} /* end H5FDsubfiling_init() */

/*-------------------------------------------------------------------------
 * Function:    Public/Client H5FD__open_subfiles
 *
 * Purpose:     Wrapper for the internal 'open__subfiles' function
 *              Similar to the other public wrapper functions, we
 *              discover (via the sf_context) the number of io concentrators
 *              and pass that to the internal function so that vector
 *              storage arrays can be stack based rather than explicitly
 *              allocated and freed.
 *
 *              The Internal function is resposible for sending all IOC
 *              instances, the (sub)file open requests.
 *
 *              Prior to calling the internal open function, we initialize
 *              a new subfiling context that contains topology info and
 *              new MPI communicators that facilitate messaging between
 *              HDF5 clients and the IOCs.
 *
 * Return:      Success (0) or Faiure (non-zero)
 * Errors:      If MPI operations fail for some reason.
 *
 * Programmer:  Richard Warren
 *              7/17/2020
 *
 * Changes:     Initial Version/None.
 *-------------------------------------------------------------------------
 */
int
H5FD__open_subfiles(void *_config_info, uint64_t h5_file_id, int flags)
{
    int                  status;
    int64_t              context_id = -1;
    subfiling_context_t *sf_context = NULL;
    ioc_selection_t      ioc_selection;
    // char filepath[PATH_MAX];
    // char *slash;
    config_common_t *config_info = _config_info;
    char *           option_arg  = get_ioc_selection_criteria(&ioc_selection);

    HDassert(config_info);
    /* Check to see who is calling ths function::
     * We only allow the ioc or subfiling VFDs
     */
    if ((config_info->magic != H5FD_IOC_FAPL_T_MAGIC) &&
        (config_info->magic != H5FD_SUBFILING_FAPL_T_MAGIC)) {
        HDputs("Unrecgonized driver!");
        return -1;
    }

    /* Initialize/identify IO Concentrators based on the
     * config information that we have...
     */
    status = H5FDsubfiling_init(ioc_selection, option_arg, &context_id);
    if (status != SUCCEED) {
        HDputs("H5FDsubfiling_init failed!");
        return -1;
    }

    /* For statistics gathering */
    maybe_initialize_statistics();

    /* Create a new context which is associated with
     * this file (context_id)
     */
    sf_context = get__subfiling_object(context_id);
    assert(sf_context != NULL);

    /* Save some basic things in the new context */
    config_info->context_id   = context_id;
    sf_context->sf_fid        = 0;
    sf_context->sf_context_id = context_id;
    sf_context->h5_file_id    = h5_file_id;
    sf_context->h5_filename   = strdup(config_info->file_path);
    sf_context->sf_filename   = NULL;
    /* Ensure that the IOC service won't exit
     * as we prepare to start up..
     */
    H5FD_ioc_set_shutdown_flag(0);

    /* If we're actually using the IOCs, we will
     * start the service threads on the identified
     * ranks as part of the subfile opening.
     */
    return open_subfile_with_context(sf_context, h5_file_id, flags);
}

/*-------------------------------------------------------------------------
 * Function:    Public/Client H5FD__close_subfiles
 *
 * Purpose:     This is a simple wrapper function for the internal version
 *              which actually manages all subfile closing via commands
 *              to the set of IO Concentrators.
 *
 * Return:      Success (0) or Faiure (non-zero)
 * Errors:      If MPI operations fail for some reason.
 *
 * Programmer:  Richard Warren
 *              7/17/2020
 *
 * Changes:     Initial Version/None.
 *-------------------------------------------------------------------------
 */
int
H5FD__close_subfiles(int64_t context_id)
{
    subfiling_context_t *sf_context = get__subfiling_object(context_id);
    assert(sf_context != NULL);
    return close__subfiles(sf_context, sf_context->h5_file_id);
}
