#pragma once
#include "data_struct.h"
// #include <fmt/core.h>


namespace pubsub {
    enum CommandType {
        CMD_TYPE_MIN=0,
        CMD_NEW_ORDER,
        CMD_CANCEL_ORDER,
        CMD_QUERY_ORDER,
        CMD_QUERY_ACCOUNT,
        CMD_QUERY_BALANCE,
        CMD_QUERY_POSITION,

        CMD_RPT_NEW_ORDER,
        CMD_RPT_CANCEL_ORDER,
        CMD_RPT_QUERY_ORDER,
        CMD_RPT_TOTAL_ACCOUNT,
        CMD_RPT_BALANCE,
        CMD_RPT_POSITION,
        CMD_RPT_ORDER_RESPONSE,
        CMD_TYPE_MAX
    };

    static std::unordered_map<CommandType, std::string> CommandTypeEnum2StrMap {
        {CMD_TYPE_MIN, "CMD_TYPE_MIN"},
        {CMD_NEW_ORDER, "CMD_NEW_ORDER"},
        {CMD_CANCEL_ORDER, "CMD_CANCEL_ORDER"},
        {CMD_QUERY_ORDER, "CMD_QUERY_ORDER"},
        {CMD_QUERY_ACCOUNT, "CMD_QUERY_ACCOUNT"},
        {CMD_QUERY_BALANCE, "CMD_QUERY_BALANCE"},
        {CMD_QUERY_POSITION, "CMD_QUERY_POSITION"},

        {CMD_RPT_NEW_ORDER, "CMD_RPT_NEW_ORDER"},
        {CMD_RPT_CANCEL_ORDER, "CMD_RPT_CANCEL_ORDER"},
        {CMD_RPT_QUERY_ORDER, "CMD_RPT_QUERY_ORDER"},
        {CMD_RPT_TOTAL_ACCOUNT, "CMD_RPT_TOTAL_ACCOUNT"},
        {CMD_RPT_BALANCE, "CMD_RPT_BALANCE"},
        {CMD_RPT_POSITION, "CMD_RPT_POSITION"},
        {CMD_RPT_ORDER_RESPONSE, "CMD_RPT_ORDER_RESPONSE"},
	    {CMD_TYPE_MAX, "CMD_TYPE_MAX"}
    };

    static std::unordered_map<std::string, CommandType> CommandTypeStr2EnumMap {
        {"CMD_TYPE_MIN", CMD_TYPE_MIN},
        {"CMD_NEW_ORDER", CMD_NEW_ORDER},
        {"CMD_CANCEL_ORDER", CMD_CANCEL_ORDER},
        {"CMD_QUERY_ORDER", CMD_QUERY_ORDER},
        {"CMD_QUERY_ACCOUNT", CMD_QUERY_ACCOUNT},
        {"CMD_QUERY_BALANCE", CMD_QUERY_BALANCE},
        {"CMD_QUERY_POSITION", CMD_QUERY_POSITION},

        {"CMD_RPT_NEW_ORDER", CMD_RPT_NEW_ORDER},
        {"CMD_RPT_CANCEL_ORDER", CMD_RPT_CANCEL_ORDER},
        {"CMD_RPT_QUERY_ORDER", CMD_RPT_QUERY_ORDER},
        {"CMD_RPT_TOTAL_ACCOUNT", CMD_RPT_TOTAL_ACCOUNT},
        {"CMD_RPT_BALANCE", CMD_RPT_BALANCE},
        {"CMD_RPT_POSITION", CMD_RPT_POSITION},
        {"CMD_RPT_ORDER_RESPONSE", CMD_RPT_ORDER_RESPONSE},
        {"CMD_TYPE_MAX", CMD_TYPE_MAX}
    };

    struct NewOrder {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        int accountId;
        char accountName[32];
        char strategyId[32];

        char instId[32];
        int64_t clientOrderId;
        char orderSysId[64];
        char strategyRef[64];
        OffsetFlag offsetFlag;
        Direction direction;
        OrderType orderType;
        double volumeTotal;
        double limitPrice;
        bool reduceOnly;

        std::string getString() {
            const std::string s = fmt::format("[NewOrder] exchangeTypeEnum:{}, instTypeEnum:{}, accountId:{}, accountName:{}, strategyId:{}, "
                            "instId:{}, clientOrderId:{}, orderSysId:{}, strategyRef:{}, offsetFlag:{}, "
                            "direction:{}, orderType:{}, volumeTotal:{}, limitPrice:{}, reduceOnly:{}",
                            ExchangeTypeEnum2StrMap[exchangeTypeEnum], InstTypeEnum2StrMap[instTypeEnum], accountId, accountName, strategyId,
                            instId, clientOrderId, orderSysId, strategyRef, OffsetFlagEnum2StrMap[offsetFlag],
                            DirectionEnum2StrMap[direction], OrderTypeEnum2StrMap[orderType], volumeTotal,
                            limitPrice, reduceOnly);
            return s;
        }
    };

    struct CancelOrder {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        int accountId;
        char accountName[32];
        char strategyId[32];

        char instId[32];
        int64_t clientOrderId;
        char orderSysId[64];
        char orderId[64];

        std::string getString() {
            const std::string s = fmt::format("[CancelOrder] exchangeTypeEnum:{}, instTypeEnum:{}, accountId:{}, accountName:{}, strategyId:{}, "
                            "instId:{}, clientOrderId:{}, orderSysId:{}, orderId:{}",
                            ExchangeTypeEnum2StrMap[exchangeTypeEnum], InstTypeEnum2StrMap[instTypeEnum], accountId, accountName, strategyId,
                            instId, clientOrderId, orderSysId, orderId);
            return s;     
        }
    };

    struct QueryOrder {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        int accountId;
        char accountName[32];
        char strategyId[32];

        char instId[32];
        int64_t clientOrderId;
        char orderSysId[64];
        char orderId[64];

        std::string getString() {
            const std::string s = fmt::format("[QueryOrder] exchangeTypeEnum:{}, instTypeEnum:{}, accountId:{}, accountName:{}, strategyId:{}, "
                            "instId:{}, clientOrderId:{}, orderSysId:{}, orderId:{}",
                            ExchangeTypeEnum2StrMap[exchangeTypeEnum], InstTypeEnum2StrMap[instTypeEnum], accountId, accountName, strategyId,
                            instId, clientOrderId, orderSysId, orderId);
            return s;     
        }
    };

    struct QueryAccount {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        int accountId;
        char accountName[32];
        char strategyId[32];

        std::string getString() {
            const std::string s = fmt::format("[QueryAccount] exchangeTypeEnum:{}, instTypeEnum:{}, accountId:{}, accountName:{}, strategyId:{}",
                            ExchangeTypeEnum2StrMap[exchangeTypeEnum], InstTypeEnum2StrMap[instTypeEnum], accountId, accountName, strategyId);
            return s;     
        }
    };

    struct QueryBalance {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        int accountId;
        char accountName[32];
        char strategyId[32];

        char currency[16];

        std::string getString() {
            const std::string s = fmt::format("[QueryAccount] exchangeTypeEnum:{}, instTypeEnum:{}, accountId:{}, accountName:{}, strategyId:{}, currency:{}",
                            ExchangeTypeEnum2StrMap[exchangeTypeEnum], InstTypeEnum2StrMap[instTypeEnum], accountId, accountName, strategyId, currency);
            return s;     
        }
    };

    struct QueryPosition {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        int accountId;
        char accountName[32];
        char strategyId[32];

        char instId[32];

        std::string getString() {
            const std::string s = fmt::format("[QueryPosition] exchangeTypeEnum:{}, instTypeEnum:{}, accountId:{}, accountName:{}, strategyId:{}, instId:{}",
                            ExchangeTypeEnum2StrMap[exchangeTypeEnum], InstTypeEnum2StrMap[instTypeEnum], accountId, accountName, strategyId, instId);
            return s;     
        }
    };

    struct OrderResponse {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        int accountId;
        char accountName[32];
        char strategyId[32];

        char instId[32];
        int64_t clientOrderId;
        char orderSysId[64];
        char orderId[64];
        char strategyRef[64];

        OffsetFlag offsetFlag;
        Direction direction;
        OrderType orderType;
        OrderStatus orderStatus;
        double volumeTotal;
        double limitPrice;
        double volumeTraded;
        double tradePrice;
        double tradeDiff; // 本次成交量
        double fillPrice; // 本次成交价
        bool reduceOnly;

        int errorId;
        char originMsg[128];
        int64_t updateTime;

        ApiSource apiSourceEnum;
        
        std::string getString() {
            const std::string& s = fmt::format("[OrderResponse] exchangeTypeEnum:{}, instTypeEnum:{}, accountId:{}, accountName:{}, strategyId:{}, "
                                "instId:{}, clientOrderId:{}, orderSysId:{}, orderId:{}, strategyRef:{}, offsetFlag:{}, direction:{}, "
                                "orderType:{}, orderStatus:{}, volumeTotal:{}, limitPrice:{}, volumeTraded:{}, "
                                "tradePrice:{}, errorId:{}, originMsg:{}, updateTime:{}, apiSourceEnum:{}",
                                ExchangeTypeEnum2StrMap[exchangeTypeEnum], InstTypeEnum2StrMap[instTypeEnum], accountId, accountName, strategyId,
                                instId, clientOrderId, orderSysId, orderId, strategyRef, OffsetFlagEnum2StrMap[offsetFlag], DirectionEnum2StrMap[direction],
                                OrderTypeEnum2StrMap[orderType], OrderStatusEnum2StrMap[orderStatus], volumeTotal, limitPrice, volumeTraded, 
                                tradePrice, errorId, originMsg, updateTime, ApiSourceEnum2StrMap[apiSourceEnum]);
            return s;
        }
    };

    struct Balance {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        int accountId;
        char accountName[32];
        char strategyId[32];

        char currency[16];
        double total;
        double available;
        double frozen;
        double borrowed;
        double unrealizedPnl;
        bool isLast;
        int64_t updateTime;
        ApiSource apiSourceEnum;

        std::string getString() {
            const std::string& s = fmt::format("[Balance] exchangeTypeEnum:{}, instTypeEnum:{}, accountId:{}, accountName:{}, strategyId:{}, "
                                "currency:{}, total:{}, avaiable:{}, frozen:{}, borrowed:{}, isLast:{}, apiSourceEnum:{}",
                                ExchangeTypeEnum2StrMap[exchangeTypeEnum], InstTypeEnum2StrMap[instTypeEnum], accountId, accountName, strategyId,
                                currency, total, available, frozen, borrowed, isLast, ApiSourceEnum2StrMap[apiSourceEnum]);
            return s;
        }
    };

    struct Position {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        int accountId;
        char accountName[32];
        char strategyId[32];

        char instId[32];
        Direction direction;
        double volume;
        double maintMargin;
        double avgPrice;
        double unrealizedPnl;
        double liquidPrice;
        double markPrice;
        double adlQuantile;
        bool isLast;
        int64_t updateTime;
        ApiSource apiSourceEnum;
        
        std::string getString() {
            const std::string& s = fmt::format("[Position] exchangeTypeEnum:{}, instTypeEnum:{}, accountId:{}, accountName:{}, strategyId:{}, "
                                "instId:{}, direction:{}, volume:{}, maintMargin:{}, avgPrice:{}, unrealizedPnl:{}, "
                                "liquidPrice:{}, markPrice:{}, adlQuantile:{}, isLast:{}, apiSourceEnum:{}",
                                ExchangeTypeEnum2StrMap[exchangeTypeEnum], InstTypeEnum2StrMap[instTypeEnum], accountId, accountName, strategyId,
                                instId, DirectionEnum2StrMap[direction], volume, maintMargin, avgPrice, unrealizedPnl, 
                                liquidPrice, markPrice, adlQuantile, isLast, ApiSourceEnum2StrMap[apiSourceEnum]);
            return s;
        }
    };

    struct TotalAccount {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        int accountId;
        char accountName[32];
        char strategyId[32];

        double totalEquity;
        double adjEquity;
        double mmr;
        double mgnRatio;
        int64_t updateTime;
        ApiSource apiSourceEnum;

        std::string getString() {
            const std::string& s = fmt::format("[TotalAccount] exchangeTypeEnum:{}, instTypeEnum:{}, accountId:{}, accountName:{}, strategyId:{}, "
                                "totalEquity:{}, adjEquity:{}, mmr:{}, mgnRatio:{}, apiSourceEnum:{}",
                                ExchangeTypeEnum2StrMap[exchangeTypeEnum], InstTypeEnum2StrMap[instTypeEnum], accountId, accountName, strategyId,
                                totalEquity, adjEquity, mmr, mgnRatio, ApiSourceEnum2StrMap[apiSourceEnum]);
            return s;
        }
    };

    struct TCommand {
        CommandType cmdTypeEnum;

        union CommandBody {
            NewOrder newOrder;
            CancelOrder cancelOrder;
            QueryOrder queryOrder;
            QueryAccount queryAccount;
            QueryBalance queryBalance;
            QueryPosition queryPosition;
        };
        CommandBody body;

        std::string getString() {
            std::string ret = fmt::format("[{}]", CommandTypeEnum2StrMap[cmdTypeEnum]);
            if (cmdTypeEnum == CMD_NEW_ORDER) {
                ret.append(body.newOrder.getString());
            }
            else if (cmdTypeEnum == CMD_CANCEL_ORDER) {
                ret.append(body.cancelOrder.getString());
            }
            else if (cmdTypeEnum == CMD_QUERY_ORDER) {
                ret.append(body.queryOrder.getString());
            }
            else if (cmdTypeEnum == CMD_QUERY_ACCOUNT) {
                ret.append(body.queryAccount.getString());
            }
            else if (cmdTypeEnum == CMD_QUERY_BALANCE) {
                ret.append(body.queryBalance.getString());
            }
            else if (cmdTypeEnum == CMD_QUERY_POSITION) {
                ret.append(body.queryPosition.getString());
            }
            else {

            }
            return ret;
        }
    };


    struct RCommand {
        CommandType cmdTypeEnum;

        union CommandBody {
            OrderResponse orderResponse;
            Balance balance;
            Position position;
	        TotalAccount totalAccount;
        };
        CommandBody  body;
        
        std::string getString() {
            std::string ret = fmt::format("[{}]", CommandTypeEnum2StrMap[cmdTypeEnum]);
            if (cmdTypeEnum == CMD_RPT_ORDER_RESPONSE || cmdTypeEnum == CMD_RPT_NEW_ORDER || cmdTypeEnum == CMD_RPT_CANCEL_ORDER || cmdTypeEnum == CMD_RPT_QUERY_ORDER) {
                ret.append(body.orderResponse.getString());
            }
            else if (cmdTypeEnum == CMD_RPT_BALANCE) {
                ret.append(body.balance.getString());
            }
            else if (cmdTypeEnum == CMD_RPT_POSITION) {
                ret.append(body.position.getString());
            }
	        else if (cmdTypeEnum == CMD_RPT_TOTAL_ACCOUNT) {
                ret.append(body.totalAccount.getString());
            }
            else {

            }
            return ret;
        }
    };

}
