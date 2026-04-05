#pragma once
#include <boost/python/module.hpp>
#include <boost/python/enum.hpp>
#include <boost/python/def.hpp>
#include <boost/python.hpp>
#include <boost/python/class.hpp>
#include <boost/mpl/bool.hpp>
#include <boost/python/scope.hpp>
#include "pubsub/pubsub.h"
#include "data_struct.h"
#include "time_util.h"
#include "pubsub_protocol.h"
#include "shm_global.h"
#include "shm_queue.h"
#include "concurrent_queue.h"


#define ADD_PROPERTY(TYPE, ATTR) add_property(#ATTR, GET_CHAR_P(TYPE, ATTR), SET_CHAR_P(TYPE, ATTR))

#define GET_CHAR_P(TYPE, ATTR) +[](const TYPE& e){\
            size_t n(strlen(e.ATTR));\
            if (0==n||(e.ATTR[0]==' ')||\
                !std::isprint(e.ATTR[0])) return "";\
                return (const char*)&(e.ATTR);\
        }

#define SET_CHAR_P(TYPE, ATTR) +[](TYPE& e, char const* new_text){\
            memset(e.ATTR,'\0',sizeof(e.ATTR));  \
            size_t n(sizeof(e.ATTR));      \
            size_t ns(strlen(new_text)); \
            if( ns>=n|| n<1 || !(std::isalpha(new_text[0]) && std::isdigit(new_text[0])) || new_text[0]==' ')      \
                e.ATTR[0]='\0';     \
            else{                \
                strncpy(e.ATTR, new_text, ns);    \
                e.ATTR[ns] = '\0';                \
            }                    \
        }

using namespace std;
using namespace boost::python;