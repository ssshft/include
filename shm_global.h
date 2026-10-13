#pragma once

#include "data_struct.h"
#include "shmqueue/shm_queue.h"
#include "pubsub_protocol.h"
#include "shm_spmc_queue.h"
#include "concurrent_queue.h"
#include "ShmSpscQueue.h"
#include "ShmSpmcQueue.h"
#include "SpinBackoff.h"


#define TESTCLIENTORDERID 1008610010


// ============================================================================
// 一、既有队列（System V shm / 老 SPMC）。仍在 tb 与 utrade 中使用，暂不删除。
// ============================================================================

// 多写单读 tb utrade都可以使用
// 注意：这句说的是"两侧都可以用这个 typedef 声明变量"（复用同一个类型），
// **不是**"两侧在运行期都会写"。运行期确实是多写（N 个 utrade）单读（1 个 tb）。
typedef pubsub::ShmQueue<pubsub::TCommand> Utrade2TbTCommandSHM;

// 多写单读 内部queue
typedef pubsub::ConcurrentQueueWF<pubsub::RCommand, 1024*64> Tb2OmsRCommandInnerQueue;

// 单写多读 tb端使用
typedef pubsub::SPMCPublisher<pubsub::RCommand> Tb2TradeRCommandPubSHM;

// 单写多读 utrade端使用
typedef pubsub::SPMCSubscriber<pubsub::RCommand> Tb2TradeRCommandSubSHM;

// 单读单写，内部使用
typedef pubsub::SPSCQueue<pubsub::RCommand, 1024*64>  RcmdInnerQueue;

typedef pubsub::SPSCQueue<pubsub::TCommand, 1024*64>  TcmdInnerQueue;

// ── 策略 -> 交易系统：SPSC，有序、不丢 ───────────────────────────────────
//    策略侧：push()      交易系统侧：pop()
//    每个策略一条队列，交易系统侧轮询 N 条。两侧都用默认的 OpenOrCreate
typedef ShmSpscQueue<pubsub::TCommand, 1024*64> Utrade2TbTcmd;

// ── 交易系统 -> 策略：SPMC，逐条、每消费者独立游标、不跳过 ───────────────
//    交易系统侧（唯一写者）：push()
//    策略 / 独立监控侧（多消费者）：各开一个对象（默认起点 AfterLatest），pop()
//    读到 ReadStatus::Gap 时必须记录 result.lost 并去 OMS 对齐状态。
typedef ShmSpmcQueue<pubsub::RCommand, 1024*64> Tb2UtradeRcmd;

// ── 行情推送：SPMC，只取最新、允许跳过 ──────────────────────────────────
//    行情源侧（唯一写者）：push()
//    策略 / 订阅者侧（多消费者）：默认起点 AfterLatest + popLatest()
typedef ShmSpmcQueue<md::Depth1, 1024*128> Depth1Spmc;
typedef ShmSpmcQueue<md::Depth5, 1024*128> Depth5Spmc;
typedef ShmSpmcQueue<md::Depth10, 1024*128> Depth10Spmc;
typedef ShmSpmcQueue<md::Depth20, 1024*128> Depth20Spmc;
typedef ShmSpmcQueue<md::Trades, 1024*128> TradesSpmc;
typedef ShmSpmcQueue<md::Kline, 1024*128> KlineSpmc;
typedef ShmSpmcQueue<md::FundingRate, 1024*32> FundingRateSpmc;


// ── 载荷约束（与队列头文件里的 static_assert 同源）──────────────────────
// 队列对载荷只做整对象赋值和 memcpy，所以要求 trivially copyable；
// 多态类型虽然也可以是 trivially copyable，但把 vptr 复制到另一个进程毫无意义。
// 两个队列共用同一个谓词：shm_queue::validPayload<T>()（见 ShmQueueCommon.h）。
//
// 注意 md::CryptoMarketData：768 B、trivially copyable，但因为
// `Depth1 : public MDBase` 这种"基类与派生类各自带数据成员"的继承写法而
// **不是** standard-layout。这正是队列头文件里不能用 is_standard_layout
// 做断言的原因（详见 ShmQueueCommon.h 里 validPayload() 的说明）。
static_assert(std::is_trivially_copyable<pubsub::TCommand>::value, "");
static_assert(std::is_trivially_copyable<pubsub::RCommand>::value, "");
static_assert(std::is_trivially_copyable<md::CryptoMarketData>::value, "");
static_assert(!std::is_polymorphic<md::CryptoMarketData>::value, "");