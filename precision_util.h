#pragma once

#include <cstring>

#define ZERO_NUM 0.0000000001

namespace crypto {
    inline bool is_zeronum(double num){
        if(num > -ZERO_NUM && num < ZERO_NUM){
            return true;
        }
        return false;
    }

    inline double getFixedPrecision(double originNum, double precision) {
        if (precision <= 0) {
            return originNum;
        }

        double inv = 1.0 / precision;
        double scaled = originNum * inv + 1e-9;
        int64_t n = static_cast<int64_t>(std::floor(scaled));
        return n * precision;
    }

    static constexpr double POW10[] = {
        1.0,
        0.1,
        0.01,
        0.001,
        0.0001,
        0.00001,
        0.000001,
        0.0000001,
        0.00000001,
        0.000000001,
        0.0000000001,
    };
}
