import sys
VERSION=''
with open('/inc/version.inc') as f:
    lines = f.readlines()
    VERSION = lines[0].split('=')[1].strip()
sys.path.append('/opt/version/'+VERSION)
sys.path.append('/opt/version/'+VERSION+'/python-libs')
import time
from data_struct import *
from pypubsub import *

def gen_tcmd():
    now = int(time.time() * 1e6)
    tcmd = TCommand()
    tcmd.header.exchangeTypeEnum = ExchangeType_BINANCE
    tcmd.header.instTypeEnum = InstType_SPOT
    tcmd.header.cmdTypeEnum = CMD_NEW_ORDER
    tcmd.header.insertTime = now
    # tcmd.header.accountId = "I am happy" + str(now)
    tcmd.header.setAccountId("I am happy" + str(now))
    newOrder = TNewOrder()
    newOrder.insertTime = now
    tcmd.setTNewOrder(newOrder)
    return tcmd

def gen_rcmd():
    now = int(time.time() * 1e6)
    rcmd = RCommand()
    rcmd.header.exchangeTypeEnum = ExchangeType_BINANCE
    rcmd.header.instTypeEnum = InstType_SPOT
    rcmd.header.cmdTypeEnum = CMD_RPT_NEW_ORDER
    rcmd.header.insertTime = now
    rcmd.header.setAccountId("I am happy" + str(now))
    newOrder = RNewOrder()
    newOrder.insertTime = now
    rcmd.setRNewOrder(newOrder)
    return rcmd

if __name__=='__main__':
    tcmdQueue = TCommandPubSuber(1000310)
    rcmdQueue = RCommandPubSuber("100036")
    while True:
        tcmd = gen_tcmd()
        tcmdQueue.push(tcmd)
        print(tcmd.getString())
        time.sleep(1)
        rcmd = gen_rcmd()
        rcmdQueue.push(rcmd)
        print(rcmd.getString())
        time.sleep(1)



# if __name__=='__main__':
#     queue = TestPubSuber("Helloworld")
#     while True:
#         tcmd = TestTCommand()
#         tcmd.exchangeTypeEnum = ExchangeType_BINANCE
#         tcmd.instTypeEnum = InstType_SPOT
#         tcmd.cmdTypeEnum  = CMD_NEW_ORDER
#         tcmd.insertTime   = int(time.time() * 1e6)
#         # tcmd.accountId = "I am happy" + str(tcmd.insertTime)
#         tcmd.setAccountId("I am happy" + str(tcmd.insertTime))
#
#         queue.push(tcmd)
#         print(tcmd.accountId, ":", tcmd.insertTime)
#         time.sleep(5)

