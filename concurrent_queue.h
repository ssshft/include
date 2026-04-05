#pragma once

#include "concurrentqueue/concurrentqueue.h"
#include "readerwriterqueue.h"
#include <boost/lockfree/queue.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include "WFMPMC/WFMPMC.h"
#include <assert.h>
#include <stdio.h>
#include <nanomsg/nn.h>
#include <nanomsg/pipeline.h>
#include <nanomsg/pubsub.h>
#include "pubsub_protocol.h"
#include "time_util.h"
#include <zmq.hpp>


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

// #define RCOMMAND_FORMAT "{\"cmdTypeEnum\":\"%s\",\"body\":%s}"
namespace pubsub{

    class RCommandPuber{
    public:
        RCommandPuber(const char* url){
            puberSock = nn_socket (AF_SP, NN_PUB);
            // nn_setsockopt (puberSock, NN_SUB, NN_SUB_SUBSCRIBE, "ABC", 3);
            int linger = 1000;
            nn_setsockopt(puberSock, NN_SOL_SOCKET, NN_LINGER, &linger, sizeof (linger));
            nn_bind(puberSock, url);
        }

        ~RCommandPuber(){
            nn_shutdown (puberSock, 0);
        }

        void publish(const pubsub::RCommand &rcmd){
            /*
            if(rcmd.cmdTypeEnum == pubsub::CMD_RPT_BALANCE){
                char msg[512];
                sprintf(msg, RBALANCE_FORMAT, ExchangeTypeEnum2StrMap[rcmd.header.exchangeTypeEnum].c_str(), InstTypeEnum2StrMap[rcmd.header.instTypeEnum].c_str(),
                    rcmd.header.accountId, rcmd.header.strategyId,
                    rcmd.body.balance.currency,rcmd.body.balance.total, rcmd.body.balance.available, rcmd.body.balance.unrealizedPnl,rcmd.body.balance.frozen,
                    ApiSourceEnum2StrMap[rcmd.body.balance.apiSourceEnum].c_str(),rcmd.header.cmdTime
                );
                publish(msg);
            }
            else if(rcmd.header.cmdTypeEnum == pubsub::CMD_RPT_POSITION){
                char msg[512];
                sprintf(msg, RPOSITION_FORMAT, ExchangeTypeEnum2StrMap[rcmd.header.exchangeTypeEnum].c_str(), InstTypeEnum2StrMap[rcmd.header.instTypeEnum].c_str(),
                    rcmd.header.accountId, rcmd.header.strategyId,
                    rcmd.body.position.instId,DirectionEnum2StrMap[rcmd.body.position.direction].c_str(),
                    rcmd.body.position.volume, rcmd.body.position.maintMargin, rcmd.body.position.avgPrice,
                    rcmd.body.position.unrealizedPnl, rcmd.body.position.liquidPrice, rcmd.body.position.markPrice,
                    rcmd.body.position.adlQuantile,
                    ApiSourceEnum2StrMap[rcmd.body.position.apiSourceEnum].c_str(),rcmd.header.cmdTime
                );
                publish(msg);
            }
            else if(rcmd.header.cmdTypeEnum == pubsub::CMD_RPT_ORDER_TRADE){
                char msg[2048];
                sprintf(msg, RORDERTRADE_FORMAT, ExchangeTypeEnum2StrMap[rcmd.header.exchangeTypeEnum].c_str(), InstTypeEnum2StrMap[rcmd.header.instTypeEnum].c_str(),
                    rcmd.header.accountId, rcmd.header.strategyId,
                    rcmd.body.orderTrade.instId, rcmd.body.orderTrade.clientOrderId, rcmd.body.orderTrade.orderSysId,
                    rcmd.body.orderTrade.orderId, rcmd.body.orderTrade.strategyRef, OffsetFlagEnum2StrMap[rcmd.body.orderTrade.offsetFlag].c_str(),
                    DirectionEnum2StrMap[rcmd.body.orderTrade.direction].c_str(), OrderTypeEnum2StrMap[rcmd.body.orderTrade.orderType].c_str(),
                    OrderStatusEnum2StrMap[rcmd.body.orderTrade.orderStatus].c_str(),rcmd.body.orderTrade.volumeTotal,
                    rcmd.body.orderTrade.limitPrice, rcmd.body.orderTrade.reduceOnly == true ? "True" : "False", rcmd.body.orderTrade.tradePrice,
                    rcmd.body.orderTrade.volumeTraded, rcmd.body.orderTrade.isMaker == true ? "True" : "False", rcmd.body.orderTrade.tradedDiff,
                    ApiSourceEnum2StrMap[rcmd.body.orderTrade.apiSourceEnum].c_str(), rcmd.body.orderTrade.insertTime, rcmd.header.cmdTime,
                    rcmd.body.orderTrade.tsSent, rcmd.body.orderTrade.tsNet, rcmd.body.orderTrade.ErrorID,rcmd.body.orderTrade.originMsg
                );
                publish(msg);
            }
            else if(rcmd.header.cmdTypeEnum == pubsub::CMD_RPT_TOTAL_ACCOUNT){
                char msg[512];
                sprintf(msg, RTOTALACCOUNT_FORMAT, ExchangeTypeEnum2StrMap[rcmd.header.exchangeTypeEnum].c_str(), InstTypeEnum2StrMap[rcmd.header.instTypeEnum].c_str(),
                    rcmd.header.accountId, rcmd.header.strategyId,
                    rcmd.body.totalAccount.totalEquity, rcmd.body.totalAccount.adjEquity, rcmd.body.totalAccount.mmr, rcmd.body.totalAccount.mgnRatio,
                    ApiSourceEnum2StrMap[rcmd.body.totalAccount.apiSourceEnum].c_str(),rcmd.header.cmdTime
                );
                publish(msg);
            }
            else{

            }
            */
        }

        void publish(const char *msg){
            int sz_msg = strlen(msg) ; //+ 1 '\0' too
            // printf ("SERVER: PUBLISHING %s\n", msg);
            nn_send (puberSock, msg, sz_msg, 0);
        }

    protected:
        int puberSock=0;
    };


    //单写多读
    template<class Topic>
    class ConcurrentQueueNanoMsgPubSuber{
    public:
        ConcurrentQueueNanoMsgPubSuber(const char* url, bool isPuber = true){
            try{
                if(isPuber){
                    puberSock = nn_socket (AF_SP, NN_PUB);
                    int linger = 1000;
                    nn_setsockopt(puberSock, NN_SOL_SOCKET, NN_LINGER, &linger, sizeof (linger));
                    nn_bind(puberSock, url);
                }
                else{
                    suberSock = nn_socket(AF_SP, NN_SUB);
                    nn_setsockopt(suberSock, NN_SUB, NN_SUB_SUBSCRIBE, "", 0);
                    nn_connect(suberSock, url);
                }
            }
            catch(exception &e){
                fprintf(stderr, "%s,%s", __FUNCTION__, e.what());
            }

        }

        ~ConcurrentQueueNanoMsgPubSuber(){
            // delete pQueue;
        }

        void push(const Topic &topic){
            int bytes = nn_send(puberSock, &topic, sizeof(Topic), 0);
            // assert (bytes == sizeof(Topic));
        }

        bool pop(Topic &topic){
            try{
                char *buf = NULL;
                int bytes = nn_recv(suberSock, &buf, NN_MSG, 0);
                memset(&topic, 0, sizeof(Topic));
                memcpy(&topic, buf, sizeof(Topic));
                nn_freemsg (buf);
                return true;
            }
            catch(exception &e){
                cout << e.what() << endl;
                return false;
            }
        }

    protected:
        int suberSock=0;
        int puberSock=0;
        // char *buf = NULL;
    };


    //多写单读
    template<class Topic>
    class ConcurrentQueueNanoMsgPipeline{
    public:
        ConcurrentQueueNanoMsgPipeline(const char* url, bool sender = false){
            try{
                if(!sender){
                    pullSock = nn_socket(AF_SP, NN_PULL);
                    // assert (pullSock >= 0);
                    nn_bind(pullSock, url);
                }
                else{
                    pushSock = nn_socket(AF_SP, NN_PUSH);
                    // assert (pushSock >= 0);
                    nn_connect(pushSock, url);
                }
            }
            catch(exception &e){
                fprintf(stderr, "%s,%s", __FUNCTION__, e.what());
            }
        }

        ~ConcurrentQueueNanoMsgPipeline(){
            // delete pQueue;
        }

        bool push(const Topic &topic){
            int bytes = nn_send (pushSock, &topic, sizeof(Topic), NN_DONTWAIT);
            // printf ("NODE0: SENDING %d data", bytes);
            return (bytes == sizeof(Topic));
            // return true;
        }

        bool publish(const Topic &topic){
            int bytes = nn_send(pushSock, &topic, sizeof(Topic), NN_DONTWAIT);
            // printf ("NODE0: SENDING %d data", bytes);
            return (bytes == sizeof(Topic));
            // return true;
        }

        bool pop(Topic &topic){
            char *buf = NULL;
            //非阻塞下模式接收
            auto nbytes = nn_recv(pullSock, &buf, NN_MSG, NN_DONTWAIT);
            if(nbytes >= 0){
                memset(&topic, 0, sizeof(Topic));
                memcpy(&topic, buf, sizeof(Topic));
                nn_freemsg (buf);
                return true;
            }
            else{
                return false;
            }
        }

    protected:
        int pushSock=0;
        int pullSock=0;
        // char *buf = NULL;
    };


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
    template<class Topic, int32_t size = 10000>
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


    template <class Topic, int32_t QueueSize = 1024 * 16>
    class ConcurrentQueueZMQ {
    public:
        ConcurrentQueueZMQ() : context(1), sender(context, zmq::socket_type::push), receiver(context, zmq::socket_type::pull), running(true), pending_count(0) {
            endpoint = "inproc://" + generateUUID();
            sender.setsockopt(ZMQ_SNDHWM, QueueSize);
            receiver.setsockopt(ZMQ_RCVHWM, QueueSize);

            sender.bind(endpoint);
            receiver.connect(endpoint);
        }

        ~ConcurrentQueueZMQ() {
            stop();
        }

        void push(const Topic& topic) {
            zmq::message_t message(serializeSize(topic));
            serialize(topic, message.data());
            sender.send(message, zmq::send_flags::none);
            pending_count.fetch_add(1, std::memory_order_relaxed);
        }

        bool pop(Topic& topic) {
            zmq::message_t message;
            if (receiver.recv(message, zmq::recv_flags::none)) {
                deserialize(message.data(), message.size(), topic);
                pending_count.fetch_sub(1, std::memory_order_relaxed);
                return true;
            }
            return false;
        }

        int64_t get_left() {
            return pending_count.load(std::memory_order_relaxed);
        }

        void stop() {
            if (running) {
                running = false;
                receiver.close();
                sender.close();
                context.close();
            }
        }

    private:
        zmq::context_t context;
        zmq::socket_t sender;
        zmq::socket_t receiver;
        bool running;
        std::atomic<int64_t> pending_count;
        std::string endpoint;

        std::string generateUUID() { // 待优化
            static thread_local std::mt19937_64 rng(std::random_device{}());
            std::ostringstream oss;
            oss << std::hex << rng();
            return oss.str();
        }

        size_t serializeSize(const Topic& topic) {
            if constexpr (std::is_same_v<Topic, std::string>) {
                return topic.size();
            }
            else {
                static_assert(std::is_trivially_copyable_v<Topic>, "Only POD struct supported!");
                return sizeof(Topic);
            }
        }

        void serialize(const Topic& topic, void* buffer) {
            if constexpr (std::is_same_v<Topic, std::string>) {
                memcpy(buffer, topic.data(), topic.size());
            }
            else {
                memcpy(buffer, &topic, sizeof(Topic));
            }
        }

        void deserialize(const void* buffer, size_t size, Topic& topic) {
            if constexpr (std::is_same_v<Topic, std::string>) {
                topic.assign(static_cast<const char*>(buffer), size);
            }
            else {
                memcpy(&topic, buffer, sizeof(Topic));
            }
        }
    };


    // publisher
    template <typename T>
    class ConcurrentPubSubZMQPublisher {
    public:
        explicit ConcurrentPubSubZMQPublisher(const std::string& proxy_sub_endpoint) : context_(1), socket_(context_, zmq::socket_type::pub) {
            socket_.connect(proxy_sub_endpoint);
        }

        void publish(const std::string& topic, const T& data) {
            zmq::message_t topic_msg(topic.c_str(), topic.size());
            zmq::message_t data_msg(sizeof(T));
            std::memcpy(data_msg.data(), &data, sizeof(T));

            socket_.send(topic_msg, zmq::send_flags::sndmore);
            socket_.send(data_msg, zmq::send_flags::none);
        }

    private:
        zmq::context_t context_;
        zmq::socket_t socket_;
    };


    // subscriber
    template <typename T>
    class ConcurrentPubSubZMQSubscriber {
    public:
        ConcurrentPubSubZMQSubscriber(const std::string& proxy_pub_endpoint, const std::string& topic) : context_(1), socket_(context_, zmq::socket_type::sub) {
            socket_.connect(proxy_pub_endpoint);
            socket_.set(zmq::sockopt::subscribe, topic);
        }

        bool receive(std::string& topic, T& data) {
            zmq::message_t topic_msg;
            zmq::message_t data_msg;

            if (!socket_.recv(topic_msg, zmq::recv_flags::none)) return false;
            if (!socket_.recv(data_msg, zmq::recv_flags::none)) return false;

            topic.assign(static_cast<char*>(topic_msg.data()), topic_msg.size());
            if (data_msg.size() != sizeof(T)) return false;
            std::memcpy(&data, data_msg.data(), sizeof(T));
            return true;
        }

    private:
        zmq::context_t context_;
        zmq::socket_t socket_;
    };
}
