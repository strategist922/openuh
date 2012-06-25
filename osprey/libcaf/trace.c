/*
 Runtime library for supporting Coarray Fortran 

 Copyright (C) 2011 University of Houston.

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


#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>

#include "caf_rtl.h"
#include "trace.h"

#define BUF_SIZE 256

extern unsigned long _this_image;
extern unsigned long _num_images;
extern struct shared_memory_slot *common_slot;

typedef enum {
    OFF = 0,
    ON,
} __trace_state_t;   /* tracing states */

typedef struct {
    libcaf_trace_t level;
    char *text;
    __trace_state_t state;
} __trace_table_t;

#define INIT_LEVEL(L, State) { LIBCAF_LOG_##L,  #L, State }

static
__trace_table_t tracers[] =
{
    INIT_LEVEL(FATAL,       ON),
    INIT_LEVEL(DEBUG,       OFF),
    INIT_LEVEL(TIME,        OFF),
    INIT_LEVEL(NOTICE,      OFF),
    INIT_LEVEL(TIME_SUMMARY,OFF),
    INIT_LEVEL(INIT,        OFF),
    INIT_LEVEL(MEMORY,      OFF),
    INIT_LEVEL(CACHE,       OFF),
    INIT_LEVEL(BARRIER,     OFF),
    INIT_LEVEL(REDUCE,      OFF),
    INIT_LEVEL(SYMBOLS,     OFF)
};

static int
__trace_enable(libcaf_trace_t level)
{
    int i;
    if (level < NUM_TRACERS) {
        tracers[level].state = ON;
        return 1;
    }
    return 0;
}

static int
__trace_enable_text(char *trace)
{
    int i;
    for (i = 0; i < NUM_TRACERS; i++) {
        if (strcasecmp(trace, tracers[i].text) == 0) {
            tracers[i].state = ON;
            return 1;
        }
    }
    return 0;
}


static void
__trace_enable_all(void)
{
    int i;

    for (i = 0; i < NUM_TRACERS; i++) {
        tracers[i].state = ON;
    }
}

static char *
__level_to_string(libcaf_trace_t level)
{
    if (level < NUM_TRACERS) {
        return tracers[level].text;
    }
    return "?";
}

static void
show_trace_levels(void)
{
    char buf[BUF_SIZE];
    int i;
    strcpy(buf, "Enabled Messages: ");

    for (i = 0; i < NUM_TRACERS; i++) {
        if (tracers[i].state == ON) {
            strncat(buf, tracers[i].text, strlen(tracers[i].text));
            strncat(buf, " ", 1);
        }
    }

    __libcaf_trace(LIBCAF_LOG_INIT, buf);
}

int __trace_is_enabled(libcaf_trace_t level)
{
    if (level < NUM_TRACERS)
        return tracers[level].state;
    else
        return 0;
}

static FILE *trace_log_stream;

/*
 * default log to stderr. If env variable set, try to append to that file
 * instead. Otherwise, fall back to std err
 */
static void
logging_filestream_init(void)
{
    char *logfile;
    FILE *fp;

    trace_log_stream = stderr;

    logfile = getenv("UHCAF_TRACE_FILE");
    if (logfile == (char *) NULL)  return;

    fp = fopen(logfile, "a");
    if (fp != (FILE *) NULL) {
        trace_log_stream = fp;
    }
}

void
__libcaf_tracers_init(void)
{
    char *caf_tracelevels = getenv("UHCAF_TRACE");
    logging_filestream_init();

    if (caf_tracelevels != (char *) NULL) {
        char *opt = strtok(caf_tracelevels, ",:;");

        while (opt != (char *) NULL) {
            if (strcasecmp(opt, "all") == 0) {
                __trace_enable_all();
                break;
            }
            (void) __trace_enable_text(opt);
            opt = strtok( (char *)NULL, ",:;");
        }
    }

    /*It TIME is enabled, TIME_SUMMARY should also be enabled by default*/
    if ( __trace_is_enabled(LIBCAF_LOG_TIME))
    {
        (void) __trace_enable_text("TIME_SUMMARY");
    }

    show_trace_levels();
}

static void __trace_time(__timer_start_stop_t event, __timer_type_t type,
                            char *tmp)
{
    static clock_t start_cpu_clock;
    clock_t end_cpu_clock;
    static double elapsed_cpu_time;
    static double rolled_up_cpu_time[4] = {0,0,0,0};
    /* 4 = number of elements in enum __timer_type_t*/

    struct timeval wall_time_struct;
    static double start_wall_time;
    double end_wall_time;
    static double elapsed_wall_time;
    static double rolled_up_wall_time[4] = {0,0,0,0};
    /* 4 = number of elements in enum __timer_type_t*/


    if ( event == START )
    {
        start_cpu_clock = clock();
        gettimeofday(&wall_time_struct, NULL);
        start_wall_time = wall_time_struct.tv_sec
                    + (wall_time_struct.tv_usec/1000000.0);
    }
    else if ( event == STOP )
    {
        end_cpu_clock = clock();
        gettimeofday(&wall_time_struct, NULL);
        end_wall_time = wall_time_struct.tv_sec
                    + (wall_time_struct.tv_usec/1000000.0);

        elapsed_wall_time=end_wall_time-start_wall_time;
        elapsed_cpu_time =
            ((double)(end_cpu_clock-start_cpu_clock))/CLOCKS_PER_SEC;

        rolled_up_cpu_time[type] += elapsed_cpu_time;
        rolled_up_wall_time[type] += elapsed_wall_time;
    }
    else if ( event == PRINT )
    {
        snprintf(tmp, BUF_SIZE, "Wall-clock time: %lf, CPU time: %lf",
                elapsed_wall_time, elapsed_cpu_time);
    }
    else /* Accumulate */
    {
        snprintf(tmp, BUF_SIZE,
            "\nTotal time for INIT: wall-clock=%lf, CPU= %lf\n"
            "Total time for READ: wall-clock=%lf, CPU= %lf\n"
            "Total time for WRITE: wall-clock=%lf, CPU= %lf\n"
            "Total time for SYNC: wall-clock=%lf, CPU= %lf\n",
            rolled_up_wall_time[INIT], rolled_up_cpu_time[INIT],
            rolled_up_wall_time[READ], rolled_up_cpu_time[READ],
            rolled_up_wall_time[WRITE], rolled_up_cpu_time[WRITE],
            rolled_up_wall_time[SYNC], rolled_up_cpu_time[SYNC]);
    }
}

void __stop_timer(__timer_type_t type)
{
    if ( (!__trace_is_enabled(LIBCAF_LOG_TIME)) &&
            (!__trace_is_enabled(LIBCAF_LOG_TIME_SUMMARY)))
    {
        return;
    }

    __trace_time(STOP, type, (char*)0);

}

void __start_timer()
{
    if ( (!__trace_is_enabled(LIBCAF_LOG_TIME)) &&
            (!__trace_is_enabled(LIBCAF_LOG_TIME_SUMMARY)))
    {
        return;
    }

    __trace_time(START, DUMMY, (char*)0);
}

/*This is a function which prints the entire shared memory table*/
static void __print_shared_memory_slots()
{
    struct shared_memory_slot *temp_slot;
    fprintf(trace_log_stream,
         "Printing the shared memory slot info of image%lu:\n"
         "Above common-slot: ",_this_image);
    fflush(trace_log_stream);

    temp_slot = common_slot->prev;
    while (temp_slot)
    {
        fprintf(trace_log_stream, "addr=%p-size=%lu-feb=%u, ",
                temp_slot->addr, temp_slot->size, temp_slot->feb);
        temp_slot = temp_slot->prev;
    }
    fprintf(trace_log_stream,"\n");
    fflush(trace_log_stream);

    fprintf(trace_log_stream,"Common-slot & below: ");
    temp_slot = common_slot;
    while (temp_slot)
    {
        fprintf(trace_log_stream, "addr=%p-size=%lu-feb=%u, ",
                temp_slot->addr, temp_slot->size, temp_slot->feb);
        temp_slot = temp_slot->next;
    }
    fprintf(trace_log_stream,"\n");
    fflush(trace_log_stream);
}

void
__libcaf_trace(libcaf_trace_t msg_type, char*fmt, ...)
{
    if (! __trace_is_enabled(msg_type)) {
        return;
    }

    {
        char tmp1[BUF_SIZE];
        char tmp2[BUF_SIZE];
        va_list ap;

        snprintf(tmp1, BUF_SIZE, "LIBCAF(IMG %lu)[%s]:", _this_image,
                __level_to_string(msg_type));

        va_start(ap, fmt);
        vsnprintf(tmp2, BUF_SIZE, fmt, ap);
        va_end(ap);

        strncat(tmp1, tmp2, BUF_SIZE-strlen(tmp1)-1);

        if (msg_type == LIBCAF_LOG_TIME) {
            char time_str[BUF_SIZE];
            __trace_time(PRINT, DUMMY, time_str);
            strncat(tmp1, time_str, BUF_SIZE-strlen(tmp1)-1);
        }

        if (msg_type==LIBCAF_LOG_TIME_SUMMARY) {
            char time_str[BUF_SIZE];
            __trace_time(PRINT_ROLLUP, DUMMY, time_str);
            strncat(tmp1, time_str, BUF_SIZE-strlen(tmp1)-1);
        }

        strncat(tmp1, "\n", BUF_SIZE-strlen(tmp1)-1);

        fputs(tmp1, trace_log_stream);
        fflush(trace_log_stream);

        /* Print shared memory slots only for image 1 */
        if (msg_type == LIBCAF_LOG_MEMORY && _this_image==1) {
            __print_shared_memory_slots();
        }

    }

    if (msg_type == LIBCAF_LOG_FATAL) {
        /* TODO: need to abort running image and send signal to other images
         * to exit ASAP */
        caf_exit_(1);
    }

}
