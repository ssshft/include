import sys
VERSION=''
with open('/inc/version.inc') as f:
    lines = f.readlines()
    VERSION = lines[0].split('=')[1].strip()
sys.path.append('/opt/version/'+VERSION)
sys.path.append('/opt/version/'+VERSION+'/python-libs')
import time
from data_struct import *
from test_pubsub import *
from pypubsub import *

if __name__=='__main__':
    tcmd = TCommand()
    rcmd = RCommand()
    tcmdQueue = TCommandPubSuber(1000310)
    rcmdQueue = RCommandPubSuber("100036")
    while True:
        now = int(time.time()*1e6)
        if tcmdQueue.pop(tcmd):
            print("delay:",now - tcmd.header.insertTime)
            print(tcmd.getString())
        now = int(time.time()*1e6)

        if rcmdQueue.pop(rcmd):
            print("delay:",now - rcmd.header.insertTime)
            print(rcmd.getString())