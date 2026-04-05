#pragma once
//#include "shm_topic.h"
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
#include "WFMPMC.h"
#include "shm_util.h"
//using MsgQ = WFMPMC<Topic, size>;

namespace pubsub{
    template<class Topic,int size>
    class MPMC_Shm_Queue{
        using MsgQ = WFMPMC<Topic, size> ;
    public:
        MPMC_Shm_Queue(const char *topic){
            q = getMsgQ(topic);
        }

        ~MPMC_Shm_Queue(){
            delete q;
        }

        void push(Topic &data){
            if(!q->full())
                q->tryPush(data);
//            while(!q->tryPush(data));
//            auto idx = q->getWriteIdx();
//            Topic* d;
//            while((d = q->getWritable(idx)) == nullptr)
//            memcpy(d,&data,sizeof(data)+1);
//            q->commitWrite(idx);
        }

        bool pop(Topic &data){
            if(q->empty()){
                return false;
            }
            if(q->tryPop(data)){
                return true;
            }
            return false;
            #if 0
            if(q->empty()){
                return false;
            }
            data = q->pop();
            return true;
            #endif
        }
    private:
        MsgQ* getMsgQ(const char *filename){
            std::string path = "/";
            path += filename;
            fprintf(stdout,"%s,size of msgq:%d\n",path.c_str(),sizeof(MsgQ));
            return crypto::shmmap<MsgQ>(path.c_str());
        }

    protected:
        MsgQ* q;
    };
}