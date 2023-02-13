#include "rtt/rtt.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

struct RttConfig {
    using UpChannelConfigs = rtt::make_ChannelConfigs_t<rtt::ChannelConfig<64>>;

    using DownChannelConfigs
      = rtt::make_ChannelConfigs_t<rtt::ChannelConfig<64>, rtt::BufferMode::trim>;

    static constexpr auto ControlBlockId{rtt::DefaultControlBlockId};
};

using RttType = rtt::ControlBlock<RttConfig>;

static inline constinit typename RttType::Storage_t storage{};

static inline constinit RttType rttControlBlock{storage};

int main() {
    while(true) {
        std::array<std::byte, 16> buffer;
        std::span<std::byte>      readBytes = rttControlBlock.read<0>(buffer);
        if(!readBytes.empty()) {
            rttControlBlock.write<0>(readBytes);
        }
    }
}
