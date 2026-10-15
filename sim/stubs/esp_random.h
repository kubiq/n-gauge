// Stand-in: the hardware RNG is libc's on the PC.
#pragma once
#include <cstdint>
#include <cstdlib>
inline uint32_t esp_random() { return ((uint32_t)std::rand() << 16) ^ (uint32_t)std::rand(); }
