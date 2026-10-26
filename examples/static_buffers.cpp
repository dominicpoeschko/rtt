#include "rtt/rtt.hpp"

#include <string_view>

using RttConfig = rtt::SingleModeUpOnlyEmptyNameConfig<rtt::BufferMode::skip, 512>;
using RttType   = rtt::ControlBlock<RttConfig>;

namespace {
constinit typename RttType::Storage_t storage{};
constinit RttType                     rttControlBlock{storage};
}   // namespace

int main() {
    static constexpr std::string_view msg{"test\n"};
    rttControlBlock.write<0>(msg);
}
