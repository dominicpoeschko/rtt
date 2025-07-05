#pragma once

#include <algorithm>
#include <array>
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
    template<BufferMode Mode, std::size_t BufferSize_, typename Name>
    struct BufferControlBlock {
    private:
        //memory layout demanded by rtt specification
        char const* const   name;
        std::byte* const    buffer;
        std::uint32_t const bufferSize;
        std::uint32_t       writePosition;
        std::uint32_t       readPosition;
        BufferMode const    mode;

        static_assert(
          sizeof(char const* const) == 4 && sizeof(std::byte* const) == 4,
          "rtt only works on 32bit systems since memory layout is specified for 4byte pointers");

        template<bool write,
                 typename T>
        std::span<T> transfer(std::span<T> const   userBuffer,
                              std::uint32_t const& readPos,
                              std::uint32_t&       writePos) {
            static_assert(sizeof(BufferControlBlock) == 24, "layout messed up...");

            std::span<T> remainingUserBuffer = userBuffer;

            while(!remainingUserBuffer.empty()) {
                auto calcNumBytesToCopy
                  = [localReadPosition = *reinterpret_cast<std::uint32_t const volatile*>(
                       std::addressof(readPos))](std::uint32_t localWritePos,
                                                 std::uint32_t remainingUserBufferSize) {
                        std::uint32_t const noWrapBufferSpace = [&]() {
                            if constexpr(write) {
                                return localReadPosition > localWritePos
                                       ? localReadPosition - localWritePos - 1U
                                       : BufferSize - (localWritePos - localReadPosition + 1U);
                            } else {
                                return localWritePos > localReadPosition
                                       ? BufferSize - localWritePos
                                       : localReadPosition - localWritePos;
                            }
                        }();
                        return std::min(
                          std::min<std::uint32_t>(noWrapBufferSpace, (BufferSize - localWritePos)),
                          remainingUserBufferSize);
                    };

                auto calcNewWritePos
                  = [](std::uint32_t localWritePos, std::uint32_t numBytesToCopy) {
                        //unfortunately, this really makes a difference in the generated code...
                        if constexpr(std::has_single_bit(BufferSize)) {
                            return (localWritePos + numBytesToCopy) % BufferSize;
                        } else {
                            std::uint32_t const newWritePos = localWritePos + numBytesToCopy;
                            if(newWritePos == BufferSize) {
                                return 0U;
                            }
                            return newWritePos;
                        }
                    };

                std::uint32_t const numBytesToCopy
                  = calcNumBytesToCopy(writePos, remainingUserBuffer.size());

                if(numBytesToCopy == 0) {
                    if constexpr(Mode != BufferMode::block) {
                        break;
                    } else {
                        continue;
                    }
                }
                if constexpr(Mode == BufferMode::skip) {
                    if(numBytesToCopy
                         + calcNumBytesToCopy(calcNewWritePos(writePosition, numBytesToCopy),
                                              remainingUserBuffer.size() - numBytesToCopy)
                       != remainingUserBuffer.size())
                    {
                        break;
                    }
                }

                if constexpr(write) {
                    std::memcpy(
                      std::next(buffer, static_cast<std::make_signed_t<std::size_t>>(writePos)),
                      remainingUserBuffer.data(),
                      numBytesToCopy);
                } else {
                    std::memcpy(
                      remainingUserBuffer.data(),
                      std::next(buffer, static_cast<std::make_signed_t<std::size_t>>(writePos)),
                      numBytesToCopy);
                }
                //TODO add data memory barrier asm("dmb")
                remainingUserBuffer = remainingUserBuffer.subspan(numBytesToCopy);
                writePos            = calcNewWritePos(writePos, numBytesToCopy);
            }
            if constexpr(write) {
                return remainingUserBuffer;
            } else {
                return std::span<T>{userBuffer.begin(), remainingUserBuffer.begin()};
            }
        }

    public:
        static constexpr auto BufferSize = BufferSize_;

        constexpr explicit BufferControlBlock(std::byte* const buffer_)
          : name{std::string_view{Name{}}.data()}
          , buffer{buffer_}
          , bufferSize{BufferSize}
          , writePosition{}
          , readPosition{}
          , mode{Mode} {}

        std::span<std::byte const> write(std::span<std::byte const> bufferToWrite) {
            return transfer<true>(bufferToWrite, readPosition, writePosition);
        }

        std::span<std::byte> read(std::span<std::byte> bufferToReadTo) {
            return transfer<false>(bufferToReadTo, writePosition, readPosition);
        }
    };
}   // namespace detail

template<typename Config>
struct ControlBlock {
private:
    template<typename UpConfig, typename DownConfig>
    struct BufferControlBlocks_impl;

    template<template<typename...> typename U,
             typename... Us,
             template<typename...> typename D,
             typename... Ds>
    struct BufferControlBlocks_impl<U<Us...>, D<Ds...>> {
        using Type
          = std::tuple<detail::BufferControlBlock<Us::Mode, Us::Size, typename Us::Name>...,
                       detail::BufferControlBlock<Ds::Mode, Ds::Size, typename Ds::Name>...>;
        static constexpr std::size_t UpSize{sizeof...(Us)};
        static constexpr std::size_t DownSize{sizeof...(Ds)};
    };

    using BufferControlBlocks = BufferControlBlocks_impl<typename Config::UpChannelConfigs,
                                                         typename Config::DownChannelConfigs>;

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
    static constexpr typename BufferControlBlocks::Type
    bufferControlBlocksInit_impl(UserStorage& buffers,
                                 std::index_sequence<Is...>) {
        using std::data;
        using std::get;
        using std::size;
        static_assert(((size(std::remove_cvref_t<decltype(get<Is>(buffers))>{})
                        == std::tuple_element_t<Is, typename BufferControlBlocks::Type>::BufferSize)
                       && ...),
                      "buffer size does not match");
        return {
          std::tuple_element_t<Is, typename BufferControlBlocks::Type>{data(get<Is>(buffers))}...};
    }

    template<typename UserStorage>
    static constexpr typename BufferControlBlocks::Type
    bufferControlBlocksInit(UserStorage& buffers) {
        return bufferControlBlocksInit_impl(
          buffers,
          std::make_index_sequence<BufferControlBlocks::UpSize + BufferControlBlocks::DownSize>{});
    }

    //memory layout demanded by rtt specification
    std::array<char, 16> const controlBlockId;
    std::uint32_t const        numUpBuffers;
    std::uint32_t const        numDownBuffers;
    [[no_unique_address]]   //for the strange case when there are 0 up and 0 down buffers...
    typename BufferControlBlocks::Type bufferControlBlocks;

public:
    using Storage_t = typename Storage<typename Config::UpChannelConfigs,
                                       typename Config::DownChannelConfigs>::Type;

    template<typename UserStorage>
    constexpr explicit ControlBlock(UserStorage& buffers)
      : controlBlockId{Config::ControlBlockId}
      , numUpBuffers{BufferControlBlocks::UpSize}
      , numDownBuffers{BufferControlBlocks::DownSize}
      , bufferControlBlocks{bufferControlBlocksInit(buffers)} {
        static_assert(sizeof(ControlBlock)
                        == 24 + (BufferControlBlocks::UpSize + BufferControlBlocks::DownSize) * 24,
                      "layout messed up...");
    }

    template<std::size_t                   BufferNumber,
             std::ranges::contiguous_range InputRange>
        requires std::is_trivially_copyable_v<std::ranges::range_value_t<InputRange>>
    std::span<std::byte const> write(InputRange const& bufferToWrite) {
        static_assert(BufferControlBlocks::UpSize > BufferNumber, "BufferNumber incorrect");
        return std::get<BufferNumber>(bufferControlBlocks)
          .write(std::as_bytes(std::span{bufferToWrite}));
    }

    template<std::size_t                   BufferNumber,
             std::ranges::contiguous_range OutputRange>
        requires std::is_trivially_copyable_v<std::ranges::range_value_t<OutputRange>>
    std::span<std::byte> read(OutputRange&& bufferToReadTo) {
        static_assert(BufferControlBlocks::DownSize > BufferNumber, "BufferNumber incorrect");
        return std::get<BufferNumber + BufferControlBlocks::UpSize>(bufferControlBlocks)
          .read(std::as_writable_bytes(std::span{bufferToReadTo}));
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
