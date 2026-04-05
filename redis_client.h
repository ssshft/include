#pragma once
#include <unistd.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <set>
#include <mutex>
#include "cpp_redis/cpp_redis"
#include "tacopie/tacopie"
#include "log_engine.h"
#include "crypto_exception.h"


typedef std::function<void(const std::string& , const std::string&)> callbackFun; // 回调函数

class RedisClient{
public:
    RedisClient(const char* ip = "127.0.0.1", const int port = 9379, const char* passwd = "", bool enable_kv = true, bool enable_sub = true,
        int max_retries = 5, int retry_interval_ms = 1000, int maintenance_interval_ms=60000) {
        strcpy(this->host, ip);
        this->port = port;
        strcpy(this->passwd, passwd);
        this->enable_kv = enable_kv;
        this->enable_sub = enable_sub;
        this->max_retries = max_retries;
        this->retry_interval_ms = retry_interval_ms;
        this->maintenance_interval_ms = maintenance_interval_ms;
        this->is_connected_flag.store(false);
        this->is_suber_connected_flag.store(false);
        this->should_stop.store(false);

        ensure_connection();
        std::thread maintainRedisThread(&RedisClient::redis_maintainance, this);
        maintainRedisThread.detach();
    }

    ~RedisClient() {
        should_stop.store(true);
        if(client.is_connected()) {
            client.disconnect();
        }
        if (suber.is_connected()) {
            suber.disconnect();
        }
    }

    // ------常用接口------

    inline bool set(const std::string& key, const std::string& value) {
        if (!enable_kv) {
            return false;
        }
        if (!ensure_connection()) {
            return false;
        }

        try {
            client.set(key, value);
            client.sync_commit();
            return true;
        }
        catch (std::exception& e) {
            LOG_ERROR("{}", e.what());
            is_connected_flag.store(false);
        }
        return false;
    }

    inline bool get(const std::string& key, std::string& value) {
        if (!enable_kv) {
            return false;
        }
        if (!ensure_connection()) {
            return false;
        }

        bool success = false;
        try {
            client.get(key, [&](cpp_redis::reply& reply) {
                if (reply.is_string()) {
                    value = reply.as_string();
                    success = true;
                }
            });
            client.sync_commit();
        }
        catch (std::exception& e) {
            LOG_ERROR("{}", e.what());
            is_connected_flag.store(false);
        }
        return success;
    }

    inline void publish(const char* key, const char* value) {
        if (!enable_kv) {
            return;
        }
        if (!ensure_connection()) {
            return;
        }

        try {     
            client.publish(key, value);
            client.sync_commit();
        }
        catch (std::exception& e) {
            LOG_ERROR("{}", e.what());
            is_connected_flag.store(false);
        }
    }

    // ------------------订阅相关------------

    inline void subscribe(const std::string& channel, callbackFun on_msg) {
        if (!enable_sub) {
            return;
        }

        std::lock_guard<std::mutex> lock(sub_mutex);
        subscribed_channels.insert(channel);
        callFun = on_msg;
        if (!ensure_connection()) {
            LOG_ERROR("Failed to establish connection for subscribe");
            return;
        }
        suber.subscribe(channel, [this](const std::string& chan, const std::string& msg) {
            if (callFun) {
                callFun(chan, msg);
            }
        });
        suber.commit();
    }

    inline void unsubscribe(const std::string& channel) {
        if (!enable_sub) {
            return;
        }

        std::lock_guard<std::mutex> lock(sub_mutex);
        subscribed_channels.erase(channel);
        suber.unsubscribe(channel);
        suber.commit();
    }


private:

    void resubscribe_all() {
        for (const auto& channel : subscribed_channels) {
            try {
                suber.subscribe(channel, [this](const std::string& chan, const std::string& msg) {
                    if (callFun) {
                        callFun(chan, msg);
                    }
                });
                LOG_INFO("Resubscribed to channel: {}", channel);
            }
            catch (const std::exception& e) {
                LOG_ERROR("Failed to resubscribe to channel {}:{}", channel, e.what());
            }
        }

        if (!subscribed_channels.empty()) {
            suber.commit();
        }
    }

    bool reconnect_kv() {
        std::lock_guard<std::mutex> lock(conn_mutex);
        for (int i = 0; i < max_retries; ++i) {
            try {
                client.disconnect();
                client.connect(host, port, [&](const std::string &host, std::size_t port, cpp_redis::connect_state status) {
                    if(status == cpp_redis::connect_state::ok){
                            LOG_INFO("redis client connected with {}:{}", host, port);
                            is_connected_flag.store(true);
                    }
                    else if (status == cpp_redis::connect_state::dropped) {
                            LOG_ERROR("redis client disconnected with {}:{}", host, port);
                            is_connected_flag.store(false);
                    }
                });
                if(crypto::str_cmp(passwd, "") == false) {
                    client.auth(passwd, [&](cpp_redis::reply& reply){
                        if(crypto::str_cmp(reply.as_string().c_str(), "OK")){
                            LOG_INFO("redis client login success");
                        }
                        else {
                            string errmsg = string("redis login failed, redis reply:") + reply.as_string();
                            cryptothrow(errmsg.c_str(), -1);
                        }
                    });
                }
                client.sync_commit();

                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (client.is_connected()) {
                    return true;
                }
            }
            catch (...) {
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(retry_interval_ms));
        }
        LOG_ERROR("Redis kv reconnect failed");
        return false;
    }

    bool reconnect_sub() {
        std::lock_guard<std::mutex> lock(sub_mutex);

        for (int i = 0; i < max_retries; ++i) {
            try {
                suber.disconnect();
                suber.connect(host, port, [&](const std::string& host, std::size_t port, cpp_redis::connect_state status) {
                    if (status == cpp_redis::connect_state::ok ) {
                        LOG_INFO("redis suber connected with {}:{}", host, port);
                        is_suber_connected_flag.store(true);
                    }
                    else if (status == cpp_redis::connect_state::dropped) {
                        LOG_ERROR("suber client disconnected from {}:{}", host, port);
                        is_suber_connected_flag.store(false);
                    }
                });
                if(crypto::str_cmp(passwd, "") == false) {
                    suber.auth(passwd,[&](cpp_redis::reply& reply) {
                        if(crypto::str_cmp(reply.as_string().c_str(), "OK")){
                            LOG_INFO("redis suber client login successfully");
                        }
                        else{
                            string errmsg = string("redis login failed, redis reply:") + reply.as_string();
                            cryptothrow(errmsg.c_str(), -1);
                        }
                    });
                }

                suber.commit();

                std::this_thread::sleep_for(std::chrono::milliseconds(5));

                if (suber.is_connected()) {
                    resubscribe_all();
                    return true;
                }
            }
            catch (...) {

            }

            std::this_thread::sleep_for(std::chrono::milliseconds(retry_interval_ms));
        }

        LOG_ERROR("Redis sub reconnect failed");
        return false;
    }

    bool ensure_connection() {
        bool kv_ok = true;
        bool sub_ok = true;

        if (enable_kv) {
            if (!client.is_connected() || !is_connected_flag.load()) {
                kv_ok = reconnect_kv();
            }
        }

        if (enable_sub) {
            if (!suber.is_connected() || !is_suber_connected_flag.load()) {
                sub_ok = reconnect_sub();
            }
        }
        return kv_ok && sub_ok;
    }

    void redis_maintainance() {
        while (!should_stop.load()) {
            ensure_connection();
            std::this_thread::sleep_for(std::chrono::milliseconds(maintenance_interval_ms));
        }
    }

private:
    int max_retries;
    int retry_interval_ms;
    int maintenance_interval_ms;
    bool enable_kv;
    bool enable_sub;

    std::atomic<bool> is_connected_flag;
    std::atomic<bool> is_suber_connected_flag;
    std::atomic<bool> should_stop;

    std::set<std::string> subscribed_channels;
    std::mutex conn_mutex;
    std::mutex sub_mutex;

    cpp_redis::client client;
    cpp_redis::subscriber suber;
    callbackFun callFun = nullptr;
    char host[64]{0};
    int  port;
    char passwd[64]{0};
};
