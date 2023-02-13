#include "rtt/rtt.hpp"

#include <span>
#include <string_view>

using RttConfig = rtt::SingleModeUpOnlyEmptyNameConfig<rtt::BufferMode::trim, 16>;
using RttType   = rtt::ControlBlock<RttConfig>;

static inline constinit typename RttType::Storage_t storage{};
static inline constinit RttType                     rttControlBlock{storage};

int main() {
    static constexpr std::string_view msg{"more then bufferSize of the up buffer long\n"};
    std::span<std::byte const>        remaining = rttControlBlock.write<0>(msg);
    while(!remaining.empty()) {
        remaining = rttControlBlock.write<0>(remaining);
    }
}
