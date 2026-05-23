#pragma once
#include <chrono>
#include <string>
#include <functional>
#include <utility>

class Timer {
public:
    using Clock = std::chrono::high_resolution_clock;

    Timer(std::string /*name*/, std::function<void(double)> callback)
        : m_callback(std::move(callback)) {
        m_start = Clock::now();
    }

    ~Timer() {
        auto end = Clock::now();
        std::chrono::duration<double, std::milli> elapsed = end - m_start;
        m_callback(elapsed.count());
    }

private:
    Clock::time_point m_start;
    std::function<void(double)> m_callback;
};

#define TRACE_SCOPE(name, engine) \
    Timer timer##__LINE__(name, [&](double ms) { \
        engine.set(name, std::to_string(ms) + " ms", ms); \
    })
