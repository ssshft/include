//
// Created by qyang on 2022/1/17.
//

#ifndef DB_SUBER_H
#define DB_SUBER_H
#include "shm_topic.h"

#define BUFFSIZE 1024
namespace pubsub {
    template<class Topic>
    class Subscriber {

    public:
        Subscriber(const char *topic) {
            assert(sizeof(Topic) <= BUFFSIZE);
//            std::cout << "Topic size: " <<sizeof(Topic) << std::endl;
            std::string path = "";// /dev/shm/
            path += topic;
            q = shmmap<MsgQ>(path.c_str());
            if (!q) {
                fprintf(stderr, "connect with %s error", topic);
                exit(1);
            }
            lastIdx = q->sub(true);
        }
        ~Subscriber(){
//            delete q;
//            delete buf;
            fprintf(stdout, "%s desctruct\n", __FUNCTION__ );
        }

        inline Topic *get_data() {
            auto res = q->read(lastIdx, buf, sizeof(buf));
            if (res == MsgQ::ReadNeedReSub) {
                lastIdx = q->sub(true);
                res = q->read(lastIdx, buf, sizeof(buf));
            }
            if (res == MsgQ::ReadOK) {
                MsgQ::MsgHeader *header = (MsgQ::MsgHeader *) buf;
                Topic *topic = (Topic *) (header + 1);
//            memcpy(&topic, msg, sizeof(Topic));
                return topic;
            }
            return nullptr;
        }

        inline bool get_data(Topic &topic) {//Topic &topic Topic *get_data(Topic &topic){
            auto res = q->read(lastIdx, buf, sizeof(buf));
            if (res == MsgQ::ReadNeedReSub) {
                lastIdx = q->sub(true);
                res = q->read(lastIdx, buf, sizeof(buf));
            }
            if (res == MsgQ::ReadOK) {
                MsgQ::MsgHeader *header = (MsgQ::MsgHeader *) buf;
                Topic *msg = (Topic *) (header + 1);
                memcpy(&topic, msg, sizeof(Topic));
                return true;
            }
            return false;
//        auto res = q->read(lastIdx, (void *)&topic, sizeof(Topic));
//        if(res == MsgQ::ReadNeedReSub) {
////            cout << "topic: " << argv[i] << " need resub" << endl;
//            lastIdx = q->sub(true);
//            res = q->read(lastIdx, buf, sizeof(buf));
//        }
//        if(res == MsgQ::ReadOK) {
////            auto now = rdtsc();
//            MsgQ::MsgHeader *header = (MsgQ::MsgHeader *) buf;
//            Topic *msg = (Topic*)(header + 1);
////            memcpy(&topic, msg, sizeof(Topic));
//            return msg;
//        }
//        return nullptr;
        }
    private:
        MsgQ *q;
        uint64_t lastIdx;

        char buf[BUFFSIZE];
    };
}
#endif //DB_SUBER_H
