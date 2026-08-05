#pragma once
//
// Ed25519 私钥签名器 (Binance ws-api session.logon / REST 也可用)。
//
// 用法:
//   crypto::Ed25519Signer signer;
//   if (!signer.init_from_pem(acc.secretKey)) { LOG_ERROR("..."); return; }
//   std::string sig_b64 = signer.sign_base64("apiKey=...&timestamp=...");
//
// 输入格式支持:
//   1) 标准 PEM (-----BEGIN PRIVATE KEY-----\n...\n-----END PRIVATE KEY-----)
//   2) JSON 里带 "\\n" 转义的 PEM (会自动展开)
//   3) 纯 base64 body (不带 BEGIN/END, 有些 config 只存 body); PKCS8 raw seed fallback
//
// 线程安全: init_from_pem 只在启动期调用; sign_base64 是纯读, EVP_PKEY 内部按 OpenSSL
// 契约可以从多线程并发调用 (每次 sign 都新建 EVP_MD_CTX)。
//

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include "base64.hpp"

namespace crypto {

class Ed25519Signer {
public:
    Ed25519Signer() = default;

    ~Ed25519Signer() {
        reset();
    }

    Ed25519Signer(const Ed25519Signer&) = delete;
    Ed25519Signer& operator=(const Ed25519Signer&) = delete;

    Ed25519Signer(Ed25519Signer&& o) noexcept : pkey_(o.pkey_) { o.pkey_ = nullptr; }
    Ed25519Signer& operator=(Ed25519Signer&& o) noexcept {
        if (this != &o) { reset(); pkey_ = o.pkey_; o.pkey_ = nullptr; }
        return *this;
    }

    // 从 PEM 字符串加载 Ed25519 私钥. 成功返回 true, 失败返回 false 并保持 pkey_ = nullptr。
    // 输入里的 "\\n" (JSON 里存 PEM 时常见的 escape 形式) 会被自动展开为 '\n'。
    bool init_from_pem(std::string pem) {
        reset();
        if (pem.empty()) return false;

        // "\\n" → "\n"
        for (size_t p = 0; (p = pem.find("\\n", p)) != std::string::npos; ) {
            pem.replace(p, 2, "\n");
            p += 1;
        }

        // (1) 标准 PEM
        if (pem.find("BEGIN") != std::string::npos) {
            BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
            if (bio) {
                EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
                BIO_free(bio);
                if (pkey) { pkey_ = pkey; return true; }
            }
        }

        // (2) fallback: 从 body 抽 base64 → decode → 从 PKCS8 前缀提取 32B seed
        //     Ed25519 私钥 PKCS8 编码: 48B = 16B 固定前缀 + 32B seed
        std::string b64 = extract_b64_chars(pem);
        auto der = b64_decode(b64);
        if (der.size() == 48) {
            static const unsigned char kPrefix[16] = {
                0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06,
                0x03, 0x2b, 0x65, 0x70, 0x04, 0x22, 0x04, 0x20
            };
            if (std::memcmp(der.data(), kPrefix, 16) == 0) {
                EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(
                    EVP_PKEY_ED25519, nullptr, der.data() + 16, 32);
                if (pkey) { pkey_ = pkey; return true; }
            }
        }
        return false;
    }

    // 签名 payload, 返回标准 base64 (含 '+' / '/' / '=' padding, 非 URL-safe)。
    // 放 JSON body 里直接用即可; 放 REST query 需要外层再 url_encode。
    // 失败 (pkey 未初始化 / OpenSSL 出错) 返回空串。
    std::string sign_base64(std::string_view payload) const {
        if (!pkey_) return {};
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (!ctx) return {};

        // Ed25519 走 single-shot (EVP_DigestSign, 不能 update 分段)
        if (EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey_) <= 0) {
            EVP_MD_CTX_free(ctx);
            return {};
        }
        size_t siglen = 0;
        if (EVP_DigestSign(ctx, nullptr, &siglen,
                           reinterpret_cast<const unsigned char*>(payload.data()),
                           payload.size()) <= 0) {
            EVP_MD_CTX_free(ctx);
            return {};
        }
        std::vector<unsigned char> sig(siglen);
        if (EVP_DigestSign(ctx, sig.data(), &siglen,
                           reinterpret_cast<const unsigned char*>(payload.data()),
                           payload.size()) <= 0) {
            EVP_MD_CTX_free(ctx);
            return {};
        }
        EVP_MD_CTX_free(ctx);
        return websocketpp::base64_encode(sig.data(), siglen);
    }

    bool valid() const noexcept { return pkey_ != nullptr; }

private:
    void reset() noexcept {
        if (pkey_) { EVP_PKEY_free(pkey_); pkey_ = nullptr; }
    }

    // 只保留 base64 字母表字符 (A-Za-z0-9+/=), 用来从 PEM (含空白/注释) 抽 base64 body
    static std::string extract_b64_chars(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '+' || c == '/' || c == '=') {
                out.push_back(static_cast<char>(c));
            }
        }
        return out;
    }

    static std::vector<unsigned char> b64_decode(const std::string& b64) {
        std::vector<unsigned char> out;
        if (b64.empty()) return out;
        BIO* bmem = BIO_new_mem_buf(b64.data(), static_cast<int>(b64.size()));
        BIO* b64f = BIO_new(BIO_f_base64());
        BIO_set_flags(b64f, BIO_FLAGS_BASE64_NO_NL);
        BIO* bio  = BIO_push(b64f, bmem);
        out.resize((b64.size() * 3) / 4 + 4);
        int n = BIO_read(bio, out.data(), static_cast<int>(out.size()));
        BIO_free_all(bio);
        if (n <= 0) { out.clear(); return out; }
        out.resize(static_cast<size_t>(n));
        return out;
    }

    EVP_PKEY* pkey_ = nullptr;
};


// REST query string 里放 base64 签名需要 URL-encode (Ed25519 base64 会含 '+' '/' '=')
// 严格 RFC3986: unreserved = ALPHA / DIGIT / '-' / '_' / '.' / '~'
inline std::string url_encode_component(std::string_view s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out.append(buf, 3);
        }
    }
    return out;
}

}  // namespace crypto