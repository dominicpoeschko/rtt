#include "rtt/rtt.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

struct RttConfig {
    using UpChannelConfigs = rtt::make_ChannelConfigs_t<rtt::ChannelConfig<64>>;

    using DownChannelConfigs
      = rtt::make_ChannelConfigs_t<rtt::ChannelConfig<64, rtt::BufferMode::trim>>;

    static constexpr auto controlBlockId{rtt::defaultControlBlockId};
};

using RttType = rtt::ControlBlock<RttConfig>;

namespace {
constinit typename RttType::Storage_t storage{};

constinit RttType rttControlBlock{storage};
}   // namespace

int main() {
    while(true) {
        std::array<std::byte, 16> buffer;
        std::span<std::byte>      readBytes = rttControlBlock.read<0>(buffer);
        if(!readBytes.empty()) { rttControlBlock.write<0>(readBytes); }
    }
}
