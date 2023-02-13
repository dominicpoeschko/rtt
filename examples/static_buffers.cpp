#include "rtt/rtt.hpp"

#include <string_view>

using RttConfig = rtt::SingleModeUpOnlyEmptyNameConfig<rtt::BufferMode::skip, 512>;
using RttType   = rtt::ControlBlock<RttConfig>;

static inline constinit typename RttType::Storage_t storage{};
static inline constinit RttType                     rttControlBlock{storage};

int main() {
    static constexpr std::string_view msg{"test\n"};
    rttControlBlock.write<0>(msg);
}
