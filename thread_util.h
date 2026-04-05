#pragma once
#include "data_struct.h"
#include "shmqueue/shm_queue.h"
#include "program_util.h"
#include "log_engine.h"
#include "fmt/core.h"
#include <string.h>
#include "unordered_map"
#include "vector"
#include "pubsub/pubsub.h"
#include "pubsub_protocol.h"



typedef std::function<void (pubsub::TCommand &)> FUNC;
class WorkerThread{
public:
    WorkerThread(int coreNum, FUNC f ) : busyFlag(false){
        pt = f;
        std::thread t([&](){
            pubsub::TCommand tcmd;
            while(1){
                if(myQ.pop(tcmd)){
                    pt(tcmd);
                    busyFlag = false;
                }
                else{
                    usleep(1);
                }
            }
        });
        crypto::set_cpu(t, coreNum);
        t.detach();
    }
    // void set_pt_handler(std::function<void (pubsub::TCommand &tcmd)> pt){

    // }
    bool try_add_job(pubsub::TCommand &tcmd){
        if(busyFlag == false){
            busyFlag = true;
            myQ.push(tcmd);
            return true;
        }
        return false;
    }

private:
    bool is_busy(){
        return busyFlag;
    }
    std::function<void (pubsub::TCommand &tcmd)> pt;
    pubsub::ConcurrentQueueBoost<pubsub::TCommand> myQ;
    std::atomic<bool> busyFlag;
};

class MyThreadPool{
public:
    MyThreadPool(size_t size, int coreNum, FUNC f){
        for(size_t i = 0; i < size; i++){
            WorkerThread *wt = new WorkerThread(coreNum+(i % 2), f);
            // WorkerThread *wt = new WorkerThread(coreNum+i, f);
            workerVec.push_back(wt);
        }
    }

    void force_to_add_job(pubsub::TCommand &tcmd){
        while(1){
            for(auto wt : workerVec){
                if(wt->try_add_job(tcmd)){
                    return;
                }
            }
        }
    }
private:
    std::vector<WorkerThread *> workerVec;
};
