// The offset hand-off between the target and the debug probe, with a thread playing the probe: up buffers in block
// mode and staged skip mode, the target polling unread() until the probe has drained, and a down buffer the probe
// fills. Built for the host's own pointer size (not -m32), so it runs under ThreadSanitizer: the probe thread takes
// the target's write offset with an acquire load and gives its read offset back with a release store, as rtt's host
// build does on its side - a missing hand-off is a data race report on the ring's bytes. The control block's
// layout is not used here (rtt::ControlBlock needs 4-byte pointers; ring_tests checks it -m32).
#include "rtt/rtt.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <span>
#include <thread>
#include <vector>

#ifdef __clang__
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

template<rtt::BufferMode Mode, std::size_t Size>
using BufferT = rtt::Detail::Buffer<Mode, Size, rtt::EmptyName>;

// a buffer control block as the target keeps it, without the 4-byte-pointer layout of a real control block
template<rtt::BufferMode Mode, std::size_t Size>
struct Ring {
    std::unique_ptr<std::byte[]>    storage = std::make_unique<std::byte[]>(Size);
    rtt::Detail::BufferControlBlock block{.name       = "probe",
                                          .buffer     = storage.get(),
                                          .bufferSize = Size,
                                          .mode       = Mode};
};

std::byte byteOf(std::size_t n) { return std::byte{static_cast<std::uint8_t>(n * 7U + 3U)}; }

// The probe's side of an up buffer: what it reads over the debug port, written independently of rtt. Takes every
// byte the target published, calls onByte(byte) for each, and stops after `total` bytes.
template<std::size_t Size,
         typename OnByte>
void probeDrains(rtt::Detail::BufferControlBlock& block,
                 std::size_t const                total,
                 OnByte&&                         onByte) {
    std::atomic_ref<std::uint32_t> const wrOff{block.writePosition};
    std::atomic_ref<std::uint32_t> const rdOff{block.readPosition};
    std::uint32_t                        rd  = rdOff.load(std::memory_order_relaxed);
    std::size_t                          got = 0;
    while(got != total) {
        std::uint32_t const wr = wrOff.load(std::memory_order_acquire);
        if(wr == rd) {
            std::this_thread::yield();
            continue;
        }
        while(rd != wr && got != total) {
            onByte(block.buffer[rd]);
            ++got;
            rd = static_cast<std::uint32_t>((rd + 1U) % Size);
        }
        rdOff.store(rd, std::memory_order_release);
    }
}

// Block mode: every byte arrives, in order, while the target waits for room in its write loop.
template<std::size_t Size>
void upBlock(char const* what) {
    using Buffer                = BufferT<rtt::BufferMode::block, Size>;
    auto                  ring  = Ring<rtt::BufferMode::block, Size>{};
    constexpr std::size_t total = 100000;
    std::size_t           bad   = 0;

    std::thread probe{[&] {
        std::size_t expected = 0;
        probeDrains<Size>(ring.block, total, [&](std::byte b) {
            if(b != byteOf(expected++)) { ++bad; }
        });
    }};

    std::mt19937                               random{Size};
    std::uniform_int_distribution<std::size_t> length{1, 2 * Size};
    std::size_t                                next   = 0;
    bool                                       short_ = false;
    for(std::size_t sent = 0; sent != total;) {
        std::vector<std::byte> chunk(std::min(length(random), total - sent));
        for(auto& b : chunk) { b = byteOf(next++); }
        if(!Buffer::write(ring.block, chunk).empty()) { short_ = true; }
        sent += chunk.size();
    }
    probe.join();
    CHECK(bad == 0 && !short_, what);
}

// Staged skip-mode records, published whole: the probe sees each record complete and in order. The stage waits
// for room (keepWaiting says yes), the loop that polls the probe's offset.
template<std::size_t Size>
void upStaged(char const* what) {
    using Buffer                  = BufferT<rtt::BufferMode::skip, Size>;
    auto                  ring    = Ring<rtt::BufferMode::skip, Size>{};
    constexpr std::size_t records = 5000;
    constexpr std::size_t length  = 5;   // a length byte and four counter bytes
    std::size_t           bad     = 0;

    std::thread probe{[&] {
        std::size_t index = 0;
        std::size_t at    = 0;
        probeDrains<Size>(ring.block, records * length, [&](std::byte b) {
            auto const want = at == 0 ? std::byte{length} : byteOf(index * 4U + at);
            if(b != want) { ++bad; }
            if(++at == length) {
                at = 0;
                ++index;
            }
        });
    }};

    std::uint32_t waits = 0;
    for(std::size_t i = 0; i != records; ++i) {
        std::array<std::byte, length> record{std::byte{length}};
        for(std::size_t k = 1; k != length; ++k) { record[k] = byteOf(i * 4U + k); }
        std::uint32_t staged = ring.block.writePosition;
        bool const    ok     = Buffer::stage(ring.block, record, staged, [&](std::uint32_t) {
            ++waits;
            std::this_thread::yield();
            return true;
        });
        CHECK(ok, "a waiting stage always fits in the end");
        Buffer::publish(ring.block, staged);
    }
    probe.join();
    CHECK(bad == 0, what);
    std::printf("%s: %u waits for room\n", what, static_cast<unsigned>(waits));
}

// The target polls unread() until the probe has taken everything: a loop that loads the probe's offset each time.
template<std::size_t Size>
void drainPoll(char const* what) {
    using Buffer                = BufferT<rtt::BufferMode::skip, Size>;
    auto                  ring  = Ring<rtt::BufferMode::skip, Size>{};
    constexpr std::size_t total = Size - 1U;   // one full ring

    std::vector<std::byte> data(total);
    for(std::size_t i = 0; i != total; ++i) { data[i] = byteOf(i); }
    CHECK(Buffer::write(ring.block, data).empty(), "a full ring's worth fits");
    CHECK(Buffer::unread(ring.block) == total, "all of it unread before the probe runs");

    std::size_t bad = 0;
    std::thread probe{[&] {
        std::size_t expected = 0;
        probeDrains<Size>(ring.block, total, [&](std::byte b) {
            if(b != byteOf(expected++)) { ++bad; }
        });
    }};
    std::size_t polls = 0;
    while(Buffer::unread(ring.block) != 0) { ++polls; }
    probe.join();
    CHECK(bad == 0 && Buffer::loadOffset(ring.block.readPosition) == ring.block.writePosition,
          what);
    std::printf("%s: %zu polls\n", what, polls);
}

// A down buffer: the probe writes, the target reads what was published.
template<std::size_t Size>
void down(char const* what) {
    using Buffer                = BufferT<rtt::BufferMode::skip, Size>;
    auto                  ring  = Ring<rtt::BufferMode::skip, Size>{};
    constexpr std::size_t total = 100000;

    std::thread probe{[&] {
        std::atomic_ref<std::uint32_t> const wrOff{ring.block.writePosition};
        std::atomic_ref<std::uint32_t> const rdOff{ring.block.readPosition};
        std::uint32_t                        wr   = wrOff.load(std::memory_order_relaxed);
        std::size_t                          sent = 0;
        while(sent != total) {
            std::uint32_t const rd   = rdOff.load(std::memory_order_acquire);
            std::size_t const   free = (rd + Size - wr - 1U) % Size;
            if(free == 0) {
                std::this_thread::yield();
                continue;
            }
            for(std::size_t k = 0; k != free && sent != total; ++k) {
                ring.block.buffer[wr] = byteOf(sent++);
                wr                    = static_cast<std::uint32_t>((wr + 1U) % Size);
            }
            wrOff.store(wr, std::memory_order_release);
        }
    }};

    std::size_t                 got = 0;
    std::size_t                 bad = 0;
    std::array<std::byte, Size> in{};
    while(got != total) {
        auto const n = Buffer::read(ring.block, in);
        for(auto const b : n) {
            if(b != byteOf(got++)) { ++bad; }
        }
        if(n.empty()) { std::this_thread::yield(); }
    }
    probe.join();
    CHECK(bad == 0, what);
}

}   // namespace

int main() {
    upBlock<16>("up, block: every byte in order, a power of two");
    upBlock<13>("up, block: every byte in order, any size");
    upStaged<16>("up, staged: whole records in order, a power of two");
    upStaged<13>("up, staged: whole records in order, any size");
    drainPoll<64>("unread() polled until the probe drained the ring");
    down<16>("down: every byte in order, a power of two");
    down<13>("down: every byte in order, any size");
    if(failures == 0) { std::puts("all checks passed"); }
    return failures == 0 ? 0 : 1;
}
