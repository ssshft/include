#pragma once

#include <string>
#include "data_struct.h"
#include "redis_client.h"
#include "key_util.h"
#include "string_util.h"
#include <oneapi/tbb/concurrent_unordered_map.h>


namespace sm{
    class SecurityManager {
    public:
        SecurityManager(const char *ip = "127.0.0.1", const int port = 9379, const char* passwd = "", bool needUpdate = true) {
            redisClient = new RedisClient(ip, port, passwd, true, false);
            cache_all();
            if (needUpdate) {
                std::thread maintainThread(&SecurityManager::instrumentInfo_maintainance, this);
                maintainThread.detach();
            }
        }

        ~SecurityManager() {
            if (redisClient) {
                delete redisClient;
                redisClient = nullptr;
            }
        }

        inline bool get_all_instruments(std::vector<md::InstrumentInfo>& instInfoVec) {
            std::string raw_infolist_json;
            std::string key = crypto::get_all_instuments_key();
            auto ok = redisClient->get(key, raw_infolist_json);
            if (ok && !raw_infolist_json.empty()){
                rapidjson::Document d;
                rapidjson::Value& array = d.Parse<rapidjson::kParseNumbersAsStringsFlag>(raw_infolist_json.c_str());
                if (d.HasParseError()) {
                    return false;
                }
              
                for (rapidjson::SizeType i = 0; i < array.Size(); ++i) {
                    const rapidjson::Value& object = array[i];
                    if (object.IsObject()) {
                        md::InstrumentInfo info;
                        memset(&info, 0, sizeof(md::InstrumentInfo));
                        info.exchangeTypeEnum = ExchangeType(std::stoi(object["exchangeTypeEnum"].GetString()));
                        info.instTypeEnum = InstType(std::stoi(object["instTypeEnum"].GetString()));
                        strcpy(info.instId, object["instId"].GetString());
                        strcpy(info.originInstId, object["originInstId"].GetString());
                        strcpy(info.base, object["base"].GetString());
                        strcpy(info.quote, object["quote"].GetString());
                        strcpy(info.margin, object["margin"].GetString());
                        info.value = std::stod(object["value"].GetString());
                        info.tickSize = std::stod(object["tickSize"].GetString());
                        info.lotSize = std::stod(object["lotSize"].GetString());
                        info.minSize = std::stod(object["minSize"].GetString());
                        info.maxSize = std::stod(object["maxSize"].GetString());
                        info.minAmount = std::stod(object["minAmount"].GetString());
                        info.magnifyNumber = std::stod(object["magnifyNumber"].GetString());
                        info.reduceNumber = std::stod(object["reduceNumber"].GetString());
    
                        instInfoVec.push_back(info);
                    }
                }
                return true;
            }
            else {
                LOG_ERROR("error when sync smc info");
            }
            return false;
        }

        inline bool parse_info(const std::string& msg, md::InstrumentInfo& info) {
            rapidjson::Document d;
            rapidjson::Value& object = d.Parse<rapidjson::kParseNumbersAsStringsFlag>(msg.c_str());
            if (!d.HasParseError() && object.IsObject()) {
                memset(&info, 0, sizeof(md::InstrumentInfo));
                info.exchangeTypeEnum = ExchangeType(std::stoi(object["exchangeTypeEnum"].GetString()));
                info.instTypeEnum = InstType(std::stoi(object["instTypeEnum"].GetString()));
                strcpy(info.instId, object["instId"].GetString());
                strcpy(info.originInstId, object["originInstId"].GetString());
                strcpy(info.base, object["base"].GetString());
                strcpy(info.quote, object["quote"].GetString());
                strcpy(info.margin, object["margin"].GetString());
                info.value = std::stod(object["value"].GetString());
                info.tickSize = std::stod(object["tickSize"].GetString());
                info.lotSize = std::stod(object["lotSize"].GetString());
                info.minSize = std::stod(object["minSize"].GetString());
                info.maxSize = std::stod(object["maxSize"].GetString());
                info.minAmount = std::stod(object["minAmount"].GetString());
                info.magnifyNumber = std::stod(object["magnifyNumber"].GetString());
                info.reduceNumber = std::stod(object["reduceNumber"].GetString());

                return true;
            }
            return false;
        }

        inline bool get_instrument_info(const char* exchId, const char* instType, const char* instId, md::InstrumentInfo& info) {
            std::string key = crypto::get_instrumentInfo_channel_key(exchId, instType, instId);
            auto found = _infoMap.find(key);
            if(found != _infoMap.end()) {
                memcpy(&info, &found->second, sizeof(md::InstrumentInfo));
                return true;
            }

            return false;
        }

        inline bool get_instrument_info(ExchangeType exchId, InstType instType, const char* instId, md::InstrumentInfo& info) {
            return get_instrument_info(ExchangeTypeEnum2StrMap[exchId].c_str(), InstTypeEnum2StrMap[instType].c_str(), instId, info);
        }

    private:
        void cache_one(md::InstrumentInfo& info) {
            std::string key1 = crypto::get_instrumentInfo_channel_key(info.exchangeTypeEnum, info.instTypeEnum, info.instId);
            std::string key2 = crypto::get_instrumentInfo_channel_key(info.exchangeTypeEnum, info.instTypeEnum, info.originInstId);
            _infoMap[key1] = info;
            _infoMap[key2] = info;
        }

        void cache_all() {
            std::vector<md::InstrumentInfo> instInfoVec;
            if (get_all_instruments(instInfoVec)) {
                for (auto info : instInfoVec) {
                    cache_one(info);
                }
            }
            else {
                LOG_ERROR("cache smc info error occurs");
            }
        }

        void instrumentInfo_maintainance() {
            while (1) {
                std::this_thread::sleep_for(std::chrono::minutes(5));
                cache_all();
            }
        }

    private:
        RedisClient* redisClient = nullptr;
        oneapi::tbb::concurrent_unordered_map<std::string, md::InstrumentInfo> _infoMap;
    };
}
