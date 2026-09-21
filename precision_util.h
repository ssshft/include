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

    static constexpr int64_t POW10_INT[] = {
        1LL, 
        10LL, 
        100LL, 
        1000LL, 
        10000LL, 
        100000LL, 
        1000000LL, 
        10000000LL,
        100000000LL, 
        1000000000LL, 
        10000000000LL, 
        100000000000LL,
        1000000000000LL, 
        10000000000000LL, 
        100000000000000LL,
        1000000000000000LL, 
        10000000000000000LL, 
        100000000000000000LL,
        1000000000000000000LL
    };

    // 从十进制字符串数出有效小数位数（去掉末尾 0）
    // "0.05"     → 2
    // "0.010000" → 2
    // "0.25000"  → 2
    // "1.0000"   → 0
    // "100"      → 0
    inline int countDecimalDigits(std::string_view s) {
        auto dot = s.find('.');
        auto epos = s.find_first_of("eE");
        if (dot == std::string_view::npos) {
            return 0;                      // 无小数点，整数
        }

        size_t end = (epos == std::string_view::npos) ? s.size() : epos;
        int digits = static_cast<int>(end - dot - 1);
        while (digits > 0 && s[dot + digits] == '0') {
            --digits;   // 去尾零
        }

        return digits;
    }

    // 把 rawValue 量化到 step 的整数倍（向零取整），step = stepInt / pow10
    inline double quantize(double rawValue, int64_t pow10, int64_t stepInt) noexcept {
        if (stepInt <= 0 || pow10 <= 0) {
            return rawValue;    // 保底
        }

        int64_t scaled = static_cast<int64_t>(std::llround(rawValue * static_cast<double>(pow10)));
        int64_t ticks  = scaled / stepInt;
        if (scaled < 0 && scaled % stepInt != 0) {
            --ticks;   // 负数向负无穷取整
        }

        return static_cast<double>(ticks * stepInt) / static_cast<double>(pow10);
    }
}
