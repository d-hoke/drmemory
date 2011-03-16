/* **********************************************************
 * Copyright (c) 2007-2009 VMware, Inc.  All rights reserved.
 * **********************************************************/

/* Dr. Memory: the memory debugger
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; 
 * version 2.1 of the License, and no later version.

 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* Dr. Memory: the memory debugger
 * a.k.a. DRMemory == DynamoRIO Memory checker
 */

/* Features:
 * Reads of uninitialized memory
 *   Including register definedness tracking
 *   Including system call in/out params
 *   TODO: bit-level checking
 * Heap tracking:
 *   Access to unaddressable memory
 *     Redzones to catch overflow/underflow
 *   Access to freed memory: delay the actual free
 *   Double/invalid free
 *   Leaks
 *   TODO: app custom malloc interface
 * Access to un-reserved TLS slots
 */
/* TODO: Multiple threads:
 * 
 *   - Pathological races between mallocs and frees can result in Dr. Memory's
 *     shadow memory structures becoming mis-aligned and subsequent false
 *     positives.  However, such a scenario will always be preceded by either
 *     an invalid free error or a double free error.
 * 
 *   - General races between memory accesses and Dr. Memory's shadow memory
 *     can occur but errors will only occur with the presence of erroneous
 *     race conditions in the application.
 */

/***************************************************************************/

#include "dr_api.h"
#include "drmemory.h"
#include "readwrite.h"
#include "fastpath.h"
#include "report.h"
#include "shadow.h"
#include "syscall.h"
#include "alloc_drmem.h"
#include "alloc.h"
#include "heap.h"
#include "replace.h"
#include "leak.h"
#include "stack.h"
#include "perturb.h"
#include <stddef.h> /* for offsetof */

char logsubdir[MAXIMUM_PATH];
#ifndef USE_DRSYMS
file_t f_fork = INVALID_FILE;
#else
file_t f_results;
file_t f_suppress;
#endif
static uint num_threads;

#if defined(__DATE__) && defined(__TIME__)
static const char * const build_date = __DATE__ " " __TIME__;
#else
static const char * const build_date = "unknown";
#endif

/* store pages that contain known structures so we don't blanket-define them at init */
#define KNOWN_TABLE_HASH_BITS 8
static hashtable_t known_table;

static void
set_thread_initial_structures(void *drcontext);

client_id_t client_id;

/***************************************************************************
 * OPTIONS
 */

static void
drmem_options_init(const char *opstr)
{
    options_init(opstr);
    /* set globals */
    op_print_stderr = options.stderr && !options.quiet;
    op_verbose_level = options.verbose;
    op_pause_at_assert = options.pause_at_assert;
    op_pause_via_loop = options.pause_via_loop;
    op_ignore_asserts = options.ignore_asserts;
}

/***************************************************************************
 * DYNAMORIO EVENTS
 */

#ifdef WINDOWS
app_pc ntdll_base;
app_pc ntdll_end;
#else
app_pc libc_base;
app_pc libc_end;
static app_pc libdr_base, libdr_end;
static app_pc libdr2_base, libdr2_end;
static app_pc libdrmem_base, libdrmem_end;
#endif
app_pc app_base;
app_pc app_end;

#ifdef STATISTICS
/* statistics
 * FIXME: make per-thread to avoid races (or use locked inc)
 * may want some of these to be 64-bit.
 * some are now split off into stack.c
 */

uint num_nudges;

void 
dump_statistics(void)
{
    int i;
    dr_fprintf(f_global, "Statistics:\n");
    dr_fprintf(f_global, "nudges: %d\n", num_nudges);
    dr_fprintf(f_global, "adjust_esp:%10u slow; %10u fast\n", adjust_esp_executions,
               adjust_esp_fastpath);
    dr_fprintf(f_global, "slow_path invocations: %10u\n", slowpath_executions);
    dr_fprintf(f_global, "med_path invocations: %10u, fast: %10u\n",
               medpath_executions, movs4_med_fast);
    dr_fprintf(f_global, "movs4: src unalign: %10u, dst unalign: %10u, src undef: %10u\n",
               movs4_src_unaligned, movs4_dst_unaligned, movs4_src_undef);
    dr_fprintf(f_global, "reads:  slow: %8u, fast: %8u, fast4: %8u, total: %8u\n",
               read_slowpath, read_fastpath, read4_fastpath,
               read_slowpath+read_fastpath+read4_fastpath);
    dr_fprintf(f_global, "writes: slow: %8u, fast: %8u, fast4: %8u, total: %8u\n",
               write_slowpath, write_fastpath, write4_fastpath,
               write_slowpath+write_fastpath+write4_fastpath);
    dr_fprintf(f_global, "pushes: slow: %8u, fast: %8u, fast4: %8u, total: %8u\n",
               push_slowpath, push_fastpath, push4_fastpath,
               push_slowpath+push_fastpath+push4_fastpath);
    dr_fprintf(f_global, "pops:   slow: %8u, fast: %8u, fast4: %8u, total: %8u\n",
               pop_slowpath, pop_fastpath, pop4_fastpath,
               pop_slowpath+pop_fastpath+pop4_fastpath);
    dr_fprintf(f_global, "slow instead of fast: %8u, b/c unaligned: %8u, 8@border: %8u\n",
               slow_instead_of_fast, slowpath_unaligned, slowpath_8_at_border);
    dr_fprintf(f_global, "app instrs: fastpath: %7u, no dup: %7u, xl8: %7u\n",
               app_instrs_fastpath, app_instrs_no_dup, xl8_app_for_slowpath);
    dr_fprintf(f_global, "addr exceptions: header: %7u, tls: %5u, alloca: %5u\n",
               heap_header_exception, tls_exception, alloca_exception);
    dr_fprintf(f_global, "more addr exceptions: ld DR: %5u, cpp DR: %5u\n",
               loader_DRlib_exception, cppexcept_DRlib_exception);
    dr_fprintf(f_global, "addr con'td: strlen: %5u, strcpy: %5u, str/mem: %5u\n",
               strlen_exception, strcpy_exception, strmem_unaddr_exception);
    dr_fprintf(f_global, "def exceptions:  andor: %7u, rawmemchr: %5u, strrchr: %5u\n",
               andor_exception, rawmemchr_exception, strrchr_exception);
    dr_fprintf(f_global, "reg spills: dead:%8u, xchg:%8u, spill:%8u slow:%8u own:%8u\n",
               reg_dead, reg_xchg, reg_spill, reg_spill_slow, reg_spill_own);
    dr_fprintf(f_global, "bb reg spills: used %8u, unused %8u\n",
               reg_spill_used_in_bb, reg_spill_unused_in_bb);
    dr_fprintf(f_global, "post-call-site flushes:%8u\n",
               post_call_flushes);
    dr_fprintf(f_global, "shadow blocks allocated: %6u, freed: %6u\n",
               shadow_block_alloc, shadow_block_free);
    dr_fprintf(f_global, "special shadow blocks, unaddr: %6u, undef: %6u, def: %6u\n",
               num_special_unaddressable, num_special_undefined, num_special_defined);
    dr_fprintf(f_global, "faults writing to special shadow blocks: %6u\n",
               num_faults);
    dr_fprintf(f_global, "app mallocs: %8u, frees: %8u, large mallocs: %6u\n",
               num_mallocs, num_frees, num_large_mallocs);
    dr_fprintf(f_global, "unique malloc stacks: %8u\n", alloc_stack_count);
    dr_fprintf(f_global, "callstack fp scans: %8u\n", find_next_fp_scans);
    dr_fprintf(f_global, "symbol names truncated: %8u\n", symbol_names_truncated);
    dr_fprintf(f_global, "stack swaps: %8u, triggers: %8u\n",
               stack_swaps, stack_swap_triggers);
    dr_fprintf(f_global, "push addr tot: %8u heap: %6u mmap: %6u\n",
               push_addressable, push_addressable_heap, push_addressable_mmap);
    dr_fprintf(f_global, "delayed free bytes: %8u\n", delayed_free_bytes);
    dr_fprintf(f_global, "app heap regions: %8u\n", heap_regions);
    dr_fprintf(f_global, "addr checks elided: %8u\n", addressable_checks_elided);
    dr_fprintf(f_global, "aflags saved at top: %8u\n", aflags_saved_at_top);
    dr_fprintf(f_global, "xl8 sharing: %8u shared, %6u not:conflict, %6u not:disp-sz\n",
               xl8_shared, xl8_not_shared_reg_conflict, xl8_not_shared_disp_too_big);
    dr_fprintf(f_global,
               "\t%6u not:slowpaths, %6u not:unalign, %6u not:mem2mem, %6u not:offs\n",
               xl8_not_shared_slowpaths, xl8_not_shared_unaligned,
               xl8_not_shared_mem2mem, xl8_not_shared_offs);
    dr_fprintf(f_global, "\t%6u instrs slowpath, %6u count slowpath\n",
               xl8_shared_slowpath_instrs, xl8_shared_slowpath_count);
    dr_fprintf(f_global,
               "midchunk legit ptrs: %5u size, %5u new, %5u inheritance, %5u string\n",
               midchunk_postsize_ptrs, midchunk_postnew_ptrs,
               midchunk_postinheritance_ptrs, midchunk_string_ptrs);
    if (options.perturb) {
        perturb_dump_statistics();
    }
    if (options.leaks_only) {
        dr_fprintf(f_global, "zeroing loop aborts: %6u fault, %6u thresh\n",
                   zero_loop_aborts_fault, zero_loop_aborts_thresh);
    }

    heap_dump_stats(f_global);

    dr_fprintf(f_global, "\nSystem calls invoked:\n");
    for (i = 0; i < MAX_SYSNUM; i++) {
        if (syscall_invoked[i] > 0) {
            dr_fprintf(f_global, "\t%4u: %8u\n", i, syscall_invoked[i]);
        }
    }

    dr_fprintf(f_global, "\nPer-opcode slow path executions:\n");
    for (i = 0; i <= OP_LAST; i++) {
        if (slowpath_count[i] > 0) {
            dr_fprintf(f_global, "\t%3u %10s: %12"UINT64_FORMAT_CODE"\n", 
                       i, decode_opcode_name(i), slowpath_count[i]);
        }
    }
    dr_fprintf(f_global, "\nPer-size slow path executions:\n");
    dr_fprintf(f_global, "\t1-byte: %12"UINT64_FORMAT_CODE"\n", slowpath_sz1);
    dr_fprintf(f_global, "\t2-byte: %12"UINT64_FORMAT_CODE"\n", slowpath_sz2);
    dr_fprintf(f_global, "\t4-byte: %12"UINT64_FORMAT_CODE"\n", slowpath_sz4);
    dr_fprintf(f_global, "\t8-byte: %12"UINT64_FORMAT_CODE"\n", slowpath_sz8);
    dr_fprintf(f_global, "\tOther:  %12"UINT64_FORMAT_CODE"\n", slowpath_szOther);
}
#endif /* STATISTICS */

static void
close_file(file_t f)
{
    /* with DRi#357, DR now isolates log files so little to do here */
    dr_close_file(f);
}

#define dr_close_file DO_NOT_USE_dr_close_file

static void 
event_exit(void)
{
    LOGF(2, f_global, "in event_exit\n");

    check_reachability(true/*at exit*/);

    if (options.pause_at_exit)
        wait_for_user("pausing at exit");

    instrument_exit();

    if (options.perturb)
        perturb_exit();

    /* heap_region_exit() also checks for memory leaks,
     * so it must be called before shadow_exit() or alloc_exit().
     * FIXME: also report leaks from non-heap NtAllocateVirtualMemory?
     */
    heap_region_exit();

    syscall_exit();
    alloc_drmem_exit();
    if (options.shadowing)
        shadow_exit();
    hashtable_delete(&known_table);

#ifdef STATISTICS
    dump_statistics();
#endif

    if (!options.perturb_only)
        report_exit();
    utils_exit();

    /* To help postprocess.pl to perform sideline processing of errors, we add
     * a few markers to the log files.
     */
#ifndef USE_DRSYMS
    /* Note that if we exit before a child starts up, the child will
     * write to f_fork after we write LOG END.
     */
    dr_fprintf(f_fork, "LOG END\n");
    close_file(f_fork);
#else
    close_file(f_results);
    close_file(f_suppress);
#endif
    dr_fprintf(f_global, "LOG END\n");
    close_file(f_global);
}

static file_t
open_logfile(const char *name, bool pid_log, int which_thread)
{
    file_t f;
    char logname[MAXIMUM_PATH];
    int len;
    uint extra_flags = IF_LINUX_ELSE(DR_FILE_ALLOW_LARGE, 0);
    ASSERT(logsubdir[0] != '\0', "logsubdir not set up");
    if (pid_log) {
        len = dr_snprintf(logname, BUFFER_SIZE_ELEMENTS(logname),
                          "%s%c%s.%d.log", logsubdir, DIRSEP, name, dr_get_process_id());
    } else if (which_thread >= 0) {
        len = dr_snprintf(logname, BUFFER_SIZE_ELEMENTS(logname), 
                          "%s%c%s.%d.%d.log", logsubdir, DIRSEP, name,
                          which_thread, dr_get_thread_id(dr_get_current_drcontext()));
        /* have DR close on fork so we don't have to track and iterate */
        extra_flags |= DR_FILE_CLOSE_ON_FORK;
    } else {
        len = dr_snprintf(logname, BUFFER_SIZE_ELEMENTS(logname),
                          "%s%c%s", logsubdir, DIRSEP, name);
    }
    ASSERT(len > 0, "logfile name buffer max reached");
    NULL_TERMINATE_BUFFER(logname);
    f = dr_open_file(logname, DR_FILE_WRITE_OVERWRITE | extra_flags);
    ASSERT(f != INVALID_FILE, "unable to open log file");
    if (which_thread > 0) {
        void *drcontext = dr_get_current_drcontext();
        dr_log(drcontext, LOG_ALL, 1, 
               "DrMemory: log for thread %d is %s\n",
               dr_get_thread_id(drcontext), logname);
        NOTIFY("thread logfile is %s\n", logname);
    }
    return f;
}

static void
create_thread_logfile(void *drcontext)
{
    per_thread_t *pt = (per_thread_t *) dr_get_tls_field(drcontext);
    uint which_thread = atomic_add32_return_sum((volatile int *)&num_threads, 1) - 1;
    LOGF(0, f_global, "new thread #%d id=%d\n",
         which_thread, dr_get_thread_id(drcontext));

    if (!options.thread_logs) {
        pt->f = f_global;
    } else {
        /* we're going to dump our data to a per-thread file */
        pt->f = open_logfile("thread", false, which_thread/*tid suffix*/);
        LOGPT(1, pt, "thread logfile fd=%d\n", pt->f);
    }
}

static void 
event_thread_init(void *drcontext)
{
    per_thread_t *pt = thread_alloc(drcontext, sizeof(*pt), HEAPSTAT_MISC);
    client_per_thread_t *cpt = thread_alloc(drcontext, sizeof(*cpt), HEAPSTAT_MISC);
    memset(pt, 0, sizeof(*pt));
    memset(cpt, 0, sizeof(*cpt));
    pt->client_data = (void *) cpt;
    /* store it in the slot provided in the drcontext */
    dr_set_tls_field(drcontext, (void *)pt);
    create_thread_logfile(drcontext);
    LOGPT(2, pt, "in event_thread_init()\n");
    if (options.shadowing) {
        if (!options.leaks_only) {
            /* For 1st thread we can't get mcontext so we wait for 1st bb.
             * For subsequent we can.  Xref i#117/PR 395156.
             * FIXME: other threads injected or created early like
             * we've seen on Windows could mess this up.
             */
            static bool first_time = true;
            if (first_time) /* 1st thread: no lock needed */
                first_time = false;
            else
                set_thread_initial_structures(drcontext);
        }
        shadow_thread_init(drcontext);
    }
    syscall_thread_init(drcontext);
    report_thread_init(drcontext);
    if (options.perturb)
        perturb_thread_init();
}

static void 
event_thread_exit(void *drcontext)
{
    per_thread_t *pt = (per_thread_t *) dr_get_tls_field(drcontext);
    LOGPT(2, pt, "in event_thread_exit() %d\n", dr_get_thread_id(drcontext));
    if (options.perturb)
        perturb_thread_exit();
    report_thread_exit(drcontext);
    if (options.thread_logs) {
        dr_fprintf(pt->f, "LOG END\n");
        close_file(pt->f);
    }
#ifdef WINDOWS
    if (!options.leaks_only && options.shadowing) {
        /* the kernel de-allocs teb so we need to explicitly handle it */
        TEB *teb = get_TEB_from_tid(dr_get_thread_id(drcontext));
        ASSERT(teb != NULL, "invalid param");
        shadow_set_range((app_pc)teb, (app_pc)teb + sizeof(*teb), SHADOW_UNADDRESSABLE);
    }

    while (pt->prev != NULL)
        pt = pt->prev;
    while (pt != NULL) {
        per_thread_t *tmp = pt;
        LOG(2, "freeing per_thread_t "PFX"\n", tmp);
        syscall_thread_exit(drcontext, tmp);
        thread_free(drcontext, pt->client_data,
                    sizeof(client_per_thread_t), HEAPSTAT_MISC);
        pt = pt->next;
        thread_free(drcontext, tmp, sizeof(*tmp), HEAPSTAT_MISC);
    }
#else
    syscall_thread_exit(drcontext, pt);
    thread_free(drcontext, pt->client_data, sizeof(client_per_thread_t), HEAPSTAT_MISC);
    thread_free(drcontext, pt, sizeof(*pt), HEAPSTAT_MISC);
#endif
    /* with PR 536058 we do have dcontext in exit event so indicate explicitly
     * that we've cleaned up the per-thread data
     */
    dr_set_tls_field(drcontext, NULL);
}

static dr_emit_flags_t
event_basic_block(void *drcontext, void *tag, instrlist_t *bb,
                  bool for_trace, bool translating)
{
    IF_DEBUG(per_thread_t *pt = (per_thread_t *)
             dr_get_tls_field(dr_get_current_drcontext()));
    LOGPT(2, pt, "in event_basic_block(tag="PFX")%s%s\n", tag,
          for_trace ? " for trace" : "",
          translating ? " translating" : "");
    return instrument_bb(drcontext, tag, bb, for_trace, translating);
}

#ifdef LINUX
bool
is_in_client_or_DR_lib(app_pc pc)
{
    return ((pc >= libdr_base && pc < libdr_end) ||
            (pc >= libdr2_base && pc < libdr2_end) ||
            (pc >= libdrmem_base && pc < libdrmem_end) ||
            (pc >= syscall_auxlib_start() && pc < syscall_auxlib_end()));
}
#endif

/* It's ok for start to not be the allocation base but then probably
 * best to use 0 for size since start+size will be a limit if size != 0.
 * Pass 0 for size to walk an entire module or allocation region.
 */
byte *
mmap_walk(app_pc start, size_t size,
          IF_WINDOWS_(MEMORY_BASIC_INFORMATION *mbi_start) bool add)
{
#ifdef WINDOWS
    app_pc start_base;
    app_pc pc = start;
    MEMORY_BASIC_INFORMATION mbi = {0};
    app_pc map_base = mbi.AllocationBase;
    app_pc map_end = (byte *)mbi.AllocationBase + mbi.RegionSize;
    ASSERT(!options.leaks_only && options.shadowing, "shadowing disabled");
    if (mbi_start == NULL) {
        if (dr_virtual_query(start, &mbi, sizeof(mbi)) != sizeof(mbi)) {
            ASSERT(false, "error walking initial memory mappings");
            return pc; /* FIXME: return error code */
        }
    } else
        mbi = *mbi_start;
    if (mbi.State == MEM_FREE)
        return pc;
    map_base = mbi.AllocationBase;
    start_base = map_base;
    map_end = (byte *)mbi.AllocationBase + mbi.RegionSize;
    LOG(2, "mmap_walk %s "PFX": alloc base is "PFX"\n", add ? "add" : "remove",
         start, start_base);
    if (mbi.State == MEM_RESERVE)
        map_end = map_base;
    if (pc + mbi.RegionSize < pc)
        return NULL;
    pc += mbi.RegionSize;
    while (dr_virtual_query(pc, &mbi, sizeof(mbi)) == sizeof(mbi) &&
           mbi.AllocationBase == start_base /*not map_base: we skip reserved pieces*/ &&
           (size == 0 || pc < start+size)) {
        ASSERT(mbi.State != MEM_FREE, "memory walk error");
        if (mbi.State == MEM_RESERVE) {
            /* set up until pc, then start accumulating again after it 
             * unlike Linux /proc/pid/maps the .bss is embedded inside the IMAGE region
             */
            LOG(2, "\tpiece "PFX"-"PFX"\n", map_base, map_end);
            shadow_set_range(map_base, map_end,
                             add ? SHADOW_DEFINED : SHADOW_UNADDRESSABLE);
            map_base = map_end + mbi.RegionSize;
            map_end = map_base;
        } else {
            /* best to batch adjacent committed image/data regions together */
            map_end += mbi.RegionSize;
        }
        if (pc + mbi.RegionSize < pc)
            return NULL;
        pc += mbi.RegionSize;
    }
    if (map_end > map_base) {
        shadow_set_range(map_base, map_end, add ? SHADOW_DEFINED : SHADOW_UNADDRESSABLE);
        LOG(2, "\tpiece "PFX"-"PFX"\n", map_base, map_end);
    }
    return pc;
#else /* WINDOWS */
    dr_mem_info_t info;
    module_data_t *data;
    app_pc pc, module_start, module_end;
    bool image;
    ASSERT(!options.leaks_only && options.shadowing, "shadowing disabled");

    /* we assume that only a module will have multiple pieces with different prots */
    data = dr_lookup_module(start);
    if (data != NULL) {
        image = true;
        module_start = data->start;
        module_end = data->end;
    } else {
        image = false;
        module_start = start;
        module_end = start + PAGE_SIZE;
    }
    dr_free_module_data(data);

    LOG(2, "mmap_walk %s "PFX"\n", add ? "add" : "remove", start);
    pc = start;
    /* Be wary of SIGBUS if we read into .bss before it's set up: PR 528744 */
    while (pc < module_end && (size == 0 || pc < start+size)) {
        if (!dr_query_memory_ex(pc, &info)) {
            /* failed: bail */
            break;
        }
        ASSERT(pc >= info.base_pc && pc < info.base_pc + info.size, "mem query error");
#ifdef DEBUG
        /* can be data region that has a piece unmapped later, or can have
         * mmaps merged together (PR 475114)
         */
        if (add && pc != info.base_pc) {
            LOG(1, "WARNING: mmap_walk "PFX": subsection "PFX" != query "PFX"\n",
                start, pc, info.base_pc);
        }
#endif
        ASSERT(ALIGNED(pc, PAGE_SIZE), "memory iterator not page aligned");
        if (info.prot == DR_MEMPROT_NONE) {
            /* A hole in a module, probably due to alignment */
            if (!add) /* just in case */
                shadow_set_range(pc, pc+info.size, SHADOW_UNADDRESSABLE);
            pc = info.base_pc + info.size;
            continue;
        }
        LOG(2, "\tmmap piece "PFX"-"PFX" prot=%x\n", pc, pc+info.size, info.prot);
        shadow_set_range(pc, info.base_pc + info.size,
                         add ? SHADOW_DEFINED : SHADOW_UNADDRESSABLE);
        pc = info.base_pc + info.size;
    }
    return pc;
#endif /* WINDOWS */
}

static void
memory_walk(void)
{
#ifdef WINDOWS
    TEB *teb = get_TEB();
    app_pc pc = NULL;
    MEMORY_BASIC_INFORMATION mbi;
    uint type = 0;
    LOG(2, "walking memory looking for images\n");
    /* Strategy: walk through every block in memory 
     */
    while (dr_virtual_query(pc, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        ASSERT(pc == mbi.BaseAddress, "memory walk error");
        /* Memory mapped data or image is always addressable and defined.
         * We consider heap to be unaddressable until fine-grained allocated.
         */
        if (mbi.State != MEM_FREE) {
            LOG(3, "mem walk: "PFX"-"PFX"\n", 
                mbi.BaseAddress, (app_pc)mbi.BaseAddress+mbi.RegionSize);
            if (mbi.Type == MEM_IMAGE || mbi.Type == MEM_MAPPED) {
                ASSERT(mbi.BaseAddress == mbi.AllocationBase, "memory walk error");
                /* Due to PE section alignment there can be unaddressable regions
                 * inside a mapped file.  Optimization: can we always
                 * avoid the walk for MEM_MAPPED?
                 */
                pc = mmap_walk(pc, 0, &mbi, true/*add*/);
                if (pc == NULL)
                    break;
                else
                    continue;
            } else if (mbi.State == MEM_COMMIT) {
                if ((app_pc)mbi.BaseAddress !=
                    (app_pc)teb->StackLimit - PAGE_SIZE/*guard page*/ &&
                    mbi.BaseAddress != teb->StackLimit &&
                    !is_in_heap_region(mbi.AllocationBase) &&
                    !dr_memory_is_dr_internal(mbi.AllocationBase) &&
                    !dr_memory_is_in_client(mbi.AllocationBase) &&
                    /* Avoid teb, peb, env, etc. pages where we have finer-grained
                     * information.  We assume it's sufficient to look only at
                     * the start of the region.
                     */
                    hashtable_lookup(&known_table, (void*)PAGE_START(pc)) == NULL) {
                    /* what can we do?  we assume it's all defined */
                    LOG(2, "initial unknown committed region "PFX"-"PFX"\n",
                         mbi.BaseAddress, (app_pc)mbi.BaseAddress+mbi.RegionSize);
                    /* FIXME PR 406328: mark no-access regions as unaddressable,
                     * and watch mprotect so can shift back and forth
                     */
                    shadow_set_range((app_pc)mbi.BaseAddress,
                                     (app_pc)mbi.BaseAddress+mbi.RegionSize,
                                     SHADOW_DEFINED);
                    /* The heap walk should have added any mmapped chunks so
                     * we assume we don't need to add this to the malloc table
                     * and it will never have free() called on it.
                     */
                }
            }
        }
        if (pc + mbi.RegionSize < pc)
            break;
        pc += mbi.RegionSize;
    }
#else /* WINDOWS */
    /* Full memory walk should cover module innards.
     * If not we could do module iterator plus mmap_walk().
     */
    dr_mem_info_t info;
    app_pc pc = NULL, end;
    app_pc cur_brk = get_brk();
    app_pc pc_to_add;
    uint type = 0;
    LOG(2, "brk="PFX"\n", cur_brk);
    while (pc < (app_pc)POINTER_MAX && dr_query_memory_ex(pc, &info)) {
        end = info.base_pc+info.size;
        pc_to_add = NULL;
        LOG(2, "  query "PFX"-"PFX" prot=%x type=%d\n",
            info.base_pc, end, info.prot, info.type);
        ASSERT(pc == info.base_pc ||
               /* PR 424012: DR memory regions change via commit-on-demand */
               dr_memory_is_dr_internal(info.base_pc), "memory iterator mismatch");
        ASSERT(ALIGNED(pc, PAGE_SIZE), "memory iterator not page aligned");
        if (dr_memory_is_in_client(info.base_pc)) {
            /* leave client memory unaddressable */
            LOG(2, "  => client memory\n");

            /* FIXME: Currently dr_memory_is_in_client() returns true only if 
             * the address argument is inside a client library, but that can
             * change in future and any memory (library, bss, mmaps, heap
             * alloc) used by a client can make it return true.  In that case
             * the fix is to identify if the address in question is inside a
             * module or not and then decide whether it should be shadowed.
             * This can be done by using a linker defined variable to find a
             * section address and then computing the client library base from
             * that.
             * FIXME: If there are multiple client libraries, Derek, was
             * concerned about marking a whole library as shadowable region
             * because pc was in the first library.  I don't that will happen
             * because page protections will change between bss & image, thus
             * mmap regions of two different client libraries won't get merged.
             * Still check when doing multiple clients.
             */
            /* PR 483720: app memory merged to the end of drmem's bss. */
            if (info.base_pc >= libdrmem_base && info.base_pc < libdrmem_end &&
                end > libdrmem_end) {
                LOG(2, "  Dr. Memory library memory ends @ "PFX", merged by kernel\n",
                    libdrmem_end);
                pc_to_add = libdrmem_end;
            } else if (info.base_pc >= syscall_auxlib_start() &&
                       info.base_pc < syscall_auxlib_end() &&
                       end > syscall_auxlib_end()) {
                LOG(2, "  Dr. Memory aux library memory ends @ "PFX", merged by kernel\n",
                    syscall_auxlib_end());
                pc_to_add = syscall_auxlib_end();
            }
        } else if (dr_memory_is_dr_internal(info.base_pc)) {
            /* ignore DR memory: leave unaddressable */
            LOG(2, "  => DR memory\n");
            /* PR 447413: split off memory merged on end of DR lib */
            if (info.base_pc >= libdr_base && info.base_pc < libdr_end &&
                end > libdr_end) {
                LOG(2, "  DR memory ends @ "PFX", merged by kernel\n", libdr_end);
                pc_to_add = libdr_end;
            } else if (info.base_pc >= libdr2_base && info.base_pc < libdr2_end &&
                end > libdr2_end) {
                LOG(2, "  DR2 memory ends @ "PFX", merged by kernel\n", libdr2_end);
                pc_to_add = libdr2_end;
            }
        } else if (info.type == DR_MEMTYPE_DATA) {
            if (end == cur_brk) {
                /* this is the heap */
                LOG(2, "  => heap\n");
                /* we call heap_region_add in heap_iter_region from heap_walk  */
            } else if (hashtable_lookup(&known_table, (void*)PAGE_START(pc)) != NULL) {
                /* we assume there's only one entry in the known_table:
                 * the initial stack 
                 */
                LOG(2, "  => stack\n");
            } else {
                /* FIXME: how can we know whether this is a large heap alloc?
                 * For small apps there are none at startup, but large apps
                 * might create some in their lib init prior to DR init.
                 */
                pc_to_add = pc;
                /* FIXME PR 406328: mark no-access regions as unaddressable,
                 * and watch mprotect so can shift back and forth
                 */
                type = SHADOW_DEFINED;
            }
        } else if (info.type == DR_MEMTYPE_IMAGE) {
            pc_to_add = pc;
            type = SHADOW_DEFINED;
            /* workaround for PR 618178 where /proc/maps is wrong on suse
             * and lists last 2 pages of executable as heap!
             */
            if (pc == app_base && end < app_end) {
                LOG(1, "WARNING: workaround for invalid executable end "PFX" => "PFX"\n",
                    end, app_end);
                end = app_end;
            }
        }

        if (pc_to_add != NULL && !options.leaks_only && options.shadowing) {
            LOG(2, "\tpre-existing region "PFX"-"PFX" prot=%x type=%d\n",
                pc_to_add, end, info.prot, info.type);
            /* FIXME PR 406328: if no-access should we leave as unaddressable?
             * would need to watch mprotect.  but without doing so, we won't
             * complain prior to an access that crashes!
             */
            shadow_set_range(pc_to_add, end, type);
        }
        
        if (pc + info.size < pc) /* overflow */
            break;
        pc += info.size;
    }
#endif /* WINDOWS */
}

static void
set_known_range(app_pc start, app_pc end)
{
    app_pc pc;
    for (pc = (app_pc)PAGE_START(start); pc <= (app_pc)PAGE_START(end);
         pc += PAGE_SIZE) {
        hashtable_add(&known_table, (void*)pc, (void*)pc);
    }
}

static void
set_initial_range(app_pc start, app_pc end)
{
    set_known_range(start, end);
    shadow_set_range(start, end, SHADOW_DEFINED);
}

#ifdef WINDOWS
static void
set_initial_unicode_string(UNICODE_STRING *us)
{
    /* Length field is size in bytes not counting final 0 */
    if (us->Buffer != NULL) {
        set_initial_range((app_pc)us->Buffer,
                          (app_pc)(us->Buffer)+us->Length+sizeof(wchar_t));
        shadow_set_range((app_pc)(us->Buffer)+us->Length+sizeof(wchar_t),
                         ((app_pc)(us->Buffer)+us->MaximumLength),
                         SHADOW_UNDEFINED);
    }
}
#endif

#ifdef WINDOWS
void
set_teb_initial_shadow(TEB *teb)
{
    ASSERT(teb != NULL, "invalid param");
    set_initial_range((app_pc)teb, (app_pc)teb + offsetof(TEB, TlsSlots));
    /* FIXME: ideally we would know which fields were added in which windows
     * versions, and only define as far as the current version.
     * FIXME: each subsequent version adds new fields, so should we just say
     * the whole page is defined?!?
     */
    set_initial_range((app_pc)teb + offsetof(TEB, TlsLinks),
                      (app_pc)teb + sizeof(*teb));
}
#endif

/* Called for 1st thread at 1st bb (b/c can't get mcontext at thread init:
 * i#117/PR 395156) and later threads from thread init event.
 */
static void
set_thread_initial_structures(void *drcontext)
{
#ifdef WINDOWS
    dr_mcontext_t mc;
    byte *stack_reserve;
    size_t stack_reserve_sz;
    IF_DEBUG(bool ok;)
    TEB *teb = get_TEB();
    /* FIXME: we currently assume the whole TEB except the 64 tls slots are
     * defined, b/c we're not in early enough to watch the others.
     */
    /* For non-primary threads we typically add the TEB in post-NtCreateThread,
     * but for remotely created threads we need to process here as well.
     */
    LOG(2, "setting initial structures for thread w/ TEB "PFX"\n", teb);
    set_teb_initial_shadow(teb);

    if (is_wow64_process()) {
        /* Add unknown wow64-only structure TEB->0xf70->0x14d0
         * For wow64, the TEB is not a single-page alloc region but the 3rd
         * page, and TEB->GdiBatchCount points at the first page.  It is further
         * de-refed here:
         *   UNADDRESSABLE ACCESS: pc @0x7d4e7d7b reading 0x7efdc4d0-0x7efdc4d4 4 bytes
         * Everyone seems to agree that TEB offset 0xf70 is just a dword/uint
         * GdiBatchCount: yet clearly it's something else for wow64:
         *   kernel32!BasepReport32bitAppLaunching:
         *   7d4e7d75 8b80700f0000     mov     eax,[eax+0xf70]
         *   7d4e7d7b 8b80d0140000     mov     eax,[eax+0x14d0]
         */
        app_pc ref1 = (app_pc) teb->GdiBatchCount;
        if (ref1 != NULL &&
            PAGE_START(ref1) == PAGE_START(teb) - 2*PAGE_SIZE) {
            /* I used to only allow the +14d0-+14d4 but in other apps I see
             * many refs.  Since we don't have the data structs we allow both
             * pages.  FIXME: limit to system libraries and not app code.
             */
            ASSERT(dr_memory_is_readable((app_pc)PAGE_START(ref1),
                                         PAGE_SIZE*2),
                   "wow64 pre-teb assumptions wrong");
            set_initial_range((app_pc)PAGE_START(ref1), (app_pc)PAGE_START(teb));
        }
    }

    /* PR 408521: for other injection types we need to get cur esp so we
     * can set base part of stack for primary thread.
     * For drinject, stack is clean, except on Vista where a few words
     * are above esp.
     * Note that this is the start esp: the APC esp is handled in
     * client_handle_Ki.
     */
    IF_DEBUG(ok = )
        dr_get_mcontext(drcontext, &mc, NULL);
    ASSERT(ok, "unable to get mcontext for thread");
    ASSERT(mc.xsp <= (reg_t)teb->StackBase && mc.xsp > (reg_t)teb->StackLimit,
           "initial xsp for thread invalid");
    /* Even for XP+ where csrss frees the stack, the stack alloc is in-process
     * and we see it and mark defined since a non-heap alloc.
     * Thus we must mark unaddr explicitly here.
     * For Vista+ where NtCreateThreadEx is used, the kernel creates the stack,
     * and we set its shadow values here.
     */
    stack_reserve_sz = allocation_size((byte *)mc.xsp, &stack_reserve);
    LOG(1, "thread initial stack: "PFX"-"PFX"-"PFX", TOS="PFX"\n",
        stack_reserve, teb->StackLimit, teb->StackBase, mc.xsp);
    ASSERT(stack_reserve <= (byte *)teb->StackLimit, "invalid stack reserve");
    ASSERT(stack_reserve + stack_reserve_sz == teb->StackBase ||
           stack_reserve + stack_reserve_sz ==
           ((byte *)teb->StackBase) + PAGE_SIZE/*guard page*/,
           "abnormal initial thread stack");
    shadow_set_range(stack_reserve, (byte *)mc.xsp, SHADOW_UNADDRESSABLE);
    set_initial_range((byte *)mc.xsp, (byte *)teb->StackBase);
#else /* WINDOWS */
    /* Anything to do here?  most per-thread user address space structures
     * will be written by user-space code, which we will observe.
     * This includes a thread's stack: we'll see the mmap to allocate,
     * and we'll see the initial function's argument written to the stack, etc.
     * We do want to change the stack from defined to unaddressable,
     * but we originally couldn't get the stack bounds here (xref PR
     * 395156) so we instead watch the arg to SYS_clone.
     */
#endif /* WINDOWS */
}

/* We can't get app xsp at init time (i#117) so we call this on 1st bb */
static void
set_initial_structures(void *drcontext)
{
#ifdef WINDOWS
    app_pc pc;
    TEB *teb = get_TEB();
    /* We can't use teb->ProcessEnvironmentBlock b/c i#249 points it at private PEB */
    PEB *peb = get_app_PEB();
    RTL_USER_PROCESS_PARAMETERS *pparam = peb->ProcessParameters;
    /* Mark the PEB structs we know about defined
     * FIXME: should we go to the end of the page to cover unknown fields,
     * at the risk of false negatives?
     */
    LOG(1, "app PEB is "PFX"-"PFX"\n", peb, (app_pc)(peb) + sizeof(PEB));
    set_initial_range((app_pc)peb, (app_pc)(peb) + sizeof(PEB));
    /* We try to catch TLS usage errors by not marking the expansion slots
     * (per-thread, teb->TlsExpansionSlots) addressable until allocated
     */
    /* Tls*Bitmap is usually in ntdll .data but we make sure it's defined */
    set_initial_range((app_pc)peb->TlsBitmap->Buffer,
                      (app_pc)peb->TlsBitmap->Buffer +
                      peb->TlsBitmap->SizeOfBitMap/sizeof(byte));
    if (peb->TlsExpansionBitmap != NULL) {
        set_initial_range((app_pc)peb->TlsExpansionBitmap->Buffer,
                         (app_pc)peb->TlsExpansionBitmap->Buffer +
                         peb->TlsExpansionBitmap->SizeOfBitMap/sizeof(byte));
    }
    set_initial_unicode_string(&peb->CSDVersion);
    set_initial_range((app_pc)pparam, (app_pc)pparam +
                      sizeof(RTL_USER_PROCESS_PARAMETERS));
    set_initial_unicode_string(&pparam->CurrentDirectoryPath);
    set_initial_unicode_string(&pparam->DllPath);
    set_initial_unicode_string(&pparam->ImagePathName);
    set_initial_unicode_string(&pparam->CommandLine);
    set_initial_unicode_string(&pparam->WindowTitle);
    set_initial_unicode_string(&pparam->DesktopName);
    set_initial_unicode_string(&pparam->ShellInfo);
    set_initial_unicode_string(&pparam->RuntimeData);

    /* find end of unicode strings: 2 zero wchars */
    pc = (app_pc) pparam->Environment;
    while (*(uint*)pc != 0) {
        ASSERT(pc - (app_pc)pparam->Environment < 64*1024, "env var block too long");
        pc++;
    }
    set_initial_range((app_pc)(pparam->Environment), pc+sizeof(uint)+1/*open-ended*/);

    pc = vsyscall_pc(drcontext, (byte *)
                     dr_get_proc_address(ntdll_base, "NtMapViewOfSection"));
    if (pc != NULL) {
        set_initial_range(pc, pc + VSYSCALL_SIZE);
        /* assumption: KUSER_SHARED_DATA is at start of vsyscall page */
        pc = (app_pc) PAGE_START(pc);
        ASSERT(pc == (app_pc) KUSER_SHARED_DATA_START,
               "vsyscall/KUSER_SHARED_DATA not where expected");
        set_initial_range(pc, pc + sizeof(KUSER_SHARED_DATA));
    } else {
        /* not using vsyscall page so either int or wow64.  go ahead and
         * use hardcoded address.
         */
        pc = (app_pc) KUSER_SHARED_DATA_START;
        set_initial_range(pc, pc + sizeof(KUSER_SHARED_DATA));
    }
#else /* WINDOWS */
    /* We can't get app xsp at init time (i#117) so we call this on 1st bb 
     * For subsequent threads we do this when handling the clone syscall
     */
    dr_mcontext_t mc;
    app_pc stack_base;
    size_t stack_size;
    dr_get_mcontext(drcontext, &mc, NULL);
    if (dr_query_memory((app_pc)mc.xsp, &stack_base, &stack_size, NULL)) {
        LOG(1, "initial stack is "PFX"-"PFX", sp="PFX"\n",
            stack_base, stack_base + stack_size, mc.xsp);
        set_known_range(stack_base, (app_pc)mc.xsp);
        set_initial_range((app_pc)mc.xsp, stack_base + stack_size);
        /* rest is unaddressable by default, and memory walk skips known range */
    } else {
        ASSERT(false, "can't determine initial stack region");
    }
    /* FIXME: vdso, if not covered by memory_walk() */
#endif /* WINDOWS */

    set_thread_initial_structures(drcontext);
}

static void
heap_iter_region(app_pc start, app_pc end _IF_WINDOWS(HANDLE heap))
{
    if (options.track_heap) {
        heap_region_add(start, end, true/*arena*/, 0);
        IF_WINDOWS(heap_region_set_heap(start, heap);)
    } else if (!options.leaks_only && options.shadowing)
        shadow_set_range(start, end, SHADOW_UNDEFINED);
}

static void
heap_iter_chunk(app_pc start, app_pc end)
{
    if (!options.leaks_only && options.shadowing)
        shadow_set_range(start, end, SHADOW_DEFINED);
    /* We track mallocs even if not counting leaks in order to
     * handle failed frees properly
     */
    /* We don't have the asked-for size so we use real end for both */
    malloc_add(start, end, end, true/*pre_us*/, 0, NULL, NULL);
}

/* Walks the heap blocks that are already allocated at client init time,
 * to determine addressability.
 * FIXME: how know definedness?
 */
static void
heap_walk(void)
{
    heap_iterator(heap_iter_region, heap_iter_chunk);
}

/* We wait to call this until 1st bb so we know stack pointer
 * (can't get mcontext at init or thread_init time: i#117)
 */
void
set_initial_layout(void)
{
    /* must do heap walk and initial structures walk before memory walk
     * so we do not blanket-define pages with known structures.
     * on linux, though, there's only one heap and we need the memory
     * walk to find it.
     */
#ifdef WINDOWS
    if (options.leaks_only || options.shadowing)
        heap_walk();
    if (!options.leaks_only && options.shadowing) {
        set_initial_structures(dr_get_current_drcontext());
        memory_walk();
    }
#else
    if (!options.leaks_only && options.shadowing) {
        /* identify stack before memory walk */
        set_initial_structures(dr_get_current_drcontext());
    }
    if (options.leaks_only || options.shadowing) {
        /* leaks_only still needs memory_walk to find heap base */
        memory_walk();
        heap_walk();
    }
#endif
}

static void
print_version(file_t f)
{
    dr_fprintf(f, "Dr. Memory version %s build %d built on %s\n",
               VERSION_STRING, BUILD_NUMBER, build_date);
}

/* also initializes logsubdir */
static void
create_global_logfile(void)
{
    uint count = 0;
    const char *appnm = dr_get_application_name();
    const uint LOGDIR_TRY_MAX = 1000;
    /* PR 408644: pick a new subdir inside base logdir */
    /* PR 453867: logdir must have pid in its name */
    do {
        dr_snprintf(logsubdir, BUFFER_SIZE_ELEMENTS(logsubdir), 
                    "%s%cDrMemory-%s.%d.%03d",
                    options.logdir, DIRSEP, appnm == NULL ? "null" : appnm,
                    dr_get_process_id(), count);
        NULL_TERMINATE_BUFFER(logsubdir);
        /* FIXME PR 514092: if the base logdir is unwritable, we shouldn't loop
         * UINT_MAX times: it looks like we've hung.
         * Unfortuantely dr_directory_exists() is Windows-only and
         * dr_create_dir returns only a bool, so for now we just
         * fail if we hit 1000 dirs w/ same pid.
         */
    } while (!dr_create_dir(logsubdir) && ++count < LOGDIR_TRY_MAX);
    if (count >= LOGDIR_TRY_MAX) {
        NOTIFY_ERROR("Unable to create subdir in log base dir %s\n", options.logdir);
        ASSERT(false, "unable to create unique logsubdir");
        dr_abort();
    }

    f_global = open_logfile("global", true/*pid suffix*/, -1);
#ifdef LINUX
    /* make it easier for wrapper script to find this logfile */
    dr_fprintf(f_global, "process=%d, parent=%d\n",
               dr_get_process_id(), dr_get_parent_id());
#endif
    /* make sure "Dr. Memory" is 1st (or 2nd on linux) in file (for PR 453867) */
    print_version(f_global);
    NOTIFY("log dir is %s\n", logsubdir);
    LOGF(1, f_global, "global logfile fd=%d\n", f_global);

#ifdef USE_DRSYMS
    if (!options.perturb_only) {
        f_results = open_logfile("results.txt", false, -1);
        print_version(f_results);
        if (options.resfile == dr_get_process_id()) {
            /* notify front-end of results path */
            file_t outf;
            char fname[MAXIMUM_PATH];
            dr_snprintf(fname, BUFFER_SIZE_ELEMENTS(fname), "%s%cresfile.%d",
                        options.logdir, DIRSEP, dr_get_process_id());
            NULL_TERMINATE_BUFFER(fname);
            outf = dr_open_file(fname, DR_FILE_WRITE_OVERWRITE);
            if (outf == INVALID_FILE)
                usage_error("Cannot write to \"%s\", aborting\n", fname);
            else {
                dr_fprintf(outf, "%s%cresults.txt", logsubdir, DIRSEP);
# undef dr_close_file
                dr_close_file(outf);
# define dr_close_file DO_NOT_USE_dr_close_file
            }
        }
        f_suppress = open_logfile("suppress.txt", false, -1);
    }
#else
    /* PR 453867: we need to tell postprocess.pl when to fork a new copy.
     * Risky to write to parent logdir, since could be in middle
     * of a leak callstack, so we use a separate file.
     */
    f_fork = open_logfile("fork.log", false, -1);
#endif
}

#ifdef LINUX
static void 
event_fork(void *drcontext)
{
    /* we want a whole new log dir to avoid clobbering the parent's */
# ifdef DEBUG
    per_thread_t *pt = (per_thread_t *) dr_get_tls_field(drcontext);
# endif
# ifndef USE_DRSYMS
    file_t f_parent_fork = f_fork;
# endif
    close_file(f_global);
    create_global_logfile();

# ifndef USE_DRSYMS
    /* PR 453867: tell postprocess.pl to fork a new copy.
     * Even if multiple threads fork simultaneously, these writes are atomic.
     */
    ELOGF(0, f_parent_fork, "FORK child=%d logdir=%s\n",
          dr_get_process_id(), logsubdir);
    close_file(f_parent_fork);
# endif

    /* note that we mark all thread logs as close-on-fork so DR will iterate
     * over them and close them all
     */
    create_thread_logfile(drcontext);
    LOGF(0, f_global, "new logfile after fork\n");
    LOG(0, "new logfile after fork fd=%d\n", pt->f);
    if (!options.shadowing) {
        /* notify postprocess (PR 574018) */
        LOG(0, "\n*********\nDISABLING MEMORY CHECKING via option\n");
    }

    report_fork_init();

    if (options.perturb)
        perturb_fork_init();
}

static dr_signal_action_t
event_signal(void *drcontext, dr_siginfo_t *info)
{
    /* alloc ignores signals used only for instrumentation */
    dr_signal_action_t res = event_signal_instrument(drcontext, info);
    if (res == DR_SIGNAL_DELIVER)
        return event_signal_alloc(drcontext, info);
    else
        return res;
}
#else
static bool
event_exception(void *drcontext, dr_exception_t *excpt)
{
    return event_exception_instrument(drcontext, excpt);
}
#endif

static void
event_nudge(void *drcontext, uint64 argument)
{
    /* PR 474554: use nudge/signal for mid-run summary/output
     * For now we have only one use, so we don't need the argument
     */
#ifdef USE_DRSYMS
    static int nudge_count;
    int local_count = atomic_add32_return_sum(&nudge_count, 1);
    LOGF(0, f_results, NL"==========================================================================="NL"SUMMARY AFTER NUDGE #%d:"NL, local_count);
#endif
#ifdef STATISTICS
    dump_statistics();
#endif
    STATS_INC(num_nudges);
    if (options.perturb_only)
        return;
    report_leak_stats_checkpoint();
    check_reachability(false/*!at exit*/);
    report_summary();
    report_leak_stats_revert();
    LOGF(0, f_global, "NUDGE\n");
}

static void
event_module_load(void *drcontext, const module_data_t *info, bool loaded)
{
#ifdef STATISTICS
    /* measure module processing time: mostly symbols (xref i#313) */
    print_timestamp_elapsed_to_file(f_global, "pre-module-load ");
#endif
    if (!options.perturb_only)
        callstack_module_load(drcontext, info, loaded);
    if (!options.leaks_only && options.shadowing)
        replace_module_load(drcontext, info, loaded);
    alloc_module_load(drcontext, info, loaded);
#ifdef STATISTICS
    print_timestamp_elapsed_to_file(f_global, "post-module-load ");
#endif
}

static void
event_module_unload(void *drcontext, const module_data_t *info)
{
    if (!options.perturb_only)
        callstack_module_unload(drcontext, info);
    if (!options.leaks_only && options.shadowing)
        replace_module_unload(drcontext, info);
    alloc_module_unload(drcontext, info);
}

#if defined(LINUX) && defined(DEBUG)
/* Checks whether the module can be treated as a single contiguous addr range,
 * which is the case if either it really is contiguous (DR's module_data_t.contiguous)
 * or if all holes are marked no-access and are thus unusable for other allocs.
 */
static bool
check_contiguous(module_data_t *data)
{
    dr_mem_info_t info;
    app_pc pc;
    if (data->contiguous)
        return true;
    /* Even if there are gaps, the linux loader normally fills them w/
     * no-access memory, which we check for here.
     */
    pc = data->start;
    while (pc >= data->start/*overflow*/ && pc < data->end) {
        if (!dr_query_memory_ex(pc, &info))
            return false;
        if (info.type == DR_MEMTYPE_FREE)
            return false;
        pc = info.base_pc+info.size;
    }
    return true;
}
#endif

DR_EXPORT void 
dr_init(client_id_t id)
{
    void *drcontext = dr_get_current_drcontext(); /* won't work on 0.9.4! */
    const char *appnm = dr_get_application_name();
#ifdef LINUX
    dr_module_iterator_t *iter;
#endif
    module_data_t *data;
    const char *opstr;

    client_id = id;
    opstr = dr_get_options(client_id);
    ASSERT(opstr != NULL, "error obtaining option string");
    drmem_options_init(opstr);
    utils_init();

    /* now that we know whether -quiet, print basic info */
    NOTIFY("Dr. Memory version %s\n", VERSION_STRING);
    NOTIFY("options are \"%s\"\n", opstr);

    /* glibc malloc 8-byte-aligns all its allocs: but 2x redzone_size matches that */
    ASSERT(!options.size_in_redzone || options.redzone_size >= sizeof(size_t),
           "redzone size not large enough to store size");

    create_global_logfile();
    LOG(0, "options are \"%s\"\n", opstr);

    dr_register_exit_event(event_exit);
    dr_register_thread_init_event(event_thread_init);
    dr_register_thread_exit_event(event_thread_exit);
    dr_register_bb_event(event_basic_block);
    dr_register_restore_state_ex_event(event_restore_state);
    dr_register_delete_event(event_fragment_delete);
    dr_register_module_load_event(event_module_load);
    dr_register_module_unload_event(event_module_unload);
    dr_register_nudge_event(event_nudge, client_id);
#ifdef LINUX
    dr_register_fork_init_event(event_fork);
    dr_register_signal_event(event_signal);
#else
    dr_register_exception_event(event_exception);
#endif

    /* make it easy to tell, by looking at log file, which client executed */
    dr_log(NULL, LOG_ALL, 1, "client = Dr. Memory version %s\n", VERSION_STRING);

    if (!options.perturb_only)
        report_init();

    if (options.shadowing)
        shadow_init();

#ifdef WINDOWS
    data = dr_lookup_module_by_name("ntdll.dll");
    ASSERT(data != NULL, "cannot find ntdll.dll");
    ntdll_base = data->start;
    ntdll_end = data->end;
    dr_free_module_data(data);
    ASSERT(ntdll_base != NULL, "internal error finding ntdll.dll base");
    LOG(2, "ntdll is "PFX"-"PFX"\n", ntdll_base, ntdll_end);
#else
    iter = dr_module_iterator_start();
    while (dr_module_iterator_hasnext(iter)) {
        data = dr_module_iterator_next(iter);
        const char *modname = dr_module_preferred_name(data);
        LOG(2, "module %s "PFX"-"PFX"\n",
            modname == NULL ? "<noname>" : modname, data->start, data->end);
        /* we need to know DR, DrMem, and libc bounds.  we use these to skip over
         * DR memory in memory_walk().  we leave DR and DrMem libs as unaddressable
         * and use is_loader_exception() to exempt ld.so reading their .dynamic
         * sections.
         */
        if (modname != NULL) {
            if (strncmp(modname, "libdynamorio.", 13) == 0) {
                LOG(2, "marking DR lib as defined\n");
                libdr_base = data->start;
                libdr_end = data->end;
                ASSERT(check_contiguous(data), "lib not contiguous!");
            } else if (strncmp(modname, "libdynamorio-", 13) == 0) {
                LOG(2, "marking DR lib2 as defined\n");
                libdr2_base = data->start;
                libdr2_end = data->end;
                ASSERT(check_contiguous(data), "lib not contiguous!");
            } else if (strncmp(modname, "libdrmemory.", 12) == 0) {
                ASSERT(dr_memory_is_in_client(data->start), "client lib mismatch");
                libdrmem_base = data->start;
                libdrmem_end = data->end;
                ASSERT(check_contiguous(data), "lib not contiguous!");
            } else if (strncmp(modname, "libc.", 5) == 0) {
                libc_base = data->start;
                libc_end = data->end;
                ASSERT(check_contiguous(data), "lib not contiguous!");
                LOG(1, "libc is "PFX"-"PFX"\n", libc_base, libc_end);
            }
        }
        dr_free_module_data(data);
    }
    dr_module_iterator_stop(iter);
#endif

#ifdef WINDOWS
    data = dr_lookup_module(get_TEB()->ProcessEnvironmentBlock->ImageBaseAddress);
#else
    if (appnm == NULL)
        data = NULL;
    else
        data = dr_lookup_module_by_name(appnm);
#endif
    if (data == NULL) {
# ifndef VMX86_SERVER /* remove once have PR 363063 */
        NOTIFY("WARNING: cannot find executable image\n");
# endif
    } else {
        app_base = data->start;
        app_end = data->end;
        dr_free_module_data(data);
        ASSERT(app_base != NULL, "internal error finding executable base");
        LOG(2, "executable is "PFX"-"PFX"\n", app_base, app_end);
    }

    heap_region_init(handle_new_heap_region, handle_removed_heap_region);

    /* must be before alloc_drmem_init() */
    syscall_init(drcontext _IF_WINDOWS(ntdll_base));

    hashtable_init(&known_table, KNOWN_TABLE_HASH_BITS, HASH_INTPTR, false/*!strdup*/);
    alloc_drmem_init();

    if (options.perturb)
        perturb_init();

    instrument_init();
}
