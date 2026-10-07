OMS Shared Memory Order Store

共享内存环形订单存储 —— 替代 tbb::concurrent_unordered_map 的固定容量、跨进程可查、有 GC 兜底的 OMS 订单表实现。

一、动机

原方案 (OrderManager.h):

tbb::concurrent_unordered_map<std::string, std::string> clientOrderId2OrderSysIdMap;
tbb::concurrent_unordered_map<std::string, std::string> orderId2OrderSysIdMap;
tbb::concurrent_unordered_map<std::string, pubsub::RCommand> orderSysId2OrderResponseMap;

痛点:





无 GC, 长期跑必然 OOM



每 entry 附带 shared_ptr / node overhead, 内存效率低



跨进程共享需另一跳 IPC (Redis / socket)



crash 丢

本方案:





固定 100k slot × 1024B ≈ 100MB, 内存永远封顶



环形覆盖 FINISHED 单, 24h 卡单强制回收兜底



mmap 到 /dev/shm, 策略进程直接 mmap 读, 免 IPC



seqlock 无锁读, 单写多读, 读侧 ~200ns



跨进程持久, tb crash 后 restart 自动 recover orphan slot



二、架构

┌────────────────────────────────────────────────────────────────┐
│ tb process                                                     │
│  ┌────────────────┐   pubsub MPSC   ┌────────────────┐         │
│  │ TradeUnit(N)   │────────────────>│ OMS thread     │         │
│  │ (per exchange) │                 │ (single writer)│         │
│  └────────────────┘                 │                │         │
│                                     │  upsert(rcmd)  │         │
│                                     └────────┬───────┘         │
└──────────────────────────────────────────────┼─────────────────┘
                                               │ mmap 写
                                               ▼
                          ┌──────────────────────────────┐
                          │ /dev/shm/tb_oms.dat          │
                          │ [Header 4KB]                 │
                          │ [100k × 1024B Slots ~100MB]  │
                          │ [3 × 4N × 32B Index ~48MB]   │
                          │ 总 ~146MB, 固定不涨          │
                          └──────────────┬───────────────┘
                                         │ mmap 读 (无锁)
                    ┌────────────────────┼────────────────────┐
                    ▼                    ▼                    ▼
             ┌────────────┐        ┌────────────┐       ┌────────────┐
             │ strategy 1 │        │ strategy 2 │  ...  │ oms_query  │
             │ (readonly) │        │ (readonly) │       │ (tool)     │
             └────────────┘        └────────────┘       └────────────┘

三、数据结构

SHM 布局

[OmsShmHeader (4 KB)]
  magic / version / capacity / next_slot_hint / stats / TTL 配置
[OmsSlot 数组 (N × 1024B)]
  每单一条: seqlock + state + 3 时间戳 + 3 key 副本 + RCommand
[主索引 IDX_ORDER_SYS_ID (4N × 32B)]
[别名索引 IDX_CLIENT_ORDER  (4N × 32B)]
[别名索引 IDX_EXCHANGE_ID   (4N × 32B)]

索引容量是 next_pow2(4N) 而**不是** 4N, 且**必须是 2 的幂** (三处探测都用 (hash+i) & (cap-1))。
倍数取 4 而不是 2 的原因见 OMS_SHM_REVIEW.md §2.5: 环填满后 empty 必然排干到 0, 插入能否成功
只取决于 kMaxProbeIndex 步内能否碰到 tombstone, 上界是最长连续 live 段。2N 时可复用桶密度
只有 0.5, 实测最长连续段 19 → 35 → 38~55 (随容量增长) 全部 > 32 → 静默丢单; 4N 把密度提到
0.75, 连续段塌到 13~20。代价是索引内存翻倍 (100k slot: 121.7 → 145.7 MiB)。

Slot 状态机

     ┌─────────────────────────────────────────┐
     │                                         │
     ▼                                         │
  EMPTY ─── alloc + write ──> LIVE ─── terminal update ──> FINISHED
     ▲                         │                              │
     │                         │                              │ age > min_reclaim_age
     │                         │                              │ 或 age > max_live_stale
     │                         │                              ▼
     └──────── reclaim ─────── ┴────── force_reclaim ──── RECLAIMING (transient)





EMPTY → 从未用过, 或已 recycle 完成



LIVE → 活单 (NEW / PARTFILLED / PENDING)



FINISHED → 终结 (FILLED / CANCELED / REJECTED / FAILED)



RECLAIMING → writer 独占中间态, reader 见到应返回 not-found

回收策略 (3 优先级)







状态



条件



说明





EMPTY



无



首选





FINISHED



now - finish_time_ns > min_reclaim_age_ns (默认 60s)



常见路径





LIVE



now - last_update_time_ns > max_live_stale_ns (默认 24h)



卡单兜底, 打 WARN + 计数



四、接口

共享 API (基类 OmsShmSegment, Writer / Reader 都有)

查询 (seqlock 无锁, reader / writer 都能调):

pubsub::RCommand out;
seg.lookup_by_orderSysId("t-strat1-9382", out);
seg.lookup_by_clientOrderId("t-strat19382", out);               // **已复合**的字符串 (strategyId + cid)
seg.lookup_by_orderId("ex-8001001", out);
seg.exists_by_orderSysId("t-strat1-9382");   // 便捷: 只查存不存在

★ 上面的 bool 版只回答"能不能查到"。**对账场景请改用三态版 `_ex()`** —— 见下面 B3 那段。
  注意 `lookup_by_clientOrderId` 收的是**已复合**的 key 字符串 (writer 存的就是复合 key);
  按 (strategyId, cid) 查请用 `lookup_by_client(strategyId, cid, out)`。

查询三态 (B3): `lookup_by_*_ex()` 返回 `LookupStatus`

auto st = seg.lookup_by_orderSysId_ex("t-strat1-9382", out);
switch (st) {
    case OmsShmSegment::LookupStatus::OK:        /* 找到, out 可用 */            break;
    case OmsShmSegment::LookupStatus::NOT_FOUND: /* **确认**不存在 */             break;
    case OmsShmSegment::LookupStatus::BUSY:      /* 索引里有这条, 但这一瞬间读不到
                                                    (写者正在写 / slot 正在被 reclaim)
                                                    → 稍后重试, **别**当"不存在" */ break;
}
const char* s = OmsShmSegment::to_string(st);   // "OK" / "NOT_FOUND" / "BUSY"

★ 为什么必须有 BUSY: 旧的 bool 版把 NOT_FOUND 和 BUSY 混成同一个 false。 重启对账
  据此会把**活单判成死单** → 重复下单 / 误平仓。 判据是"BUSY 与 NOT_FOUND 必须可区分"。
★ `lookup_by_*` (bool) 语义不变 = (status == OK), 所有老调用点不受影响。
★ BUSY 次数记在**进程内**: `seg.local_lookup_busy()`。 **不落 SHM** —— reader 的映射是
  PROT_READ, 写共享计数器会 SIGBUS (实测过)。 对账/策略侧周期读它打进自己的 metrics 即可。

// 遍历 (慢, O(N), 只用于对账 / snapshot)
seg.iterate_live([](const pubsub::RCommand& r){ /* ... */ });
seg.iterate_finished(...);

// 监控
auto stats = seg.stats();
if (stats.live_stale > 0) alert(...);
if (seg.local_lookup_busy() > 0) warn("读侧被写者抢了 / 索引有陈旧条目");

Writer (只在 OMS 单进程/单线程调用, 有写权限)

oms::shm::OmsShmWriter writer;
writer.open("/dev/shm/tb_oms.dat", /*capacity=*/131072);  // slot 数, 1 .. 2^28; 索引容量自动 = next_pow2(4N)

// 写路径
pubsub::RCommand rcmd = /* 从 TradeUnit 收到的订单事件 */;
uint32_t slot = writer.upsert(rcmd);   // 存在则 update, 不存在则 insert
if (slot == oms::shm::kInvalidSlot) {
    // 环耗尽, 记录告警
}

// OMS 也能查 —— 用于 query_order 响应、update 前判断、对账
pubsub::RCommand cur;
if (writer.lookup_by_orderSysId(sysId, cur)) {
    if (cur.body.orderResponse.orderStatus == OS_FILLED) {
        // 已成交, 拒绝撤单请求
    }
}

// 罕见: 手动删除
writer.remove(rcmd.body.orderResponse.orderSysId);

// 运行期调阈值 (不用重启)
writer.set_max_live_stale_ns(6ULL * 3600 * 1'000'000'000);   // 卡单阈值改 6h

// 危险: 清空 (仅工具 / 测试)
writer.reset_all();

Reader (策略 / 工具, 多进程并发, 只读 mmap)

oms::shm::OmsShmReader reader;
reader.open("/dev/shm/tb_oms.dat");   // PROT_READ, 写操作会 SEGV

// Reader 继承基类所有查询 API
reader.lookup_by_orderSysId(...);
reader.iterate_live(...);
reader.stats();

为什么 Writer 也有查询: OMS 主循环里典型逻辑:





收到 execution report → upsert 写入



策略调 query_order → OMS 用 lookup_* 从自己写的 shm 读回



收到撤单请求 → 先 lookup 检查状态是否允许撤



WS 重连后对账 → iterate_live 遍历所有本地活单, 比对交易所快照

并发安全: Writer 也走 seqlock read, 与 Reader 完全对称。 单写者契约保证 write 路径不冲突, 但查询路径本身允许在 OMS 主线程和策略进程之间任意并发。



五、工具

oms_query —— CLI 查单

oms_query --shm=/dev/shm/tb_oms.dat --sys=<orderSysId>
oms_query --shm=/dev/shm/tb_oms.dat --cid=<clientOrderId>   # int 或字符串都行
oms_query --shm=/dev/shm/tb_oms.dat --oid=<exchangeOrderId>
oms_query --shm=/dev/shm/tb_oms.dat --list-live
oms_query --shm=/dev/shm/tb_oms.dat --list-finished
oms_query --shm=/dev/shm/tb_oms.dat --list-stale       # 即将被强制回收的 LIVE 卡单
oms_query --shm=/dev/shm/tb_oms.dat --stats

oms_bench —— 性能测试

oms_bench --shm=/dev/shm/tb_bench.dat --capacity=131072 \
          --iters=1000000 --readers=4 --mixed-ttl-ms=0 --reset

测量: insert / update / lookup 吞吐 + p50 / p95 / p99 / p999 / max 延迟。 混合场景 1 writer + N readers
(writer 和每个 reader 的吞吐都会打印)。

★ --mixed-ttl-ms (默认 0) 只在 MIXED 相位生效, 它决定这个相位能不能"跑得下去":

    可持续写入速率上限 ≈ slot_cap / min_reclaim_age

  生产默认 TTL 是 60s, 所以 131072 slot 只够约 2185 单/秒; 而本机实测写入 ~70 万单/秒
  → 环 0.2 秒就写满, 之后**每一张单都失败**并刷 [OmsShm][ERROR]。
  MIXED 相位因此默认用 0 (= FINISHED 立刻可回收), 保证任何机器/容量都不会写满;
  想复现"环写满"的行为, 显式给一个大值 (如 --mixed-ttl-ms=60000)。

  自检: 相位结束会打印 `✓ 可持续: 0 丢单` 或 `✗ 不可持续: 丢了 N 单 … 上限 ≈ M 单/秒`。

★ --reset 时若文件里已有的容量 ≠ --capacity, 会 unlink 重建。
  (打开已存在的文件时容量一律从 header 读, --capacity 会被静默忽略 ——
   一个遗留的 16k 文件会让 --capacity=131072 白传, 测出来的东西完全不是以为的。)

oms_demo —— ⚠ 当前**不是** demo

`oms_demo.cpp` 目前与 `oms_bench.cpp` **逐字节相同** (md5 一致), 所以 `./oms_shm.sh demo`
跑的实际是性能测试, 而不是本节原先描述的"功能演示"。功能演示请看 oms_test。
待办 (D1): 要么补一个真的 demo, 要么删掉 oms_demo。

oms_test —— 功能 / 边界 / 并发断言集 (推荐入口)

覆盖 (18 节 / 182 条断言): 布局常量 / 三层索引往返 / orderId 生命周期 / LIVE→FINISHED→reclaim /
环满不覆盖 LIVE / 慢路径救回 / key 同步失败 / 别名插入失败 / 卡单强制回收 / 稳态 tombstone /
探测余量 / iterate 跳过计数 / 版本与容量拒绝 / 崩溃恢复 / reset_all / 并发 seqlock 撕裂读 /
remove 与 recover_orphan_slots 的返回值与幂等 / is_open + created_new + close 往返。
退出码 0 = 全绿。推荐用 `./oms_shm.sh test` 跑 (它会先强制重新构建, 再顺带校验
`oms_query --stats` 的输出格式仍满足 doctor 的解析契约)。

oms_shm.sh —— 一站式运维脚本

./oms_shm.sh build               # 编译 4 个工具 (query / bench / demo / test)
./oms_shm.sh check               # 环境检查 (/dev/shm 大小 / tmpfs / 权限)
./oms_shm.sh demo                # ⚠ 目前等价于 bench (见上)
./oms_shm.sh stats [shm]         # 打 stats
./oms_shm.sh live [shm]          # 列活单
./oms_shm.sh stale [shm]         # 列卡单
./oms_shm.sh bench               # 跑性能测试
./oms_shm.sh test [dir]          # 跑断言集 + 解析契约检查 (推荐)
./oms_shm.sh watch [shm]         # 持续监控 (5s 刷新)
./oms_shm.sh doctor [shm]        # 一键健康检查, 红字提示问题
CONFIRM=1 ./oms_shm.sh reset [shm]  # 危险: 清空 SHM



六、部署

1. 系统 / Docker 配置

Docker Compose:

services:
  tb:
    shm_size: 384m           # 100k slot 需要 ~146MB (slot 100MB + 索引 48MB), 加 headroom
    volumes:
      - tb_shm:/dev/shm      # 可选: 独立命名 volume, 便于备份
volumes:
  tb_shm:
    driver: local
    driver_opts:
      type: tmpfs
      device: tmpfs
      o: size=384m

Kubernetes (StatefulSet):

volumes:
  - name: tb-shm
    emptyDir:
      medium: Memory
      sizeLimit: 384Mi

系统 tmpfs (/etc/fstab):

tmpfs   /dev/shm   tmpfs   defaults,size=1G,nodev,nosuid   0 0

2. 编译工具

cd new_dev/tb/tools
./oms_shm.sh build

生成: oms_query, oms_bench, oms_demo, oms_test。

3. 启动前检查

./oms_shm.sh check

输出示例:

✓ /dev/shm size = 256MB (adequate)
✓ /dev/shm is tmpfs
⚠ /dev/shm/tb_oms.dat not created yet

4. 从 OrderManager (tbb map) 迁移

核心映射表:







老代码 (OrderManager.cpp)



新代码 (OmsShmWriter)





orderSysId2OrderResponseMap[sid] = rcmd



writer.upsert(rcmd)





clientOrderId2OrderSysIdMap[strategyId+cid] = sid



自动 (upsert 内建 3 层索引)





orderId2OrderSysIdMap[oid] = sid



自动





iter = orderSysId2OrderResponseMap.find(sid); iter->second



writer.lookup_by_orderSysId(sid, out)





getOrderSysId(cid, strategyId, ...) 双查



writer.lookup_by_client(strategyId, cid, out), 失败退 lookup_by_orderId(oid)





iter->second.body.orderResponse.xxx = new_val (原地改)



read-modify-write: lookup → merge → upsert





iterate map for 对账



writer.iterate_live(cb) / iterate_finished(cb)





load_data from SQLite



✂️ (原为 no-op)





store_rcmd_data to SQLite



保留为可选冷层, 独立线程定期 dump iterate_finished





getOrderSysId(exchangeType, strategyId) (生成新 ID)



✅ 保留原逻辑, 与存储无关

读-改-写模式 (对应 onOrderUpdate L237-311):

bool OrderManager::onOrderUpdate(pubsub::RCommand& rcmd) {
    pubsub::RCommand cur;
    if (!shm_writer_.lookup_by_orderSysId(
            rcmd.body.orderResponse.orderSysId, cur)) {
        return false;   // 找不到 slot
    }

    // === 原来在 iter->second 上原地做的所有 merge 逻辑, 现在改在 cur 上做 ===
    // 1) 溢出检查
    if (rcmd.body.orderResponse.volumeTraded
            > cur.body.orderResponse.volumeTotal + ZERO_NUM) {
        LOG_ERROR("overfilled: ...");
    }

    // 2) BINANCE REST 已成交但无成交价 → 丢
    if ((rcmd.body.orderResponse.orderStatus == OS_PARTFILLED ||
         rcmd.body.orderResponse.orderStatus == OS_FILLED) &&
        rcmd.body.orderResponse.apiSourceEnum == AS_ADD_NEW_ORDER &&
        rcmd.body.orderResponse.exchangeTypeEnum == BINANCE) {
        return false;
    }

    // 3) 成交量单调递增
    bool volumeIncreased = false;
    if (rcmd.body.orderResponse.volumeTraded > cur.body.orderResponse.volumeTraded) {
        cur.body.orderResponse.tradeDiff =
            rcmd.body.orderResponse.volumeTraded - cur.body.orderResponse.volumeTraded;
        cur.body.orderResponse.volumeTraded = rcmd.body.orderResponse.volumeTraded;
        cur.body.orderResponse.tradePrice   = rcmd.body.orderResponse.tradePrice;
        cur.body.orderResponse.fillPrice    = rcmd.body.orderResponse.fillPrice;
        cur.body.orderResponse.updateTime   = crypto::getCurrentTime();
        volumeIncreased = true;
    }

    // 4) 状态优先级前进 (只允许 status 递增)
    bool statusAdvanced = false;
    if (!crypto::isFinalOrderStatus(cur.body.orderResponse.orderStatus)) {
        int oldP = crypto::getOrderStatusPriority(cur.body.orderResponse.orderStatus);
        int newP = crypto::getOrderStatusPriority(rcmd.body.orderResponse.orderStatus);
        if (newP > oldP) {
            cur.body.orderResponse.orderStatus = rcmd.body.orderResponse.orderStatus;
            cur.body.orderResponse.updateTime  = crypto::getCurrentTime();
            statusAdvanced = true;
        }
    }

    // 5) 补 orderId (交易所 ACK 之后才有)
    if (rcmd.body.orderResponse.orderId[0]) {
        std::strncpy(cur.body.orderResponse.orderId,
                     rcmd.body.orderResponse.orderId, ORDER_SIZE);
    }

    // 6) REJECTED 保留错误信息
    if (rcmd.body.orderResponse.orderStatus == OS_REJECTED) {
        cur.body.orderResponse.errorId = rcmd.body.orderResponse.errorId;
        // ⚠ 别用 ORIGINMSG_SIZE (=256): originMsg 是 char[128], strncpy 会按 n 补 NUL,
        //   越界写 128 字节并把紧随其后的 updateTime / apiSourceEnum 清零。
        std::snprintf(cur.body.orderResponse.originMsg,
                      sizeof(cur.body.orderResponse.originMsg),
                      "%s", rcmd.body.orderResponse.originMsg);
    }

    // === 原子写回 ===
    shm_writer_.upsert(cur);

    // === 决定是否往上层 push (跟旧代码一致) ===
    if (rcmd.body.orderResponse.orderStatus == OS_FAILED) {
        std::memcpy(&rcmd, &cur, sizeof(rcmd));
        rcmd.cmdTypeEnum = pubsub::CMD_RPT_ORDER_RESPONSE;
        rcmd.body.orderResponse.orderStatus = OS_FAILED;
        rcmd.body.orderResponse.apiSourceEnum = AS_CANCEL_ORDER;
        return true;
    }

    bool isQueryOrder = rcmd.cmdTypeEnum == pubsub::CMD_RPT_QUERY_ORDER;
    if (!statusAdvanced && !volumeIncreased && !isQueryOrder) return false;

    cur.body.orderResponse.apiSourceEnum = rcmd.body.orderResponse.apiSourceEnum;
    cur.cmdTypeEnum = pubsub::CMD_RPT_ORDER_RESPONSE;
    std::memcpy(&rcmd, &cur, sizeof(rcmd));
    return true;
}

关键点:





lookup + upsert 不会 race —— OMS 单写者契约保证串行



upsert 会保持已有的 3 个别名索引 (orderSysId/client/orderId), 老 slot 数据完整覆盖



若 orderId 从空变有 (下单 ACK 后), upsert 里 update_slot 会自动补 orderId 别名索引

新增 API: lookup_by_client(strategyId, cid, out) 匹配老 OMS strategyId+cid 复合 key 语义, 防跨策略同 int64 cid 撞车。 CLI 用 --strategy=X --cid=N 或直接 --cid=<already_composed_string> 。

5. 集成到 OMS

修改 tb/include/oms/OrderManager.h, 删除 3 个 tbb map, 加 shm writer:

#include "oms/OmsShm.h"

class OrderManager {
    // 老 3 个 tbb map 全部删掉:
    //   tbb::concurrent_unordered_map<string, string> clientOrderId2OrderSysIdMap;
    //   tbb::concurrent_unordered_map<string, string> orderId2OrderSysIdMap;
    //   tbb::concurrent_unordered_map<string, RCommand> orderSysId2OrderResponseMap;
    oms::shm::OmsShmWriter shm_;   // 三件套一次搞定
    // ...
public:
    void preStart() {
        std::string path = "/dev/shm/tb_oms.dat";
        uint32_t cap = 131072;
        // config 可覆盖
        shm_.open(path, cap);
        LOG_INFO("OMS SHM ready path={} cap={}", path, cap);
    }
};

getOrderSysId (辅助函数) 改成:  ★ 三态, 并且**直接吐出完整报单体** (见 §12.8)

oms::shm::OmsShmSegment::LookupStatus
OrderManager::getOrderSysId(int64_t cid, const char* strategyId,
                            pubsub::RCommand& out, const char* orderId) {
    // 一次 lookup 就同时拿到 orderSysId 和整张报单体 —— 老代码要查两次
    // (client key→sysId, sysId→报单体), 因为老的第一张表**只存字符串**。
    auto ls = shm_.lookup_by_client_ex(strategyId, cid, out);
    if (ls == LookupStatus::OK) return ls;
    if (orderId && orderId[0]) {
        ls = shm_.lookup_by_orderId_ex(orderId, out);
        if (ls == LookupStatus::OK) return ls;
    }
    return ls;      // ★ 返回三态而不是 bool: BUSY 不等于 NOT_FOUND
}

processTcmd 里的 orderSysId2OrderResponseMap[orderSysId] = rcmd 变 shm_.upsert(rcmd); (三个索引自动落地)。

Phase 1 dual-write 建议: 保留原 tbb map 一周, 边跑边对比确认无 mismatch, 再删。



七、监控与告警

关键指标 (通过 oms_query --stats 或 stats() API 拿)







指标



告警阈值



含义





total_alloc_failures



> 0



有单没写进 SHM (🔴严重); 成因看下面 alloc_exhausted (环满) / key_sync_failures (索引饱和)





total_alloc_exhausted



> 0



真·环满: 快慢两遍全表扫描都没找到可回收 slot → **单丢了** (🔴严重); 看 live / finished / min_reclaim_age




total_alloc_slowpath



> 0



快路径 128 步落空、被全表扫描**救回** (🟡单没丢, 但余量在消耗); 在途单过于集中 → 考虑加大 slot_capacity




total_key_sync_failures



> 0



key 变更时索引插入失败, 已保持旧 key (🟡索引饱和); 订单仍可查, 只是没跟上这次变更



total_alias_insert_failures



> 0



主索引 OK 但 clientOrderId / orderId 别名插入失败 (🟡索引饱和); 单**在** SHM 里 (按 orderSysId 查得到), 但用该别名查不到 → 策略按 clientOrderId 查活单会落空 (漏撤单 / 重复下单)。**不计入** total_alloc_failures



sustainable_insert_rate



> 0



可持续写入速率上限 = slot_cap / min_reclaim_age (单/秒); 0 = 不限制 (min_reclaim_age=0 时)。**这是容量与 TTL 一起决定的硬上限** —— 稳态占用 ≈ 写入速率 × min_reclaim_age, 实际下单速率超过它, 环必然写满并开始丢单 (跟上层 finalize 及不及时无关)




worst_live_run



>= probe_max



索引已饱和: 探测步数被连续 live 段吃光 → 正在丢单 (🔴严重); 余量 < 1.5 倍则 🟡



total_stale_live_reclaims



> 0



有卡单被强制回收 (🟡上层 bug)





live_stale



> 0



目前有 24h+ 无更新的活单 (🟡观察)





finished 占比



> 80%



FINISHED 占比过高, min_reclaim_age 是否合理





reclaiming



稳态 = 0



若长期非 0, 可能有 crash 未 recover

推荐告警

# cron 每分钟
* * * * * /path/to/oms_shm.sh doctor >> /var/log/oms_health.log 2>&1

或直接接 Prometheus (代码里加 --metrics-port= 参数暴露 HTTP)。

B3: lookup BUSY 次数 (进程内, 不在上表)

`seg.local_lookup_busy()` —— "索引里有这条, 但这一瞬间读不到一致快照"的次数。

★ **刻意不放进 SHM**, 所以不在 `--stats` / `doctor` 里: reader 的映射是 PROT_READ
  (open() 里 `read_only ? PROT_READ : PROT_READ|PROT_WRITE`), 往共享 header 写计数器
  会撞写保护页 → SIGBUS。 而且策略进程都是只读 reader, 共享计数收不到它们的 BUSY,
  只会变成一个"静默少报"的假健康指标 —— 比没有更糟。
★ 所以它按**进程**计: 每个进程读自己的, 打进自己的 metrics / 日志。
★ 判读: 偶发非零后归零 = 正常 (读者被写者抢了一下, 重试即可); **持续增长** = 读侧长期被抢,
  或索引里有陈旧条目 —— 结合 `worst_live_run` / `probe_max` 一起看。

八、故障处理

Q1: tb 起来看到 "recover orphan RECLAIMING slot" WARN

说明: 前次 tb 崩在 alloc 之后 / 写完成之前, slot 被自动归位。 通常 recover 1-2 个无害; 若 recover 数十以上, 检查前次崩溃日志。

Q2: alloc_failures > 0, tb 报 upsert 失败

v4 起成因可以直接从计数分辨, 不用猜:

total_alloc_exhausted > 0   → 真·环满 (快慢两遍全表扫描都没找到可回收 slot), 单确实丢了。
                              stderr 有 "[ERROR] alloc_slot 快慢两遍扫描都失败" + 各状态快照
key_sync_failures    > 0    → 索引饱和, key 变更时插不进去 (单还在, 已保持旧 key, 见 Q8)
total_alloc_slowpath > 0    → 单没丢 (被慢路径救回), 但余量在消耗 → 预警

注意 v4 起 alloc_slot 是"快路径 128 步 + 全表兜底"两段式: 128 步落空**不再直接丢单**,
只有全表扫也找不到才会失败 —— 所以 alloc_failures > 0 现在**必然**是真环满。

另外先看 stderr 里那句判读 (B6 起日志自带结论), 两种成因动作完全相反:





是否策略在海量下单不撤?



是否 max_live_stale_ns 太长 (默认 24h) ?



- 日志写 "最长连续 live 段 >= probe_max → 聚集": **不要**加大 slot_cap。 环满后 empty 必然
  排干到 0, 与 slot_cap 无关; 要加大的是 index_capacity 的倍数 (现在是 next_pow2(4*slot_cap))。
  `oms_query --stats` 的 worst_live_run / probe_max 就是余量。
- 日志写 "max_live_run < probe_max, 本不该失败": 异常, 连同日志里的快照上报。

仍要排除的: 是否策略在海量下单不撤? 是否 max_live_stale_ns 太长 (默认 24h) ? slot 环本身
耗尽时 (与索引无关), `oms_query --stats` 会显示 live 顶到 capacity 且 empty=0。

Q3: stale_live_reclaims 持续增长

根因: 有一批订单永远不 finalize (bug), 被 24h TTL 强制回收。 排查:

oms_query --list-stale        # 看哪些 orderSysId 卡了
grep "<orderSysId>" logs/     # 反查策略侧 / 交易所回报

Q4: 升级 pubsub 结构后 tb 起不来

根因: sizeof(RCommand) 变了, shm 布局不兼容。 处理:

# 撤所有活单 (通过交易所 UI/API)
CONFIRM=1 ./oms_shm.sh reset
# 重启新版 tb

Q5: 想现场调 max_live_stale (不重启 tb)

在 OMS 里加个 admin RPC 调 writer.set_max_live_stale_ns(ns) 即可, 立即生效。

Q6: reader 端 lookup 一直 NOT_FOUND, 但 tb 显示单在

原因: 通常是 shm 路径不一致。 确认 reader / writer 用同一 path。 另可能是版本不匹配, magic 校验拒绝加载。

★ 如果只是**偶尔**查不到 (高并发下 0.0x%), 先用三态版确认到底是哪种:

auto st = reader.lookup_by_orderSysId_ex(sid, out);
if (st == OmsShmSegment::LookupStatus::BUSY) {
    // 索引里有这条, 只是这一瞬间读不到 (写者正在写 / slot 正在被 reclaim)
    // → 重试即可。 用 bool 版会把它和"不存在"混在一起, 看起来就像单丢了。
}

`reader.local_lookup_busy()` 持续增长说明读侧一直被写者抢 (或索引里有陈旧条目);
单次非零、之后归零属正常。

Q7: 升级到索引 4N 之后 tb 起不来, 报 "magic/version mismatch" 或 "< 4 * slot_capacity"

这是**预期**的, 不是 bug。 索引容量算法从 2N 改成 next_pow2(4N) 时 kVersion 从 2 提到了 3
(旧文件的 index_capacity 也是 2 的幂, 光靠幂校验挡不住, 所以必须靠版本号)。 老文件索引密度
只有 0.5, 环满后会静默丢单, 不能带病继续跑。 处理:

# 撤所有活单 (通过交易所 UI/API) —— 重建会丢掉 shm 里的活单状态
CONFIRM=1 ./oms_shm.sh reset     # 或直接 rm /dev/shm/tb_oms.dat
# 重启新版 tb, 文件会按 4N 重建
./oms_shm.sh doctor              # 确认 ✓ worst_live_run 远小于 probe_max



Q8: key_sync_failures > 0 (key 变更时索引插入失败)

含义: 报单体里的 key 变了 (典型是 orderId 首次从空变成交易所单号), 但索引 32 步内没有可复用
bucket, 新 key 挂不上去。 v4 起的处理是**保持旧 key 与副本不变** —— 订单仍然查得到, 只是没
跟上这次变更。

v4 之前是另一回事: 顺序是 tombstone(旧) → memcpy(新) → insert(新) 且**丢弃返回值**, 一旦
insert 失败就变成"旧条目已删、副本已改、新条目没挂上" —— **新旧 key 都查不到**, slot 彻底
不可达 (不只是"新 key 查不到")。 实测 (kMaxProbeIndex=1 的头文件副本, 5 轮改 key, 320 次)
旧代码破坏 32 次不变量, 新代码 0 次。

动作: 按索引饱和处理 —— 看 oms_query --stats 的 index live/tomb/empty 与 worst_live_run,
必要时加大 index_capacity 的倍数。 日志前 8 条必打、之后每 4096 次一条, 计数永远准。




九、契约与不变量 (违反必崩)





单写者 —— 只允许 OMS 主线程调 upsert / remove / reset_all。 多线程写会破坏 next_slot_hint 语义。



只读 Reader —— 策略进程不得写 slot / 索引 / **header 里的任何计数器**。 mmap 用 PROT_READ 时写会段错。
  ★ 这条包括"看起来只是观测"的计数: 在 lookup 热路径里 `header()->xxx.fetch_add(1)` 一样会崩
    (实测 §17 并发用例 20 次挂 2 次, EXC_BAD_ACCESS code=2)。 读侧事件一律用**进程内**计数。



RCommand 是 POD —— union / trivially copyable, 不能含 std::string / 智能指针 / vptr。 目前 pubsub 已符合。



同机 SHM —— 不能跨主机 (endianness / alignment 假设)。



kSlotSize 与 pubsub::RCommand 大小绑定 —— 后者变化时 static_assert 会拦, 需 bump kSlotSize + shm 重建。



十、后续可选优化





CRC 校验 —— slot 加 4B CRC, 读时校验腐蚀。 (~50ns 开销, 一般 HFT 不做)



Prometheus exporter —— 暴露 stats 到 /metrics



SQLite 冷层 —— 定期 dump FINISHED 到 SQLite, 长期历史



Multi-writer 模式 —— slot-level CAS + hash CAS (复杂度大幅上升, 通常不推荐)



NUMA-aware slot 分片 —— 多路 CPU 时按 slot_idx 分区就近分配



十一、性能参考 (x86_64 3GHz)

跑 ./oms_shm.sh bench 典型数值:

INSERT   throughput ≈ 2-3M ops/sec    p50 ≈ 300ns   p99 ≈ 800ns
UPDATE   throughput ≈ 3-4M ops/sec    p50 ≈ 250ns   p99 ≈ 600ns
LOOKUP   throughput ≈ 5-8M ops/sec    p50 ≈ 150ns   p99 ≈ 400ns
MIXED    reader in parallel: 3-5M lookups/sec/thread

远超实际交易频率 (即使 HFT 也仅千级 orders/sec), 完全够用。 瓶颈永远是网络 / 交易所 RTT。



十二、接入 tb/OrderManager (已落地)

tb 的 `om::OrderManager` 原先用三张 `tbb::concurrent_unordered_map` 缓存在途订单:

    clientOrderId2OrderSysIdMap   // fmt::format("{}{}", strategyId, cid) → orderSysId
    orderId2OrderSysIdMap         // 交易所 orderId → orderSysId
    orderSysId2OrderResponseMap   // orderSysId → pubsub::RCommand

这三张表现在合并成**一个 OmsShm 段** (`oms::shm::OmsShmWriter`), 报单体存 slot,
三张 hash 索引指向 slot。

12.1 API 映射

    老写法                                          新写法
    ----------------------------------------------  ------------------------------------------------
    map[sysid] = rcmd;                              omsShm_.upsert(rcmd);      // 一次建三样
    map.find(sysid) → iter->second                  查: lookup_by_orderSysId_ex(sysid, out)
    原地改 iter->second, 靠引用生效                  改副本 out, 然后 upsert(out) 写回 (必须!)
    clientOrderId2OrderSysIdMap.find(...)            lookup_by_client_ex(strategyId, cid, out)
    orderId2OrderSysIdMap.find(orderId)              lookup_by_orderId_ex(orderId, out)
    getOrderSysId: 先查表1拿 sysId, 再查表2         getOrderSysId: **一次** lookup 直接吐报单体
                    (两张表, 两次查询)                (三张索引指向同一个 slot, 见 §12.8)

复合 client key 的拼法两版完全一致 —— `compose_client_key()` 当初就是照
`fmt::format("{}{}", strategyId, clientOrderId)` 写的。

★ 最容易写错的一条: 老代码拿的是 map 里 value 的**引用**, 改动是原地生效的 —— 包括那些
  `return false` 的分支 (tradeDiff/fillPrice 归零、orderId 补写、errorId 落库)。
  换成 OmsShm 之后 `op` 是 lookup 出来的**副本**, 所以**每一个 return 之前都要 upsert 写回**,
  否则行为就与老代码不一致 (静默丢更新)。

12.2 单写者前提

`OmsShmWriter` 的契约是**单线程串行调用**。tb 侧满足这个前提:
`TbOperation::run()` 起两个线程, 但只有 `execute()` 这一个线程会碰 OrderManager
(`processTcmd` / `processRcmd` 都在它的 while 循环里串行调用); 另一个 `executeTcmd()`
只负责把 tcmd 派发给 trade client。

⚠ 如果将来把 tcmd 和 rcmd 拆到两个线程, **必须**在 OrderManager 里加锁, 否则会破坏
  `next_slot_hint` 和 slot 状态机。

12.3 配置与容量规划

`om::OmsShmConfig` (定义在 `tb/include/oms/OrderManager.h`):

    path               = "/dev/shm/tb_oms.dat"
    slot_capacity      = 100000
    min_reclaim_age_ns = 60s     // FINISHED 多久后可回收
    max_live_stale_ns  = 24h     // LIVE 僵尸兜底

**可持续写入速率上限 = slot_capacity / min_reclaim_age** (见 §七)。默认值只有
**约 1666 单/秒**; 超过就必然写满。tb 启动时会把这行打进日志:

    oms shm ready: path=... slot_cap=... sustainable_insert_rate=1666/s

12.4 与老实现的行为差异 (三条, 都是有意的)

(1) **环写满时新单被拒**。老代码的 map 无上限, `processTcmd(CMD_NEW_ORDER)` 永远返回 true;
    现在 `upsert` 失败会返回 **false**, `TbOperation` 因此**不会**把这张单发给交易所。
    取舍: 宁可不下单, 也不发一张 OMS 不认识的单 (那种单之后撤不掉、查不到)。
    日志已限流 (前 8 次 + 之后每 4096 次), 不会刷屏。

(2) **已回收的单查不到**。`min_reclaim_age` 到期后 FINISHED 的 slot 会被复用, 之后拿
    旧 clientOrderId/orderId 来撤单/查询会得到 `NOT_FOUND` → `OS_REJECTED` +
    `OMSOrderNotFoundError`。老实现永远查得到。**这个 TTL 必须按"上层最晚可能来撤单的时长"定。**

(3) **重启后订单还在** (新能力, 不是差异而是收益)。老实现是进程内 map, tb 重启即全丢,
    重启后撤单会报 `OMSOrderNotFoundError`; 现在重新 open 同一路径即可恢复
    (open 时还会 `recover_orphan_slots()` 清理上次崩溃留下的 RECLAIMING 槽)。

12.5 已知待办

`OS_FAILED` (撤单失败) 那条分支里, 老代码只把 `OS_FAILED` 写进**发出去的回复**, 不写回
存储 —— 所以 slot 里的状态仍是 `OS_PENDING_NEW` (= LIVE)。老实现无所谓, 但在 OmsShm 里
这意味着这个 slot 要等 `max_live_stale_ns` 才能回收。
**建议把 `max_live_stale_ns` 从 24h 调到 10~30 分钟** (卡单/僵尸单本来就该很快判死)。
若要更彻底, 可以在那条分支里把 `OS_FAILED` 一并写回 slot —— 但那会改变存储状态的可见性
(策略可见的行为不变), 属于独立决策, 未做。

12.6 验证方式

`OrderManager` 的新旧两版用**同一份差分驱动**回放同一脚本, 把返回值 + 每次推给策略的
RCommand 打成规范化轨迹 (orderSysId 按首次出现顺序映射成 S1/S2..., updateTime 不打印):

    场景 A (正常生命周期/撤单/查询/部分成交/拒绝/失败/超量/只靠 orderId 反查/C_SWAP 成交算术)
      老版 vs 新版: **93 行轨迹逐字节相同**
    场景 B (22 字符 strategyId): 两版**不同** —— 见 §12.7
    场景 C (capacity=64 灌 70 张 LIVE 单): 成功 64 / 被拒 6, 被拒的没发给交易所
    场景 D (min_reclaim_age=0): 灌满后已 FINISHED 的单被回收 → 撤单 NOT_FOUND
    场景 E (重新 open 同一路径): created_new=0, 重启前建的活单仍可撤

12.7 顺带修掉的老 bug: orderSysId 被截断

`getOrderSysId(cid, strategyId, out, orderId)` 里老代码写的是
`strncpy(orderSysId, ..., INSTID_SIZE)` —— `INSTID_SIZE` 是 **32**, 而 orderSysId 是
`char[64]`、值形如 `x-<strategyId><rdtsc>`。strategyId 超过约 13 个字符时就会**被截断**,
调用方随后拿这个截断值去查报单体必然查不到, 于是走进 "oms not found order response" 分支 ——
撤单回复里成交量/orderId 全是 0, 缓存状态整个丢掉。

差分场景 B 实测 (strategyId = `utrade_btc_strategy_001`, orderSysId 长 41 字节):

    老版 B03 CANCEL:  sysid=S2(截断后的新串)  oid=(空)  volTot=0.0000   ← 缓存丢了
    新版 B03 CANCEL:  sysid=S1              oid=EX-B1 volTot=1.0000   ← 正确

已改成 `ORDER_SIZE` (64, 与调用方缓冲区一致)。

12.8 `getOrderSysId` 的两次查询合并成一次 (以及一个 `strncpy` 越界陷阱)

**为什么老代码要查两次**: 它是两张表 ——
`clientOrderId2OrderSysIdMap[key]` **只存 orderSysId 字符串**, 报单体在
`orderSysId2OrderResponseMap[sysId]` 里, 所以必须再查一次。

**OmsShm 下第二次是多余的**: 三张索引都指向**同一个 slot**, slot 里就是完整 `RCommand`。
`lookup_by_client_ex(strategyId, cid, out)` 已经把整张报单体通过 `out` 返回了。
`sizeof(pubsub::RCommand) = 536 B` —— 每次 lookup 都是 hash + probe + 536 字节 memcpy,
一次撤单/查询原本要做两遍。

`getOrderSysId` 因此改成**三态 + 直接吐出报单体**, 两个调用点 (CANCEL / QUERY) 合并成
一次查询 + 一套三态处理。`BUSY` 不再被压成 `false` —— 老写法会把一张**活单**回成
`OS_REJECTED` + `OMSOrderNotFoundError` (就是 §12.4 的 B3, 只是发生在撤单入口)。

⚠ **`ORIGINMSG_SIZE` 是个坑, 别用**。它是 **256**, 而 `OrderResponse::originMsg` 是
`char[128]`。`strncpy(dst, src, n)` 会**按 n 补 NUL** —— 也就是**无条件写满 256 字节**,
与 `src` 多长无关:

    originMsg        offset 384, 128 字节 → 到 512 结束
    updateTime       offset 512          ← 被覆盖
    apiSourceEnum    offset 520          ← 被覆盖
    sizeof(OrderResponse) = 528, 而写入区间是 [384, 640)

结果是把**同一个分支里刚设好的** `updateTime` / `apiSourceEnum` 清零, 并溢出结构体 112 字节
(实测 `updateTime` 1234567890123456 → 0)。正确写法是让边界跟着字段走:

    std::snprintf(dst, sizeof(dst), "%s", src);      // 永远截断 + 永远补 NUL

**同一写法在 Gateio 还有 4 处** (`GateioSpotTrade.cpp:737/747/887`,
`GateioUSTrade.cpp:1076`), 都是把交易所错误原文写进 `char[128]`, 尚未修。

十三、文件清单

new_dev/
├── include/oms/
│   ├── OmsShm.h                # 核心 header (writer + reader + 布局)
│   └── README.md               # 本文档
├── tb/include/oms/
│   └── OrderManager.h          # 接入方: OmsShmConfig + OmsShmWriter 成员
├── tb/src/oms/
│   └── OrderManager.cpp        # 接入方: 三张 tbb map 已换成一次 upsert
└── tb/tools/
    ├── oms_query.cpp           # CLI 查询
    ├── oms_bench.cpp           # 性能测试
    ├── oms_demo.cpp            # ⚠ 目前与 oms_bench.cpp 逐字节相同 (待办 D1)
    ├── oms_test.cpp            # 功能/边界/并发断言集
    └── oms_shm.sh              # 一站式运维脚本

