#pragma once

#include "string_util.h"
#include <fmt/format.h>


namespace crypto {
    inline std::string get_md_channel_key(const char* exchId, const char* instType, const char* marketType, const char* instId) {
        const std::string& key = fmt::format("{}.{}.{}.{}", exchId, instType, instId, marketType);
        return key;
    }

    inline std::string get_md_channel_key(ExchangeType exchangeType, InstType instType, md::MarketType marketType, const char* instId){
        const string& exchId = ExchangeTypeEnum2StrMap[exchangeType];
        const string& instTstr = InstTypeEnum2StrMap[instType];
        const string& marketTstr = md::MarketTypeEnum2StrMap[marketType];
        const string& key = fmt::format("{}.{}.{}.{}", exchId, instTstr, instId, marketTstr);
        return key;
    }

    inline std::string get_exchId_accountId_key(const char *exchId, const char *accountId){
        std::string key = "";
        key.append(exchId).append(".").append(accountId);
        return key;
    }

    inline std::string get_tradeclient_key(const char *exchId, const char *strategyId){
        std::string key = fmt::format("{}.{}", exchId, strategyId);
        return key;
    }

    inline std::string get_account_balance_key(const char* exchId, const char* instType, const char* strategyId, const char* accountId, const char* currency) {
        const std::string& key = fmt::format("{}.{}.{}.{}.{}", exchId, instType, strategyId, accountId, currency);
        return key;
    }

    inline std::string get_account_balance_key(ExchangeType exchangeTypeEnum, InstType instTypeEnum, const char* strategyId, const char* accountId, const char* currency) {
        const std::string& exchId = ExchangeTypeEnum2StrMap[exchangeTypeEnum];
        const std::string& instType = InstTypeEnum2StrMap[instTypeEnum];
        return get_account_balance_key(exchId.c_str(), instType.c_str(), strategyId, accountId, currency);
    }

    inline std::string get_account_position_key(const char* exchId, const char* instType, const char* strategyId, const char* accountId, const char* direction, const char* instId) {
        const std::string& key = fmt::format("{}.{}.{}.{}.{}.{}", exchId, instType, strategyId, accountId, direction, instId);
        return key;
    }

    inline std::string get_account_position_key(ExchangeType exchangeTypeEnum, InstType instTypeEnum, const char* strategyId, const char* accountId, Direction directionEnum, const char* instId) {
        const std::string& exchId = ExchangeTypeEnum2StrMap[exchangeTypeEnum];
        const std::string& instType = InstTypeEnum2StrMap[instTypeEnum];
        const std::string& direction = DirectionEnum2StrMap[directionEnum];
        return get_account_position_key(exchId.c_str(), instType.c_str(), strategyId, accountId, direction.c_str(), instId);
    }

    inline std::string get_exch_position_key(ExchangeType exchangeTypeEnum, InstType instTypeEnum, const char* accountId) {
        const std::string& exchId = ExchangeTypeEnum2StrMap[exchangeTypeEnum];
        const std::string& instType = InstTypeEnum2StrMap[instTypeEnum]; 
        return fmt::format("{}.{}.{}", exchId, instType, accountId);
    }

    inline string get_db_all_channels_key(){
        return "DB.COULD.SUBSCRIBE.CHANNELS";
    }

    inline std::string get_all_instuments_key() {
        return "ALL.INSTRUMENTINFO";
    }

    inline std::string get_instrumentInfo_channel_key(const std::string& exchId, const std::string& instType, const std::string& instId) {
        const std::string& key = fmt::format("{}.{}.{}.INSTRUMENT_INFO", exchId, instType, instId);
        return key;
    }

    inline std::string get_instrumentInfo_channel_key(ExchangeType exchangeTypeEnum, InstType instTypeEnum, const std::string& instId) {
        const std::string& exchId = ExchangeTypeEnum2StrMap[exchangeTypeEnum];
        const std::string& instType = InstTypeEnum2StrMap[instTypeEnum];
        return get_instrumentInfo_channel_key(exchId, instType, instId);
    }
}
