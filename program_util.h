#ifndef INCLUDE_PROGRAM_UTIL_H
#define INCLUDE_PROGRAM_UTIL_H
#include <stdio.h>
#include <sys/stat.h>
#include <fstream>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sched.h>
#include <thread>
#include <pthread.h>
#include <sys/types.h>
#include <iostream>
#include <filesystem>


namespace fs = std::filesystem;

namespace crypto {

    inline bool is_file_existed (const std::string& name) {
        struct stat buffer;
        return (stat (name.c_str(), &buffer) == 0);
    }

    inline std::string read_file(const char* filePath) {
        std::ifstream t(filePath);
        std::stringstream buffer;
        buffer << t.rdbuf();
        std::string contents(buffer.str());
        return contents;
    }

    inline int set_cpu(std::thread &th, int i){
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(i, &cpuset);
        int rc = pthread_setaffinity_np(th.native_handle(), sizeof(cpu_set_t), &cpuset);
        if (rc != 0) {
            std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
            return -1;
        }
        // cpu_set_t mask;
        // CPU_ZERO(&mask);
        // CPU_SET(i,&mask);
        // // printf("thread %u, i = %d\n",pthread_self(),i);
        // if(-1 == pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask))
        return 0;
    }

    // 把**调用线程自己**钉到核 i（i < 0 = 什么都不做，直接返回 0）。
    //
    // 和上面 set_cpu(std::thread&, int) 的区别 —— 不是风格问题，是时序问题：
    //   set_cpu 从外面钉一条 std::thread 对象，必须在对象还活着时调用；
    //   set_cpu_self 是线程自己钉自己，放在线程函数的**第一行**用。
    //   后者没有"线程已经被创建、但还没被钉"的窗口。对自旋线程这个窗口是真问题：
    //   它一醒来就会开始烧核，那几微秒烧在错误的核上，pin 完才搬家。
    //   另外 detach 出去的线程根本拿不到 std::thread 对象，只能用这个。
    //
    // 返回 0 = 成功（含 i<0 跳过），-1 = 失败（核号 ≥ nproc 时内核返回 EINVAL）。
    inline int set_cpu_self(int i) {
        if (i < 0) {
            return 0;
        }
        // CPU_SET 对 i >= CPU_SETSIZE 是**未定义行为**（越界写 cpu_set_t），
        // 不能指望 pthread_setaffinity_np 去拦 —— 它拿到的是已经被写坏的 mask。
        // 配置里手抖写个 5000 就会踩坏栈，所以这里先挡一道。
        if (i >= CPU_SETSIZE) {
            std::cerr << "set_cpu_self: core " << i << " >= CPU_SETSIZE(" << CPU_SETSIZE << ")\n";
            return -1;
        }
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(i, &cpuset);
        int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
        if (rc != 0) {
            std::cerr << "Error calling pthread_setaffinity_np(self, core " << i << "): " << rc << "\n";
            return -1;
        }
        return 0;
    }

    inline bool is_process_exist(long pid){
        struct stat sts;
        std::string path = "/proc/" + std::to_string((long long)pid);
        if (stat(path.c_str(), &sts) == -1 && errno == ENOENT) {
          return false;
        }
        return true;
    }

    inline long get_program_pid(const std::string program) {
        long pid = 0 ;
        std::string filename = std::string("/run/") + program + std::string(".pid");
        if ( !is_file_existed(filename) )
            return pid;
        std::ifstream iffile;
        iffile.open( filename );
        if ( iffile.fail() )
            throw std::runtime_error("get_program_pid failed: "   );
        iffile >> pid;
        iffile.close();
        return pid;
    }

    inline bool ensure_one_instance(const std::string program){
        auto pid = get_program_pid(program);
        return (!is_process_exist(pid));
    }

    inline int write_program_pid(const std::string program)
    {
        pid_t pid = getpid();
        std::ofstream iffile;
        iffile.open( std::string("/run/") + program + std::string(".pid") );
        if ( iffile.fail() )
            throw std::runtime_error("write_program_pid failed: "   );
        iffile << pid;
        iffile.close();
        return 0;
    }

    inline bool create_directory(const std::string& path) {
        bool created = false;
        try {
            // 创建所有不存在的目录
            fs::create_directories(path);
            created = true;
        } catch (const fs::filesystem_error& e) {
            fprintf(stderr, "Failed to create log directory %s: %s\n", path.c_str(), e.what());
        }
        return created;
    }

}
#endif //INCLUDE_PROGRAM_UTIL_H