#pragma once

#include <string>
#include <sys/time.h>
#include <atomic>
#include <stdlib.h>
#include <cstdlib>
#include <random>
#include <time.h>
#include <iostream>
#include "boost/date_time/posix_time/posix_time.hpp"


#include <cstdint>
#include <cstdio>
#include <ctime>

namespace crypto {

    class TscClock {
    public:
        static TscClock& instance() {
            static TscClock tscClock;
            return tscClock;
        }

        inline int64_t now_ns() {
            uint64_t tsc = rdtsc();
            if (__builtin_expect(tsc - last_sync_tsc > sync_interval_, 0)) {
                resync(tsc);
            }

            return base_ns_ + (int64_t)((tsc - base_tsc_) * ns_per_cycle_);
        }

    private:
        TscClock() {
            ns_per_cycle_ = calibrate();
            resync(rdtsc());
            sync_interval_ = (uint64_t)(30.0e9 * ns_per_cycle_);
        }

        void resync(uint64_t tsc) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            base_tsc_ = tsc;
            base_ns_ = ts.tv_sec * 1000000000LL + ts.tv_nsec;
            last_sync_tsc_ = tsc;
        }

        double calibrate() {
            double mhz = 0;
            FILE* f = fopen("/proc/cpuinfo", "r");
            if (f) {
                char line[256];
                while (fgets(line, sizeof(line), f)) {
                    if (sscanf(line, "cpu MHz : %lf", &mhz) == 1) {
                        break;
                    }
                
                }
                fclose(f);
            }

            if (mhz > 100.0) {
                return 1000.0 / mhz;
            }

            uint64_t t1 = rdtsc();
            struct timespec ts1;
            clock_gettime(CLOCK_REALTIME, &ts1);
            struct timespec req{0, 100000000};
            nanosleep(&req, nullptr);

            uint64_t t2 = rdtsc();
            struct timespec ts2;
            clock_gettime(CLOCK_REALTIME, &ts2); 

            int64_t ns = (ts2.tv_sec - ts1.tv_sec) * 1000000000LL + (ts2.tv_nsec - ts1.tv_nsec);
            return (double)ns / (t2 - t1);
        }

        static inline uint64_t rdtsc() {
            uint32_t lo, hi;

            asm volatile("rdtsc": "=a"(lo), "=d"(hi));
            return ((uint64_t)hi << 32) | lo;
        } 

        double ns_per_cycle_{1.0};
        uint64_t base_tsc_{0};
        int64_t base_ns_{0};
        uint64_t last_sync_tsc_{0};
        uint64_t sync_interval_{0};
    };

    inline int64_t getCurrentTimeNs() {
        return TscClock::instance().now_ns();
    }






    //产生(10,1000]的真随机数
    static std::random_device rd;
    static std::atomic<long> atomic_rdtscp_count((rd() % (100-1))+1);

    inline long rdtscp(){
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME,&ts);
        return (ts.tv_sec + atomic_rdtscp_count++) * 1000000000 + ts.tv_nsec;
    }
   
    inline unsigned long rdtsc() {
        return __builtin_ia32_rdtsc();
    }

    inline uint64_t get_rdtsc_timestamp() {
        uint32_t lo, hi;

        asm volatile("rdtsc": "=a"(lo), "=d"(hi));
        return ((uint64_t)hi << 32) | lo;
    }

    inline long getCurrentTimeNano() { // ns
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME,&ts);
        return ts.tv_sec*1000000000+ts.tv_nsec;
    }

    inline long getCurrentTime(){ // us
        struct timeval tv;
        gettimeofday(&tv, NULL);    //该函数在sys/time.h头文件中
        return tv.tv_sec * 1000000 + tv.tv_usec ;
    }

    inline long getCurrentTimeMilli() // ms
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);    //该函数在sys/time.h头文件中
        return (long)(tv.tv_sec * 1000 + tv.tv_usec * 0.001);
    }

    inline long getCurrentTimeSeconds(){ // s
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return tv.tv_sec;
    }

    inline string getTimestamp() {
        char timestamp[32]{0};
        time_t t;
        time(&t);
        struct tm* ptm = gmtime(&t);
        strftime(timestamp, 32, "%FT%T.123Z", ptm);
        return timestamp;
    }

    inline string get_date_str(){
        char dateStr[16] = {0};
        struct timeval tv;
        gettimeofday(&tv,NULL);
        time_t now;
        struct tm *tm_now;
        time(&now);
        tm_now = localtime(&now);
        sprintf(dateStr, "%d%02d%02d",
                tm_now->tm_year + 1900,
                tm_now->tm_mon + 1,
                tm_now->tm_mday  );
        return string(dateStr);
    }
}
