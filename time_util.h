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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>

#include "hft_time.h"

namespace crypto {

    // 兼容旧代码：TSC 锚到 REALTIME，每 30s 重同步；
    // 不要用于新的延时统计。新代码用 hft::time::mono_ns()。
    class TscClock {
    public:
        static TscClock& instance() {
            static TscClock tscClock;
            return tscClock;
        }

        inline int64_t now_ns() {
            uint64_t tsc = rdtsc();
            if (__builtin_expect(tsc - last_sync_tsc_ > sync_interval_, 0)) {
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
            struct timespec req{0, 100000000LL};
            nanosleep(&req, nullptr);

            uint64_t t2 = rdtsc();
            struct timespec ts2;
            clock_gettime(CLOCK_REALTIME, &ts2); 

            int64_t ns = (ts2.tv_sec - ts1.tv_sec) * 1000000000LL + (ts2.tv_nsec - ts1.tv_nsec);
            return (double)ns / (t2 - t1);
        }

        static inline uint64_t rdtsc() {
            return hft::time::tsc_ticks();
        } 

        double ns_per_cycle_{1.0};
        uint64_t base_tsc_{0};
        int64_t base_ns_{0};
        uint64_t last_sync_tsc_{0};
        uint64_t sync_interval_{0};
    };

    // 单调 ns：用于差值/延时。墙钟 ns 用 getCurrentTimeNano()。
    // 旧实现是 TSC 锚到 REALTIME；项目内无调用点，现统一为 CLOCK_MONOTONIC。
    inline int64_t getCurrentTimeNs() {
        return static_cast<int64_t>(hft::time::mono_ns());
    }

    // 单调 µs：给“单位已经是 µs、但需要算差值”的旧代码用。
    // 墙钟 µs 仍然用 getCurrentTime()。
    inline int64_t getMonotonicTime() {
        return hft::time::mono_us();
    }


    //产生(10,1000]的真随机数
    static std::random_device rd;
    static std::atomic<long> atomic_rdtscp_count((rd() % (100-1))+1);

    // 注意：这不是 RDTSCP，也不是时间戳，而是“REALTIME + 原子计数”的唯一 ID
    // 生成器。新代码建议直接用 next_unique_id()。
    inline int64_t rdtscp(){
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME,&ts);
        return (ts.tv_sec + atomic_rdtscp_count++) * 1000000000LL + ts.tv_nsec;
    }

    inline int64_t next_unique_id() {
        return rdtscp();
    }
   
    // 原始 TSC tick，不是 ns；只用于同一线程内的短段或唯一 ID。
    inline  uint64_t rdtsc() {
        return hft::time::tsc_ticks();
    }

    // 原始 TSC tick，不是 ns；用于唯一 ID/排序。
    inline int64_t get_rdtsc_timestamp() {
        return static_cast<int64_t>(hft::time::tsc_ticks());
    }

    // 墙钟 ns：会受 NTP 影响，不要用于差值。
    inline int64_t getCurrentTimeNano() { // ns
        return static_cast<int64_t>(hft::time::wall_ns());
    }

    // 墙钟 µs：兼容旧代码。
    inline int64_t getCurrentTime() { // us
        return hft::time::wall_us();
    }

    inline int64_t getCurrentTimeMilli() { // ms
        return hft::time::wall_ms();
    }

    inline int64_t getCurrentTimeSeconds(){ // s
        return hft::time::wall_s();
    }

    inline std::string getTimestampIso() {
        using namespace std::chrono;
        auto now = system_clock::now();
        auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
        std::time_t t = system_clock::to_time_t(now);
        std::tm tm{};
        gmtime_r(&t, &tm);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec,
                      static_cast<long long>(ms.count()));
        return buf;
    }

    inline std::string get_date_str(){
        char dateStr[16] = {0};
        struct timeval tv;
        gettimeofday(&tv,NULL);
        time_t now;
        struct tm *tm_now;
        time(&now);
        tm_now = localtime(&now);
        std::snprintf(dateStr, sizeof(dateStr), "%d%02d%02d",
                tm_now->tm_year + 1900,
                tm_now->tm_mon + 1,
                tm_now->tm_mday  );
        return std::string(dateStr);
    }
}
