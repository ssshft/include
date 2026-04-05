#include <iostream>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <list>
#include "shm_queue.h"
#include "pubsub_protocol.h"
#include "shm_global.h"
#include "time_util.h"
using namespace std;
typedef pubsub::ShmQueue<pubsub::TCommand> Queue;

int main(int argc, const char *argv[])
{
    Queue *pQueue = new Queue(TBCryptoTCommandField,1024,false,true);
    pubsub::TCommand cmd;
    while(1){
       if(pQueue->pop(cmd)){
           auto now = crypto::getCurrentTime();
           cout << "delay: "<< now - cmd.header.insertTime << " ,dequeue time:" << cmd.header.insertTime << endl;
       }

    }
}
