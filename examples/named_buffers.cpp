#include "rtt/rtt.hpp"

#include <string_view>

struct UpName {
    consteval operator std::string_view() const { return "UpBuffer"; }
};

struct RttConfig {
    using UpChannelConfigs = rtt::make_ChannelConfigs_t<
      rtt::ChannelConfig<64, rtt::BufferMode::skip, UpName>,
      rtt::ChannelConfig<512, rtt::BufferMode::block, rtt::EmptyName>>;

    using DownChannelConfigs = rtt::EmptyChannelConfig;

    static constexpr auto ControlBlockId{rtt::DefaultControlBlockId};
};

using RttType = rtt::ControlBlock<RttConfig>;

static inline constinit typename RttType::Storage_t storage{};

static inline constinit RttType rttControlBlock{storage};

int main() {
    static constexpr std::string_view msg{"test\n"};
    rttControlBlock.write<0>(msg);
    rttControlBlock.write<1>(msg);
}
