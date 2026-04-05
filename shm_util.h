#pragma once

#include <bits/stdc++.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
//#include "timestamp.h"
//#include "cpupin.h"
//#include "common.h"
#include <string>

namespace crypto{
template<class T>
inline T* shmmap(const char * filename) {
    int fd = shm_open(filename, O_CREAT | O_RDWR, 0666);
    if(fd == -1) {
        std::cerr << "shm_open failed: " << strerror(errno) << std::endl;
        return nullptr;
    }
    if(ftruncate(fd, sizeof(T))) {
        std::cerr << "ftruncate failed: " << strerror(errno) << std::endl;
        close(fd);
        return nullptr;
    }
    T* ret = (T*)mmap(0, sizeof(T), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if(ret == MAP_FAILED) {
        std::cerr << "mmap failed: " << strerror(errno) << std::endl;
        return nullptr;
    }
    return ret;
}

}