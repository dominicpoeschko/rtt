#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace rtt {
//values defined by rtt specification
enum class BufferMode : std::uint32_t { skip = 0, trim = 1, block = 2 };

namespace detail {
    //memory layout demanded by rtt specification
    struct BufferControlBlock {
        char const* const   name{};
        std::byte* const    buffer{};
        std::uint32_t const bufferSize{};
        std::uint32_t       writePosition{};
        std::uint32_t       readPosition{};
        BufferMode const    mode{};
    };

    template<BufferMode Mode, std::size_t BufferSize_, typename Name>
    struct Buffer {
    private:
        static_assert(
          sizeof(char const*) == 4 && sizeof(std::byte*) == 4,
          "rtt only works on 32bit systems since memory layout is specified for 4byte pointers");

        struct Space {
            std::size_t contiguous;   // up to the buffer's end
            std::size_t total;
        };

        // free bytes to write at `pos`, or filled bytes to read there
        template<bool write>
        static Space space(std::size_t const otherPos,
                           std::size_t const pos) {
            if constexpr(write) {
                if(otherPos > pos) { return {otherPos - pos - 1U, otherPos - pos - 1U}; }
                std::size_t const total = BufferSize - 1U - (pos - otherPos);
                return {std::min(total, BufferSize - pos), total};
            } else {
                if(pos > otherPos) { return {BufferSize - pos, BufferSize - pos + otherPos}; }
                return {otherPos - pos, otherPos - pos};
            }
        }

        static std::uint32_t nextPos(std::uint32_t const pos,
                                     std::size_t const   numBytes) {
            // no division: % only where it is a mask (a power of two), a compare otherwise
            if constexpr(std::has_single_bit(BufferSize)) {
                return static_cast<std::uint32_t>((pos + numBytes) % BufferSize);
            } else {
                auto const newPos = static_cast<std::uint32_t>(pos + numBytes);
                return newPos == BufferSize ? 0U : newPos;
            }
        }

        // the data before the offset that publishes it: SEGGER_RTT.h puts a DMB there (RTT__DMB) on
        // the cores that may reorder memory accesses - ARMv7E-M, ARMv8-M baseline and mainline
        static void publishFence() {
#if defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_8M_BASE__) || defined(__ARM_ARCH_8M_MAIN__) \
  || defined(__ARM_ARCH_8_1M_MAIN__)
            asm volatile("dmb" ::: "memory");
#else
            std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
        }

        template<bool write,
                 typename T>
        static std::span<T> transfer(std::byte* const     buffer,
                                     std::span<T> const   userBuffer,
                                     std::uint32_t const& otherPosition,
                                     std::uint32_t&       ownPosition) {
            std::span<T>  remaining = userBuffer;
            std::uint32_t pos       = ownPosition;

            while(!remaining.empty()) {
                Space const free = space<write>(
                  *reinterpret_cast<std::uint32_t const volatile*>(std::addressof(otherPosition)),
                  pos);

                // skip: a write goes in whole or not at all; a read takes what is there
                if constexpr(write && Mode == BufferMode::skip) {
                    if(free.total < remaining.size()) { break; }
                }
                if(free.total == 0) {
                    if constexpr(Mode != BufferMode::block) {
                        break;
                    } else {
                        continue;
                    }
                }

                // at most two pieces: up to the buffer's end, then from its start
                std::size_t left  = std::min(free.total, remaining.size());
                std::size_t piece = std::min(free.contiguous, left);
                while(piece != 0) {
                    auto* const ring
                      = std::next(buffer, static_cast<std::make_signed_t<std::size_t>>(pos));
                    if constexpr(write) {
                        std::memcpy(ring, remaining.data(), piece);
                    } else {
                        std::memcpy(remaining.data(), ring, piece);
                    }
                    remaining = remaining.subspan(piece);
                    pos       = nextPos(pos, piece);
                    left -= piece;
                    piece = left;
                }
                publishFence();
                ownPosition = pos;
            }
            if constexpr(write) {
                return remaining;
            } else {
                return std::span<T>{userBuffer.begin(), remaining.begin()};
            }
        }

    public:
        static constexpr auto BufferSize = BufferSize_;

        static constexpr BufferControlBlock make(std::byte* const buffer) {
            return {.name       = std::string_view{Name{}}.data(),
                    .buffer     = buffer,
                    .bufferSize = BufferSize,
                    .mode       = Mode};
        }

        static std::span<std::byte const> write(BufferControlBlock&        block,
                                                std::span<std::byte const> bufferToWrite) {
            return transfer<true>(block.buffer,
                                  bufferToWrite,
                                  block.readPosition,
                                  block.writePosition);
        }

        static std::span<std::byte> read(BufferControlBlock&  block,
                                         std::span<std::byte> bufferToReadTo) {
            return transfer<false>(block.buffer,
                                   bufferToReadTo,
                                   block.writePosition,
                                   block.readPosition);
        }
    };
}   // namespace detail

template<typename Config>
struct ControlBlock {
private:
    template<typename UpConfig, typename DownConfig>
    struct Buffers_impl;

    template<template<typename...> typename U,
             typename... Us,
             template<typename...> typename D,
             typename... Ds>
    struct Buffers_impl<U<Us...>, D<Ds...>> {
        using Types = std::tuple<detail::Buffer<Us::Mode, Us::Size, typename Us::Name>...,
                                 detail::Buffer<Ds::Mode, Ds::Size, typename Ds::Name>...>;
        static constexpr std::size_t UpSize{sizeof...(Us)};
        static constexpr std::size_t DownSize{sizeof...(Ds)};
    };

    using Buffers
      = Buffers_impl<typename Config::UpChannelConfigs, typename Config::DownChannelConfigs>;

    template<std::size_t I>
    using BufferAt = std::tuple_element_t<I, typename Buffers::Types>;

    static constexpr std::size_t NumBuffers = Buffers::UpSize + Buffers::DownSize;

    // std::array<T, 0> is not empty in libc++
    struct NoBuffers {};

    using BufferControlBlocks = std::
      conditional_t<NumBuffers == 0, NoBuffers, std::array<detail::BufferControlBlock, NumBuffers>>;

    template<typename UpConfig, typename DownConfig>
    struct Storage;

    template<template<typename...> typename U,
             typename... Us,
             template<typename...> typename D,
             typename... Ds>
    struct Storage<U<Us...>, D<Ds...>> {
        using Type
          = std::tuple<std::array<std::byte, Us::Size>..., std::array<std::byte, Ds::Size>...>;
    };

    template<typename UserStorage,
             std::size_t... Is>
    static constexpr BufferControlBlocks bufferControlBlocksInit(UserStorage& buffers,
                                                                 std::index_sequence<Is...>) {
        using std::data;
        using std::get;
        using std::size;
        static_assert(
          ((size(std::remove_cvref_t<decltype(get<Is>(buffers))>{}) == BufferAt<Is>::BufferSize)
           && ...),
          "buffer size does not match");
        return {BufferAt<Is>::make(data(get<Is>(buffers)))...};
    }

    //memory layout demanded by rtt specification
    std::array<char, 16> const                controlBlockId;
    std::uint32_t const                       numUpBuffers;
    std::uint32_t const                       numDownBuffers;
    [[no_unique_address]] BufferControlBlocks bufferControlBlocks;

public:
    using Storage_t = typename Storage<typename Config::UpChannelConfigs,
                                       typename Config::DownChannelConfigs>::Type;

    template<typename UserStorage>
    constexpr explicit ControlBlock(UserStorage& buffers)
      : controlBlockId{Config::ControlBlockId}
      , numUpBuffers{Buffers::UpSize}
      , numDownBuffers{Buffers::DownSize}
      , bufferControlBlocks{bufferControlBlocksInit(buffers,
                                                    std::make_index_sequence<NumBuffers>{})} {
        static_assert(sizeof(detail::BufferControlBlock) == 24, "layout messed up...");
        static_assert(sizeof(ControlBlock) == 24 + NumBuffers * 24, "layout messed up...");
    }

    template<std::size_t                   BufferNumber,
             std::ranges::contiguous_range InputRange>
        requires std::is_trivially_copyable_v<std::ranges::range_value_t<InputRange>>
    std::span<std::byte const> write(InputRange const& bufferToWrite) {
        static_assert(Buffers::UpSize > BufferNumber, "BufferNumber incorrect");
        return BufferAt<BufferNumber>::write(std::get<BufferNumber>(bufferControlBlocks),
                                             std::as_bytes(std::span{bufferToWrite}));
    }

    template<std::size_t                   BufferNumber,
             std::ranges::contiguous_range OutputRange>
        requires std::is_trivially_copyable_v<std::ranges::range_value_t<OutputRange>>
    std::span<std::byte> read(OutputRange&& bufferToReadTo) {
        static_assert(Buffers::DownSize > BufferNumber, "BufferNumber incorrect");
        constexpr std::size_t Index = BufferNumber + Buffers::UpSize;
        return BufferAt<Index>::read(
          std::get<Index>(bufferControlBlocks),
          std::as_writable_bytes(std::span{std::forward<OutputRange>(bufferToReadTo)}));
    }
};

struct EmptyName {
    constexpr operator std::string_view() const { return {}; }
};

template<std::size_t Size_, BufferMode Mode_ = rtt::BufferMode::block, typename Name_ = EmptyName>
struct ChannelConfig {
    static constexpr std::size_t Size = Size_;
    static constexpr BufferMode  Mode = Mode_;
    using Name                        = Name_;
};

static constexpr std::array<char, 16> DefaultControlBlockId{
  {'S', 'E', 'G', 'G', 'E', 'R', ' ', 'R', 'T', 'T', 0, 0, 0, 0, 0, 0}
};

template<typename... Configs>
using make_ChannelConfigs_t = std::tuple<Configs...>;

using EmptyChannelConfig = make_ChannelConfigs_t<>;

template<BufferMode Mode, std::size_t... Sizes>
struct SingleModeUpOnlyEmptyNameConfig {
    using UpChannelConfigs = make_ChannelConfigs_t<ChannelConfig<Sizes, Mode, EmptyName>...>;

    using DownChannelConfigs = EmptyChannelConfig;

    static constexpr auto ControlBlockId{DefaultControlBlockId};
};
}   // namespace rtt
