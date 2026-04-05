#include "python_interface/include/python_interface.h"

class TCommandPubSuber{
    typedef pubsub::ConcurrentQueueNanoMsgPipeline<pubsub::TCommand> QueuePipeline;
public:
    QueuePipeline *m_pTqueue;

    TCommandPubSuber(const char *url){
        m_pTqueue = new QueuePipeline(url, true);
    }

    ~TCommandPubSuber(){
        // if(m_pTqueue != nullptr){
        //     delete m_pTqueue;
        // }
    }

    // bool pop(pubsub::TCommand &cmd) {
    //     return m_pTqueue->pop(cmd);
    // }

    void push(const pubsub::TCommand &tcmd) {
        m_pTqueue->push(tcmd);
    }

    void publish(const pubsub::TCommand &tcmd) {
        m_pTqueue->push(tcmd);
    }


};

class RCommandPubSuber{
    typedef pubsub::ConcurrentQueueNanoMsgPubSuber<pubsub::RCommand> QueuePubSuber;
public:
    QueuePubSuber *m_pTqueue;

    RCommandPubSuber(const char *url){
        m_pTqueue = new QueuePubSuber(url, false);
    }

    ~RCommandPubSuber(){
        // if(m_pTqueue != nullptr){
        //     delete m_pTqueue;
        // }
    }

    bool pop(pubsub::RCommand &rcmd){
        return m_pTqueue->pop(rcmd);
    }

    // void push(const pubsub::RCommand &cmd) {
    //     m_pTqueue->push(cmd);
    // }
};

#if 0
class TCommandPubSuber{
public:
//    char topicStr[128];
    CMD_QUEUE *m_pTqueue;

    TCommandPubSuber(int topic){
        m_pTqueue = new CMD_QUEUE(topic, 1024);
    }

    ~TCommandPubSuber(){
        if(m_pTqueue != nullptr){
            delete m_pTqueue;
        }
    }

    bool pop(pubsub::TCommand &cmd) {
        return m_pTqueue->pop(cmd);
    }

    void push(const pubsub::TCommand &cmd) {
        m_pTqueue->push(cmd);
    }
};

class RCommandPubSuber{
public:
    RPT_QUEUE *m_pTqueue;

    RCommandPubSuber(const char *topic){
        m_pTqueue = new RPT_QUEUE(topic);
    }

    ~RCommandPubSuber(){
        if(m_pTqueue != nullptr){
            delete m_pTqueue;
        }
    }

    bool pop(pubsub::RCommand &cmd) {
        return m_pTqueue->pop(cmd);
    }

    void push(const pubsub::RCommand &cmd) {
        m_pTqueue->push(cmd);
    }
};
#endif
#if 0
class CryptoMarketDataPubSuber{
public:
//    char topicStr[128];
    RPT_QUEUE *m_pTqueue;

    CryptoMarketDataPubSuber(const char *topic){
        m_pTqueue = new RPT_QUEUE(topic);
    }

    ~CryptoMarketDataPubSuber(){
        if(m_pTqueue != nullptr){
            delete m_pTqueue;
        }
    }

    bool pop(pubsub::RCommand &cmd) {
        return m_pTqueue->pop(cmd);
    }

    void push(const pubsub::RCommand &cmd) {
        m_pTqueue->push(cmd);
    }
};
#endif

BOOST_PYTHON_MODULE(pypubsub){
    class_<pubsub::TNewOrder>("TNewOrder",init<>())
        .def("getInstId", &pubsub::TNewOrder::getInstId)
        .def("setInstId", &pubsub::TNewOrder::setInstId)
        .ADD_PROPERTY(pubsub::TNewOrder, instId)
        .def_readwrite("clientOrderId",&pubsub::TNewOrder::clientOrderId)
        .def("getOrderSysId", &pubsub::TNewOrder::getOrderSysId)
        .def("setOrderSysId", &pubsub::TNewOrder::setOrderSysId)
        .ADD_PROPERTY(pubsub::TNewOrder, orderSysId)
        .def("getStrategyRef", &pubsub::TNewOrder::getStrategyRef)
        .def("setStrategyRef", &pubsub::TNewOrder::setStrategyRef)
        .ADD_PROPERTY(pubsub::TNewOrder, strategyRef)
        .def_readwrite("offsetFlag",&pubsub::TNewOrder::offsetFlag)
        .def_readwrite("direction",&pubsub::TNewOrder::direction)
        .def_readwrite("orderType",&pubsub::TNewOrder::orderType)
        .def_readwrite("volumeTotal",&pubsub::TNewOrder::volumeTotal)
        .def_readwrite("limitPrice",&pubsub::TNewOrder::limitPrice)
        .def_readwrite("reduceOnly",&pubsub::TNewOrder::reduceOnly)
        .def("getString", &pubsub::TNewOrder::getString)
    ;

    class_<pubsub::RNewOrder>("RNewOrder",init<>())
        .def("getInstId", &pubsub::RNewOrder::getInstId)
        .def("setInstId", &pubsub::RNewOrder::setInstId)
        .ADD_PROPERTY(pubsub::RNewOrder, instId)
        .def_readwrite("clientOrderId",&pubsub::RNewOrder::clientOrderId)
        .def("getOrderSysId", &pubsub::RNewOrder::getOrderSysId)
        .def("setOrderSysId", &pubsub::RNewOrder::setOrderSysId)
        .ADD_PROPERTY(pubsub::RNewOrder, orderSysId)
        .def("getStrategyRef", &pubsub::RNewOrder::getStrategyRef)
        .def("setStrategyRef", &pubsub::RNewOrder::setStrategyRef)
        .ADD_PROPERTY(pubsub::RNewOrder, strategyRef)
        .def_readwrite("offsetFlag",&pubsub::RNewOrder::offsetFlag)
        .def_readwrite("direction",&pubsub::RNewOrder::direction)
        .def_readwrite("orderType",&pubsub::RNewOrder::orderType)
        .def_readwrite("volumeTotal",&pubsub::RNewOrder::volumeTotal)
        .def_readwrite("limitPrice",&pubsub::RNewOrder::limitPrice)
        .def_readwrite("reduceOnly",&pubsub::RNewOrder::reduceOnly)

        .def_readwrite("orderStatus",&pubsub::RNewOrder::orderStatus)
        .def("getOrderId", &pubsub::RNewOrder::getOrderId)
        .def("setOrderId", &pubsub::RNewOrder::setOrderId)
        .ADD_PROPERTY(pubsub::RNewOrder, orderId)
        .def_readwrite("volumeTraded",&pubsub::RNewOrder::volumeTraded)
        .def_readwrite("tradePrice",&pubsub::RNewOrder::tradePrice)
        .def_readwrite("ErrorID",&pubsub::RNewOrder::ErrorID)
        .def("getOriginMsg", &pubsub::RNewOrder::getOriginMsg)
        .def("setOriginMsg", &pubsub::RNewOrder::setOriginMsg)
        .ADD_PROPERTY(pubsub::RNewOrder, originMsg)
        .def_readwrite("tsSent",&pubsub::RNewOrder::tsSent)
        .def_readwrite("tsNet",&pubsub::RNewOrder::tsNet)
        .def("getString", &pubsub::RNewOrder::getString)


    ;

    class_<pubsub::TCancelOrder>("TCancelOrder",init<>())
        .def("getInstId", &pubsub::TCancelOrder::getInstId)
        .def("setInstId", &pubsub::TCancelOrder::setInstId)
        .ADD_PROPERTY(pubsub::TCancelOrder, instId)
        .def_readwrite("clientOrderId",&pubsub::TCancelOrder::clientOrderId)
        .def("getOrderId", &pubsub::TCancelOrder::getOrderId)
        .def("setOrderId", &pubsub::TCancelOrder::setOrderId)
        .ADD_PROPERTY(pubsub::TCancelOrder, orderId)
        .def("getOrderSysId", &pubsub::TCancelOrder::getOrderSysId)
        .def("setOrderSysId", &pubsub::TCancelOrder::setOrderSysId)
        .ADD_PROPERTY(pubsub::TCancelOrder, orderId)
        .def_readwrite("cancelOrderTypeEnum",&pubsub::TCancelOrder::cancelOrderTypeEnum)
        .def("getString", &pubsub::TCancelOrder::getString)
    ;

    class_<pubsub::RCancelOrder>("RCancelOrder",init<>())
        .def("getInstId", &pubsub::RCancelOrder::getInstId)
        .def("setInstId", &pubsub::RCancelOrder::setInstId)
        .ADD_PROPERTY(pubsub::RCancelOrder, instId)
        .def_readwrite("clientOrderId",&pubsub::RCancelOrder::clientOrderId)
        .def("getOrderId", &pubsub::RCancelOrder::getOrderId)
        .def("setOrderId", &pubsub::RCancelOrder::setOrderId)
        .ADD_PROPERTY(pubsub::RCancelOrder, orderId)
        .def("getOrderSysId", &pubsub::RCancelOrder::getOrderSysId)
        .def("setOrderSysId", &pubsub::RCancelOrder::setOrderSysId)
        .ADD_PROPERTY(pubsub::RCancelOrder, orderId)
        .def_readwrite("cancelOrderTypeEnum",&pubsub::RCancelOrder::cancelOrderTypeEnum)
        .def_readwrite("volumeTraded",&pubsub::RCancelOrder::volumeTraded)
        .def_readwrite("tradePrice",&pubsub::RCancelOrder::tradePrice)
        .def_readwrite("orderStatus",&pubsub::RCancelOrder::orderStatus)
        .def_readwrite("ErrorID",&pubsub::RCancelOrder::ErrorID)
        .def("getOriginMsg", &pubsub::RCancelOrder::getOriginMsg)
        .def("setOriginMsg", &pubsub::RCancelOrder::setOriginMsg)
        .ADD_PROPERTY(pubsub::RCancelOrder, originMsg)
        .def_readwrite("tsSent",&pubsub::RCancelOrder::tsSent)
        .def_readwrite("tsNet",&pubsub::RCancelOrder::tsNet)
        .def("getString", &pubsub::RCancelOrder::getString)
    ;

    class_<pubsub::TQueryOrder>("TQueryOrder",init<>())
        .def("getInstId", &pubsub::TQueryOrder::getInstId)
        .def("setInstId", &pubsub::TQueryOrder::setInstId)
        .ADD_PROPERTY(pubsub::TQueryOrder, instId)
        .def_readwrite("clientOrderId",&pubsub::TQueryOrder::clientOrderId)
        .def("getOrderId", &pubsub::TQueryOrder::getOrderId)
        .def("setOrderId", &pubsub::TQueryOrder::setOrderId)
        .ADD_PROPERTY(pubsub::TQueryOrder, orderId)
        .def("getOrderSysId", &pubsub::TQueryOrder::getOrderSysId)
        .def("setOrderSysId", &pubsub::TQueryOrder::setOrderSysId)
        .ADD_PROPERTY(pubsub::TQueryOrder, orderSysId)
        .def_readwrite("queryOrderTypeEnum",&pubsub::TQueryOrder::queryOrderTypeEnum)
        .def("getString", &pubsub::TQueryOrder::getString)
    ;

    class_<pubsub::RQueryOrder>("RQueryOrder",init<>())
        .def("getInstId", &pubsub::RQueryOrder::getInstId)
        .def("setInstId", &pubsub::RQueryOrder::setInstId)
        .ADD_PROPERTY(pubsub::RQueryOrder, instId)
        .def_readwrite("clientOrderId",&pubsub::RQueryOrder::clientOrderId)
        .def("getOrderId", &pubsub::RQueryOrder::getOrderId)
        .def("setOrderId", &pubsub::RQueryOrder::setOrderId)
        .ADD_PROPERTY(pubsub::RQueryOrder, orderId)
        .def("getOrderSysId", &pubsub::RQueryOrder::getOrderSysId)
        .def("setOrderSysId", &pubsub::RQueryOrder::setOrderSysId)
        .ADD_PROPERTY(pubsub::RQueryOrder, orderSysId)
        .def_readwrite("queryOrderTypeEnum",&pubsub::RQueryOrder::queryOrderTypeEnum)
        .def_readwrite("orderStatus",&pubsub::RQueryOrder::orderStatus)
        .def_readwrite("volumeTotal",&pubsub::RQueryOrder::volumeTotal)
        .def_readwrite("limitPrice",&pubsub::RQueryOrder::limitPrice)
        .def_readwrite("volumeTraded",&pubsub::RQueryOrder::volumeTraded)
        .def_readwrite("tradePrice",&pubsub::RQueryOrder::tradePrice)
        .def_readwrite("ErrorID",&pubsub::RQueryOrder::ErrorID)
        .def("getOriginMsg", &pubsub::RQueryOrder::getOriginMsg)
        .def("setOriginMsg", &pubsub::RQueryOrder::setOriginMsg)
        .ADD_PROPERTY(pubsub::RQueryOrder, originMsg)
        .def("getString", &pubsub::RQueryOrder::getString)
    ;

    class_<pubsub::TQueryAccount>("TQueryAccount",init<>())

    ;

    class_<pubsub::TQueryPosition>("TQueryPosition",init<>())
        .def("getInstId", &pubsub::TQueryPosition::getInstId)
        .def("setInstId", &pubsub::TQueryPosition::setInstId)
    ;

    class_<pubsub::TQueryBalance>("TQueryBalance",init<>())
        .def("getCurrency", &pubsub::TQueryBalance::getCurrency)
        .def("setCurrency", &pubsub::TQueryBalance::setCurrency)
    ;

    class_<pubsub::ROrderResponse>("ROrderResponse",init<>())
        .def("getInstId", &pubsub::ROrderResponse::getInstId)
        .def("setInstId", &pubsub::ROrderResponse::setInstId)
        .ADD_PROPERTY(pubsub::ROrderResponse, instId)
        // .def("getClientOrderId", &pubsub::ROrderResponse::getClientOrderId)
        // .def("setClientOrderId", &pubsub::ROrderResponse::setClientOrderId)
        // .ADD_PROPERTY(pubsub::ROrderResponse, clientOrderId)
        // .def_readwrite("clientOrderId",&pubsub::ROrderResponse::clientOrderId)
        .def("getOrderId", &pubsub::ROrderResponse::getOrderId)
        .def("setOrderId", &pubsub::ROrderResponse::setOrderId)
        .ADD_PROPERTY(pubsub::ROrderResponse, orderId)
        .def("getOrderSysId", &pubsub::ROrderResponse::getOrderSysId)
        .def("setOrderSysId", &pubsub::ROrderResponse::setOrderSysId)
        .ADD_PROPERTY(pubsub::ROrderResponse, orderSysId)
        .def_readwrite("orderStatus",&pubsub::ROrderResponse::orderStatus)
        .def_readwrite("volumeTotal",&pubsub::ROrderResponse::volumeTotal)
        .def_readwrite("limitPrice",&pubsub::ROrderResponse::limitPrice)
        .def_readwrite("volumeTraded",&pubsub::ROrderResponse::volumeTraded)
        .def_readwrite("tradePrice",&pubsub::ROrderResponse::tradePrice)
        .def_readwrite("apiSourceEnum",&pubsub::ROrderResponse::apiSourceEnum)
        // .def_readwrite("offsetFlag",&pubsub::ROrderResponse::offsetFlag)
        // .def_readwrite("direction",&pubsub::ROrderResponse::direction)
        // .def_readwrite("orderType",&pubsub::ROrderResponse::orderType)
        // .def_readwrite("orderStatus",&pubsub::ROrderResponse::orderStatus)
        // .def_readwrite("volumeTotal",&pubsub::ROrderResponse::volumeTotal)
        // .def_readwrite("limitPrice",&pubsub::ROrderResponse::limitPrice)
        // .def_readwrite("volumeTraded",&pubsub::ROrderResponse::volumeTraded)
        // .def_readwrite("tradePrice",&pubsub::ROrderResponse::tradePrice)
        // .def_readwrite("insertTime",&pubsub::ROrderResponse::insertTime)
        // .def_readwrite("updateTime",&pubsub::ROrderResponse::updateTime)
        // .def_readwrite("reduceOnly",&pubsub::ROrderResponse::reduceOnly)
        // .def_readwrite("tsParse",&pubsub::ROrderResponse::tsParse)
        // .def_readwrite("ErrorID",&pubsub::ROrderResponse::ErrorID)
        // .def("getOriginMsg", &pubsub::ROrderResponse::getOriginMsg)
        // .def("setOriginMsg", &pubsub::ROrderResponse::setOriginMsg)
        // .ADD_PROPERTY(pubsub::ROrderResponse, originMsg)

        .def("getString", &pubsub::ROrderResponse::getString)
    ;

    class_<pubsub::ROrderTrade>("ROrderTrade",init<>())
        .def_readwrite("exchangeTypeEnum",&pubsub::ROrderTrade::exchangeTypeEnum)
        .def_readwrite("instTypeEnum",&pubsub::ROrderTrade::instTypeEnum)
        // .def_readwrite("cmdTypeEnum",&pubsub::ROrderTrade::cmdTypeEnum)
        // .def_readwrite("cmdTime",&pubsub::ROrderTrade::cmdTime)
        .def("getOrderId", &pubsub::ROrderTrade::getOrderId)
        .def("setOrderId", &pubsub::ROrderTrade::setOrderId)
        .ADD_PROPERTY(pubsub::ROrderTrade, orderId)
        .def("getStrategyId", &pubsub::ROrderTrade::getStrategyId)
        .def("setStrategyId", &pubsub::ROrderTrade::setStrategyId)
        .ADD_PROPERTY(pubsub::ROrderTrade, strategyId)
        .def("getInstId", &pubsub::ROrderTrade::getInstId)
        .def("setInstId", &pubsub::ROrderTrade::setInstId)
        .ADD_PROPERTY(pubsub::ROrderTrade, instId)
        .def("getOrderSysId", &pubsub::ROrderTrade::getOrderSysId)
        .def("setOrderSysId", &pubsub::ROrderTrade::setOrderSysId)
        .ADD_PROPERTY(pubsub::ROrderTrade, orderSysId)
        .def("getStrategyRef", &pubsub::ROrderTrade::getStrategyRef)
        .def("setStrategyRef", &pubsub::ROrderTrade::setStrategyRef)
        .ADD_PROPERTY(pubsub::ROrderTrade, strategyRef)

        .def_readwrite("clientOrderId",&pubsub::ROrderTrade::clientOrderId)
        .def_readwrite("offsetFlag",&pubsub::ROrderTrade::offsetFlag)
        .def_readwrite("orderType",&pubsub::ROrderTrade::orderType)
        .def_readwrite("direction",&pubsub::ROrderTrade::direction)
        .def_readwrite("orderStatus",&pubsub::ROrderTrade::orderStatus)
        .def_readwrite("volumeTotal",&pubsub::ROrderTrade::volumeTotal)
        .def_readwrite("limitPrice",&pubsub::ROrderTrade::limitPrice)
        .def_readwrite("tradedDiff",&pubsub::ROrderTrade::tradedDiff)
        .def_readwrite("volumeTraded",&pubsub::ROrderTrade::volumeTraded)
        .def_readwrite("tradePrice",&pubsub::ROrderTrade::tradePrice)
        .def_readwrite("reduceOnly",&pubsub::ROrderTrade::reduceOnly)
        .def_readwrite("isMaker",&pubsub::ROrderTrade::isMaker)
        .def_readwrite("apiSourceEnum",&pubsub::ROrderTrade::apiSourceEnum)
        .def_readwrite("insertTime",&pubsub::ROrderTrade::insertTime)
        .def_readwrite("updateTime",&pubsub::ROrderTrade::updateTime)
        .def_readwrite("ErrorID",&pubsub::ROrderTrade::ErrorID)
        .def("getOriginMsg", &pubsub::ROrderTrade::getOriginMsg)
        .def("setOriginMsg", &pubsub::ROrderTrade::setOriginMsg)
        .ADD_PROPERTY(pubsub::ROrderTrade, originMsg)
        .def_readwrite("tsSent",&pubsub::ROrderTrade::tsSent)
        .def_readwrite("tsNet",&pubsub::ROrderTrade::tsNet)
        .def("getString", &pubsub::ROrderTrade::getString)

    ;

    class_<pubsub::RTrade>("RTrade",init<>())
        .def("getInstId", &pubsub::RTrade::getInstId)
        .def("setInstId", &pubsub::RTrade::setInstId)
        .ADD_PROPERTY(pubsub::RTrade, instId)
        .def("getOrderSysId", &pubsub::RTrade::getOrderSysId)
        .def("setOrderSysId", &pubsub::RTrade::setOrderSysId)
        .ADD_PROPERTY(pubsub::RTrade, orderSysId)

        .def("getOrderId", &pubsub::RTrade::getOrderId)
        .def("setOrderId", &pubsub::RTrade::setOrderId)
        .ADD_PROPERTY(pubsub::RTrade, orderId)
        .def("getTradeId", &pubsub::RTrade::getTradeId)
        .def("setTradeId", &pubsub::RTrade::setTradeId)
        .ADD_PROPERTY(pubsub::RTrade, tradeId)
        // .def_readwrite("offsetFlag",&pubsub::RTrade::offsetFlag)
        // .def_readwrite("direction",&pubsub::RTrade::direction)
        .def_readwrite("volumeTraded",&pubsub::RTrade::volumeTraded)
        .def_readwrite("tradePrice",&pubsub::RTrade::tradePrice)
        // .def_readwrite("tradeFee",&pubsub::RTrade::tradeFee)
        .def_readwrite("isMaker",&pubsub::RTrade::isMaker)
        // .def_readwrite("tradeTime",&pubsub::RTrade::tradeTime)
        .def_readwrite("apiSourceEnum",&pubsub::RTrade::apiSourceEnum)
        .def("getString", &pubsub::RTrade::getString)
    ;

    class_<pubsub::RBalance>("RBalance", init<>())
        .def("getCurrency", &pubsub::RBalance::getCurrency)
        .def("setCurrency", &pubsub::RBalance::setCurrency)
        .ADD_PROPERTY(pubsub::RBalance, currency)
        .def_readwrite("available", &pubsub::RBalance::available)
        .def_readwrite("frozen", &pubsub::RBalance::frozen)
        .def_readwrite("total", &pubsub::RBalance::total)
        .def_readwrite("unrealizedPnl", &pubsub::RBalance::unrealizedPnl)
        // .def_readwrite("updateTime",&pubsub::RBalance::updateTime)
        .def_readwrite("apiSourceEnum", &pubsub::RBalance::apiSourceEnum)
        .def("getString", &pubsub::RBalance::getString);

    class_<pubsub::RPosition>("RPosition",init<>())
        .def("getInstId", &pubsub::RPosition::getInstId)
        .def("setInstId", &pubsub::RPosition::setInstId)
        .ADD_PROPERTY(pubsub::RPosition, instId)
        .def_readwrite("direction",&pubsub::RPosition::direction)
        .def_readwrite("volume",&pubsub::RPosition::volume)
        .def_readwrite("maintMargin",&pubsub::RPosition::maintMargin)
        .def_readwrite("avgPrice",&pubsub::RPosition::avgPrice)
        .def_readwrite("unrealizedPnl",&pubsub::RPosition::unrealizedPnl)
        .def_readwrite("markPrice",&pubsub::RPosition::markPrice)
        .def_readwrite("liquidPrice",&pubsub::RPosition::liquidPrice)
        .def_readwrite("adlQuantile",&pubsub::RPosition::adlQuantile)
        .def_readwrite("apiSourceEnum",&pubsub::RPosition::apiSourceEnum)
        .def("getString", &pubsub::RPosition::getString)
        ;

    class_<pubsub::TCommandHeader>("TCommandHeader",init<>())
        .def_readwrite("exchangeTypeEnum",&pubsub::TCommandHeader::exchangeTypeEnum)
        .def_readwrite("instTypeEnum",&pubsub::TCommandHeader::instTypeEnum)
        .def_readwrite("cmdTypeEnum",&pubsub::TCommandHeader::cmdTypeEnum)
        .def_readwrite("cmdTime",&pubsub::TCommandHeader::cmdTime)
        .def("getStrategyId", &pubsub::TCommandHeader::getStrategyId)
        .def("setStrategyId", &pubsub::TCommandHeader::setStrategyId)
        .ADD_PROPERTY(pubsub::TCommandHeader, strategyId)
    ;

    class_<pubsub::TCommand>("TCommand",init<>())
        .def_readwrite("header",&pubsub::TCommand::header)
        .def("getTNewOrder", &pubsub::TCommand::getTNewOrder)
        .def("setTNewOrder", &pubsub::TCommand::setTNewOrder)
        .def("getTCancelOrder", &pubsub::TCommand::getTCancelOrder)
        .def("setTCancelOrder", &pubsub::TCommand::setTCancelOrder)
        .def("getTQueryOrder", &pubsub::TCommand::getTQueryOrder)
        .def("setTQueryOrder", &pubsub::TCommand::setTQueryOrder)
        .def("getTQueryPosition", &pubsub::TCommand::getTQueryPosition)
        .def("setTQueryPosition", &pubsub::TCommand::setTQueryPosition)
        .def("getTQueryBalance", &pubsub::TCommand::getTQueryBalance)
        .def("setTQueryBalance", &pubsub::TCommand::setTQueryBalance)
        .def("getString",  &pubsub::TCommand::getString)
    ;

    class_<pubsub::RCommandHeader>("RCommandHeader",init<>())
        .def_readwrite("exchangeTypeEnum",&pubsub::RCommandHeader::exchangeTypeEnum)
        .def_readwrite("instTypeEnum",&pubsub::RCommandHeader::instTypeEnum)
        .def_readwrite("cmdTypeEnum",&pubsub::RCommandHeader::cmdTypeEnum)
        .def_readwrite("cmdTime",&pubsub::RCommandHeader::cmdTime)
        .def("getStrategyId", &pubsub::RCommandHeader::getStrategyId)
        .def("setStrategyId", &pubsub::RCommandHeader::setStrategyId)
        .ADD_PROPERTY(pubsub::RCommandHeader, strategyId)
        .def("getAccountId", &pubsub::RCommandHeader::getAccountId)
        .def("setAccountId", &pubsub::RCommandHeader::setAccountId)
        .ADD_PROPERTY(pubsub::RCommandHeader, accountId)
    ;

    class_<pubsub::RCommand>("RCommand",init<>())
        .def_readwrite("header",&pubsub::RCommand::header)
        .def("getRNewOrder", &pubsub::RCommand::getRNewOrder)
        .def("setRNewOrder", &pubsub::RCommand::setRNewOrder)
        .def("getROrderResponse", &pubsub::RCommand::getROrderResponse)
        .def("setROrderResponse", &pubsub::RCommand::setROrderResponse)
        .def("getRCancelOrder", &pubsub::RCommand::getRCancelOrder)
        .def("setRCancelOrder", &pubsub::RCommand::setRCancelOrder)
        .def("getRQueryOrder", &pubsub::RCommand::getRQueryOrder)
        .def("setRQueryOrder", &pubsub::RCommand::setRQueryOrder)
        .def("getRTrade", &pubsub::RCommand::getRTrade)
        .def("setRTrade", &pubsub::RCommand::setRTrade)
        .def("getRBalance", &pubsub::RCommand::getRBalance)
        .def("setRBalance", &pubsub::RCommand::setRBalance)
        .def("getRPosition", &pubsub::RCommand::getRPosition)
        .def("setRPosition", &pubsub::RCommand::setRPosition)
        .def("getROrderTrade", &pubsub::RCommand::getROrderTrade)
        .def("setROrderTrade", &pubsub::RCommand::setROrderTrade)
        .def("getString", &pubsub::RCommand::getString)
    ;

    // class_<TCommandPuber>("TCommandPuber",init<char *>())
    //     // .def("pop", &TCommandPubSuber::pop)
    //     .def("push", &TCommandPuber::push)
    // ;

    class_<TCommandPubSuber>("TCommandPubSuber",init<char *>())
        // .def("pop", &TCommandPubSuber::pop)
        .def("push", &TCommandPubSuber::push)
    ;

    class_<RCommandPubSuber>("RCommandPubSuber",init<char *>())
        .def("pop", &RCommandPubSuber::pop)
        // .def("push", &RCommandPubSuber::push)
    ;
}