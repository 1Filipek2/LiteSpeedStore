#pragma once
#include <chrono>
#include <string>
#include <iostream>
#include <functional>

class Timer {
public:
    using Clock = std::chrono::high_resolution_clock;

    Timer(std::string name, std::function<void(double)> callback)
        : m_name(std::move(name)), m_callback(std::move(callback)) {
        m_start = Clock::now();
    }

    ~Timer() {
        auto end = Clock::now();
        std::chrono::duration<double, std::milli> elapsed = end - m_start;
        m_callback(elapsed.count()); // res to -> engine
    }

private:
    std::string m_name;
    Clock::time_point m_start;
    std::function<void(double)> m_callback;
};

#define TRACE_SCOPE(name, engine) Timer timer##__LINE__(name, [&](double ms) { \
    engine.set(name, std::to_string(ms) + " ms"); \
})