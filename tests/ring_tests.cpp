// rtt::detail::Buffer as a pipe, checked against a deque; its edges; block mode against a reader
// thread; offsets a host may have corrupted; and the control block's layout as the J-Link reads it.
// Built -m32: rtt only compiles for 4-byte pointers.
#include "rtt/rtt.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <random>
#include <span>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

// the tests poke at raw storage and at the control block's bytes on purpose
#if defined(__clang__)
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

namespace {

int failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if(!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++failures;                                         \
        }                                                       \
    } while(0)

struct Name {
    constexpr operator std::string_view() const { return "test"; }
};

template<rtt::BufferMode Mode, std::size_t Size>
using BufferT = rtt::detail::Buffer<Mode, Size, Name>;

// Storage on the heap and exactly Size bytes long, so ASan sees a byte past its end.
template<rtt::BufferMode Mode, std::size_t Size>
struct Ring {
    std::unique_ptr<std::byte[]>    storage = std::make_unique<std::byte[]>(Size);
    rtt::detail::BufferControlBlock block   = BufferT<Mode, Size>::make(storage.get());
};

// the byte a running counter stands for
std::byte byteOf(std::size_t n) { return std::byte{static_cast<std::uint8_t>(n)}; }

template<std::size_t N>
std::array<std::byte,
           N>
bytes(std::size_t first) {
    std::array<std::byte, N> out{};
    for(auto& b : out) { b = byteOf(first++); }
    return out;
}

template<rtt::BufferMode Mode,
         std::size_t     Size>
void pipe(char const* what) {
    using Buffer                                    = BufferT<Mode, Size>;
    auto                                       ring = Ring<Mode, Size>{};
    std::deque<std::byte>                      model;
    std::mt19937                               random{Size * 7 + static_cast<unsigned>(Mode)};
    std::uniform_int_distribution<std::size_t> length{0, Size + 2};
    std::size_t                                next = 0;
    bool                                       ok   = true;

    for(int step = 0; step < 20000 && ok; ++step) {
        std::vector<std::byte> chunk(length(random));
        if(random() % 2 == 0) {
            for(auto& b : chunk) { b = byteOf(next++); }
            auto const free    = Size - 1 - model.size();
            auto const rest    = Buffer::write(ring.block, chunk);
            auto const written = chunk.size() - rest.size();
            if constexpr(Mode == rtt::BufferMode::skip) {
                // all or nothing
                ok = ok && (chunk.size() <= free ? written == chunk.size() : written == 0);
            } else {
                ok = ok && written == std::min(chunk.size(), free);
            }
            model.insert(model.end(), chunk.begin(), chunk.begin() + static_cast<long>(written));
            next -= rest.size();
        } else {
            auto const got = Buffer::read(ring.block, chunk);
            ok             = ok && got.size() == std::min(chunk.size(), model.size());
            for(std::size_t i = 0; ok && i < got.size(); ++i) {
                ok = got[i] == model.front();
                model.pop_front();
            }
        }
        ok = ok && ring.block.writePosition < Size && ring.block.readPosition < Size;
    }
    CHECK(ok, what);
}

// The edges the random pipe hits only by chance.
void edges() {
    {
        using Buffer = BufferT<rtt::BufferMode::skip, 16>;
        auto ring    = Ring<rtt::BufferMode::skip, 16>{};
        CHECK(Buffer::write(ring.block, bytes<16>(0)).size() == 16,
              "skip: Size bytes do not fit an empty buffer, nothing written");
        CHECK(ring.block.writePosition == 0, "skip: a refused write leaves WrOff alone");
        CHECK(Buffer::write(ring.block, bytes<15>(0)).empty(), "skip: Size-1 bytes fit");
        CHECK(ring.block.writePosition == 15, "skip: WrOff at Size-1");
        CHECK(Buffer::write(ring.block, bytes<1>(0)).size() == 1,
              "skip: a full buffer takes nothing");
        CHECK(Buffer::write(ring.block, std::span<std::byte const>{}).empty(),
              "skip: an empty write succeeds on a full buffer");
    }
    {
        using Buffer = BufferT<rtt::BufferMode::trim, 16>;
        auto ring    = Ring<rtt::BufferMode::trim, 16>{};
        CHECK(Buffer::write(ring.block, bytes<20>(0)).size() == 5,
              "trim: Size-1 of 20 bytes go in");
        CHECK(ring.block.writePosition == 15, "trim: WrOff at Size-1");
    }
    {
        // offsets at 13: a write of 5 wraps (3 to the end, 2 from the start), a read takes it back
        using Buffer             = BufferT<rtt::BufferMode::skip, 16>;
        auto ring                = Ring<rtt::BufferMode::skip, 16>{};
        ring.block.writePosition = 13;
        ring.block.readPosition  = 13;
        CHECK(Buffer::write(ring.block, bytes<5>(0xA0)).empty(), "wrap: 5 bytes written");
        CHECK(ring.block.writePosition == 2, "wrap: WrOff past the end");
        CHECK(ring.storage[13] == std::byte{0xA0} && ring.storage[15] == std::byte{0xA2}
                && ring.storage[0] == std::byte{0xA3} && ring.storage[1] == std::byte{0xA4},
              "wrap: the tail at the end, the rest at the start");
        std::array<std::byte, 8> in{};
        auto const               got = Buffer::read(ring.block, in);
        CHECK(got.size() == 5 && std::ranges::equal(got, bytes<5>(0xA0)),
              "wrap: read across the end");
        CHECK(ring.block.readPosition == 2, "wrap: RdOff past the end");
        CHECK(Buffer::read(ring.block, in).empty(), "wrap: then empty");
    }
    {
        // at exactly the end: the offset goes back to 0, never to Size (an odd size: no mask)
        using Buffer             = BufferT<rtt::BufferMode::trim, 13>;
        auto ring                = Ring<rtt::BufferMode::trim, 13>{};
        ring.block.writePosition = 10;
        ring.block.readPosition  = 10;
        CHECK(Buffer::write(ring.block, bytes<3>(0)).empty(), "end: 3 bytes to the end");
        CHECK(ring.block.writePosition == 0, "end: WrOff wraps to 0");
    }
    {
        // Size 1 holds nothing, Size 2 one byte
        auto one = Ring<rtt::BufferMode::trim, 1>{};
        CHECK((BufferT<rtt::BufferMode::trim, 1>::write(one.block, bytes<1>(0)).size() == 1),
              "Size 1: never any space");
        auto two = Ring<rtt::BufferMode::trim, 2>{};
        CHECK((BufferT<rtt::BufferMode::trim, 2>::write(two.block, bytes<3>(0)).size() == 2),
              "Size 2: one byte");
    }
}

// Block mode: the writer waits for the reader instead of dropping. The reader plays the J-Link:
// it reads the offsets and moves RdOff through atomic_ref, as the probe does over the bus.
template<std::size_t Size>
void blockAgainstReader(char const* what) {
    using Buffer                      = BufferT<rtt::BufferMode::block, Size>;
    auto                  ring        = Ring<rtt::BufferMode::block, Size>{};
    constexpr std::size_t Total       = 200000;
    std::atomic<bool>     ok          = true;
    std::atomic<bool>     writerShort = false;

    std::thread reader{[&] {
        std::atomic_ref<std::uint32_t> const wrOff{ring.block.writePosition};
        std::atomic_ref<std::uint32_t> const rdOff{ring.block.readPosition};
        std::size_t                          expected = 0;
        std::size_t                          got      = 0;
        while(got != Total) {
            std::uint32_t const wr = wrOff.load(std::memory_order_acquire);
            std::uint32_t       rd = rdOff.load(std::memory_order_relaxed);
            while(rd != wr) {
                if(ring.storage[rd] != byteOf(expected)) { ok = false; }
                ++expected;
                ++got;
                rd = static_cast<std::uint32_t>((rd + 1U) % Size);
            }
            rdOff.store(rd, std::memory_order_release);
        }
    }};

    std::mt19937                               random{Size};
    std::uniform_int_distribution<std::size_t> length{1, 3 * Size};
    std::size_t                                next = 0;
    for(std::size_t sent = 0; sent != Total;) {
        std::vector<std::byte> chunk(std::min(length(random), Total - sent));
        for(auto& b : chunk) { b = byteOf(next++); }
        if(!Buffer::write(ring.block, chunk).empty()) { writerShort = true; }
        sent += chunk.size();
    }
    reader.join();
    CHECK(ok && !writerShort, what);
}

// A host offset past the buffer (a corrupt RdOff/WrOff, a probe reading garbage) must never make
// the target write or read outside its storage, nor move its own offset out of the buffer.
template<rtt::BufferMode Mode,
         std::size_t     Size>
void corruptHostOffsets(char const* what) {
    using Buffer                                    = BufferT<Mode, Size>;
    auto                                       up   = Ring<Mode, Size>{};   // the target writes
    auto                                       down = Ring<Mode, Size>{};   // the target reads
    std::mt19937                               random{Size + 99};
    std::array<std::uint32_t, 6> const         bad{Size,
                                                   Size + 1,
                                                   2 * Size,
                                                   0x7FFFFFFF,
                                                   0xFFFFFFFE,
                                                   0xFFFFFFFF};
    std::uniform_int_distribution<std::size_t> length{0, 2 * Size};
    bool                                       ok = true;

    for(int step = 0; step < 4000 && ok; ++step) {
        std::vector<std::byte> chunk(length(random));
        std::uint32_t const    hostOffset = random() % 3 == 0
                                            ? static_cast<std::uint32_t>(random() % Size)
                                            : bad[random() % bad.size()];
        if(random() % 2 == 0) {
            up.block.readPosition = hostOffset;
            Buffer::write(up.block, chunk);
            ok = up.block.writePosition < Size;
        } else {
            down.block.writePosition = hostOffset;
            Buffer::read(down.block, chunk);
            ok = down.block.readPosition < Size;
        }
    }
    CHECK(ok, what);
}

// The control block as the J-Link reads it: 24-byte header, then 24 bytes per buffer, up first,
// in declaration order.
struct Up0 {
    constexpr operator std::string_view() const { return "up0"; }
};

struct Up1 {
    constexpr operator std::string_view() const { return "up1"; }
};

struct Up2 {
    constexpr operator std::string_view() const { return "up2"; }
};

struct Down0 {
    constexpr operator std::string_view() const { return "down0"; }
};

struct Down1 {
    constexpr operator std::string_view() const { return "down1"; }
};

struct LayoutConfig {
    using UpChannelConfigs
      = rtt::make_ChannelConfigs_t<rtt::ChannelConfig<16, rtt::BufferMode::skip, Up0>,
                                   rtt::ChannelConfig<32, rtt::BufferMode::trim, Up1>,
                                   rtt::ChannelConfig<48, rtt::BufferMode::block, Up2>>;
    using DownChannelConfigs
      = rtt::make_ChannelConfigs_t<rtt::ChannelConfig<64, rtt::BufferMode::skip, Down0>,
                                   rtt::ChannelConfig<80, rtt::BufferMode::skip, Down1>>;
    static constexpr auto ControlBlockId{rtt::DefaultControlBlockId};
};

std::uint32_t word(void const* base,
                   std::size_t offset) {
    std::uint32_t v{};
    std::memcpy(&v, static_cast<std::byte const*>(base) + offset, sizeof v);
    return v;
}

void controlBlockLayout() {
    using Block = rtt::ControlBlock<LayoutConfig>;
    Block::Storage_t storage{};
    Block            block{storage};

    CHECK(sizeof(Block) == 24 + 5 * 24, "control block size");
    CHECK(std::memcmp(&block, "SEGGER RTT", 11) == 0, "the id opens the control block");
    CHECK(word(&block, 16) == 3 && word(&block, 20) == 2, "3 up, 2 down buffers");

    struct Expected {
        std::string_view name;
        std::byte const* buffer;
        std::uint32_t    size;
        std::uint32_t    mode;
    };

    std::array<Expected, 5> const expected{
      {{"up0", std::get<0>(storage).data(), 16, 0},
       {"up1", std::get<1>(storage).data(), 32, 1},
       {"up2", std::get<2>(storage).data(), 48, 2},
       {"down0", std::get<3>(storage).data(), 64, 0},
       {"down1", std::get<4>(storage).data(), 80, 0}}
    };
    for(std::size_t i = 0; i != expected.size(); ++i) {
        std::size_t const at = 24 + 24 * i;
        // NOLINTNEXTLINE(performance-no-int-to-ptr): the name pointer as the J-Link sees it
        auto const* const name = reinterpret_cast<char const*>(std::uintptr_t{word(&block, at)});
        CHECK(std::string_view{name} == expected[i].name, "buffer i carries its own name");
        CHECK(word(&block, at + 4) == reinterpret_cast<std::uintptr_t>(expected[i].buffer),
              "buffer i points at its own storage");
        CHECK(word(&block, at + 8) == expected[i].size, "buffer i has its own size");
        CHECK(word(&block, at + 20) == expected[i].mode, "buffer i has its own mode");
    }

    // write<1> goes to up1: its storage and its WrOff, nothing else
    std::array<std::byte, 3> const out{std::byte{0xA1}, std::byte{0xA2}, std::byte{0xA3}};
    block.write<1>(out);
    CHECK(word(&block, 24 + 24 * 1 + 12) == 3, "write<1> moves up1's WrOff");
    CHECK(word(&block, 24 + 24 * 0 + 12) == 0 && word(&block, 24 + 24 * 2 + 12) == 0,
          "write<1> leaves up0 and up2 alone");
    CHECK(std::get<1>(storage)[0] == std::byte{0xA1} && std::get<1>(storage)[2] == std::byte{0xA3},
          "write<1> fills up1's storage");

    // read<1> comes from down1: what the host put there and announced in its WrOff
    std::get<4>(storage)[0]       = std::byte{0xD1};
    std::get<4>(storage)[1]       = std::byte{0xD2};
    std::uint32_t const hostWrOff = 2;
    std::memcpy(reinterpret_cast<std::byte*>(&block) + 24 + 24 * 4 + 12, &hostWrOff, 4);
    std::array<std::byte, 4> in{};
    auto const               got = block.read<1>(in);
    CHECK(got.size() == 2 && in[0] == std::byte{0xD1} && in[1] == std::byte{0xD2},
          "read<1> takes down1's bytes");
    CHECK(word(&block, 24 + 24 * 4 + 16) == 2, "read<1> moves down1's RdOff");
}

}   // namespace

int main() {
    controlBlockLayout();
    pipe<rtt::BufferMode::skip, 16>("skip, a power of two");
    pipe<rtt::BufferMode::skip, 13>("skip, any size");
    pipe<rtt::BufferMode::trim, 16>("trim, a power of two");
    pipe<rtt::BufferMode::trim, 13>("trim, any size");
    edges();
    blockAgainstReader<16>("block: every byte arrives in order, a power of two");
    blockAgainstReader<13>("block: every byte arrives in order, any size");
    corruptHostOffsets<rtt::BufferMode::skip, 16>("corrupt host offset, skip, a power of two");
    corruptHostOffsets<rtt::BufferMode::skip, 13>("corrupt host offset, skip, any size");
    corruptHostOffsets<rtt::BufferMode::trim, 16>("corrupt host offset, trim, a power of two");
    corruptHostOffsets<rtt::BufferMode::trim, 13>("corrupt host offset, trim, any size");
    if(failures == 0) { std::puts("all checks passed"); }
    return failures == 0 ? 0 : 1;
}
