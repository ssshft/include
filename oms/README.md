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
seg.lookup_by_clientOrderId(static_cast<int64_t>(9382), out);   // 匹配 pubsub 原生 long
seg.lookup_by_clientOrderId(std::string_view("9382"), out);     // 字符串版, 兼容 CLI
seg.lookup_by_orderId("ex-8001001", out);
seg.exists_by_orderSysId("t-strat1-9382");   // 便捷: 只查存不存在

// 遍历 (慢, O(N), 只用于对账 / snapshot)
seg.iterate_live([](const pubsub::RCommand& r){ /* ... */ });
seg.iterate_finished(...);

// 监控
auto stats = seg.stats();
if (stats.live_stale > 0) alert(...);

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
          --iters=1000000 --readers=4 --reset

测量: insert / update / lookup 吞吐 + p50 / p95 / p99 / p999 / max 延迟。 混合场景 1 writer + N readers。

oms_demo —— 使用示例

完整流程演示: create → insert × 5 → update → lookup × 3 → iterate → stats → crash recover。

oms_shm.sh —— 一站式运维脚本

./oms_shm.sh build               # 编译 3 个工具
./oms_shm.sh check               # 环境检查 (/dev/shm 大小 / tmpfs / 权限)
./oms_shm.sh demo                # 跑一次功能演示
./oms_shm.sh stats [shm]         # 打 stats
./oms_shm.sh live [shm]          # 列活单
./oms_shm.sh stale [shm]         # 列卡单
./oms_shm.sh bench               # 跑性能测试
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

生成: oms_query, oms_bench, oms_demo。

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
        std::strncpy(cur.body.orderResponse.originMsg,
                     rcmd.body.orderResponse.originMsg, ORIGINMSG_SIZE);
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

getOrderSysId (辅助函数) 改成:

bool OrderManager::getOrderSysId(int64_t cid, const char* strategyId,
                                  char* orderSysId_out, const char* orderId) {
    pubsub::RCommand out;
    if (shm_.lookup_by_client(strategyId, cid, out)) {
        std::strncpy(orderSysId_out, out.body.orderResponse.orderSysId, ORDER_SIZE);
        return true;
    }
    if (orderId && orderId[0]) {
        if (shm_.lookup_by_orderId(orderId, out)) {
            std::strncpy(orderSysId_out, out.body.orderResponse.orderSysId, ORDER_SIZE);
            return true;
        }
    }
    return false;
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



只读 Reader —— 策略进程不得写 slot / 索引。 mmap 用 PROT_READ 时写会段错。



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



十二、文件清单

new_dev/
├── include/oms/
│   ├── OmsShm.h                # 核心 header (writer + reader + 布局)
│   └── README.md               # 本文档
└── tb/tools/
    ├── oms_query.cpp           # CLI 查询
    ├── oms_bench.cpp           # 性能测试
    ├── oms_demo.cpp            # 使用示例
    └── oms_shm.sh              # 一站式运维脚本

