#pragma once

#include "data_struct.h"
#include "shmqueue/shm_queue.h"
#include "pubsub_protocol.h"
#include "shm_spmc_queue.h"
#include "concurrent_queue.h"


#define TESTCLIENTORDERID 1008610010


// 多写单读 tb utrade都可以使用
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