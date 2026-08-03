#pragma once

#include <string>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include "data_struct.h"
#include "redis_client.h"
#include "key_util.h"
#include "string_util.h"

#ifdef USE_INFO_SHM
    #include "InstrumentInfoShm.h"
#endif


// ============================================================================
// SecurityManager
//
// 读侧统一入口, db / tb / utrade 等所有下游进程用。
//
// 数据源两种 (编译期宏 USE_INFO_SHM 切换, 与 contractinfo 保持一致):
//   defined  → 从共享内存 mmap 读 (sm::shm::Reader), 无 Redis 依赖
//   undefined → 从 Redis 读 (旧逻辑)
//
// ============================================================================
// 性能设计要点 (最重要):
//
// 读侧 hot path (db / tb 每次订阅 / 每次下单都要查):
//   get_instrument_info() 只做:
//     1. std::atomic_load(shared_ptr)   ~5ns
//     2. std::unordered_map::find()     ~50-100ns (纯只读, 无锁)
//     3. memcpy 拷贝出去                ~10ns
//   合计 ~100ns, 单核可达 10M/s。
//
// 定时刷新 (5 分钟一次):
//   ⚠️ 关键: 不能一边刷一边影响正在跑的 get_instrument_info。
//   做法: **double-buffered atomic-swap**
//     1. 后台线程在**本地临时 map** 上全量重建 (完全不碰 _infoMap)
//     2. 建完后 一次 std::atomic_store(shared_ptr) 原子替换指针
//     3. 已经在跑 find() 的读线程持的是 shared_ptr 副本, 继续用老 map, 安全退出
//     4. 之后新的 find() 用新 map, 老 map 引用计数归零自动析构
//   结果: refresh 期间读侧 **零阻塞**、**零抖动**、**零锁竞争**。
//
// 老实现使用 tbb::concurrent_unordered_map 每次 insert 都占 bucket 锁,
// 7k+ 条 insert 期间读侧偶发被拖慢, HFT 场景不可接受。这里改成不可变 map
// 加 shared_ptr 原子交换后彻底消除这一路径。
// ============================================================================


namespace sm {

    class SecurityManager {
    public:
        using InfoMap = std::unordered_map<std::string, md::InstrumentInfo>;

        SecurityManager(const char* ip = "127.0.0.1", const int port = 9379,
                        const char* passwd = "", bool needUpdate = true)
        {
            // 初始化空 map, 避免第一次 get 前 _infoMap 为 nullptr
            std::atomic_store(&_infoMap, std::shared_ptr<const InfoMap>(std::make_shared<InfoMap>()));

#ifdef USE_INFO_SHM
            (void)ip; (void)port; (void)passwd;
            const char* env = std::getenv("BTS_INFO_SHM_NAME");
            std::string name = env ? env : sm::shm::kDefaultShmName;
            if (!shmReader_.open(name, /*wait_for_writer_ms=*/5000)) {
                LOG_ERROR("[sm] cannot open instrument info SHM '{}', "
                          "ensure contractinfo is running.", name);
            } else {
                // 陈旧数据检测: last_update_us 距当前超过阈值 → contractinfo 可能挂了。
                // db/tb 仍能用旧快照, 但操作员需要知晓。 阈值取 10 分钟 (contractinfo 60s 发一次
                // + 一次网络重试, 10 分钟内没写就明显异常)。
                uint64_t last_us = shmReader_.last_update_us();
                if (last_us > 0) {
                    uint64_t now_us = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count());
                    if (now_us > last_us + 10ULL * 60 * 1000 * 1000) {
                        LOG_WARN("[sm] SHM instrument info is stale: last_update was "
                                 "{} us ago (>10 min). contractinfo may be offline.",
                                 now_us - last_us);
                    }
                } else {
                    // last_update_us==0 说明 SHM 段刚创建 writer 还没首次 publish,
                    // 或者 SHM 是空段, 稍后 rebuild_from_source 也会拿到空 vec。
                    LOG_WARN("[sm] SHM instrument info has never been published, "
                             "waiting for contractinfo's first publish (60s cycle).");
                }
            }
            rebuild_from_source();   // 首次装载
            if (needUpdate) {
                std::thread(&SecurityManager::instrumentInfo_maintainance_shm, this).detach();
            }
#else
            redisClient = new RedisClient(ip, port, passwd, true, false);
            rebuild_from_source();
            if (needUpdate) {
                std::thread(&SecurityManager::instrumentInfo_maintainance, this).detach();
            }
#endif
        }

        ~SecurityManager() {
#ifndef USE_INFO_SHM
            if (redisClient) {
                delete redisClient;
                redisClient = nullptr;
            }
#endif
        }


        // ====================================================================
        // 公开 API (不因 USE_INFO_SHM 变化)
        // ====================================================================

        // 全量拉取, 只读 SHM 或 Redis (不使用 _infoMap 缓存)
        inline bool get_all_instruments(std::vector<md::InstrumentInfo>& instInfoVec) {
#ifdef USE_INFO_SHM
            return shmReader_.snapshot(instInfoVec);
#else
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
            LOG_ERROR("error when sync smc info");
            return false;
#endif
        }

        // 单条 JSON 解析。 旧版 Redis pub/sub 回调路径用得到; 新版 SHM 模式虽然
        // 没有单条更新通道, 但保留此工具函数向后兼容 (外部可能仍有代码调用它,
        // 而且它本身只做 JSON→struct 转换, 和数据源无关)。
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

        // hot path: 每次订阅 / 下单都要查, ~100ns
        // 内部只做 atomic_load(shared_ptr) → unordered_map::find(),
        // 不涉及任何锁 / SHM / Redis 访问。
        inline bool get_instrument_info(const char* exchId, const char* instType, const char* instId, md::InstrumentInfo& info) {
            std::string key = crypto::get_instrumentInfo_channel_key(exchId, instType, instId);
            auto snapshot = std::atomic_load(&_infoMap);
            if (!snapshot) return false;
            auto found = snapshot->find(key);
            if (found != snapshot->end()) {
                memcpy(&info, &found->second, sizeof(md::InstrumentInfo));
                return true;
            }
            return false;
        }

        inline bool get_instrument_info(ExchangeType exchId, InstType instType, const char* instId, md::InstrumentInfo& info) {
            return get_instrument_info(ExchangeTypeEnum2StrMap[exchId].c_str(), InstTypeEnum2StrMap[instType].c_str(), instId, info);
        }

    private:
        // 在**本地临时 map** 上全量重建, 建完后一次 atomic_store 交换指针。
        // 读侧不会因为 refresh 而变慢。
        void rebuild_from_source() {
            std::vector<md::InstrumentInfo> vec;
            if (!get_all_instruments(vec)) {
                LOG_ERROR("[sm] rebuild_from_source: no data from source");
                return;
            }

            auto newMap = std::make_shared<InfoMap>();
            // 每条 info 存两个 key (instId 和 originInstId), reserve 二倍余量
            newMap->reserve(vec.size() * 2 + 128);
            for (auto& info : vec) {
                std::string k1 = crypto::get_instrumentInfo_channel_key(info.exchangeTypeEnum, info.instTypeEnum, info.instId);
                std::string k2 = crypto::get_instrumentInfo_channel_key(info.exchangeTypeEnum, info.instTypeEnum, info.originInstId);
                (*newMap)[k1] = info;
                (*newMap)[k2] = info;
            }

            // 一次原子交换. 读侧的 atomic_load 要么看到旧 map (安全, 引用计数保持),
            // 要么看到新 map, 不会看到构建中的半成品。
            std::atomic_store(&_infoMap, std::shared_ptr<const InfoMap>(std::move(newMap)));
        }

#ifndef USE_INFO_SHM
        void instrumentInfo_maintainance() {
            while (1) {
                std::this_thread::sleep_for(std::chrono::minutes(5));
                rebuild_from_source();
            }
        }
#else
        // SHM 模式: 5 分钟醒一次, 检测 generation 变化才 rebuild,
        // 否则纯 no-op, 完全不动缓存。
        void instrumentInfo_maintainance_shm() {
            uint64_t last_seen_gen = shmReader_.generation();
            while (1) {
                std::this_thread::sleep_for(std::chrono::minutes(5));
                uint64_t cur = shmReader_.generation();
                if (cur == last_seen_gen) continue;
                rebuild_from_source();
                last_seen_gen = shmReader_.generation();
            }
        }
#endif

    private:
#ifdef USE_INFO_SHM
        sm::shm::Reader shmReader_;
#else
        RedisClient* redisClient = nullptr;
#endif
        // 不可变 map + atomic shared_ptr, 读写完全隔离。
        // 类型 std::shared_ptr<const InfoMap> 强调 map 装载后不再改动 (只能整体替换)。
        std::shared_ptr<const InfoMap> _infoMap;
    };
}