#pragma once
#include "data_struct.h"
#include "shm_global.h"
#include "key_util.h"
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
        // shmStr = trade_config.json 里的 OMS.Utrade2TbTCommandSHM（是个字符串，
        // 例如 "100031"）。每个 strategyId 派生一条自己的 SPSC 队列：
        //   name = "{shmStr}_{strategyId}"      （crypto::get_strategy_shm）
        // 与 tb 侧 TbOperation.cpp:85-86 的写法保持一致。
        TradeClient(const std::string& shmStr, const std::vector<std::string>& vSid) {
            for (const auto& strategyId : vSid) {
                if (mTcmd.find(strategyId) != mTcmd.end()) {
                    // 重复的 strategyId 会让下面的 new 覆盖前一个指针（泄漏），
                    // 并且会对同一个 shm name 打开两次
                    LOG_WARN("tradeclient duplicate strategyId, skip: {}", strategyId);
                    continue;
                }
                std::string strategyTcmdShm = crypto::get_strategy_shm(shmStr, strategyId);
                mTcmd[strategyId] = new Utrade2TbTcmd(strategyTcmdShm);
            }
        }

        ~TradeClient() {
            for(auto iter = mTcmd.begin(); iter != mTcmd.end(); ++iter) {
                delete iter->second;
                iter->second = nullptr;
            }
            mTcmd.clear();
        }
       
        void add_new_order(ExchangeType exchangeTypeEnum, InstType instTypeEnum, const char* strategyId,
                const char* instId, OffsetFlag offsetFlag, Direction direction,
                OrderType orderType, double price, double volume, long clientOrderId,
                bool reduceOnly = false, const char* strategyRef = "") {

            auto iter = mTcmd.find(strategyId);
            if (iter != mTcmd.end()) {
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
                iter->second->push(tcmd);
            }
            else {
                LOG_ERROR("tradeclient cannot find strategyId: {}", strategyId);
            }

        }

        void cancel_order(ExchangeType exchangeTypeEnum, InstType instTypeEnum, const char* strategyId,
                          const char* instId, const char* orderId = "", long clientOrderId = 0) {
            auto iter = mTcmd.find(strategyId);
            if (iter != mTcmd.end()) {
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
                iter->second->push(tcmd);
            }
            else {
                LOG_ERROR("tradeclient cannot find strategyId: {}", strategyId);
            }
        }

        void query_order(ExchangeType exchangeTypeEnum, InstType instTypeEnum, const char* strategyId,
                         const char* instId, const char* orderId = "", long clientOrderId = 0) {

            auto iter = mTcmd.find(strategyId);
            if (iter != mTcmd.end()) {
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
                iter->second->push(tcmd);
            }
            else {
                LOG_ERROR("tradeclient cannot find strategyId: {}", strategyId);
            }
        }

        void query_account(ExchangeType exchangeTypeEnum, InstType instTypeEnum, const char* strategyId) {
            auto iter = mTcmd.find(strategyId);
            if (iter != mTcmd.end()) {
                pubsub::TCommand tcmd;
                memset(&tcmd, 0, sizeof(pubsub::TCommand));
                tcmd.cmdTypeEnum = pubsub::CMD_QUERY_ACCOUNT;
                tcmd.body.queryAccount.exchangeTypeEnum = exchangeTypeEnum;
                tcmd.body.queryAccount.instTypeEnum = instTypeEnum;
                strncpy(tcmd.body.queryAccount.strategyId, strategyId, STRATEGYID_SIZE);

                LOG_DEBUG("{}", tcmd.getString());
                iter->second->push(tcmd);
            }
            else {
                LOG_ERROR("tradeclient cannot find strategyId: {}", strategyId);
            }
        }

        void query_position(ExchangeType exchangeTypeEnum, InstType instTypeEnum, const char* strategyId, const char* instId) {
            auto iter = mTcmd.find(strategyId);
            if (iter != mTcmd.end()) {
                pubsub::TCommand tcmd;
                memset(&tcmd, 0, sizeof(pubsub::TCommand));
                tcmd.cmdTypeEnum = pubsub::CMD_QUERY_POSITION;
                tcmd.body.queryPosition.exchangeTypeEnum = exchangeTypeEnum;
                tcmd.body.queryPosition.instTypeEnum = instTypeEnum;
                strncpy(tcmd.body.queryPosition.strategyId, strategyId, STRATEGYID_SIZE);
                strncpy(tcmd.body.queryPosition.instId, instId, INSTID_SIZE);

                LOG_DEBUG("{}", tcmd.getString());
                iter->second->push(tcmd);
            }
            else {
                LOG_ERROR("tradeclient cannot find strategyId: {}", strategyId);
            }
        }

        void query_balance(ExchangeType exchangeTypeEnum, InstType instTypeEnum, const char* strategyId, const char* currency) {
            auto iter = mTcmd.find(strategyId);
            if (iter != mTcmd.end()) {
                pubsub::TCommand tcmd;
                memset(&tcmd, 0, sizeof(pubsub::TCommand));
                tcmd.cmdTypeEnum = pubsub::CMD_QUERY_BALANCE;
                tcmd.body.queryBalance.exchangeTypeEnum = exchangeTypeEnum;
                tcmd.body.queryBalance.instTypeEnum = instTypeEnum;
                strncpy(tcmd.body.queryBalance.strategyId, strategyId, STRATEGYID_SIZE);
                strncpy(tcmd.body.queryBalance.currency, currency, INSTID_SIZE);

                LOG_DEBUG("{}", tcmd.getString());
                iter->second->push(tcmd);
            }
            else {
                LOG_ERROR("tradeclient cannot find strategyId: {}", strategyId);
            } 
        }

    private:
        std::unordered_map<std::string, Utrade2TbTcmd*> mTcmd;
    };
}
