#ifndef INCLUDE_STRING_UTIL_H
#define INCLUDE_STRING_UTIL_H
#include <string>
#include <vector>
#include <sstream>
#include <string.h>
#include <iomanip>
#include <openssl/hmac.h>
#include "boost/algorithm/string.hpp"
#include <openssl/sha.h>
#include <string>
#include <random>
#include "magic_enum.hpp"
#include "time_util.h"
#include "base64.hpp"
#include "data_struct.h"
#include <fmt/format.h>
#include <charconv>


#define DOUBLE_EPSLION 0.00000001


namespace crypto{

    inline std::string sha512(const std::string& input) {
        
        unsigned char md_value[EVP_MAX_MD_SIZE];
        unsigned int md_len = 0;

        OpenSSL_add_all_digests();

        EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
        if (mdctx == nullptr) {
            std::cerr << "Failed to create EVP_MD_CTX" << std::endl;
            return "";
        }

        const EVP_MD* md = EVP_get_digestbyname("SHA512");
        if (md == nullptr) {
            std::cerr << "Unknown message digest SHA512" << std::endl;
            EVP_MD_CTX_free(mdctx);
            return "";
        }

        if (1 != EVP_DigestInit_ex(mdctx, md, nullptr)) {
            std::cerr << "EVP_DigestInit_ex failed" << std::endl;
            EVP_MD_CTX_free(mdctx);
            return "";
        }

        if (1 != EVP_DigestUpdate(mdctx, input.data(), input.size())) {
            std::cerr << "EVP_DigestUpdate failed" << std::endl;
            EVP_MD_CTX_free(mdctx);
            return "";
        }

        if (1 != EVP_DigestFinal_ex(mdctx, md_value, &md_len)) {
            std::cerr << "EVP_DigestFinal_ex failed" << std::endl;
            EVP_MD_CTX_free(mdctx);
            return "";
        }

        EVP_MD_CTX_free(mdctx);

        std::string hash = "";
        for (unsigned int i = 0; i < md_len; ++i) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", md_value[i]);
            hash.append(buf);
        }

        return hash;
    }


    // 通用: HMAC-SHA256 → base64 (OKX API 用这个格式, 不是 hex)
    inline std::string hmacSha256Base64(const std::string& key, const std::string& data) {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int  digest_len = 0;
        HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest, &digest_len);
        return websocketpp::base64_encode(digest, digest_len);
    }

    // 通用: HMAC-SHA256 → hex (Bybit / Binance REST 用)
    inline std::string hmacSha256Hex(const std::string& key, const std::string& data) {
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int  length = 0;
        HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash, &length);
        std::string out;
        out.reserve(length * 2);
        static const char* hex = "0123456789abcdef";
        for (unsigned int i = 0; i < length; ++i) {
            out.push_back(hex[hash[i] >> 4]);
            out.push_back(hex[hash[i] & 0x0F]);
        }
        return out;
    }


    // 老函数名, 保持向后兼容 (原实现返回 hex 是 bug, 没被使用; 现修正为 base64)
    inline std::string HmacEncodeOKX(const std::string& key, const std::string& data) {
        return hmacSha256Base64(key, data);
    }

    inline std::string HmacEncodeBybit(const std::string& key, const std::string& data) {
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int length;
        HMAC(EVP_sha256(), key.c_str(), key.length(), reinterpret_cast<const unsigned char*>(data.c_str()), data.length(), hash, &length);
        std::stringstream ss;
        ss << std::hex << std::setfill('0');
        for (unsigned int i = 0; i < length; i++) {
            ss << std::setw(2) << static_cast<int>(hash[i]);
        }
        
        return ss.str();
    }

    inline std::string encryptWithHMACForBtse(const std::string& key, const std::string& data) {
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int length;
        HMAC(EVP_sha384(), key.c_str(), key.length(), reinterpret_cast<const unsigned char*>(data.c_str()), data.length(), hash, &length);
        std::stringstream ss;
        ss << std::hex << std::setfill('0');
        for (unsigned int i = 0; i < length; i++) {
            ss << std::setw(2) << static_cast<int>(hash[i]);
        }
        
        return ss.str();
    }

    inline std::string encryptWithHMACForGateio(const std::string &key, const std::string &data) {
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int length;
        HMAC(EVP_sha512(), key.c_str(), key.length(), reinterpret_cast<const unsigned char*>(data.c_str()), data.length(), hash, &length);
        std::stringstream ss;
        ss << std::hex << std::setfill('0');
        for (unsigned int i = 0; i < length; i++) {
            ss << std::setw(2) << static_cast<int>(hash[i]);
        }
        return ss.str();
    }

    inline std::string encryptWithHMACForBinance(const std::string &key, const std::string &data) {
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int length;
        HMAC(EVP_sha256(), key.c_str(), key.length(), reinterpret_cast<const unsigned char*>(data.c_str()), data.length(), hash, &length);
        std::stringstream ss;
        ss << std::hex << std::setfill('0');
        for (unsigned int i = 0; i < length; i++) {
            ss << std::setw(2) << static_cast<int>(hash[i]);
        }
        
        return ss.str();
    }

    inline std::string encryptWithHMACForHtx(const std::string& key, const std::string& data) {
        unsigned int md_len;
        unsigned char* str = HMAC(EVP_sha256(), key.c_str(), key.length(), reinterpret_cast<const unsigned char*>(data.c_str()), data.length(), nullptr, &md_len);
        
        char signature[100];
        EVP_EncodeBlock((unsigned char*)signature, str, md_len);

        return std::string(signature);
    }

    inline std::string encode_colon(const std::string& timestamp) {
        std::string result = "";
        result.reserve(timestamp.size() + 6);

        for (char c : timestamp) {
            if (c == ':') {
                result += "%3A";
            }
            else {
                result += c;
            }
        }
        return result;
    }

    inline std::string url_encode_fast(const std::string& value) {
        static const char* hex = "0123456789ABCDEF";
        std::string result = "";
        result.reserve(value.size() + 3);

        for (unsigned char c : value) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                result += c;
            }
            else {
                result += '%';
                result += hex[c >> 4];
                result += hex[c & 0x0F];
            }
        }

        return result;
    }


    inline std::string getBinanceSignatureRest(const std::string& key, const std::string& data) {
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int length;
        HMAC(EVP_sha256(), key.c_str(), key.length(), reinterpret_cast<const unsigned char*>(data.c_str()), data.length(), hash, &length);
        std::stringstream ss;
        ss << std::hex << std::setfill('0');
        for (unsigned int i = 0; i < length; i++) {
            ss << std::setw(2) << static_cast<int>(hash[i]);
        }
        
        return ss.str();
    }

    inline std::string getGateioSignatureWs(const std::string& channel, const std::string& event, const std::string& time, const std::string& apiSecret) {
        std::string s("");
        s.append("channel=").append(channel).append("&event=").append(event).append("&time=").append(time);
        std::string hmacsha512hex = crypto::encryptWithHMACForGateio(apiSecret, s);
        return hmacsha512hex;
    }


    inline std::string getGateioSignatureRest(const std::string& method, const std::string& url, const std::string& time, const std::string& queryString, const std::string& payloadString, const std::string& apiSecret) {
        std::string s("");
        std::string hashed_payload = crypto::sha512(payloadString);
        s.append(method).append("\n").append(url).append("\n").append(queryString).append("\n").append(hashed_payload).append("\n").append(time) ;
        std::string hmacsha512hex = crypto::encryptWithHMACForGateio(apiSecret, s);
        return hmacsha512hex;
    }

    inline std::string getGateioSignatureWsApi(const std::string& channel, const std::string& event, const std::string& time, const std::string& reqPara, const std::string& apiSecret) {
        std::string s("");
        s.append(event).append("\n").append(channel).append("\n").append(reqPara).append("\n").append(time);
        std::string hmacsha512hex = crypto::encryptWithHMACForGateio(apiSecret, s);
        return hmacsha512hex;
    }

    // OKX REST 签名: payload = timestamp + method(大写) + requestPath + body
    // 用于 REST 请求头 OK-ACCESS-SIGN
    inline std::string getOkxSignatureRest(const std::string& apiSecret, const std::string& timestamp, const std::string& method, const std::string& requestPath, const std::string& body) {
        std::string s;
        s.reserve(timestamp.size() + method.size() + requestPath.size() + body.size());
        s.append(timestamp).append(method).append(requestPath).append(body);
        return crypto::hmacSha256Base64(apiSecret, s);
    }

    // OKX WS login 签名: payload 固定 = timestamp(秒字符串) + "GET" + "/users/self/verify"
    // 用于 WebSocket op=login 的 args.sign
    inline std::string getOkxSignatureWsLogin(const std::string& apiSecret, const std::string& timestamp_sec, const std::string& requestPath) {
        std::string s;
        s.reserve(timestamp_sec.size() + 25);
        // s.append(timestamp_sec).append("GET/users/self/verify");
        s.append(timestamp_sec).append(requestPath);
        return crypto::hmacSha256Base64(apiSecret, s);
    }

   // =========================================================================
    // Bybit 签名 (HMAC-SHA256 → hex, 跟 Gateio/OKX 风格一致的 payload 拼串封装)
    //
    //   Bybit V5 有两种签名场景:
    //     REST 头:      payload = timestamp + apiKey + recvWindow + body
    //                   → X-BAPI-SIGN
    //     WS op=auth:   payload = "GET/realtime" + expires_ms
    //                   → op=auth 的 args[2]
    // =========================================================================
    inline std::string getBybitSignatureRest(const std::string& apiSecret, const std::string& timestamp_ms, const std::string& apiKey, const std::string& recvWindow, const std::string& body) {
        std::string s;
        s.reserve(timestamp_ms.size() + apiKey.size() + recvWindow.size() + body.size());
        s.append(timestamp_ms).append(apiKey).append(recvWindow).append(body);
        return crypto::hmacSha256Hex(apiSecret, s);
    }

    inline std::string getBybitSignatureWsAuth(const std::string& apiSecret, const std::string& timestamp_ms, const std::string& requestPath) {
        std::string s;
        s.reserve(12 + 16);   // "GET/realtime" + 毫秒时间戳
        s.append(requestPath).append(timestamp_ms);
        return crypto::hmacSha256Hex(apiSecret, s);
    }

    inline std::string host_of(const std::string& url) {
        std::string h = url;
        auto p = h.find("://");
        if (p != std::string::npos) {
            h = h.substr(p + 3);
        }

        auto q = h.find('/');
        if (q != std::string::npos) {
            h = h.substr(0, q);
        }

        return h;
    }

    inline double str2double(const std::string& s) {
        double d;
        std::stringstream ss;
        ss << s;
        ss >> setprecision(16) >> d;
        ss.clear();
        return d;
    }

    inline double str2double(const char* s) {
        double d;
        std::stringstream ss;
        ss << s;
        ss >> setprecision(16) >> d;
        ss.clear();
        return d;
    }

    inline bool str_equal_zero(const char* originStr) {
        double originNum = stod(originStr);
        if (originNum > -DOUBLE_EPSLION && originNum < DOUBLE_EPSLION) {
            return true;
        }
        return false;
    }

    inline bool str_equal_zero(const std::string& originStr) {
        double originNum = stod(originStr.c_str());
        if (originNum > -DOUBLE_EPSLION && originNum < DOUBLE_EPSLION){
            return true;
        }
        return false;
    }

    inline double fast_atod(std::string_view sv) {
        double val = 0.0;
        auto res = std::from_chars(sv.data(), sv.data() + sv.size(), val);
        if (res.ec != std::errc()) {
            return NAN;
        }
        return val;
    }

    inline int64_t fast_atol(std::string_view sv) {
        int64_t val = 0;
        auto res = std::from_chars(sv.data(), sv.data() + sv.size(), val);
        if (res.ec != std::errc()) {
            return NAN;
        }
        return val;
    }

    inline std::string to_lower(const char* originStr) {
        std::string origin(originStr);
        std::string targetStr(boost::algorithm::to_lower_copy(origin));
        return targetStr;
    }

    inline std::string to_lower(const std::string& originStr) {
        std::string targetStr(boost::algorithm::to_lower_copy(originStr));
        return targetStr;
    }

    inline std::string to_lower(std::string& originStr) {
        std::string targetStr(boost::algorithm::to_lower_copy(originStr));
        return targetStr;
    }

    inline std::string to_upper(const char* originStr) {
        std::string origin(originStr);
        std::string targetStr(boost::algorithm::to_upper_copy(origin));
        return targetStr;
    }

    inline std::string to_upper(const std::string& originStr) {
        std::string targetStr(boost::algorithm::to_upper_copy(originStr));
        return targetStr;
    }

    inline std::string to_upper(std::string& originStr) {
        std::string targetStr(boost::algorithm::to_upper_copy(originStr));
        return targetStr;
    }

     inline bool str_cmp(const std::string& originStr, const std::string& targetStr) {
       if (strcmp(originStr.c_str(), targetStr.c_str() ) == 0) {
            return true;
       }
       return false;
    }

    inline bool str_cmp(const char* originStr, const char* targetStr) {
        if (strcmp(originStr, targetStr) == 0) {
            return true;
        }
        return false;
    }

    inline bool has_str(const std::string& originStr, const std::string& targetStr) {
        size_t idx = originStr.find(targetStr);
        if (idx != std::string::npos) {
            return true;
        }
        return false;
    }

    inline bool has_str(const char* originStr, std::string_view targetStr) {
        return std::strstr(originStr, targetStr.data()) != nullptr;
    }

    inline std::vector<std::string>& split(const std::string& s, char delim, std::vector<std::string>& elems) {
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss, item, delim)) {
            elems.push_back(item);
        }
        return elems;
    }

    inline std::vector<std::string> split(const std::string& s, char delim) {
        std::vector<std::string> elems;
        return split(s, delim, elems);
    }

    inline std::vector<std::string>& split(const std::string& s, const std::string& delims, std::vector<std::string>& elems) {
        char* tok;
        char cchars [s.size()+1];
        char* cstr = &cchars[0];
        strcpy(cstr, s.c_str());
        tok = std::strtok(cstr, delims.c_str());
        while (tok != NULL) {
            elems.push_back(tok);
            tok = std::strtok(NULL, delims.c_str());
        }
        return elems;
    }

    inline std::vector<std::string> split(const std::string& s, const std::string& delims) {
        std::vector<std::string> elems;
        return split(s, delims, elems);
    }

    inline std::string get_random_str(int length) {
        char tmp;							// tmp: 暂存一个随机数
        std::string buffer;						// buffer: 保存返回值

        // 下面这两行比较重要:
        std::random_device rd;					// 产生一个 std::random_device 对象 rd
        std::default_random_engine random(rd());	// 用 rd 初始化一个随机数发生器 random

        for (int i = 0; i < length; i++) {
            tmp = random() % 36;	// 随机一个小于 36 的整数，0-9、A-Z 共 36 种字符
            if (tmp < 10) {			// 如果随机数小于 10，变换成一个阿拉伯数字的 ASCII
                tmp += '0';
            } else {				// 否则，变换成一个大写字母的 ASCII
                tmp -= 10;
                tmp += 'A';
            }
            buffer += tmp;
        }
        return buffer;
    }

    inline std::string get_gateio_client_order_id(const char *strategyId="") {
        return fmt::format("t-{}{}", strategyId, crypto::rdtscp());
    }

    inline std::string get_binance_client_order_id(const char *strategyId=""){
        return fmt::format("BNC{}{}", strategyId, crypto::rdtscp());
    }

    inline std::string get_okx_client_order_id(const char *strategyId=""){
        return fmt::format("OKXC{}{}", strategyId, crypto::rdtscp());
    }

    inline std::string get_bybit_client_order_id(const char *strategyId=""){
        return fmt::format("BBC{}{}", strategyId, crypto::rdtscp());
    }

    inline std::string get_client_order_id(const char *strategyId=""){
        return fmt::format("EXC{}{}", strategyId, crypto::rdtscp());
    }

    inline std::string get_client_order_id(ExchangeType exchangeTypeEnum, const char *strategyId=""){
        switch (exchangeTypeEnum){
            case BINANCE:{
                return get_binance_client_order_id(strategyId);
            }
            case GATEIO:{
                return get_gateio_client_order_id(strategyId);
            }
            case OKX:{
                return get_okx_client_order_id(strategyId);
            }
            case BYBIT:{
                return get_bybit_client_order_id(strategyId);
            }
            default:{
                return get_client_order_id(strategyId);
            }
        }
    }

    inline int get_int_rand(int min, int max) {
        return (rand() % (max-min)) + min;
    }

    inline std::string subreplace(std::string resource_str, std::string sub_str, std::string new_str) {
        std::string dst_str = resource_str;
        size_t pos = 0;
        while ((pos = dst_str.find(sub_str)) != std::string::npos)   //替换所有指定子串
        {
            dst_str.replace(pos, sub_str.length(), new_str);
        }
        return dst_str;
    }

    inline void replace_string(std::string& str, const std::string& search, const std::string& replace) {
        size_t pos = 0;
        while ((pos = str.find(search, pos)) != std::string::npos) {
            str.replace(pos, search.length(), replace);
            pos += replace.length();
        }
    }

    template <size_t N>
    inline void copy_sv_to_char_array(char (&dst)[N], std::string_view src) noexcept {
        size_t n = std::min<size_t>(src.size(), N - 1);
        if (n > 0) {
            std::memcpy(dst, src.data(), n);
        }
        dst[n] = '\0';
    }
}


#endif //INCLUDE_STRING_UTIL_H
