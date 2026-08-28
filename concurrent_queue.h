#pragma once

#include "concurrentqueue/concurrentqueue.h"
#include "readerwriterqueue.h"
#include <boost/lockfree/queue.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include "WFMPMC/WFMPMC.h"
#include <assert.h>
#include <stdio.h>
#include "pubsub_protocol.h"
#include "time_util.h"


#include <iostream>

#define RBALANCE_FORMAT "{\"cmdTypeEnum\":\"CMD_RPT_BALANCE\",\"exchangeTypeEnum\":\"%s\",\"instTypeEnum\":\"%s\",\"accountId\":\"%s\",\"strategyId\":\"%s\","\
"\"currency\":\"%s\",\"total\":%.9f,\"available\":%.9f,\"unrealizedPnl\":%.9f,\"frozen\":%.9f, "\
"\"apiSourceEnum\":\"%s\",\"updateTime\":%ld}"

#define RPOSITION_FORMAT "{\"cmdTypeEnum\":\"CMD_RPT_POSITION\",\"exchangeTypeEnum\":\"%s\",\"instTypeEnum\":\"%s\",\"accountId\":\"%s\",\"strategyId\":\"%s\","\
"\"instId\":\"%s\",\"direction\":\"%s\",\"volume\":%.9f,\"maintMargin\":%.9f,\"avgPrice\":%.9f,\"unrealizedPnl\":%.9f, "\
"\"liquidPrice\":%.9f,\"markPrice\":%.9f,\"adlQuantile\":%.9f, "\
"\"apiSourceEnum\":\"%s\",\"updateTime\":%ld}"

#define RORDERTRADE_FORMAT "{\"cmdTypeEnum\":\"CMD_RPT_ORDER_TRADE\",\"exchangeTypeEnum\":\"%s\",\"instTypeEnum\":\"%s\",\"accountId\":\"%s\",\"strategyId\":\"%s\","\
"\"instId\":\"%s\",\"clientOrderId\":%ld,\"orderSysId\":\"%s\",\"orderId\":\"%s\",\"strategyRef\":\"%s\", "\
"\"offsetFlag\":\"%s\",\"direction\":\"%s\",\"orderType\":\"%s\",\"orderStatus\":\"%s\",\"volumeTotal\":%.9f, "\
"\"limitPrice\":%.12f,\"reduceOnly\":\"%s\",\"tradePrice\":%.9f,\"volumeTraded\":%.9f,\"isMaker\":\"%s\", "\
"\"tradedDiff\":%.9f,\"apiSourceEnum\":\"%s\",\"insertTime\":%ld,\"updateTime\":%ld,\"tsSent\":%ld, "\
"\"tsNet\":%ld,\"ErrorID\":%d,\"originMsg\":\"%s\" }"

#define RTOTALACCOUNT_FORMAT "{\"cmdTypeEnum\":\"CMD_RPT_TOTAL_ACCOUNT\",\"exchangeTypeEnum\":\"%s\",\"instTypeEnum\":\"%s\",\"accountId\":\"%s\",\"strategyId\":\"%s\","\
"\"totalEquity\":%.9f,\"adjEquity\":%.9f,\"mmr\":%.9f,\"mgnRatio\":%.9f, "\
"\"apiSourceEnum\":\"%s\",\"updateTime\":%ld}"


namespace pubsub{
    template <class Topic, int32_t size = 60000>
    class ConcurrentQueueBoost{
        typedef boost::lockfree::queue<Topic, boost::lockfree::capacity<size>> Boost_Queue;

    public:
        ConcurrentQueueBoost(){
            pQueue = new Boost_Queue();
        }
        ~ConcurrentQueueBoost() {
            //delete pQueue;
        }

        void push(const Topic &topic){
            pQueue->push(topic);
        }

        bool pop(Topic &topic){
            if (pQueue->pop(topic)){
                return true;
            }
            return false;
        }

        size_t get_left(){
            return 0;
        }

    protected:
        //        moodycamel::ConcurrentQueue<Topic> *pQueue=nullptr;
        Boost_Queue *pQueue = nullptr;
    };

    //单写单读无锁队列
    template<class Topic, int32_t size = 60000>
    class SPSCQueue{
        typedef boost::lockfree::spsc_queue<Topic, boost::lockfree::capacity<size>> Boost_Queue;
    public:
        SPSCQueue() {
            pQueue = new Boost_Queue();
        }
        ~SPSCQueue(){
            delete pQueue;
        }

        void push(const Topic &topic){
        pQueue->push(topic);
    }

    bool pop(Topic &topic){
        if (pQueue->pop(topic)){
            return true;
        }
        return false;
    }

        int64_t get_left() {
            return 0;
        }

    protected:
        Boost_Queue *pQueue=nullptr;
    };


    template <class Topic, int32_t size = 1024>
    class ConcurrentQueueWF
    {
        typedef WFMPMC<Topic, size> WF_QUEUE;
    public:
        ConcurrentQueueWF(){
            pQueue = new WF_QUEUE();

        }
        ~ConcurrentQueueWF(){
//            delete pQueue;
        }

        void push(const Topic &topic){
            pQueue->emplace(topic);
        }

        bool pop(Topic &topic){
            if(pQueue->tryPop(topic)){
                return true;
            }
            return false;
        }

        int64_t get_left(){
            return pQueue->size();
        }

    protected:
        WF_QUEUE *pQueue=nullptr;
    };
}
