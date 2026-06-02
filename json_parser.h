#pragma once

#include <simdjson.h>

inline simdjson::ondemand::parser& get_thread_local_parser() {
    thread_local simdjson::ondemand::parser parser;
    return parser;
}