//
// Created by qyang on 2022/1/17.
//

#ifndef DB_SHM_TOPIC_H
#define DB_SHM_TOPIC_H
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

#include "PubSubQueue.h"
//#include "WFMPMC.h"
namespace pubsub {
    typedef PubSubQueue<1024 * 1024 * 1024> MsgQ;

    template<class T>
    inline T *shmmap(const char *filename) {
        int fd = shm_open(filename, O_CREAT | O_RDWR, 0666);
        if (fd == -1) {
            std::cerr << "shm_open failed: " << strerror(errno) << std::endl;
            return nullptr;
        }
        if (ftruncate(fd, sizeof(T))) {
            std::cerr << "ftruncate failed: " << strerror(errno) << std::endl;
            close(fd);
            return nullptr;
        }
        T *ret = (T *) mmap(0, sizeof(T), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (ret == MAP_FAILED) {
            std::cerr << "mmap failed: " << strerror(errno) << std::endl;
            return nullptr;
        }
        return ret;
    }

    inline MsgQ *getMsgQ(const char *topic) {
        std::string path = "/";
        path += topic;
        return shmmap<MsgQ>(path.c_str());
    }
}

#endif //DB_SHM_TOPIC_H
