//
// Created by qyang on 2021/12/10.
//

#ifndef DB_REST_CLIENT_H
#define DB_REST_CLIENT_H
#include <stdio.h>
#include <cpprest/uri.h>
#include <cpprest/http_listener.h>
#include <cpprest/asyncrt_utils.h>
#include <cpprest/http_client.h>
#include <cpprest/filestream.h>
#include <iostream>
#include <cstring>

using namespace web::http;
using namespace web::http::client;
using namespace concurrency;
using namespace web;
using namespace http;
using namespace utility;
using namespace http::experimental::listener;
using namespace std;


class RestClient{
private:

public:
    RestClient() {}

    static json::value perform_get(const char *url){
        http_client client(url);
        http_response response = client.request(methods::GET).get();
        auto res = response.extract_json().get();//.body().read().get();
//        cout << res.serialize() << endl;
        return res ;
    }
};

#endif //DB_REST_CLIENT_H
