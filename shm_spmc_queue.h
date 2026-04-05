#pragma once

#include <atomic>
#include <bits/stdc++.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>


template <class T, uint32_t CNT>
class SPMCQueue {
public:
    static_assert(CNT && !(CNT & (CNT - 1)), "CNT must bo a power of 2");

    struct Reader {
        operator bool() const {
            return q;
        }

        T* read() {
            auto& blk = q->blks[next_idx % CNT];
            uint32_t new_idx = ((std::atomic<uint32_t>*)&blk.idx)->load(std::memory_order_acquire);
            if (int(new_idx - next_idx) < 0) {
                return nullptr;
            }

            next_idx = new_idx + 1;
            return &blk.data;
        }

        T* readLast() {
            T* ret = nullptr;
            while (T* cur = read()) {
                ret = cur;
            }

            return ret;
        }

        T* getLast() {
            auto& blk = q->blks[(next_idx - 1) % CNT];
            return &blk.data;
        }

        SPMCQueue<T, CNT>* q = nullptr;
        uint32_t next_idx;
    };

    Reader getReader() {
        Reader reader;
        reader.q = this;
        reader.next_idx = write_idx + 1;
        return reader;
    }

    template <typename Writer>
    void write(Writer writer) {
        auto& blk = blks[++write_idx % CNT];
        writer(blk.data);
        ((std::atomic<uint32_t>*)&blk.idx)->store(write_idx, std::memory_order_release);
    }

    void write(const T& data) {
        auto& blk = blks[++write_idx % CNT];
        memcpy(&blk.data, &data, sizeof(T));
        ((std::atomic<uint32_t>*)&blk.idx)->store(write_idx, std::memory_order_release);
    }

private:
    friend class Reader;
    struct alignas(64) Block {
        uint32_t idx = 0;
        T data;
    } blks[CNT];
    alignas(128) uint32_t write_idx = 0;
};


template <class T>
inline T* shmmap_spmc(const char* filename) {
    int fd = shm_open(filename, O_CREAT | O_RDWR, 0666);
    fchmod(fd, 0666);
    
    if (fd == -1) {
        std::cerr << "shm_open failed: " << strerror(errno) << std::endl;
        return nullptr;
    }

    if (ftruncate(fd, sizeof(T))) {
        std::cerr << "ftruncate failed: " << strerror(errno) << std::endl;
        close(fd);
        return nullptr;
    }

    T* ret = (T*)mmap(0, sizeof(T), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (ret == MAP_FAILED) {
        std::cerr << "mmap failed: " << strerror(errno) << std::endl;
        return nullptr;
    }

    return ret;
}


namespace pubsub {
    constexpr uint32_t kCnt = 1024 * 64;
    template <class T>
    class SPMCPublisher {
        using Q = SPMCQueue<T, kCnt>;
    public:
        SPMCPublisher(const char* shm_file) {
            queue_ = shmmap_spmc<Q>(shm_file);
            if (!queue_) {
                const std::string& errmsg = std::string(shm_file) + " shmmap failed!";
                throw std::runtime_error(errmsg.c_str());
            }
        }

        void push(const T& data) {
            queue_->write(data);
        }

        void publish(const T& data) {
            queue_->write(data);
        }
    
    private:
        Q* queue_ = nullptr;
    };


    template <class T>
    class SPMCSubscriber {
        using Q = SPMCQueue<T, kCnt>;
        using Reader = typename SPMCQueue<T, kCnt>::Reader;
    public:
        SPMCSubscriber(const char* shm_file) {
            queue_ = shmmap_spmc<Q>(shm_file);
            if (!queue_) {
                const std::string& errmsg = std::string(shm_file) + " shmmap failed!";
                throw std::runtime_error(errmsg.c_str());
            }
            reader_ = queue_->getReader();
        }

        T* pop_last() {
            T* msg = reader_.readLast();
            return msg;
        }

        bool pop_last(T& data) {
            T* msg = reader_.readLast();
            if (msg) {
                memcpy(&data, msg, sizeof(T));
                return true;
            }
            else {
                return false;
            }
        }

        bool get_last(T& data) {
            T* msg = reader_.getLast();
            if (msg) {
                memcpy(&data, msg, sizeof(T));
                return true;
            }
            else {
                return false;
            }
        }

        T* get_last() {
            T* msg = pop_last();
            if (msg) {
                return msg;
            }
            else {
                return reader_.getLast();
            }
        }

        T* pop() {
            T* msg = reader_.read();
            return msg;
        }

        bool pop(T& data) {
            T* msg = reader_.read();
            if (msg) {
                memcpy(&data, msg, sizeof(T));
                return true;
            }
            else {
                return false;
            }
        }

    private:
        Q* queue_ = nullptr;
        Reader reader_;
    };
};