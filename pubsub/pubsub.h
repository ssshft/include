#pragma once
#include "pubsub/suber.h"
#include "pubsub/puber.h"
#include <iostream>
#include <string>

namespace pubsub{
    template <class Topic>
    class PubSuber{
        public:
            PubSuber(const char* topicStr){//const char* topicStr
                puber = new Publisher<Topic>(topicStr);
                suber = new Subscriber<Topic>(topicStr);
                init();
            }

            ~PubSuber(){
                // if(puber != nullptr){
                //     delete puber;
                // }
                // if(suber != nullptr){
                //     delete suber;
                // }
                // fprintf(stdout, "%s desctruct executed\n", __FUNCTION__ );
            }

            //pop the last one
            void init(){
                Topic t;
                this->pop(t);
            }

            bool get_data(Topic &topic){
                 if(suber->get_data(topic)){
                    return true;
                 }
                 return false;
            }

            inline bool pop(Topic &topic){
                if(suber->get_data(topic)){
                    return true;
                }
                return false;
            }

            inline void push(const Topic &topic){
                puber->publish(topic);
            }

            inline void publish(const Topic &topic){
                puber->publish(topic);
            }
    protected:
        Publisher<Topic> *puber = nullptr;
        Subscriber<Topic> *suber = nullptr;
    };
}
