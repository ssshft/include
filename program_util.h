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