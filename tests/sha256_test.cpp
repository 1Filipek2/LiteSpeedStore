#include <catch2/catch_test_macros.hpp>

#include "persistence/SHA256.hpp"

#include <cstdint>
#include <string>

namespace {
std::string toHex(const persistence::Sha256Digest& d) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(d.size() * 2);
    for (uint8_t b : d) {
        s.push_back(hex[b >> 4]);
        s.push_back(hex[b & 0x0F]);
    }
    return s;
}
} // namespace

TEST_CASE("SHA256 matches known FIPS 180-4 test vectors", "[sha256]") {
    const std::string empty;
    REQUIRE(toHex(persistence::SHA256::hash(
                reinterpret_cast<const uint8_t*>(empty.data()), empty.size()))
            == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    const std::string abc = "abc";
    REQUIRE(toHex(persistence::SHA256::hash(
                reinterpret_cast<const uint8_t*>(abc.data()), abc.size()))
            == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
