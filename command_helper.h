#pragma once
#include "data_struct.h"
#include "shm_global.h"
#include "pubsub/pubsub.h"
#include "pubsub_protocol.h"


namespace crypto {
    inline bool convert_rcmd_2_ordertrade(const pubsub::RCommand& rcmd, pubsub::OrderResponse& orderResponse) {
        if (rcmd.cmdTypeEnum == pubsub::CMD_RPT_ORDER_RESPONSE) {
            memcpy(&orderResponse, &rcmd.body.orderResponse, sizeof(rcmd.body.orderResponse));
            return true;
        }
        return false;
    }

    inline bool convert_rcmd_2_balance(const pubsub::RCommand& rcmd, pubsub::Balance& balance) {
        if (rcmd.cmdTypeEnum == pubsub::CMD_RPT_BALANCE) {
            memcpy(&balance, &rcmd.body.balance, sizeof(rcmd.body.balance));
            return true;
        }
        return false;
    }

    inline bool convert_rcmd_2_position(const pubsub::RCommand& rcmd, pubsub::Position& position) {
        if (rcmd.cmdTypeEnum == pubsub::CMD_RPT_POSITION) {
            memcpy(&position, &rcmd.body.position, sizeof(rcmd.body.position));
            return true;
        }
        return false;
    }

    inline bool convert_rcmd_2_total_account(const pubsub::RCommand& rcmd, pubsub::TotalAccount& totalAccount) {
        if (rcmd.cmdTypeEnum == pubsub::CMD_RPT_TOTAL_ACCOUNT) {
            memcpy(&totalAccount, &rcmd.body.totalAccount, sizeof(rcmd.body.totalAccount));
            return true;
        }
        return false;
    }

}

namespace om {
    class TradeClient {
    public:
        TradeClient(int key) {
            utrade2TbTCommandShm = new Utrade2TbTCommandSHM(key);
        }

        ~TradeClient() {
            if (utrade2TbTCommandShm) {
                delete utrade2TbTCommandShm;
                utrade2TbTCommandShm = nullptr;
            }
        }
       
        void add_new_order(ExchangeType exchangeTypeEnum, InstType instTypeEnum, const char* strategyId,
                const char* instId, OffsetFlag offsetFlag, Direction direction,
                OrderType orderType, double price, double volume, long clientOrderId,
                bool reduceOnly = false, const char* strategyRef = "") {
            pubsub::TCommand tcmd;
            memset(&tcmd, 0, sizeof(pubsub::TCommand));
            tcmd.cmdTypeEnum = pubsub::CMD_NEW_ORDER;
            tcmd.body.newOrder.exchangeTypeEnum = exchangeTypeEnum;
            tcmd.body.newOrder.instTypeEnum = instTypeEnum;
            strncpy(tcmd.body.newOrder.strategyId, strategyId, STRATEGYID_SIZE);
            strncpy(tcmd.body.newOrder.instId, instId, INSTID_SIZE);
            tcmd.body.newOrder.clientOrderId = clientOrderId;
            strncpy(tcmd.body.newOrder.strategyRef, strategyRef, ORDER_SIZE);
            tcmd.body.newOrder.offsetFlag = offsetFlag;
            tcmd.body.newOrder.direction = direction;
            tcmd.body.newOrder.orderType = orderType;
            tcmd.body.newOrder.volumeTotal = volume;
            tcmd.body.newOrder.limitPrice = price;
            tcmd.body.newOrder.reduceOnly = reduceOnly;

            LOG_DEBUG("{}", tcmd.getString());
            utrade2TbTCommandShm->push(tcmd);
        }

        void cancel_order(ExchangeType exchangeTypeEnum, InstType instTypeEnum, const char* strategyId,
                          const char* instId, const char* orderId = "", long clientOrderId = 0) {
            pubsub::TCommand tcmd;
            memset(&tcmd, 0, sizeof(pubsub::TCommand));
            tcmd.cmdTypeEnum = pubsub::CMD_CANCEL_ORDER;
            tcmd.body.cancelOrder.exchangeTypeEnum = exchangeTypeEnum;
            tcmd.body.cancelOrder.instTypeEnum = instTypeEnum;
            strncpy(tcmd.body.cancelOrder.strategyId, strategyId, STRATEGYID_SIZE);
            strncpy(tcmd.body.cancelOrder.instId, instId, INSTID_SIZE);
            tcmd.body.cancelOrder.clientOrderId = clientOrderId;
            strncpy(tcmd.body.cancelOrder.orderId, orderId, ORDER_SIZE);

            LOG_DEBUG("{}", tcmd.getString());
            utrade2TbTCommandShm->push(tcmd);
        }

        void query_order(ExchangeType exchangeTypeEnum, InstType instTypeEnum, const char* strategyId,
                         const char* instId, const char* orderId = "", long clientOrderId = 0) {
            pubsub::TCommand tcmd;
            memset(&tcmd, 0, sizeof(pubsub::TCommand));
            tcmd.cmdTypeEnum = pubsub::CMD_QUERY_ORDER;
            tcmd.body.queryOrder.exchangeTypeEnum = exchangeTypeEnum;
            tcmd.body.queryOrder.instTypeEnum = instTypeEnum;
            strncpy(tcmd.body.queryOrder.strategyId, strategyId, STRATEGYID_SIZE);
            strncpy(tcmd.body.queryOrder.instId, instId, INSTID_SIZE);
            tcmd.body.queryOrder.clientOrderId = clientOrderId;
            strncpy(tcmd.body.queryOrder.orderId, orderId, ORDER_SIZE);

            LOG_DEBUG("{}", tcmd.getString());
            utrade2TbTCommandShm->push(tcmd);
        }

        void query_account(ExchangeType exchangeTypeEnum, InstType instTypeEnum, const char* strategyId) {
            pubsub::TCommand tcmd;
            memset(&tcmd, 0, sizeof(pubsub::TCommand));
            tcmd.cmdTypeEnum = pubsub::CMD_QUERY_ACCOUNT;
            tcmd.body.queryAccount.exchangeTypeEnum = exchangeTypeEnum;
            tcmd.body.queryAccount.instTypeEnum = instTypeEnum;
            strncpy(tcmd.body.queryAccount.strategyId, strategyId, STRATEGYID_SIZE);

            LOG_DEBUG("{}", tcmd.getString());
            utrade2TbTCommandShm->push(tcmd);
        }

        void query_position(ExchangeType exchangeTypeEnum, InstType instTypeEnum, const char* strategyId, const char* instId) {
            pubsub::TCommand tcmd;
            memset(&tcmd, 0, sizeof(pubsub::TCommand));
            tcmd.cmdTypeEnum = pubsub::CMD_QUERY_POSITION;
            tcmd.body.queryPosition.exchangeTypeEnum = exchangeTypeEnum;
            tcmd.body.queryPosition.instTypeEnum = instTypeEnum;
            strncpy(tcmd.body.queryPosition.strategyId, strategyId, STRATEGYID_SIZE);
            strncpy(tcmd.body.queryPosition.instId, instId, INSTID_SIZE);

            LOG_DEBUG("{}", tcmd.getString());
            utrade2TbTCommandShm->push(tcmd);
        }

        void query_balance(ExchangeType exchangeTypeEnum, InstType instTypeEnum, const char* strategyId, const char* currency) {
            pubsub::TCommand tcmd;
            memset(&tcmd, 0, sizeof(pubsub::TCommand));
            tcmd.cmdTypeEnum = pubsub::CMD_QUERY_BALANCE;
            tcmd.body.queryBalance.exchangeTypeEnum = exchangeTypeEnum;
            tcmd.body.queryBalance.instTypeEnum = instTypeEnum;
            strncpy(tcmd.body.queryBalance.strategyId, strategyId, STRATEGYID_SIZE);
            strncpy(tcmd.body.queryBalance.currency, currency, INSTID_SIZE);

            LOG_DEBUG("{}", tcmd.getString());
            utrade2TbTCommandShm->push(tcmd);
        }

    private:
        Utrade2TbTCommandSHM* utrade2TbTCommandShm{nullptr};
    };
}
