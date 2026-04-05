//
// Created by qyang on 2022/1/17.
//

#ifndef DB_PUBER_H
#define DB_PUBER_H
#include "log_engine.h"
#include "shm_topic.h"


namespace pubsub {
    template<class Topic>
    class Publisher {

    public:
        Publisher(const char *topic){
            std::string path = "";
            path += topic;
            q = shmmap<MsgQ>(path.c_str());
            if (!q) {
                fprintf(stderr, "connect with %s error", topic);
                exit(1);
            }
            // try{
            //     string chmodCmd = " chmod 777 /dev/shm/" + path;
            //     system(chmodCmd.c_str());
            // }
            // catch (const std::exception &e){
            //     fprintf(stderr, "chmod %s error", e.waht());
            // }
        }
        ~Publisher(){
//            delete q;
            // fprintf(stdout, "%s desctruct\n", __FUNCTION__ );
        }

        void publish(const Topic &topic) {
            MsgQ::MsgHeader *header = q->alloc(sizeof(Topic));
            assert(header != nullptr);
//        header->userdata = T::msg_type;
            Topic *msg = (Topic *) (header + 1);
            memcpy(msg, &topic, sizeof(Topic));
//        for(auto& v : msg->val) v = val++;
//        msg->tid = tid;
//        msg->ts = rdtsc();
            q->pub(true);
        }
    private:
        MsgQ *q;

    };
}
#endif //DB_PUBER_H
