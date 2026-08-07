#pragma once

#include <string>
#include <fstream>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <typeinfo>
#include "rapidjson/document.h"
#include "rapidjson/istreamwrapper.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/ostreamwrapper.h"


using namespace rapidjson;

struct SbeAccount {
    std::string apiKey{""};
    std::string secretKey{""};
    std::string password{""};    
};

struct ExchangeNode {
    std::string exchangeId;
    int tokenLot;
    std::vector<std::string> instType;
    std::vector<std::string> marketType;
    std::vector<std::string> instId;
    SbeAccount sbeAccount;
    
    ExchangeNode(const std::string& exchId = "", int lot = 30) : exchangeId(exchId), tokenLot(lot) {}
};

class Config {
public:
    static Config* instance() {
        static Config config;
        return &config;
    }

    void load(const char* file) {
        std::ifstream ifs(file);
        if (!ifs.is_open()) {
            fprintf(stderr, "Could not open file for reading!\n");
            exit(-1);
        }

        IStreamWrapper isw{ifs};
        Document doc{};
        doc.ParseStream(isw);

        StringBuffer buffer{};
        Writer<StringBuffer> writer{buffer};
        doc.Accept(writer);
        if (doc.HasParseError()) {
            std::cout << "Error: " << doc.GetParseError() << "Offset: " << doc.GetErrorOffset() << std::endl;
            exit(-1);
        }

        ifs.close();

        rawStr = buffer.GetString();
        d.Parse<rapidjson::kParseNumbersAsStringsFlag>(rawStr.c_str());
    }

    bool has_exchange(std::string &exchange) {
        if (d.HasMember(exchange.c_str())) {
            return true;
        }
        return false;
    }

    bool get_exchange_list(const std::string& key, std::vector<std::string>& exchVec) {
        const rapidjson::Value& exchList = d[key.c_str()];
        for (auto it = exchList.Begin(); it != exchList.End(); it++) {
            exchVec.push_back(it->GetString());
        }
        return true;  
    } 

    int get_exchange_md_info(std::unordered_map<std::string, ExchangeNode>& mExchange) {
        mExchange.clear();
        for (auto itr = d.MemberBegin(); itr != d.MemberEnd(); itr++) {
            auto key = itr->name.GetString();
 
            if (!strcmp(key, "tag") || !strcmp(key, "log") ||
                !strcmp(key, "redis")) {
                continue;
            }
            rapidjson::Value &exchange = itr->value;
            if (!(exchange.HasMember("instType") && exchange.HasMember("marketType") &&
                    exchange.HasMember("instId"))) {
                continue;
            }
            int lot = 30;
            if (exchange.HasMember("tokenLot")) {
                lot = std::stoi(exchange["tokenLot"].GetString());
            }
            ExchangeNode node(key, lot);
            for (rapidjson::SizeType i = 0; i < exchange["instType"].Size(); i++) {
                node.instType.push_back(exchange["instType"][i].GetString());
            }
            for (rapidjson::SizeType i = 0; i < exchange["marketType"].Size(); i++) {
                node.marketType.push_back(exchange["marketType"][i].GetString());
            }
            for (rapidjson::SizeType i = 0; i < exchange["instId"].Size(); i++) {
                node.instId.push_back(exchange["instId"][i].GetString());
            }

            if (exchange.HasMember("sbe")) {
                auto& sbe = exchange["sbe"];
                SbeAccount sbeAccount;
                if (sbe.HasMember("apiKey")) {
                    sbeAccount.apiKey = sbe["apiKey"].GetString();
                }

                if (sbe.HasMember("secretKey")) {
                    sbeAccount.secretKey = sbe["secretKey"].GetString()
                }

                if (sbe.HasMember("password")) {
                    sbeAccount.password = sbe["password"].GetString();
                }

                node.sbeAccount = sbeAccount;
            }
            
            mExchange[key] = node;
        }
        
        int size = mExchange.size();
        return size;
    }

    bool get_string(std::string key, std::string& ret) {
        if (d.HasMember(key.c_str())) {
            rapidjson::Value& config = d[key.c_str()];
            ret = config.GetString();
            return true;
        }
        return false;
    }

    bool get_log_config(std::string key, std::string &logvalue) {
        if (d.HasMember("log")) {
            rapidjson::Value& config = d["log"][key.c_str()];
            logvalue = config.GetString();
            return true;
        }
        else {
            return false;
        }
    }

    bool get_op_config(std::string key, std::string &opValue) {
        if (d.HasMember("op")) {
            rapidjson::Value &config = d["op"][key.c_str()];
            opValue = config.GetString();
            return true;
        }
        else {
            return false;
        }
    }

    bool get_redis_config(std::string key, std::string &logvalue) {
        if(d.HasMember("redis")) {
            rapidjson::Value &config = d["redis"][key.c_str()];
            logvalue = config.GetString();
            return true;
        }
        else {
            return false;
        }
    }

    bool get_dbprocess_config(std::string key, std::string &logvalue) {
        if(d.HasMember("dbprocess")) {
            rapidjson::Value &config = d["dbprocess"][key.c_str()];
            logvalue = config.GetString();
            return true;
        }
        else {
            return false;
        }
    }

    bool get_dbpsnapshot_config(std::string key, std::string &logvalue) {
        if(d.HasMember("dbpsnapshot")) {
            rapidjson::Value &config = d["dbpsnapshot"][key.c_str()];
            logvalue = config.GetString();
            return true;
        }
        else {
            return false;
        }
    }

    std::string get_document_str() {
        return rawStr;
    }

private:
    rapidjson::Document d;  
    std::string rawStr;
};
