#ifndef __WZPERF__H
#define __WZPERF__H
#include <time.h>

timespec diff(timespec start, timespec end);
timespec diff(timespec start, timespec end)
{
    timespec temp;
    if ((end.tv_nsec-start.tv_nsec)<0) {
        temp.tv_nsec = 1000000000+end.tv_nsec-start.tv_nsec;
    } else {
        temp.tv_nsec = end.tv_nsec-start.tv_nsec;
    }
    return temp;
}
#define TIMEIT(f,name, ...) timespec _perf_time1, _perf_time2; \
                        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &_perf_time1); \
                        f(##__VA_ARGS__); \
                        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &_perf_time2); \
                        fprintf(stderr, "%s time: %lu\n",name, diff(_perf_time1, _perf_time2).tv_nsec)

#define TIMEIT_NO_ARGS(f,name) timespec _perf_time1, _perf_time2; \
                        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &_perf_time1); \
                        f(); \
                        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &_perf_time2); \
                        fprintf(stderr, "%s time: %lu\n",name, diff(_perf_time1, _perf_time2).tv_nsec)
#define PERF_START clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &_perf_time1)
#define PERF_END clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &_perf_time2)
#define PERF_DIFF diff(_perf_time1, _perf_time2).tv_nsec
#endif