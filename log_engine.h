#pragma once
#include "fmtlog/fmtlog-inl.h"
#include "string_util.h"


#define LOG_DEBUG(format, ...) FMTLOG(fmtlog::DBG, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...) FMTLOG(fmtlog::INF, format, ##__VA_ARGS__)
#define LOG_WARN(format, ...) FMTLOG(fmtlog::WRN, format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) FMTLOG(fmtlog::ERR, format, ##__VA_ARGS__)


inline void log_maintain(const std::string& tag, const std::string& logPath, const std::string& logLevel) {
    try {
        static int lastDay = -1;
        static bool inited = false;

        time_t now;
        tm tmBuf;
        time(&now);
        localtime_r(&now, &tmBuf);

        if (!inited || tmBuf.tm_mday != lastDay) {
            lastDay = tmBuf.tm_mday;
            inited = true;

            int year = tmBuf.tm_year + 1900;
            int month = tmBuf.tm_mon + 1;
            int day = tmBuf.tm_mday;

            std::string logName = fmt::format("{}/{}_{:04d}{:02d}{:02d}.log", logPath, tag, year, month, day);
            std::string level = boost::algorithm::to_lower_copy(logLevel);
            auto levelEnum = fmtlog::DBG;

            if (level == "info") {
                levelEnum = fmtlog::INF;
            } 
            else if (level == "warning") {
                levelEnum = fmtlog::WRN;
            } 
            else if (level == "error") {
                levelEnum = fmtlog::ERR;
            }

            fmtlog::setHeaderPattern("[{HMSF}][{s}][{l}][{t}]:");
            fmtlog::setLogFile(logName.c_str());
            fmtlog::setLogLevel(levelEnum);
        }

        fmtlog::poll();
    }
    catch(std::exception& e) {

    }
}
