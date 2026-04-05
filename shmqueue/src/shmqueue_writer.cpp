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
    while(1){
        pubsub::TCommand cmd;
        cmd.header.exchangeTypeEnum = ExchangeType_GATEIO;
        cmd.header.instTypeEnum = InstType_SPOT;
        strcpy(cmd.header.accountId,"qyv420220220");
        cmd.header.cmdTypeEnum = pubsub::CMD_CANCEL_ORDER;
        strcpy(cmd.body.cancelOrder.orderId, "helloworld");
        strcpy(cmd.body.cancelOrder.clientOrderId, "helloworld");
        strcpy(cmd.body.cancelOrder.instId, "BTC-USDT");
        cmd.header.insertTime = crypto::getCurrentTime();
        pQueue->push(cmd);
        cout << "enqueue time: " << cmd.header.insertTime << endl;
//        sleep(0.1);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
