// The flip hook publishes the pending surfaces whose results the guest may read before the next
// flip. The surfaces above the small cap (64 KiB) were skipped, so a surface as large as the
// title's video frame (4 MiB) kept its results on the device and the guest read the bytes its
// own kernel had written. This is the same test as FlipStoresSmallResults, with a surface above
// the small cap: the value written before the flip must reach guest memory.
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
constexpr std::uint32_t Threads = 8;
constexpr std::uint32_t Side = 256;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t TileR64KBX = 0x1b;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t OutputHandle = 7;
constexpr std::uint32_t Untouched = 0xdeadbeefu;
constexpr std::uint32_t Written = 0x12345678u;

alignas(256) constexpr std::array<std::uint32_t, 7> StoreCode{
    0x7e040300, 0x7e060280, 0x7e0202ff, Written, 0xf0200108, 0x00020102, 0xbf810000,
};

alignas(4) std::atomic<std::uint32_t> Gate = 0;

std::uint64_t AddressOf(const void* data) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
}

std::uint32_t Low(const void* data) {
    return static_cast<std::uint32_t>(AddressOf(data));
}

std::uint32_t High(const void* data) {
    return static_cast<std::uint32_t>(AddressOf(data) >> 32u);
}

std::array<std::uint32_t, 8> TextureDescriptor(const std::uint32_t* texels) {
    const auto address = AddressOf(texels);
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format32UInt << 20u) | (((Side - 1u) & 3u) << 30u),
        ((Side - 1u) >> 2u) | ((Side - 1u) << 14u),
        0xfacu | (TileR64KBX << 20u) | (Type2D << 28u),
        0u,
        0u,
        0u,
        0u,
    };
}

std::vector<std::uint32_t> Frame(const std::uint32_t* texels) {
    std::vector<std::uint32_t> userData(16, 0u);
    const auto texture = TextureDescriptor(texels);
    std::copy(texture.begin(), texture.end(), userData.begin() + 8);
    std::vector<std::uint32_t> words;
    const auto registers = [&](const std::uint32_t first, const std::span<const std::uint32_t> values) {
        words.push_back(0xc0007600u | (static_cast<std::uint32_t>(values.size()) << 16u));
        words.push_back(first);
        words.insert(words.end(), values.begin(), values.end());
    };
    const std::array<std::uint32_t, 3> shape{Threads, 1, 1};
    registers(0x207, shape);
    const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(AddressOf(StoreCode.data()) >> 8u), static_cast<std::uint32_t>(AddressOf(StoreCode.data()) >> 40u)};
    registers(0x20c, program);
    const std::array<std::uint32_t, 1> resource{static_cast<std::uint32_t>(userData.size()) << 1u};
    registers(0x213, resource);
    registers(0x240, userData);
    words.insert(words.end(), {0xc0031500u, 1, 1, 1, 0x41u});
    words.insert(words.end(), {AgcDriver::FlipPacketHeader, OutputHandle, 0xfffffffeu, 1, 0, 0});
    words.insert(words.end(), {0xc0053c00u, 0x13u, Low(&Gate), High(&Gate), 1u, 0xffffffffu, 0x19u});
    return words;
}

class Request final : public AgcDriver::IFlipRequest {
public:
    explicit Request(std::atomic<int>& ready) : ready(ready) {}
    void GpuReady(const std::shared_ptr<AgcDriver::FrameTiming>&) override { ++ready; }
    void Fail(std::exception_ptr) noexcept override {}
private:
    std::atomic<int>& ready;
};

class Output final : public AgcDriver::IVideoOutput {
public:
    std::shared_ptr<AgcDriver::IFlipRequest> Reserve(const AgcDriver::FlipInfo&) override { return std::make_shared<Request>(ready); }
    void Fail(std::exception_ptr) noexcept override {}
    std::atomic<int> ready = 0;
};

}

int main() {
    try {
        // The flip hook publishes the large surfaces only when this is set (the guest's own read
        // of the range already publishes them, so the flip publish is opt-in).
#ifdef _WIN32
        _putenv_s("APS5_STORE_LARGE_AT_FLIP", "1");
#else
        setenv("APS5_STORE_LARGE_AT_FLIP", "1", 1);
#endif
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        // The surface's size does not depend on its address, so the size is known before the
        // memory for it is allocated.
        std::array<std::uint32_t, 8> probe{0x1000u, 0u, 0u, 0xfacu | (TileR64KBX << 20u) | (Type2D << 28u), 0u, 0u, 0u, 0u};
        probe[1] = (Format32UInt << 20u) | (((Side - 1u) & 3u) << 30u);
        probe[2] = ((Side - 1u) >> 2u) | ((Side - 1u) << 14u);
        const auto bytes = AgcDriver::Graphics::DescribeSurface(AgcDriver::Graphics::DecodeTextureResource(probe)).guestBytes;
        Require(bytes > AgcDriver::Graphics::StorageTexture::SmallStoreBytes, "the test image is not above the small cap the flip hook skips");
        Require(bytes <= AgcDriver::Graphics::StorageTexture::LargeStoreBytes, "the test image is above the large cap the flip hook publishes");
        // The guest memory is 64 KiB aligned (the tile size) and holds the surface's bytes.
        const std::size_t aligned = (static_cast<std::size_t>(bytes) + 0xffffu) & ~std::size_t{0xffffu};
        std::vector<std::uint32_t> storage((aligned + 0x10000u) / 4u);
        auto* texels = reinterpret_cast<std::uint32_t*>((reinterpret_cast<std::uintptr_t>(storage.data()) + 0xffffu) & ~std::uintptr_t{0xffffu});
        std::fill(texels, texels + bytes / 4u, Untouched);
        {
            GuestAllocations::Mutation mutation;
            mutation.Add(texels, static_cast<std::size_t>(bytes), true, true, true);
        }
        const volatile std::uint32_t* cpu = texels;
        const auto stored = [&] {
            for (std::size_t index = 0; index < bytes / 4u; ++index) {
                if (cpu[index] == Written) return true;
            }
            return false;
        };
        auto output = std::make_shared<Output>();
        AgcDriverRegisterVideoOutput_nid_postfix(OutputHandle, output);

        auto words = Frame(texels);
        Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
        AgcDriver::Submit(&packet, 0);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while ((output->ready.load() == 0 || !stored()) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool flipped = output->ready.load() == 1;
        const bool seen = stored();
        Gate.store(1, std::memory_order_release);

        AgcDriverWaitIdle_nid_postfix();
        AgcDriverUnregisterVideoOutput_nid_postfix(OutputHandle, output);
        AgcDriverShutdown_nid_postfix();
        {
            GuestAllocations::Mutation mutation;
            mutation.Remove(texels);
        }
        Require(flipped, "the flip did not reach the GPU");
        Require(seen, "a large image written before a flip did not reach guest memory while the queue kept working");
        std::puts("flip stores large results tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
