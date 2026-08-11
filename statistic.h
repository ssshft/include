#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <ostream>
#include <vector>

template <typename T>
class Statistic
{
public:
    void reserve(size_t size)
    {
        vec.reserve(size);
    }

    size_t size() const
    {
        return vec.size();
    }

    void add(T v)
    {
        vec.push_back(v);
    }

    void clear()
    {
        vec.clear();
    }

    void print(std::ostream& os)
    {
        const size_t n = vec.size();

        os << "cnt: " << n << '\n';

        if (n == 0)
            return;

        const T first = vec.front();

        std::sort(vec.begin(), vec.end());

        // 使用 long double 避免整数溢出/精度问题
        long double sum = 0;
        for (const T v : vec)
            sum += static_cast<long double>(v);

        const long double mean = sum / static_cast<long double>(n);

        long double var = 0;
        for (const T v : vec)
        {
            const long double d =
                static_cast<long double>(v) - mean;

            var += d * d;
        }

        var /= static_cast<long double>(n);

        os << "min: " << vec.front() << '\n';
        os << "max: " << vec.back() << '\n';
        os << "first: " << first << '\n';
        os << "mean: " << mean << '\n';
        os << "sd: " << std::sqrt(var) << '\n';

        os << "1%: " << percentile(1.0) << '\n';
        os << "10%: " << percentile(10.0) << '\n';
        os << "50%: " << percentile(50.0) << '\n';
        os << "90%: " << percentile(90.0) << '\n';
        os << "95%: " << percentile(95.0) << '\n';
        os << "99%: " << percentile(99.0) << '\n';
        os << "99.99%: " << percentile(99.99) << '\n';
    }

private:
    T percentile(double p) const
    {
        if (vec.empty())
            return T{};

        // nearest-rank 风格，保证 index 不越界
        size_t index =
            static_cast<size_t>(
                std::ceil(p / 100.0 * static_cast<double>(vec.size()))
            );

        if (index == 0)
            index = 1;

        --index;

        if (index >= vec.size())
            index = vec.size() - 1;

        return vec[index];
    }

private:
    std::vector<T> vec;
};