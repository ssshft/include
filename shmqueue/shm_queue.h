#ifndef INCLUDE_SHM_QUEUE_H
#define INCLUDE_SHM_QUEUE_H
#include "shmmqueue.h"
#include <cstring>
using namespace std;
namespace pubsub{
    template<class Topic>
    class ShmQueue{
    public:
        ShmQueue(key_t shmkey, size_t size=1024*64, bool multiRead=false, bool multiWrite=true){
            size_t queuesize = sizeof(Topic) * size;
            shmmqueue::eQueueModel queueModule = shmmqueue::eQueueModel::MUL_READ_MUL_WRITE;
            if(multiRead == false && multiWrite == true){
                queueModule = shmmqueue::eQueueModel::ONE_READ_MUL_WRITE;
            }
            else if(multiRead == false && multiWrite == false){
                queueModule = shmmqueue::eQueueModel::ONE_READ_ONE_WRITE;
            }
            else if(multiRead == true && multiWrite == false){
                queueModule = shmmqueue::eQueueModel::MUL_READ_ONE_WRITE;
            }
            else{

            }
            pQueue = shmmqueue::CMessageQueue::CreateInstance(shmkey, queuesize, queueModule);
        }

        ~ShmQueue(){
            // delete pQueue;
        }

        void push(const Topic &topic){
            pQueue->SendMessage((shmmqueue::BYTE *)&topic, sizeof(Topic));
        }

        bool pop(Topic &topic){
            int len = pQueue->GetMessage((shmmqueue::BYTE *) &topic);
            if(len > 0){
                return true;
            }
            return false;
        }

    private:
        shmmqueue::CMessageQueue *pQueue = nullptr;
//        shmmqueue::BYTE m_data[sizeof(Topic)] = {0};
    };
}

#endif //INCLUDE_SHM_QUEUE_H
