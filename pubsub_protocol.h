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
        char accountId[ACCOUNTID_SIZE];
        char strategyId[STRATEGYID_SIZE];

        char instId[INSTID_SIZE];
        long clientOrderId;
        char orderSysId[ORDER_SIZE];
        char strategyRef[ORDER_SIZE];
        OffsetFlag offsetFlag;
        Direction direction;
        OrderType orderType;
        double volumeTotal;
        double limitPrice;
        bool reduceOnly;

        std::string getString() {
            const std::string s = fmt::format("[NewOrder] exchangeTypeEnum:{}, instTypeEnum:{}, accountId:{}, strategyId:{}, "
                            "instId:{}, clientOrderId:{}, orderSysId:{}, strategyRef:{}, offsetFlag:{}, "
                            "direction:{}, orderType:{}, volumeTotal:{}, limitPrice:{}, reduceOnly:{}",
                            ExchangeTypeEnum2StrMap[exchangeTypeEnum], InstTypeEnum2StrMap[instTypeEnum], accountId, strategyId,
                            instId, clientOrderId, orderSysId, strategyRef, OffsetFlagEnum2StrMap[offsetFlag],
                            DirectionEnum2StrMap[direction], OrderTypeEnum2StrMap[orderType], volumeTotal,
                            limitPrice, reduceOnly);
            return s;
        }
    };

    struct CancelOrder {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        char accountId[ACCOUNTID_SIZE];
        char strategyId[STRATEGYID_SIZE];

        char instId[INSTID_SIZE];
        long clientOrderId;
        char orderSysId[ORDER_SIZE];
        char orderId[ORDER_SIZE];

        std::string getString() {
            const std::string s = fmt::format("[CancelOrder] exchangeTypeEnum:{}, instTypeEnum:{}, accountId:{}, strategyId:{}, "
                            "instId:{}, clientOrderId:{}, orderSysId:{}, orderId:{}",
                            ExchangeTypeEnum2StrMap[exchangeTypeEnum], InstTypeEnum2StrMap[instTypeEnum], accountId, strategyId,
                            instId, clientOrderId, orderSysId, orderId);
            return s;     
        }
    };

    struct QueryOrder {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        char accountId[ACCOUNTID_SIZE];
        char strategyId[STRATEGYID_SIZE];

        char instId[INSTID_SIZE];
        long clientOrderId;
        char orderSysId[ORDER_SIZE];
        char orderId[ORDER_SIZE];

        std::string getString() {
            const std::string s = fmt::format("[QueryOrder] exchangeTypeEnum:{}, instTypeEnum:{}, accountId:{}, strategyId:{}, "
                            "instId:{}, clientOrderId:{}, orderSysId:{}, orderId:{}",
                            ExchangeTypeEnum2StrMap[exchangeTypeEnum], InstTypeEnum2StrMap[instTypeEnum], accountId, strategyId,
                            instId, clientOrderId, orderSysId, orderId);
            return s;     
        }
    };

    struct QueryAccount {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        char accountId[ACCOUNTID_SIZE];
        char strategyId[STRATEGYID_SIZE];

        std::string getString() {
            const std::string s = fmt::format("[QueryAccount] exchangeTypeEnum:{}, instTypeEnum:{}, accountId:{}, strategyId:{}",
                            ExchangeTypeEnum2StrMap[exchangeTypeEnum], InstTypeEnum2StrMap[instTypeEnum], accountId, strategyId);
            return s;     
        }
    };

    struct QueryBalance {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        char accountId[ACCOUNTID_SIZE];
        char strategyId[STRATEGYID_SIZE];

        char currency[INSTID_SIZE];

        std::string getString() {
            const std::string s = fmt::format("[QueryAccount] exchangeTypeEnum:{}, instTypeEnum:{}, accountId:{}, strategyId:{}, currency:{}",
                            ExchangeTypeEnum2StrMap[exchangeTypeEnum], InstTypeEnum2StrMap[instTypeEnum], accountId, strategyId, currency);
            return s;     
        }
    };

    struct QueryPosition {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        char accountId[ACCOUNTID_SIZE];
        char strategyId[STRATEGYID_SIZE];

        char instId[INSTID_SIZE];

        std::string getString() {
            const std::string s = fmt::format("[QueryPosition] exchangeTypeEnum:{}, instTypeEnum:{}, accountId:{}, strategyId:{}, instId:{}",
                            ExchangeTypeEnum2StrMap[exchangeTypeEnum], InstTypeEnum2StrMap[instTypeEnum], accountId, strategyId, instId);
            return s;     
        }
    };

    struct OrderResponse {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        char accountId[ACCOUNTID_SIZE];
        char strategyId[STRATEGYID_SIZE];

        char instId[INSTID_SIZE];
        long clientOrderId;
        char orderSysId[ORDER_SIZE];
        char orderId[ORDER_SIZE];
        char strategyRef[ORDER_SIZE];

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
        char originMsg[ORIGINMSG_SIZE];
        long updateTime;

        ApiSource apiSourceEnum;
        
        std::string getString() {
            const std::string& s = fmt::format("[OrderResponse] exchangeTypeEnum:{}, instTypeEnum:{}, accountId:{}, strategyId:{}, "
                                "instId:{}, clientOrderId:{}, orderSysId:{}, orderId:{}, strategyRef:{}, offsetFlag:{}, direction:{}, "
                                "orderType:{}, orderStatus:{}, volumeTotal:{}, limitPrice:{}, volumeTraded:{}, "
                                "tradePrice:{}, errorId:{}, originMsg:{}, updateTime:{}, apiSourceEnum:{}",
                                ExchangeTypeEnum2StrMap[exchangeTypeEnum], InstTypeEnum2StrMap[instTypeEnum], accountId, strategyId,
                                instId, clientOrderId, orderSysId, orderId, strategyRef, OffsetFlagEnum2StrMap[offsetFlag], DirectionEnum2StrMap[direction],
                                OrderTypeEnum2StrMap[orderType], OrderStatusEnum2StrMap[orderStatus], volumeTotal, limitPrice, volumeTraded, 
                                tradePrice, errorId, originMsg, updateTime, ApiSourceEnum2StrMap[apiSourceEnum]);
            return s;
        }
    };

    struct Balance {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        char accountId[ACCOUNTID_SIZE];
        char strategyId[STRATEGYID_SIZE];

        char currency[INSTID_SIZE];
        double total;
        double available;
        double frozen;
        double borrowed;
        bool isLast;
        long updateTime;
        ApiSource apiSourceEnum;

        std::string getString() {
            const std::string& s = fmt::format("[Balance] exchangeTypeEnum:{}, instTypeEnum:{}, accountId:{}, strategyId:{}, "
                                "currency:{}, total:{}, avaiable:{}, frozen:{}, borrowed:{}, isLast:{}, apiSourceEnum:{}",
                                ExchangeTypeEnum2StrMap[exchangeTypeEnum], InstTypeEnum2StrMap[instTypeEnum], accountId, strategyId,
                                currency, total, available, frozen, borrowed, isLast, ApiSourceEnum2StrMap[apiSourceEnum]);
            return s;
        }
    };

    struct Position {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        char accountId[ACCOUNTID_SIZE];
        char strategyId[STRATEGYID_SIZE];

        char instId[INSTID_SIZE];
        Direction direction;
        double volume;
        double maintMargin;
        double avgPrice;
        double unrealizedPnl;
        double liquidPrice;
        double markPrice;
        double adlQuantile;
        bool isLast;
        long updateTime;
        ApiSource apiSourceEnum;
        
        std::string getString() {
            const std::string& s = fmt::format("[Position] exchangeTypeEnum:{}, instTypeEnum:{}, accountId:{}, strategyId:{}, "
                                "instId:{}, direction:{}, volume:{}, maintMargin:{}, avgPrice:{}, unrealizedPnl:{}, "
                                "liquidPrice:{}, markPrice:{}, adlQuantile:{}, isLast:{}, apiSourceEnum:{}",
                                ExchangeTypeEnum2StrMap[exchangeTypeEnum], InstTypeEnum2StrMap[instTypeEnum], accountId, strategyId,
                                instId, DirectionEnum2StrMap[direction], volume, maintMargin, avgPrice, unrealizedPnl, 
                                liquidPrice, markPrice, adlQuantile, isLast, ApiSourceEnum2StrMap[apiSourceEnum]);
            return s;
        }
    };

    struct TotalAccount {
        ExchangeType exchangeTypeEnum;
        InstType instTypeEnum;
        char accountId[ACCOUNTID_SIZE];
        char strategyId[STRATEGYID_SIZE];

        double totalEquity;
        double adjEquity;
        double mmr;
        double mgnRatio;
        long updateTime;
        ApiSource apiSourceEnum;

        std::string getString() {
            const std::string& s = fmt::format("[TotalAccount] exchangeTypeEnum:{}, instTypeEnum:{}, accountId:{}, strategyId:{}, "
                                "totalEquity:{}, adjEquity:{}, mmr:{}, mgnRatio:{}, apiSourceEnum:{}",
                                ExchangeTypeEnum2StrMap[exchangeTypeEnum], InstTypeEnum2StrMap[instTypeEnum], accountId, strategyId,
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
            string ret = fmt::format("[{}]", CommandTypeEnum2StrMap[cmdTypeEnum]);
            if (cmdTypeEnum == CMD_RPT_ORDER_RESPONSE) {
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
