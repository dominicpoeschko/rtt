#include "rtt/rtt.hpp"

#include <memory>
#include <string_view>
#include <vector>

using RttConfig = rtt::SingleModeUpOnlyEmptyNameConfig<rtt::BufferMode::skip, 512>;
using RttType   = rtt::ControlBlock<RttConfig>;

int main() {
    auto    storage{std::make_unique<RttType::Storage_t>()};
    RttType rttControlBlock{*storage};

    std::vector<std::byte> data;
    data.resize(100);
    rttControlBlock.write<0>(data);
}
