#pragma once

#include "BeastRestClient.h"


class Net {
public:

    static Net& Instance() {
        static Net net;
        return net;
    }

    bool syncGet(const std::string accountName, const std::string& host, const std::string& target, std::vector<std::pair<std::string, std::string>> default_headers, std::vector<std::pair<std::string, std::string>> extra_headers, std::string& body_out, int& status_out) {
        std::string key = accountName + "-" + host;
        auto it = mClients.find(key);
        if (it == mClients.end()) {
            net::RestClientConfig cfg;
            cfg.host = host;
            cfg.port = 443;
            cfg.use_tls = true;
            cfg.verify_peer = false;
            cfg.max_connections = 3;
            cfg.parallel_establish_threads = 3;
            cfg.request_queue_capacity = 16;
            cfg.request_pool_size = 8;
            cfg.request_timeout_ms = 30'000;
            cfg.idle_close_after_ms = 15000;

            try {
                auto client = std::make_shared<net::RestClient>(cfg);

                auto defaultHeaders_ = std::move(default_headers);
                for (auto& kv : defaultHeaders_) {
                    client->set_default_header(kv.first, kv.second);
                }

                it = mClients.emplace(key, std::move(client)).first;
            }
            catch (const std::exception& e) {
                LOG_ERROR("syncGet: create RestClient for {} failed: {}", key, e.what());
                return false;
            }
        }


        for (int attempt = 0; attempt < 2; ++attempt) {
            auto promise = std::make_shared<std::promise<std::pair<boost::system::error_code, net::HttpResponse>>>();
            auto future  = promise->get_future();

            it->second->async_request(boost::beast::http::verb::get, target, std::string(), "application/json", extra_headers, [promise](boost::system::error_code ec, net::HttpResponse resp) {
                    promise->set_value(std::make_pair(ec, std::move(resp)));
                });

            if (future.wait_for(std::chrono::seconds(35)) != std::future_status::ready) {
                LOG_ERROR("syncGet {} {} timeout (>35s, RestClient worker maybe stuck?)", host, target);
                return false;
            }

            auto result = future.get();
            boost::system::error_code ec = result.first;
            net::HttpResponse resp = std::move(result.second);

            if (!ec) {
                status_out = resp.status_code;
                body_out = std::move(resp.body);
                return true;
            }

            const bool is_stale = ec == boost::asio::error::eof || ec == boost::asio::error::connection_reset || ec == boost::asio::error::broken_pipe || ec == boost::beast::http::error::end_of_stream;
            if (attempt == 0 && is_stale) {
                LOG_INFO("syncGet {} {} got '{}' (likely stale keep-alive), waiting for RestClient reconnect + retry once", host, target, ec.message());
                
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (std::chrono::steady_clock::now() < deadline) {
                    if (it->second->idle_count() > 0) {
                        break;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }
    
            LOG_ERROR("syncGet {} {} failed: {}", host, target, ec.message());
            return false;
        }

        return false;
    }

    bool syncPost(const std::string accountName, const std::string& host, const std::string& target, std::string& body_in, std::vector<std::pair<std::string, std::string>> default_headers, std::vector<std::pair<std::string, std::string>> extra_headers, std::string& body_out, int& status_out) {
        std::string key = accountName + "-" + host;
        auto it = mClients.find(key);
        if (it == mClients.end()) {
            net::RestClientConfig cfg;
            cfg.host = host;
            cfg.port = 443;
            cfg.use_tls = true;
            cfg.verify_peer = false;
            cfg.max_connections = 3;
            cfg.parallel_establish_threads = 3;
            cfg.request_queue_capacity = 16;
            cfg.request_pool_size = 8;
            cfg.request_timeout_ms = 30'000;
            cfg.idle_close_after_ms = 15000;


            try {
                auto client = std::make_shared<net::RestClient>(cfg);

                auto defaultHeaders_ = std::move(default_headers);
                for (auto& kv : defaultHeaders_) {
                    client->set_default_header(kv.first, kv.second);
                }

                it = mClients.emplace(key, std::move(client)).first;
            }
            catch (const std::exception& e) {
                LOG_ERROR("syncPost: create RestClient for {} failed: {}", key, e.what());
                return false;
            }
        }


        for (int attempt = 0; attempt < 2; ++attempt) {
            auto promise = std::make_shared<std::promise<std::pair<boost::system::error_code, net::HttpResponse>>>();
            auto future  = promise->get_future();

            it->second->async_request(boost::beast::http::verb::post, target, body_in, "application/json", extra_headers, [promise](boost::system::error_code ec, net::HttpResponse resp) {
                    promise->set_value(std::make_pair(ec, std::move(resp)));
                });

            if (future.wait_for(std::chrono::seconds(35)) != std::future_status::ready) {
                LOG_ERROR("syncPost {} {} timeout (>35s, RestClient worker maybe stuck?)", host, target);
                return false;
            }

            auto result = future.get();
            boost::system::error_code ec = result.first;
            net::HttpResponse resp = std::move(result.second);

            if (!ec) {
                status_out = resp.status_code;
                body_out = std::move(resp.body);
                return true;
            }

            const bool is_stale = ec == boost::asio::error::eof || ec == boost::asio::error::connection_reset || ec == boost::asio::error::broken_pipe || ec == boost::beast::http::error::end_of_stream;
            if (attempt == 0 && is_stale) {
                LOG_INFO("syncPost {} {} got '{}' (likely stale keep-alive), waiting for RestClient reconnect + retry once", host, target, ec.message());
                
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (std::chrono::steady_clock::now() < deadline) {
                    if (it->second->idle_count() > 0) {
                        break;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }
    
            LOG_ERROR("syncPost {} {} failed: {}", host, target, ec.message());
            return false;
        }

        return false;
    }


private:

    // 每个 host 的 RestClient 缓存 (lazy 创建, 复用 TCP+TLS)
    std::unordered_map<std::string, std::shared_ptr<net::RestClient>> mClients;

};


