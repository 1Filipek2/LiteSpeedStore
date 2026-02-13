#pragma once
#include <string>
#include <optional>

class IStorageEngine {
public:
    virtual ~IStorageEngine() = default;

    virtual void put(const std::string&, const std::string&) = 0;
    virtual void erase(const std::string&) = 0;
    virtual std::optional<std::string> get(const std::string&) = 0;

    virtual void recover() = 0;
    virtual void snapshot() = 0;
};
