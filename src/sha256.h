#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sha256 {

// SHA-256 (FIPS 180-4). Возвращает hex-строку в нижнем регистре.
std::string hex(const std::vector<uint8_t>& data);

}  // namespace sha256
