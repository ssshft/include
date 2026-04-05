#pragma once
#include <string>
#include <stdio.h>
#include <cpprest/uri.h>
#include <cpprest/http_listener.h>
#include <cpprest/asyncrt_utils.h>
#include <cpprest/http_client.h>
#include <cpprest/filestream.h>
#include <iostream>
#include <cstring>
#include "data_struct.h"
#include "pubsub_protocol.h"


// typedef void (*fun_t)(pubsub::RCommand &);
namespace crypto{
    using namespace web::http;
    using namespace web::http::client;
    using namespace concurrency;
    using namespace web;
    using namespace http;
    using namespace utility;
    using namespace http::experimental::listener;
    using namespace std;

    class RestClientCPP{
        public:
            long requestTimeMilliSeconds;
            RestClientCPP(const char *baseUrl){
                _baseUrl = baseUrl;
                hotHttpClient = new http_client(_baseUrl);
                // hotHttpClient = make_shared<http_client>(_baseUrl);
            }

            ~RestClientCPP(){

            }

            http_response request(http_request &request){
                // FORMAT_REQUEST(request)
                request.headers().add("Connection", "Keep-Alive");
                request.headers().add("Keep-Alive", "timeout=3600, max=100000");
                // request.headers().add("Content-Type", "application/json");
                // requestTimeMilliSeconds = crypto::getCurrentTimeMilli();
                return hotHttpClient->request(request).get();
            }

            // void request(http_request &request, void *func){
            //     FORMAT_REQUEST(request)
            //     hotHttpClient->request(request).then([&](http_response response) {
            //         // func();
            //         return;
            //     }).wait();
            //     return;
            // }


        private:
#if 0
            void keepalive_rest(){
                if(crypto::has_str(_baseUrl.c_str(), "binance")){
                    http_request request(methods::POST);
                    const http_response &response = this->request(request);
                }
                else if(crypto::has_str(_baseUrl.c_str(), "gateio")){
                    http_request request(methods::POST);
                    uri_builder builder("/api/v4/futures/usdt/orders");
                    json::value value;
                    request.headers().add("KEY", "9265eaa2b80118c695bd9b0e799aa772");
                    request.headers().add("Timestamp",crypto::getCurrentTimeSeconds());
                    request.headers().add("SIGN","hellogateioiamhappy");
                    value["text"] = json::value::string("t-hello123456");
                    value["contract"] = json::value::string("BTCUSDT");
                    value["price"] = json::value::string("1000");
                    value["tif"] = json::value::string("gtc");
                    value["size"] = json::value::string("1");
                    request.set_body(value);
                    request.set_request_uri(builder.to_string());
                    const http_response &response = this->request(request);
                    // LOG_DEBUG("%s", response.extract_string().get().c_str());
                }
                else{
                    http_request request(methods::POST);
                    const http_response &response = this->request(request);
                }
            }
#endif
        private:
            string _baseUrl;
        public:
            http_client *hotHttpClient = nullptr;
            // shared_ptr<http_client> hotHttpClient;

    };
}

