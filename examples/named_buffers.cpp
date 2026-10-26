#include "rtt/rtt.hpp"

#include <string_view>

struct UpName {
    consteval operator std::string_view() const { return "UpBuffer"; }
};

struct RttConfig {
    using UpChannelConfigs
      = rtt::make_ChannelConfigs_t<rtt::ChannelConfig<64, rtt::BufferMode::skip, UpName>,
                                   rtt::ChannelConfig<512, rtt::BufferMode::block, rtt::EmptyName>>;

    using DownChannelConfigs = rtt::EmptyChannelConfig;

    static constexpr auto controlBlockId{rtt::defaultControlBlockId};
};

using RttType = rtt::ControlBlock<RttConfig>;

namespace {
constinit typename RttType::Storage_t storage{};

constinit RttType rttControlBlock{storage};
}   // namespace

int main() {
    static constexpr std::string_view msg{"test\n"};
    rttControlBlock.write<0>(msg);
    rttControlBlock.write<1>(msg);
}
