/*
 Runtime library for supporting Coarray Fortran

 Copyright (C) 2009-2012 University of Houston.

 This program is free software; you can redistribute it and/or modify it
 under the terms of version 2 of the GNU General Public License as
 published by the Free Software Foundation.

 This program is distributed in the hope that it would be useful, but
 WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

 Further, this software is distributed without any warranty that it is
 free of the rightful claim of any third person regarding infringement
 or the like.  Any license provided herein, whether implied or
 otherwise, applies only to this software file.  Patent licenses, if
 any, provided herein do not apply to combinations of this program with
 other software, or any other product whatsoever.

 You should have received a copy of the GNU General Public License along
 with this program; if not, write the Free Software Foundation, Inc., 59
 Temple Place - Suite 330, Boston MA 02111-1307, USA.

 Contact information:
 http://www.cs.uh.edu/~hpctools
*/

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <assert.h>

#include "dopevec.h"
#include "caf_rtl.h"
#include "util.h"

#include "comm.h"

#include "lock.h"
#include "trace.h"


extern int __ompc_init_rtl(int num_threads);

#pragma weak __ompc_init_rtl

/* initialized in comm_init() */
unsigned long _this_image;
unsigned long _num_images;

mem_usage_info_t *mem_info;

/* common_slot is a node in the shared memory link-list that keeps track
 * of available memory that can used for both allocatable coarrays and
 * asymmetric data. It is the only handle to access the link-list.*/
struct shared_memory_slot *common_slot;

/* LOCAL FUNCTION DECLARATIONIS */
static struct shared_memory_slot *find_empty_shared_memory_slot_above
    (struct shared_memory_slot *slot, unsigned long var_size);
static struct shared_memory_slot *find_empty_shared_memory_slot_below
    (struct shared_memory_slot *slot, unsigned long var_size);
static void *split_empty_shared_memory_slot_from_top
    (struct shared_memory_slot *slot, unsigned long var_size);
static void *split_empty_shared_memory_slot_from_bottom
    (struct shared_memory_slot *slot, unsigned long var_size);

static struct shared_memory_slot *find_shared_memory_slot_above
    (struct shared_memory_slot *slot, void *address);
static struct shared_memory_slot *find_shared_memory_slot_below
    (struct shared_memory_slot *slot, void *address);

static void join_3_shared_memory_slots(struct shared_memory_slot *slot);
static void join_with_prev_shared_memory_slot
    (struct shared_memory_slot *slot);
static void join_with_next_shared_memory_slot
    (struct shared_memory_slot *slot);

static void empty_shared_memory_slot(struct shared_memory_slot *slot);
static void free_prev_slots_recursively(struct shared_memory_slot *slot);
static void free_next_slots_recursively(struct shared_memory_slot *slot);


static int is_contiguous_access(const size_t strides[],
                                const size_t count[],
                                size_t stride_levels);
static void local_src_strided_copy(void *src, const size_t src_strides[],
                                   void *dest, const size_t count[],
                                   size_t stride_levels);

static void local_dest_strided_copy(void *src, void *dest,
                                    const size_t dest_strides[],
                                    const size_t count[],
                                    size_t stride_levels);


/* COMPILER BACK-END INTERFACE */

void __caf_init()
{
    static int libcaf_initialized = 0;
    if (libcaf_initialized == 0)
        libcaf_initialized = 1;
    else
        return;

    common_slot = (struct shared_memory_slot *)
        malloc(sizeof(struct shared_memory_slot));
    START_TIMER();
    comm_init(common_slot);     /* common slot is initialized in comm_init */
    STOP_TIMER(INIT);

    LIBCAF_TRACE(LIBCAF_LOG_INIT, "initialized,"
                 " num_images = %lu", _num_images);
    LIBCAF_TRACE(LIBCAF_LOG_TIME, "comm_init ");

    /* initialize the openmp runtime library, if it exists */
    if (__ompc_init_rtl)
        __ompc_init_rtl(0);
}

void __caf_finalize(int exit_code)
{
    LIBCAF_TRACE(LIBCAF_LOG_TIME_SUMMARY, "Accumulated Time:");
    LIBCAF_TRACE(LIBCAF_LOG_MEMORY_SUMMARY, "\n\tHEAP USAGE: ");
    LIBCAF_TRACE(LIBCAF_LOG_EXIT, "Before call to comm_finalize");
    comm_finalize(exit_code);

    /* does not reach */
}


void __acquire_lcb(unsigned long buf_size, void **ptr)
{
    *ptr = comm_malloc(buf_size);
    LIBCAF_TRACE(LIBCAF_LOG_MEMORY, "acquired lcb %p of size %lu",
                 *ptr, buf_size);
}

void __release_lcb(void **ptr)
{
    comm_free_lcb(*ptr);
    LIBCAF_TRACE(LIBCAF_LOG_MEMORY, "freed lcb %p", *ptr);
}

void __coarray_sync(comm_handle_t hdl)
{
    LIBCAF_TRACE(LIBCAF_LOG_SYNC, "before call to comm_sync with hdl=%p",
                 hdl);
    START_TIMER();
    comm_sync(hdl);
    STOP_TIMER(SYNC);
    LIBCAF_TRACE(LIBCAF_LOG_TIME, "comm_sync");
}

void __coarray_nbread(size_t image, void *src, void *dest, size_t nbytes,
                      comm_handle_t * hdl)
{
    check_remote_image(image);

    /* initialize to NULL */
    if (hdl)
        *hdl = NULL;

    START_TIMER();
    /* reads nbytes from src on proc 'image-1' into local dest */
    comm_nbread(image - 1, src, dest, nbytes, hdl);

    STOP_TIMER(READ);
    LIBCAF_TRACE(LIBCAF_LOG_TIME, "comm_nbread ");

    LIBCAF_TRACE(LIBCAF_LOG_COMM,
                 "Finished read from %p on Img %lu to %p"
                 " data of size %lu ", src, image, dest, nbytes);
}

void __coarray_read(size_t image, void *src, void *dest, size_t nbytes)
{
    check_remote_image(image);

    START_TIMER();
    /* reads nbytes from src on proc 'image-1' into local dest */
    comm_read(image - 1, src, dest, nbytes);

    STOP_TIMER(READ);
    LIBCAF_TRACE(LIBCAF_LOG_TIME, "comm_read ");

    LIBCAF_TRACE(LIBCAF_LOG_COMM,
                 "Finished read from %p on Img %lu to %p"
                 " data of size %lu ", src, image, dest, nbytes);
}

void __coarray_write(size_t image, void *dest, void *src, size_t nbytes)
{
    check_remote_image(image);

    START_TIMER();
    comm_write(image - 1, dest, src, nbytes);
    STOP_TIMER(WRITE);
    LIBCAF_TRACE(LIBCAF_LOG_TIME, "comm_write ");

    LIBCAF_TRACE(LIBCAF_LOG_COMM,
                 "Wrote to %p on Img %lu from %p data of"
                 " size %lu ", dest, image, src, nbytes);
}

void __coarray_strided_nbread(size_t image,
                              void *src, const size_t src_strides[],
                              void *dest, const size_t dest_strides[],
                              const size_t count[], int stride_levels,
                              comm_handle_t * hdl)
{
    int remote_is_contig = 0;
    int local_is_contig = 0;
    int i;

    check_remote_image(image);

    /* initialize to NULL */
    if (hdl)
        *hdl = NULL;

    /* runtime check if it is contiguous transfer */
    remote_is_contig =
        is_contiguous_access(src_strides, count, stride_levels);
    if (remote_is_contig) {
        local_is_contig =
            is_contiguous_access(dest_strides, count, stride_levels);
        size_t nbytes = 1;
        for (i = 0; i <= stride_levels; i++)
            nbytes = nbytes * count[i];

        if (local_is_contig) {
            __coarray_nbread(image, src, dest, nbytes, hdl);
        } else {
            void *buf;
            __acquire_lcb(nbytes, &buf);
            __coarray_nbread(image, src, buf, nbytes, hdl);
            local_dest_strided_copy(buf, dest, dest_strides, count,
                                    stride_levels);
            __release_lcb(&buf);
        }

        return;
        /* not reached */
    }

    START_TIMER();
    LIBCAF_TRACE(LIBCAF_LOG_COMM,
                 "Starting read(strided) "
                 "from %p on Img %lu to %p using %d stride_levels ",
                 src, image, dest, stride_levels);
    comm_strided_nbread(image - 1, src, src_strides, dest, dest_strides,
                        count, stride_levels, hdl);
    STOP_TIMER(READ);
    LIBCAF_TRACE(LIBCAF_LOG_TIME, "comm_nbread_strided ");

    LIBCAF_TRACE(LIBCAF_LOG_COMM,
                 "Finished read(strided) "
                 "from %p on Img %lu to %p using %d stride_levels ",
                 src, image, dest, stride_levels);
}

void __coarray_strided_read(size_t image,
                            void *src, const size_t src_strides[],
                            void *dest, const size_t dest_strides[],
                            const size_t count[], int stride_levels)
{
    int remote_is_contig = 0;
    int local_is_contig = 0;
    int i;

    check_remote_image(image);

    /* runtime check if it is contiguous transfer */
    remote_is_contig =
        is_contiguous_access(src_strides, count, stride_levels);
    if (remote_is_contig) {
        local_is_contig =
            is_contiguous_access(dest_strides, count, stride_levels);
        size_t nbytes = 1;
        for (i = 0; i <= stride_levels; i++)
            nbytes = nbytes * count[i];

        if (local_is_contig) {
            __coarray_read(image, src, dest, nbytes);
        } else {
            void *buf;
            __acquire_lcb(nbytes, &buf);
            __coarray_read(image, src, buf, nbytes);
            local_dest_strided_copy(buf, dest, dest_strides, count,
                                    stride_levels);
            __release_lcb(&buf);
        }

        return;
        /* not reached */
    }

    START_TIMER();
    LIBCAF_TRACE(LIBCAF_LOG_COMM,
                 "Starting read(strided) "
                 "from %p on Img %lu to %p using %d stride_levels ",
                 src, image, dest, stride_levels);
    comm_strided_read(image - 1, src, src_strides, dest, dest_strides,
                      count, stride_levels);
    STOP_TIMER(READ);
    LIBCAF_TRACE(LIBCAF_LOG_TIME, "comm_read_strided ");

    LIBCAF_TRACE(LIBCAF_LOG_COMM,
                 "Finished read(strided) "
                 "from %p on Img %lu to %p using %d stride_levels ",
                 src, image, dest, stride_levels);
}

void __coarray_strided_write(size_t image,
                             void *dest, const size_t dest_strides[],
                             void *src, const size_t src_strides[],
                             const size_t count[], int stride_levels)
{
    int remote_is_contig = 0;
    int local_is_contig = 0;
    int i;

    check_remote_image(image);

    /* runtime check if it is contiguous transfer */
    remote_is_contig =
        is_contiguous_access(dest_strides, count, stride_levels);
    if (remote_is_contig) {
        local_is_contig =
            is_contiguous_access(src_strides, count, stride_levels);
        size_t nbytes = 1;
        for (i = 0; i <= stride_levels; i++)
            nbytes = nbytes * count[i];

        if (local_is_contig) {
            __coarray_write(image, dest, src, nbytes);
        } else {
            void *buf;
            Error
                ("local buffer for coarray_strided_write should be contiguous");
            __acquire_lcb(nbytes, &buf);
            local_src_strided_copy(src, src_strides, buf, count,
                                   stride_levels);
            __coarray_write(image, dest, buf, nbytes);
            __release_lcb(&buf);
        }

        return;
        /* not reached */
    }

    START_TIMER();
    comm_strided_write(image - 1, dest, dest_strides, src, src_strides,
                       count, stride_levels);
    STOP_TIMER(WRITE);
    LIBCAF_TRACE(LIBCAF_LOG_TIME, "comm_write_strided ");

    LIBCAF_TRACE(LIBCAF_LOG_COMM,
                 "Finished write(strided) to %p"
                 " on Img %lu from %p using stride_levels %d ", dest,
                 image, src, stride_levels);
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * SHARED MEMORY MANAGEMENT FUNCTIONS
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * Note: The term 'shared memory' is used in the PGAS sense, i.e. the
 * memory may not be physically shared. It can however be directly
 * accessed from any other image. This is done by the pinning/registering
 * memory by underlying communication layer (GASNet/ARMCI).
 *
 * During comm_init GASNet/ARMCI creates a big chunk of shared memory.
 * Static coarrays are allocated memory from this chunk. The remaining
 * memory is left for allocatable coarrays and pointers in coarrays of
 * derived datatype (henceforth referred as asymmetric data).
 * It returns the starting address and size of this remaining memory chunk
 * by populating the structure common_slot(explained later).
 *
 * Normal fortan allocation calls are intercepted to check whether they
 * are for coarrays or asymmetric data. If yes, the following functions
 * are called which are defined below.
 *  void* coarray_allocatable_allocate_(unsigned long var_size);
 *  void* coarray_asymmetric_allocate_(unsigned long var_size);
 * Since allocatable coarrays must have symmetric address, a seperate heap
 * must be created for asymmetric data. To avoid wasting memory space by
 * statically reserving it, we use the top of heap for allocatable
 * coarrays (which grows downward) and the bottom of heap for asymmetric
 * data(which grows up). A link-list of struct shared_memory_slot is
 * used to manage allocation and deallocation.
 *
 * common_slot is an empty slot which always lies in between allocatable
 * heap and asymmetric heap, and used by both to reserve memory.
 *                          _________
 *                          | Alloc |
 *                          | heap  |
 *                          =========
 *                          | Common|
 *                          |  slot |
 *                          =========
 *                          | asymm |
 *                          | heap  |
 *                          |_______|
 * Allocatable heap comsumes common_slot from top, while assymetric heap
 * consumes from bottom. Each allocation address and size is stored in
 * a sperate slot (node in the list). Each slot has a full-empty bit(feb).
 * During deallocation (using function coarray_deallocate_) the feb is set
 * to 0 (empty). If any neighboring slot is empty, they are merged. Hence,
 * when a slot bordering common-slot is deallocated, the common-slot
 * grows.
 *
 * If there is no more space left in common slot, empty slots are used
 * from above for allocable coarrays or from below for asymmetric data.
 *
 * During exit, the function coarray_free_all_shared_memory_slots()
 * is used to free all nodes in the shared memory list.
 */

/* Static function used to find empty memory slots while reserving
 * memory for allocatable coarrays */
static struct shared_memory_slot *find_empty_shared_memory_slot_above
    (struct shared_memory_slot *slot, unsigned long var_size) {
    while (slot) {
        if (slot->feb == 0 && slot->size >= var_size)
            return slot;
        slot = slot->prev;
    }
    return 0;
}

/* Static function used to find empty memory slots while reserving
 * memory for assymetric coarrays */
static struct shared_memory_slot *find_empty_shared_memory_slot_below
    (struct shared_memory_slot *slot, unsigned long var_size) {
    while (slot) {
        if (slot->feb == 0 && slot->size >= var_size)
            return slot;
        slot = slot->next;
    }
    return 0;
}

/* Static function used to reserve top part of an empty memory slot
 * for allocatable coarrays. Returns the memory address allocated */
static void *split_empty_shared_memory_slot_from_top
    (struct shared_memory_slot *slot, unsigned long var_size) {
    struct shared_memory_slot *new_empty_slot;
    new_empty_slot = (struct shared_memory_slot *) malloc
        (sizeof(struct shared_memory_slot));
    new_empty_slot->addr = slot->addr + var_size;
    new_empty_slot->size = slot->size - var_size;
    new_empty_slot->feb = 0;
    new_empty_slot->next = slot->next;
    new_empty_slot->prev = slot;
    slot->size = var_size;
    slot->feb = 1;
    slot->next = new_empty_slot;
    if (common_slot == slot)
        common_slot = new_empty_slot;
    return slot->addr;
}

/* Static function used to reserve bottom part of an empty memory slot
 * for asymmetric data. Returns the memory address allocated*/
static void *split_empty_shared_memory_slot_from_bottom
    (struct shared_memory_slot *slot, unsigned long var_size) {
    struct shared_memory_slot *new_full_slot;
    new_full_slot = (struct shared_memory_slot *) malloc
        (sizeof(struct shared_memory_slot));
    new_full_slot->addr = slot->addr + slot->size - var_size;
    new_full_slot->size = var_size;
    new_full_slot->feb = 1;
    new_full_slot->next = slot->next;
    new_full_slot->prev = slot;
    slot->size = slot->size - var_size;
    slot->next = new_full_slot;
    return new_full_slot->addr;
}

/* Memory allocation function for allocatable coarrays. It is invoked
 * from fortran allocation function _ALLOCATE in
 * osprey/libf/fort/allocation.c
 * It finds empty slot from the shared memory list (common_slot & above)
 * and then splits the slot from top
 * Note: there is barrier as it is a collective operation*/
void *coarray_allocatable_allocate_(unsigned long var_size)
{
    struct shared_memory_slot *empty_slot;
    empty_slot = find_empty_shared_memory_slot_above(common_slot,
                                                     var_size);
    if (empty_slot == 0)
        Error
            ("No more shared memory space available for allocatable coarray. "
             "Set environment variable %s or cafrun option for more space.",
             ENV_IMAGE_HEAP_SIZE);

    LIBCAF_TRACE(LIBCAF_LOG_MEMORY, "caf_rtl.c:coarray_coarray_allocate"
                 "-> Found empty slot %p. About to split it from top.",
                 empty_slot->addr);

#ifdef TRACE
    /* update heap usage info if MEMORY_SUMMARY trace is enabled */
    if (__trace_is_enabled(LIBCAF_LOG_MEMORY_SUMMARY) && mem_info) {
        size_t current_size = mem_info->current_heap_usage + var_size;
        mem_info->current_heap_usage = current_size;
        if (mem_info->max_heap_usage < current_size)
            mem_info->max_heap_usage = current_size;
    }
#endif

    comm_barrier_all();         // implicit barrier in case of allocatable.
    if (empty_slot != common_slot && empty_slot->size == var_size) {
        empty_slot->feb = 1;
        return empty_slot->addr;
    }


    return split_empty_shared_memory_slot_from_top(empty_slot, var_size);
}

/* Memory allocation function for asymmetric data. It is invoked
 * from fortran allocation function _ALLOCATE in
 * osprey/libf/fort/allocation.c
 * It finds empty slot from the shared memory list (common_slot & below)
 * and then splits the slot from bottom */
void *coarray_asymmetric_allocate_(unsigned long var_size)
{
    struct shared_memory_slot *empty_slot;
    empty_slot = find_empty_shared_memory_slot_below(common_slot,
                                                     var_size);
    if (empty_slot == 0)
        Error("No more shared memory space available for asymmetric data. "
              "Set environment variable %s or cafrun option for more space.",
              ENV_IMAGE_HEAP_SIZE);

    LIBCAF_TRACE(LIBCAF_LOG_MEMORY, "caf_rtl.c:coarray_asymmetric_allocate"
                 "-> Found empty slot %p. About to split it from bottom. ",
                 empty_slot->addr);

#ifdef TRACE
    /* update heap usage info if MEMORY_SUMMARY trace is enabled */
    if (__trace_is_enabled(LIBCAF_LOG_MEMORY_SUMMARY)) {
        size_t current_size = mem_info->current_heap_usage + var_size;
        mem_info->current_heap_usage = current_size;
        if (mem_info->max_heap_usage < current_size)
            mem_info->max_heap_usage = current_size;
    }
#endif

    if (empty_slot != common_slot && empty_slot->size == var_size) {
        empty_slot->feb = 1;
        return empty_slot->addr;
    }
    return split_empty_shared_memory_slot_from_bottom(empty_slot,
                                                      var_size);
}

/* Static function called from coarray_deallocate.
 * It finds the slot with the address (passed in param) by searching
 * the shared memory link-list starting from the slot(passed in param)
 * and above it. Used for finding slots containing allocatable coarrays*/
static struct shared_memory_slot *find_shared_memory_slot_above
    (struct shared_memory_slot *slot, void *address) {
    while (slot) {
        if (slot->feb == 1 && slot->addr == address)
            return slot;
        slot = slot->prev;
    }
    return 0;
}

/* Static function called from coarray_deallocate.
 * It finds the slot with the address (passed in param) by searching
 * the shared memory link-list starting from the slot(passed in param)
 * and below it. Used for finding slots containing asymmetric data*/
static struct shared_memory_slot *find_shared_memory_slot_below
    (struct shared_memory_slot *slot, void *address) {
    while (slot) {
        if (slot->feb == 1 && slot->addr == address)
            return slot;
        slot = slot->next;
    }
    return 0;
}

/* Static function called from empty_shared_memory_slot (used in dealloc).
 * Merge slot with the slot above & below it. If any of these slots is the
 * common-slot, the common-slot points to the merged slot */
static void join_3_shared_memory_slots(struct shared_memory_slot *slot)
{
    slot->prev->size = slot->prev->size + slot->size + slot->next->size;
    slot->prev->next = slot->next->next;
    if (slot->next->next)
        slot->next->next->prev = slot->prev;
    if (common_slot == slot || common_slot == slot->next)
        common_slot = slot->prev;
    comm_free(slot->next);
    comm_free(slot);
}

/* Static function called from empty_shared_memory_slot (used in dealloc).
 * Merge slot with the slot above it. If any of these slots is the
 * common-slot, the common-slot points to the merged slot */
static void join_with_prev_shared_memory_slot
    (struct shared_memory_slot *slot) {
    slot->prev->size += slot->size;
    slot->prev->next = slot->next;
    if (slot->next)
        slot->next->prev = slot->prev;
    if (common_slot == slot)
        common_slot = slot->prev;
    comm_free(slot);
}

/* Static function called from empty_shared_memory_slot (used in dealloc).
 * Merge slot with the slot below it. If any of these slots is the
 * common-slot, the common-slot points to the merged slot */
static void join_with_next_shared_memory_slot
    (struct shared_memory_slot *slot) {
    struct shared_memory_slot *tmp;
    tmp = slot->next;
    slot->size += slot->next->size;
    if (slot->next->next)
        slot->next->next->prev = slot;
    slot->next = slot->next->next;
    if (common_slot == tmp)
        common_slot = slot;
    comm_free(tmp);
}

/* Static function called from coarray_deallocate.
 * Empties the slot passed in parameter:
 * 1) set full-empty-bit to 0
 * 2) merge the slot with neighboring empty slots (if found) */
static void empty_shared_memory_slot(struct shared_memory_slot *slot)
{
    slot->feb = 0;
    if (slot->prev && (slot->prev->feb == 0) && slot->next
        && (slot->next->feb == 0))
        join_3_shared_memory_slots(slot);
    else if (slot->prev && (slot->prev->feb == 0))
        join_with_prev_shared_memory_slot(slot);
    else if (slot->next && (slot->next->feb == 0))
        join_with_next_shared_memory_slot(slot);
}

/* Memory deallocation function for allocatable coarrays and asymmetric
 * data. It is invoked from fortran allocation function _DEALLOCATE in
 * osprey/libf/fort/allocation.c
 * It finds the slot from the shared memory list, set full-empty-bit to 0,
 * and then merge the slot with neighboring empty slots (if found)
 * Note: there is implicit barrier for allocatable coarrays*/
void coarray_deallocate_(void *var_address)
{
    struct shared_memory_slot *slot;
    slot = find_shared_memory_slot_above(common_slot, var_address);
    if (slot)
        comm_barrier_all();     //implicit barrier for allocatable
    else
        slot = find_shared_memory_slot_below(common_slot, var_address);
    if (slot == 0) {
        LIBCAF_TRACE(LIBCAF_LOG_NOTICE,
                     "caf_rtl.c:coarray_deallocate_->Address%p not coarray.",
                     var_address);
        return;
    }
    LIBCAF_TRACE(LIBCAF_LOG_MEMORY,
                 "caf_rtl.c:coarray_deallocate_->before deallocating %p.",
                 var_address);

#ifdef TRACE
    /* update heap usage info if MEMORY_SUMMARY trace is enabled */
    if (__trace_is_enabled(LIBCAF_LOG_MEMORY_SUMMARY)) {
        mem_info->current_heap_usage -= slot->size;
    }
#endif

    empty_shared_memory_slot(slot);
}

/* Static function called from coarray_free_all_shared_memory_slots()
 * during exit from program.
 * It recursively frees slots in the shared memory link-list starting
 * from slot passed in param and all slots above(previous) it. */
static void free_prev_slots_recursively(struct shared_memory_slot *slot)
{
    if (slot) {
        free_prev_slots_recursively(slot->prev);
        comm_free(slot);
    }
}

/* Static function called from coarray_free_all_shared_memory_slots()
 * during exit from program.
 * It recursively frees slots in the shared memory link-list starting
 * from slot passed in param and all slots below(next) it. */
static void free_next_slots_recursively(struct shared_memory_slot *slot)
{
    if (slot) {
        free_next_slots_recursively(slot->next);
        comm_free(slot);
    }
}

/* Function to delete the shared memory link-list.
 * Called during exit from comm_exit in armci_comm_layer.c or
 * gasnet_comm_layer.c.
 */
void coarray_free_all_shared_memory_slots()
{
    free_prev_slots_recursively(common_slot->prev);
    free_next_slots_recursively(common_slot);

#ifdef TRACE
    /* update heap usage info if MEMORY_SUMMARY trace is enabled */
    if (__trace_is_enabled(LIBCAF_LOG_MEMORY_SUMMARY)) {
        mem_info->current_heap_usage = 0;
    }
#endif
}

/* Translate a remote address from a given image to the local address space
 */
void coarray_translate_remote_addr(void **remote_addr, int image)
{
    LIBCAF_TRACE(LIBCAF_LOG_COMM,
                 "(start) - "
                 "remote_addr: %p, image: %d ", remote_addr, image);

    comm_translate_remote_addr(remote_addr, image - 1);

    LIBCAF_TRACE(LIBCAF_LOG_COMM,
                 "(end) - "
                 "remote_addr: %p, image: %d ", remote_addr, image);
}



#pragma weak uhcaf_check_comms_ = uhcaf_check_comms
void uhcaf_check_comms(void)
{
    comm_service();
}

/* end shared memory management functions*/

void __caf_exit(int status)
{
    LIBCAF_TRACE(LIBCAF_LOG_TIME_SUMMARY, "Accumulated Time: ");
    LIBCAF_TRACE(LIBCAF_LOG_MEMORY_SUMMARY, "\n\tHEAP USAGE: ");
    LIBCAF_TRACE(LIBCAF_LOG_EXIT, "Exiting with error code %d", status);
    comm_exit(status);

    /* does not reach */
}


void _SYNC_ALL(int *status, int stat_len, char *errmsg, int errmsg_len)
{
    LIBCAF_TRACE(LIBCAF_LOG_SYNC, "before call to comm_barrier_all");
    START_TIMER();
    comm_sync_all(status, stat_len, errmsg, errmsg_len);
    STOP_TIMER(SYNC);
    LIBCAF_TRACE(LIBCAF_LOG_TIME, "comm_sync_all ");

}

/*************CRITICAL SUPPORT **************/

void _CRITICAL()
{
    comm_critical();
}

void _END_CRITICAL()
{
    comm_end_critical();
}

/*************END CRITICAL SUPPORT **************/


void _COARRAY_LOCK(lock_t * lock, const int *image, char *success,
                   int success_len, int *status, int stat_len,
                   char *errmsg, int errmsg_len)
{
    int img;

    LIBCAF_TRACE(LIBCAF_LOG_SYNC, "before call to comm_lock");
    START_TIMER();

    if (*image == 0)
        img = _this_image;
    else
        img = *image;

    comm_lock(lock, img, success, success_len, status, stat_len,
              errmsg, errmsg_len);
    STOP_TIMER(SYNC);
    LIBCAF_TRACE(LIBCAF_LOG_TIME, "comm_lock ");
    LIBCAF_TRACE(LIBCAF_LOG_SYNC, "after call to comm_lock");
}

void _COARRAY_UNLOCK(lock_t * lock, const int *image, int *status,
                     int stat_len, char *errmsg, int errmsg_len)
{
    int img;

    LIBCAF_TRACE(LIBCAF_LOG_SYNC, "before call to comm_unlock");
    START_TIMER();

    if (*image == 0)
        img = _this_image;
    else
        img = *image;

#if defined(GASNET)
    comm_unlock(lock, img, status, stat_len, errmsg, errmsg_len);
#elif defined(ARMCI)
    /* the version uses fetch-and-store instead of compare-and-swap */
    comm_unlock2(lock, img, status, stat_len, errmsg, errmsg_len);
#endif
    STOP_TIMER(SYNC);
    LIBCAF_TRACE(LIBCAF_LOG_TIME, "comm_unlock ");
    LIBCAF_TRACE(LIBCAF_LOG_SYNC, "after call to comm_lock");
}

void _ATOMIC_DEFINE_1(atomic_t * atom, INT1 * value, int *image)
{
    LIBCAF_TRACE(LIBCAF_LOG_SYNC,
                 "before call to comm_write_unbuffered, "
                 "atom=%p, value=%p, image_idx=%d", atom, value, *image);

    if (*image == 0) {
        /* local reference */
        *atom = (atomic_t) * value;
    } else {
        atomic_t t = (atomic_t) * value;
        check_remote_image(*image);
        check_remote_address(*image, atom);
        /* atomic variables are always of size sizeof(atomic_t) bytes.  Call
         * to comm_write_unbuffered will block until the variable is defined
         * on the remote image. */
        comm_write_unbuffered(*image - 1, atom, &t, sizeof(atomic_t));
    }

    LIBCAF_TRACE(LIBCAF_LOG_SYNC, "after call to comm_write_unbuffered");
}

void _ATOMIC_DEFINE_2(atomic_t * atom, INT2 * value, int *image)
{
    LIBCAF_TRACE(LIBCAF_LOG_SYNC, "before call to comm_write_unbuffered, "
                 "atom=%p, value=%p, image_idx=%d", atom, value, *image);

    if (*image == 0) {
        /* local reference */
        *atom = (atomic_t) * value;
    } else {
        atomic_t t = (atomic_t) * value;
        check_remote_image(*image);
        check_remote_address(*image, atom);
        /* atomic variables are always of size sizeof(atomic_t) bytes.  Call
         * to comm_write_unbuffered will block until the variable is defined
         * on the remote image. */
        comm_write_unbuffered(*image - 1, atom, &t, sizeof(atomic_t));
    }

    LIBCAF_TRACE(LIBCAF_LOG_SYNC, "after call to comm_write");
}

void _ATOMIC_DEFINE_4(atomic_t * atom, INT4 * value, int *image)
{
    LIBCAF_TRACE(LIBCAF_LOG_SYNC, "before call to comm_write, "
                 "atom=%p, value=%p, image_idx=%d", atom, value, *image);

    if (*image == 0) {
        /* local reference */
        *atom = (atomic_t) * value;
    } else {
        atomic_t t = (atomic_t) * value;
        check_remote_image(*image);
        check_remote_address(*image, atom);
        /* atomic variables are always of size sizeof(atomic_t) bytes.
         * Call to comm_write will block until the variable is defined on the
         * remote image. */
        comm_write_unbuffered(*image - 1, atom, &t, sizeof(atomic_t));
    }

    LIBCAF_TRACE(LIBCAF_LOG_SYNC, "after call to comm_write");
}

void _ATOMIC_DEFINE_8(atomic_t * atom, INT8 * value, int *image)
{
    LIBCAF_TRACE(LIBCAF_LOG_SYNC, "before call to comm_write, "
                 "atom=%p, value=%p, image_idx=%d", atom, value, *image);

    if (*image == 0) {
        /* local reference */
        *atom = (atomic_t) * value;
    } else {
        atomic_t t = (atomic_t) * value;
        check_remote_image(*image);
        check_remote_address(*image, atom);
        /* atomic variables are always of size sizeof(atomic_t) bytes.
         * Call to comm_write will block until the variable is defined on the
         * remote image. */
        comm_write_unbuffered(*image - 1, atom, &t, sizeof(atomic_t));
    }

    LIBCAF_TRACE(LIBCAF_LOG_SYNC, "after call to comm_write");
}

void _ATOMIC_REF_1(INT1 * value, atomic_t * atom, int *image)
{
    LIBCAF_TRACE(LIBCAF_LOG_SYNC, "before call to comm_read, "
                 "atom=%p, value=%p, image_idx=%d", atom, value, *image);

    if (*image == 0) {
        /* local reference */
        *value = (INT1) * atom;
    } else {
        atomic_t t;
        check_remote_image(*image);
        check_remote_address(*image, atom);
        /* atomic variables are always of size sizeof(atomic_t) bytes.
         * Call to comm_read will block until the variable is defined on the
         * remote image. */
        comm_read(*image - 1, atom, &t, sizeof(atomic_t));
        *value = (INT1) t;
    }

    LIBCAF_TRACE(LIBCAF_LOG_SYNC, "after call to comm_read");
}

void _ATOMIC_REF_2(INT2 * value, atomic_t * atom, int *image)
{
    LIBCAF_TRACE(LIBCAF_LOG_SYNC, "before call to comm_read, "
                 "atom=%p, value=%p, image_idx=%d", atom, value, *image);

    if (*image == 0) {
        /* local reference */
        *value = (INT2) * atom;
    } else {
        atomic_t t;
        check_remote_image(*image);
        check_remote_address(*image, atom);
        /* atomic variables are always of size sizeof(atomic_t) bytes.
         * Call to comm_read will block until the variable is defined on the
         * remote image. */
        comm_read(*image - 1, atom, &t, sizeof(atomic_t));
        *value = (INT2) t;
    }

    LIBCAF_TRACE(LIBCAF_LOG_SYNC, "after call to comm_read");
}

void _ATOMIC_REF_4(INT4 * value, atomic_t * atom, int *image)
{
    LIBCAF_TRACE(LIBCAF_LOG_SYNC, "before call to comm_read, "
                 "atom=%p, value=%p, image_idx=%d", atom, value, *image);

    if (*image == 0) {
        /* local reference */
        *value = (atomic_t) * atom;
    } else {
        atomic_t t;
        check_remote_image(*image);
        check_remote_address(*image, atom);
        /* atomic variables are always of size sizeof(atomic_t) bytes.
         * Call to comm_read will block until the variable is defined on the
         * remote image. */
        comm_read(*image - 1, atom, &t, sizeof(atomic_t));
        *value = (INT4) t;
    }

    LIBCAF_TRACE(LIBCAF_LOG_SYNC, "after call to comm_read");
}

void _ATOMIC_REF_8(INT8 * value, atomic_t * atom, int *image)
{
    LIBCAF_TRACE(LIBCAF_LOG_SYNC, "before call to comm_read, "
                 "atom=%p, value=%p, image_idx=%d", atom, value, *image);

    if (*image == 0) {
        /* local reference */
        *value = (INT8) * atom;
    } else {
        atomic_t t;
        check_remote_image(*image);
        check_remote_address(*image, atom);
        /* atomic variables are always of size sizeof(atomic_t) bytes.
         * Call to comm_read will block until the variable is defined on the
         * remote image. */
        comm_read(*image - 1, atom, &t, sizeof(atomic_t));
        *value = (INT8) t;
    }

    LIBCAF_TRACE(LIBCAF_LOG_SYNC, "after call to comm_read");
}

void _EVENT_POST(event_t * event, int *image)
{
    if (*image == 0) {
        /* local reference */
        event_t result, inc = 1;
        comm_fadd_request(event, &inc, sizeof(event_t), _this_image - 1,
                          &result);
    } else {
        event_t result, inc = 1;
        check_remote_image(*image);
        check_remote_address(*image, event);
        comm_fadd_request(event, &inc, sizeof(event_t), *image - 1,
                          &result);
    }
}

void _EVENT_QUERY(event_t * event, int *image, char *state, int state_len)
{
    memset(state, 0, state_len);
    if (*image == 0) {
        *state = (int) (*event) != 0;
    } else {
        check_remote_image(*image);
        check_remote_address(*image, event);
        switch (state_len) {
        case 1:
            _ATOMIC_REF_1((INT1 *) & state, event, image);
            break;
        case 2:
            _ATOMIC_REF_2((INT2 *) & state, event, image);
            break;
        case 4:
            _ATOMIC_REF_4((INT4 *) & state, event, image);
            break;
        case 8:
            _ATOMIC_REF_8((INT8 *) & state, event, image);
            break;
        default:
            LIBCAF_TRACE(LIBCAF_LOG_FATAL, "_EVENT_QUERY called "
                         "using state variable with invalid length %d",
                         state_len);
        }
    }
}

void _EVENT_WAIT(event_t * event, int *image)
{
    event_t state;
    if (*image == 0) {
        int done = 0;
        do {
            state = *event;
            if (state > 0) {
                state = SYNC_FETCH_AND_ADD(event, -1);
                if (state > 0) {
                    /* event variable successfully modified */
                    return;
                } else {
                    /* shouldn't have decremented, so add 1 back */
                    state = SYNC_FETCH_AND_ADD(event, 1);
                }
            }
            comm_service();
        } while (1);
    } else {
        check_remote_image(*image);
        check_remote_address(*image, event);
        do {
            comm_read(*image - 1, event, &state, sizeof(event_t));
            if (state > 0) {
                INT4 dec = -1;
                INT4 inc = 1;
                comm_fadd_request(event, &dec, sizeof(event_t), *image - 1,
                                  &state);
                if (state > 0) {
                    /* event variable successfully modified */
                    return;
                } else {
                    /* shouldn't have decremented, so add 1 back */
                    comm_fadd_request(event, &inc, sizeof(event_t),
                                      *image - 1, &state);
                }
            }
            comm_service();
        } while (1);
    }
}


void _SYNC_MEMORY(int *status, int stat_len, char *errmsg, int errmsg_len)
{
    LIBCAF_TRACE(LIBCAF_LOG_SYNC, "in sync memory");
    START_TIMER();
    comm_sync_memory(status, stat_len, errmsg, errmsg_len);
    STOP_TIMER(SYNC);
    LIBCAF_TRACE(LIBCAF_LOG_TIME, "comm_sync_memory");
}

void _SYNC_IMAGES(int images[], int image_count, int *status, int stat_len,
                  char *errmsg, int errmsg_len)
{
    int i;
    for (i = 0; i < image_count; i++) {
        LIBCAF_TRACE(LIBCAF_LOG_SYNC,
                     "call to comm_sync_images for sync with img%d",
                     images[i]);
        check_remote_image(images[i]);
        images[i];
    }
    START_TIMER();
    comm_sync_images(images, image_count, status, stat_len, errmsg,
                     errmsg_len);
    STOP_TIMER(SYNC);
    LIBCAF_TRACE(LIBCAF_LOG_TIME, "comm_sync_images");
}

void _SYNC_IMAGES_ALL(int *status, int stat_len, char *errmsg,
                      int errmsg_len)
{
    int i;
    int image_count = _num_images;
    int *images;
    LIBCAF_TRACE(LIBCAF_LOG_SYNC,
                 "before call to comm_sync_images for sync with all images");
    images = (int *) comm_malloc(_num_images * sizeof(int));
    for (i = 0; i < image_count; i++)
        images[i] = i + 1;
    START_TIMER();
    comm_sync_images(images, image_count, status, stat_len, errmsg,
                     errmsg_len);
    STOP_TIMER(SYNC);
    LIBCAF_TRACE(LIBCAF_LOG_TIME, "comm_sync_image_all");

    comm_free(images);
}

int _IMAGE_INDEX(DopeVectorType * diminfo, DopeVectorType * sub)
{
    if (diminfo == NULL || sub == NULL) {
        LIBCAF_TRACE(LIBCAF_LOG_FATAL,
                     "image_index failed for "
                     "&diminfo=%p, &codim=%p", diminfo, sub);
    }

    int i;
    int rank = diminfo->n_dim;
    int corank = diminfo->n_codim;
    int image = 0;
    int lb_codim, ub_codim;
    int *codim = (int *) sub->base_addr.a.ptr;
    int str_m = 1;

    if (sub->dimension[0].extent != corank)
        return 0;

    for (i = 0; i < corank; i++) {
        int extent;
        str_m = diminfo->dimension[rank + i].stride_mult;
        if (i == (corank - 1))
            extent = (_num_images - 1) / str_m + 1;
        else
            extent = diminfo->dimension[rank + i].extent;
        lb_codim = diminfo->dimension[rank + i].low_bound;
        ub_codim = diminfo->dimension[rank + i].low_bound + extent - 1;
        if (codim[i] >= lb_codim
            && (ub_codim == 0 || codim[i] <= ub_codim)) {
            image += str_m * (codim[i] - lb_codim);
        } else {
            return 0;
        }
    }

    if (_num_images > image)
        return image + 1;
    else
        return 0;
}

void _THIS_IMAGE1(DopeVectorType * ret, DopeVectorType * diminfo)
{
    int i;
    int corank = diminfo->n_codim;
    int *ret_int;
    if (diminfo == NULL || ret == NULL) {
        LIBCAF_TRACE(LIBCAF_LOG_FATAL,
                     "this_image failed for "
                     "&diminfo:%p and &ret:%p", diminfo, ret);
    }
    ret->base_addr.a.ptr = comm_malloc(sizeof(int) * corank);
    ret->dimension[0].low_bound = 1;
    ret->dimension[0].extent = corank;
    ret->dimension[0].stride_mult = 1;
    ret_int = (int *) ret->base_addr.a.ptr;
    for (i = 1; i <= corank; i++) {
        ret_int[i - 1] = _THIS_IMAGE2(diminfo, &i);
    }
}

int _THIS_IMAGE2(DopeVectorType * diminfo, int *sub)
{
    int img = _this_image - 1;
    int rank = diminfo->n_dim;
    int corank = diminfo->n_codim;
    int dim = *sub;
    int str_m = 1;
    int lb_codim = 0;
    int ub_codim = 0;
    int extent, i;

    if (diminfo == NULL) {
        LIBCAF_TRACE(LIBCAF_LOG_FATAL,
                     "this_image failed for &diminfo=%p", diminfo);
    }
    if (dim < 1 || dim > corank) {
        LIBCAF_TRACE(LIBCAF_LOG_FATAL,
                     "this_image failed as %d dim" " is not present", dim);
    }

    lb_codim = diminfo->dimension[rank + dim - 1].low_bound;
    str_m = diminfo->dimension[rank + dim - 1].stride_mult;
    if (dim == corank)
        extent = (_num_images - 1) / str_m + 1;
    else
        extent = diminfo->dimension[rank + dim - 1].extent;
    ub_codim = lb_codim + extent - 1;
    if (ub_codim > 0) {
        return (((img / str_m) % extent) + lb_codim);
    } else {
        return ((img / str_m) + lb_codim);
    }
}

int _LCOBOUND_2(DopeVectorType * diminfo, int *sub)
{
    int rank = diminfo->n_dim;
    int corank = diminfo->n_codim;
    int dim = *sub;
    if (diminfo == NULL) {
        LIBCAF_TRACE(LIBCAF_LOG_FATAL,
                     "lcobound failed for &diminfo:%p", diminfo);
    }
    if (dim < 1 || dim > corank) {
        LIBCAF_TRACE(LIBCAF_LOG_FATAL,
                     "lcobound failed as dim %d not present", dim);
    }
    return diminfo->dimension[rank + dim - 1].low_bound;
}

void _LCOBOUND_1(DopeVectorType * ret, DopeVectorType * diminfo)
{
    int i;
    int rank = diminfo->n_dim;
    int corank = diminfo->n_codim;
    int *ret_int;
    if (diminfo == NULL || ret == NULL) {
        LIBCAF_TRACE(LIBCAF_LOG_FATAL,
                     "lcobound failed for diminfo:%p and ret:%p",
                     diminfo, ret);
    }
    ret->base_addr.a.ptr = comm_malloc(sizeof(int) * corank);
    ret->dimension[0].low_bound = 1;
    ret->dimension[0].extent = corank;
    ret->dimension[0].stride_mult = 1;
    ret_int = (int *) ret->base_addr.a.ptr;
    for (i = 0; i < corank; i++) {
        ret_int[i] = diminfo->dimension[rank + i].low_bound;
    }
}

int _UCOBOUND_2(DopeVectorType * diminfo, int *sub)
{
    int rank = diminfo->n_dim;
    int corank = diminfo->n_codim;
    int dim = *sub;
    int extent;
    if (diminfo == NULL) {
        LIBCAF_TRACE(LIBCAF_LOG_FATAL,
                     "ucobound failed for &diminfo:%p", diminfo);
    }
    if (dim < 1 || dim > corank) {
        LIBCAF_TRACE(LIBCAF_LOG_FATAL,
                     "ucobound failed as dim %d not present", dim);
    }

    if (dim == corank)
        extent = (_num_images - 1) /
            diminfo->dimension[rank + dim - 1].stride_mult + 1;
    else
        extent = diminfo->dimension[rank + dim - 1].extent;

    return (diminfo->dimension[rank + dim - 1].low_bound + extent - 1);
}

void _UCOBOUND_1(DopeVectorType * ret, DopeVectorType * diminfo)
{
    int i;
    int rank = diminfo->n_dim;
    int corank = diminfo->n_codim;
    int *ret_int;
    int extent;
    if (diminfo == NULL || ret == NULL) {
        LIBCAF_TRACE(LIBCAF_LOG_FATAL,
                     "ucobound failed for diminfo:%p and ret:%p",
                     diminfo, ret);
    }
    ret->base_addr.a.ptr = comm_malloc(sizeof(int) * corank);
    ret->dimension[0].low_bound = 1;
    ret->dimension[0].extent = corank;
    ret->dimension[0].stride_mult = 1;
    ret_int = (int *) ret->base_addr.a.ptr;
    for (i = 0; i < corank; i++) {
        if (i == (corank - 1))
            extent = (_num_images - 1) /
                diminfo->dimension[rank + i].stride_mult + 1;
        else
            extent = diminfo->dimension[rank + i].extent;

        ret_int[i] = diminfo->dimension[rank + i].low_bound + extent - 1;
    }
}


static int is_contiguous_access(const size_t strides[],
                                const size_t count[], size_t stride_levels)
{
    int block_size = 1;
    int is_contig = 1;
    int i;

    block_size = count[0];
    for (int i = 1; i <= stride_levels; i++) {
        if (count[i] == 1)
            continue;
        else if (block_size == strides[i - 1])
            block_size = block_size * count[i];
        else
            return 0;
    }
    return 1;
}

static void local_src_strided_copy(void *src, const size_t src_strides[],
                                   void *dest, const size_t count[],
                                   size_t stride_levels)
{
    int i, j;
    size_t num_blks;
    size_t cnt_strides[stride_levels + 1];
    size_t blockdim_size[stride_levels];
    void *dest_ptr = dest;
    void *src_ptr = src;

    /* assuming src_elem_size=dst_elem_size */
    size_t blk_size = count[0];
    num_blks = 1;
    cnt_strides[0] = 1;
    blockdim_size[0] = count[0];
    for (i = 1; i <= stride_levels; i++) {
        cnt_strides[i] = cnt_strides[i - 1] * count[i];
        blockdim_size[i] = blk_size * cnt_strides[i];
        if (src_strides[i] == blockdim_size[i])
            blk_size = src_strides[i];
        else
            num_blks *= count[i];
    }

    for (i = 1; i <= num_blks; i++) {
        memcpy(dest_ptr, src_ptr, blk_size);
        dest_ptr += blk_size;
        for (j = 1; j <= stride_levels; j++) {
            if (i % cnt_strides[j])
                break;
            src_ptr -= (count[j] - 1) * src_strides[j - 1];
        }
        src_ptr += src_strides[j - 1];
    }
}

static void local_dest_strided_copy(void *src, void *dest,
                                    const size_t dest_strides[],
                                    const size_t count[],
                                    size_t stride_levels)
{
    int i, j;
    size_t num_blks;
    size_t cnt_strides[stride_levels + 1];
    size_t blockdim_size[stride_levels];
    void *dest_ptr = dest;
    void *src_ptr = src;

    /* assuming src_elem_size=dst_elem_size */
    size_t blk_size = count[0];
    num_blks = 1;
    cnt_strides[0] = 1;
    blockdim_size[0] = count[0];
    for (i = 1; i <= stride_levels; i++) {
        cnt_strides[i] = cnt_strides[i - 1] * count[i];
        blockdim_size[i] = blk_size * cnt_strides[i];
        if (dest_strides[i] == blockdim_size[i])
            blk_size = dest_strides[i];
        else
            num_blks *= count[i];
    }

    for (i = 1; i <= num_blks; i++) {
        memcpy(dest_ptr, src_ptr, blk_size);
        src_ptr += blk_size;
        for (j = 1; j <= stride_levels; j++) {
            if (i % cnt_strides[j])
                break;
            dest_ptr -= (count[j] - 1) * dest_strides[j - 1];
        }
        dest_ptr += dest_strides[j - 1];
    }
}

/*
 * image should be between 1 .. NUM_IMAGES
 */
int check_remote_image(size_t image)
{
    const int error_len = 255;
    char error_msg[error_len];
    memset(error_msg, 0, error_len);
    if (image < 1 || image > _num_images) {
        sprintf(error_msg,
                "Image %lu is out of range. Should be in [ %u ... %lu ].",
                (unsigned long) image, 1, (unsigned long) _num_images);
        Error(error_msg);
        /* should not reach */
    }
}

/*
 * address is either the address of the local symmetric variable that must be
 * translated to the remote image, or it is a remote address on the remote
 * image
 */
int check_remote_address(size_t image, void *address)
{
    const int error_len = 255;
    char error_msg[error_len];
    memset(error_msg, 0, error_len);

    if ((address < comm_start_symmetric_mem(_this_image - 1) ||
         address > comm_end_symmetric_mem(_this_image - 1)) &&
        (address < comm_start_asymmetric_heap(image - 1) ||
         address > comm_end_asymmetric_heap(image - 1))) {
        sprintf(error_msg,
                "Address %p (translates to %p) is out of range. "
                "Should fall within [ %p ... %p ] "
                "on remote image %lu.",
                address,
                (char *) address + comm_address_translation_offset(image -
                                                                   1),
                comm_start_shared_mem(image - 1),
                comm_end_shared_mem(image - 1), (unsigned long) image);
        Error(error_msg);
        /* should not reach */
    }
}
