# rtt
C++ implementation of the SEGGER Real Time Transfer (RTT) protocol.
RTT is a protocol to transfer data beetween a microcontroller and a host
computer with the use of a debugger.

The implemation is fully configurable at compile time and there
is no runtime overhead[^1] for the initialisation of the control blocks.

## Usage
For the most basic usage see [examples/static_buffers.cpp](examples/static_buffers.cpp).

```c++
#include "rtt/rtt.hpp"
#include <string_view>

// basic configuration with 1 up buffer in skip mode with 512 bytes size.
using RttConfig = rtt::SingleModeUpOnlyEmptyNameConfig<rtt::BufferMode::skip, 512>;
using RttType   = rtt::ControlBlock<RttConfig>;

static inline constinit typename RttType::Storage_t storage{};
static inline constinit RttType                     rttControlBlock{storage};

int main() {
    static constexpr std::string_view msg{"test\n"};
    rttControlBlock.write<0>(msg);
}
```

There are more advanced usecases and configurations in the [examples](examples) folder.

### host application
For receiving the data on the host see
[segger website](https://www.segger.com/products/debug-probes/j-link/technology/about-real-time-transfer)
or [openocd website](https://openocd.org/doc/html/General-Commands.html).

### cmake
To use this project in a cmake project, just add the rtt folder via `add_subdirectory` and
link against it using `target_link_libraries`.

example:

```cmake
  add_subdirectory(rtt)
  target_link_libraries(${target_name} rtt::rtt)
```

### header only
You can also use rtt as single header. Just copy `rtt.hpp` wherever you need it.

### buffer placement
To prevent zero initializing of the storage you can place it into a ram section
which does not get initialized.

example:

```c++
[[gnu::section(".noInit")]] static inline constinit typename RttType::Storage_t rttStorage;
```

## Tests
`tests/` is a CMake project of its own, run by CI on every push:

```sh
cmake -S tests -B build && cmake --build build && ctest --test-dir build
```

- the ring against a model, its edges, block mode against a reader thread, and host offsets
  past the buffer (the target must never copy outside it), built `-m32` since rtt needs
  4-byte pointers (`-DUSE_SANITIZER=address` for ASan/UBSan)
- the examples: all compiled, the ones that end run
- with `arm-none-eabi-gcc` installed: an example cross-compiled with gcc and clang for
  Cortex-M0+ to M55 and A7/R5, warnings as errors, and a `dmb` checked in the assembly
  exactly where the core needs one

## Contribution
Feel free to report bugs or submit pull requests.

## Limitations
- On systems with cache, the user needs to ensure that the storage
  and control blocks are aligned to cache lines.
    
- Only ranges with contigues memory are supported.
  There is no reason, other then performace, to not support non contigues ranges.
  When there is a need for that, feel free to suggest an option how to configure it.

- A `dmb` is placed between the data and the offset that publishes it on
  ARMv7E-M (Cortex-M4/M7), ARMv8-M baseline/mainline (Cortex-M23/M33), ARMv8.1-M
  and ARMv7-A/R (Cortex-A/R), as `SEGGER_RTT.h` does. Cortex-M0/M0+/M3 get only a
  compiler barrier; SEGGER adds no `dmb` there either.
    
[^1]: The control blocks will be inizialised in startup code while copying the data segment.
      There will be `24 + <number of buffers> * 24` bytes copied.
