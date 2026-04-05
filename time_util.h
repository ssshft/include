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


using namespace std;
namespace crypto {
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
