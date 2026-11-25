#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../src/title_va_alias.h"
#include <array>
#include <cstdio>
#include <cstring>

using namespace d3d12test;
#define VERIFY(expr) do { if (!(expr)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #expr); return 1; } } while (0)

namespace {
void* leased_pages{};
unsigned released_leases{};
void* allocate_texture_pages(std::size_t bytes, std::uint64_t) {
    leased_pages = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (leased_pages) leonida::title_va_aliases().add_title_memory(reinterpret_cast<UINT64>(leased_pages), bytes);
    return leased_pages;
}
void release_texture_pages(void* p, std::uint64_t) {
    leonida::title_va_aliases().remove_title_memory(reinterpret_cast<UINT64>(p));
    VirtualFree(p, 0, MEM_RELEASE);
    ++released_leases;
}
int check_lease(ID3D12Device* dev, ID3D12Device* xbox) {
    D3D12_RESOURCE_DESC shape{};
    shape.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    shape.Width = shape.Height = 256;
    shape.DepthOrArraySize = shape.MipLevels = 1;
    shape.Format = DXGI_FORMAT_R32G32_FLOAT;
    shape.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    shape.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    Ref<ID3D12Resource> pc;
    VERIFY(SUCCEEDED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &shape, D3D12_RESOURCE_STATE_COMMON,
                                                  nullptr, IID_PPV_ARGS(&pc.p))));
    VERIFY(pc->GetGPUVirtualAddress() == 0);
    rt::d3d12_xs::set_descriptor_heap_allocator(allocate_texture_pages, release_texture_pages);
    using Create = HRESULT(WINAPI*)(void*, const D3D12_HEAP_PROPERTIES*, UINT, const D3D12_RESOURCE_DESC*,
                                    UINT, const D3D12_CLEAR_VALUE*, const IID*, void**);
    const auto create = reinterpret_cast<Create>(slot(xbox, 76));
    {
        Ref<ID3D12Resource> console;
        VERIFY(SUCCEEDED(create(xbox, &heap, 0, &shape, 0, nullptr, &__uuidof(ID3D12Resource),
                                reinterpret_cast<void**>(&console.p))));
        VERIFY(leased_pages && console->GetGPUVirtualAddress() == reinterpret_cast<UINT64>(leased_pages));
        VERIFY(released_leases == 0);
        auto* words = static_cast<UINT*>(leased_pages);
        words[0] = 0x365f;
        words[256 * 256 * 2 - 1] = 0x9814;
        VERIFY(console->GetGPUVirtualAddress() == reinterpret_cast<UINT64>(leased_pages));
    }
    VERIFY(released_leases == 1);
    rt::d3d12_xs::set_descriptor_heap_allocator(nullptr, nullptr);
    return 0;
}

// With the console D16S8 family made as D24S8 (LEONIDA_CONSOLE_D16S8=d24; the default on a GPU with D24S8
// attachments, every NVIDIA card), the depth provider gives a format 118 depth-stencil texture its unordered-access
// planes too (Resource::enable_depth_stencil_uav: D24 words to and from the R16 plane through the depth24 pass), so
// slots 76 and 133 make it with unordered access instead of falling back to a texture without it. The description is
// the one measured where a real boot stopped: 1280x720, one mip, the all-ones layout through slot 133.
int check_d16s8_d24_uav(ID3D12Device* xbox) {
    D3D12_RESOURCE_DESC tex{};
    tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Width = 1280;
    tex.Height = 720;
    tex.DepthOrArraySize = tex.MipLevels = 1;
    tex.SampleDesc.Count = 1;
    tex.Format = static_cast<DXGI_FORMAT>(118);
    tex.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);
    tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_CLEAR_VALUE clear{};
    clear.Format = tex.Format;
    clear.DepthStencil.Depth = 1;
    using Create = HRESULT(WINAPI*)(void*, const D3D12_HEAP_PROPERTIES*, UINT, const D3D12_RESOURCE_DESC*, UINT,
                                    const D3D12_CLEAR_VALUE*, const IID*, void**);
    Ref<ID3D12Resource> committed;
    VERIFY(SUCCEEDED(reinterpret_cast<Create>(slot(xbox, 76))(xbox, &heap, 0, &tex, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                               &clear, &__uuidof(ID3D12Resource),
                                                               reinterpret_cast<void**>(&committed.p))));
    VERIFY(committed.p && committed->GetDesc().Flags == tex.Flags);

    using Place = HRESULT(WINAPI*)(void*, UINT64, const D3D12_RESOURCE_DESC*, UINT, const D3D12_CLEAR_VALUE*,
                                   const IID*, void**);
    constexpr SIZE_T pages = 16u << 20;  // past the texture's size on any GPU; VirtualAlloc aligns it to 64 KiB
    void* va = VirtualAlloc(nullptr, pages, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    VERIFY(va);
    Ref<ID3D12Resource> placed;
    const HRESULT hr = reinterpret_cast<Place>(slot(xbox, 133))(xbox, reinterpret_cast<UINT64>(va), &tex, 0xFFFFFFFFu,
                                                                &clear, &__uuidof(ID3D12Resource),
                                                                reinterpret_cast<void**>(&placed.p));
    const bool made = SUCCEEDED(hr) && placed.p && placed->GetDesc().Flags == tex.Flags;
    placed.reset();
    VirtualFree(va, 0, MEM_RELEASE);
    VERIFY(made);
    std::printf("Xbox D16S8 made as D24S8: slots 76 and 133 make the depth-stencil texture with unordered access\n");
    return 0;
}
}

int main() {
    vktest::Device gpu;
    const auto status = gpu.initialize();
    if (status == vktest::DeviceStatus::unavailable) return 77;
    VERIFY(status == vktest::DeviceStatus::ready);
    const bool with_stencil = std::getenv("LEONIDA_TEST_D16S8_UAV") != nullptr;
    // The D16S8 family made as D24S8 (pc_format): the depth image holds 24-bit words, the UAV plane the D16 bits.
    const char* family = std::getenv("LEONIDA_CONSOLE_D16S8");
    const bool made_d24 = with_stencil && family && std::strcmp(family, "d24") == 0;
    constexpr auto xbox_d16s8 = static_cast<DXGI_FORMAT>(118);
    rt::d3d12::VulkanDeviceInfo info{};
    info.bindless = gpu.bindless;
    info.bindless_partially_bound = gpu.partially_bound;
    info.get_instance_proc = gpu.instance_proc;
    info.instance = gpu.instance;
    info.physical = gpu.physical;
    info.device = gpu.device;
    info.queue = gpu.queue;
    info.queue_family = gpu.family;
    info.buffer_device_address = true;
    ID3D12Device* dev{};
    VERIFY(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&dev))));
    constexpr UINT layers = 6, width = 32;
    std::array<UINT16, layers> original{0x1095, 0x568c, 0x242a, 0x93db, 0xb832, 0x0e69};
    D3D12_RESOURCE_DESC tex{};
    tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Width = tex.Height = 768;
    tex.DepthOrArraySize = layers;
    tex.MipLevels = 1;
    tex.SampleDesc.Count = 1;
    tex.Format = with_stencil ? xbox_d16s8 : DXGI_FORMAT_R16_TYPELESS;
    tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_CLEAR_VALUE clear{};
    clear.Format = with_stencil ? xbox_d16s8 : DXGI_FORMAT_D16_UNORM;
    clear.DepthStencil.Depth = 1;
    Ref<ID3D12Resource> depth;
    VERIFY(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                        &clear, IID_PPV_ARGS(&depth.p)) == E_INVALIDARG && !depth.p);
    Ref<ID3D12Device> proxy;
    VERIFY(SUCCEEDED(rt::d3d12_xs::create_device_proxy(dev, __uuidof(ID3D12Device),
                                                       reinterpret_cast<void**>(&proxy.p), true)));
    rt::d3d12::set_device_outer(dev, proxy.p);
    if (made_d24) VERIFY(check_d16s8_d24_uav(proxy.p) == 0);
    VERIFY(check_lease(dev, proxy.p) == 0);
    const auto allocation = dev->GetResourceAllocationInfo(0, 1, &tex);
    VERIFY(allocation.SizeInBytes >= 768u * 768 * layers * 2 && allocation.SizeInBytes != UINT64_MAX);
    using XboxCreate = HRESULT(WINAPI*)(void*, const D3D12_HEAP_PROPERTIES*, UINT, const D3D12_RESOURCE_DESC*,
                                          UINT, const D3D12_CLEAR_VALUE*, const IID*, void**);
    const auto create = reinterpret_cast<XboxCreate>(slot(proxy.p, 76));
    tex.Width = tex.Height = width;
    if (with_stencil) {
        VERIFY(SUCCEEDED(create(proxy.p, &heap, 0, &tex, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                &__uuidof(ID3D12Resource), reinterpret_cast<void**>(&depth.p))));
    } else {
        VERIFY(SUCCEEDED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                     &clear, __uuidof(ID3D12Resource), nullptr)));
        VERIFY(SUCCEEDED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                     &clear, IID_PPV_ARGS(&depth.p))));
    }
    VERIFY(depth->GetDesc().Flags == tex.Flags);
    Ref<ID3D12DescriptorHeap> dsv, uav;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, layers};
    VERIFY(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv.p))));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = layers + 1;
    VERIFY(SUCCEEDED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&uav.p))));
    const auto slot = [&](ID3D12DescriptorHeap* h, D3D12_DESCRIPTOR_HEAP_TYPE t, UINT i) {
        auto handle = h->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += i * dev->GetDescriptorHandleIncrementSize(t);
        return handle;
    };
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList7> list;
    Ref<ID3D12Fence> fence;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    VERIFY(SUCCEEDED(dev->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p))));
    VERIFY(SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
    VERIFY(SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr, IID_PPV_ARGS(&list.p))));
    VERIFY(SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2 * layers]{};
    UINT64 total{};
    dev->GetCopyableFootprints(&tex, 0, with_stencil ? 2 * layers : layers, 0, footprints, nullptr, nullptr, &total);
    auto buffer = buffer_desc(total);
    D3D12_HEAP_PROPERTIES host{D3D12_HEAP_TYPE_READBACK};
    Ref<ID3D12Resource> readback;
    VERIFY(SUCCEEDED(dev->CreateCommittedResource(&host, D3D12_HEAP_FLAG_NONE, &buffer,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback.p))));
    for (UINT i = 0; i < layers; ++i) {
        const auto d = slot(dsv.p, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, i);
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = with_stencil ? xbox_d16s8 : DXGI_FORMAT_D16_UNORM;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dv.Texture2DArray.FirstArraySlice = i;
        dv.Texture2DArray.ArraySize = 1;
        dev->CreateDepthStencilView(depth.p, &dv, d);
        const auto flags = with_stencil ? D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL : D3D12_CLEAR_FLAG_DEPTH;
        list->ClearDepthStencilView(d, flags, float(original[i]) / 65535.f, UINT8(0x30 + i), 0, nullptr);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = DXGI_FORMAT_R16_UINT;
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        uv.Texture2DArray.FirstArraySlice = i;
        uv.Texture2DArray.ArraySize = 1;
        dev->CreateUnorderedAccessView(depth.p, nullptr, &uv, slot(uav.p, hd.Type, i));
    }
    // Mix legacy and enhanced transitions. Leave slice five entirely on the depth image.
    D3D12_RECT rect{3, 4, 19, 26};
    for (UINT i = 0; i < layers - 1; ++i) {
        D3D12_TEXTURE_BARRIER enhanced{};
        enhanced.SyncBefore = D3D12_BARRIER_SYNC_DEPTH_STENCIL;
        enhanced.SyncAfter = D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW;
        enhanced.AccessBefore = D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
        enhanced.AccessAfter = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
        enhanced.LayoutBefore = D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
        enhanced.LayoutAfter = D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
        enhanced.pResource = depth.p;
        enhanced.Subresources = {0, 1, i, 1, 0, 1};
        D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_TEXTURE, 1};
        group.pTextureBarriers = &enhanced;
        auto legacy = transition(depth.p, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        legacy.Transition.Subresource = i;
        if (i % 2) list->Barrier(1, &group); else list->ResourceBarrier(1, &legacy);
        const UINT values[4]{0x8040u + i};
        list->ClearUnorderedAccessViewUint({}, slot(uav.p, hd.Type, i), depth.p, values, 1, &rect);
        std::swap(legacy.Transition.StateBefore, legacy.Transition.StateAfter);
        std::swap(enhanced.SyncBefore, enhanced.SyncAfter);
        std::swap(enhanced.AccessBefore, enhanced.AccessAfter);
        std::swap(enhanced.LayoutBefore, enhanced.LayoutAfter);
        if (i % 2) list->Barrier(1, &group); else list->ResourceBarrier(1, &legacy);
    }
    ID3D12Resource *raw_depth = depth.p, *raw_stencil = nullptr;
    if (with_stencil) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC st{};
        st.Format = DXGI_FORMAT_R8_UINT;
        st.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        st.Texture2DArray.ArraySize = layers;
        st.Texture2DArray.PlaneSlice = 1;
        dev->CreateUnorderedAccessView(depth.p, nullptr, &st, slot(uav.p, hd.Type, layers));
        const auto inspected_plane = [&](UINT index) -> ID3D12Resource* {
            leonida::gfx10::ImageWords words{};
            std::memcpy(words.data(), reinterpret_cast<const void*>(slot(uav.p,hd.Type,index).ptr), sizeof(words));
            leonida::gfx10::Image image{};
            if (!leonida::gfx10::decode(words,image)) return nullptr;
            return reinterpret_cast<ID3D12Resource*>(rt::d3d12::uav_image_resource(depth.p,image.address));
        };
        raw_depth = inspected_plane(0);
        raw_stencil = inspected_plane(layers);
        VERIFY(raw_depth && raw_stencil && raw_depth != depth.p && raw_stencil != raw_depth);
    }
    // Made as D24S8, the depth image itself is read as well, before the planes are: its 24-bit words must be the D16
    // values the clears and the UAV wrote (the UAV writes reached the depth image), its stencil the cleared bytes.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT words[2 * layers]{};
    Ref<ID3D12Resource> parent_readback;
    if (made_d24) {
        auto d24 = tex;
        d24.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        UINT64 parent_total{};
        dev->GetCopyableFootprints(&d24, 0, 2 * layers, 0, words, nullptr, nullptr, &parent_total);
        auto parent_buffer = buffer_desc(parent_total);
        VERIFY(SUCCEEDED(dev->CreateCommittedResource(&host, D3D12_HEAP_FLAG_NONE, &parent_buffer,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(&parent_readback.p))));
        auto read = transition(depth.p, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &read);
        for (UINT i = 0; i < 2 * layers; ++i) {
            D3D12_TEXTURE_COPY_LOCATION from{}, to{};
            from.pResource = depth.p; from.SubresourceIndex = i;
            to.pResource = parent_readback.p; to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = words[i];
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        std::swap(read.Transition.StateBefore, read.Transition.StateAfter);
        list->ResourceBarrier(1, &read);
    }
    auto ready = transition(depth.p, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             with_stencil ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &ready);
    for (UINT i = 0; i < (with_stencil ? 2 * layers : layers); ++i) {
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = i < layers ? raw_depth : raw_stencil; from.SubresourceIndex = i % layers;
        to.pResource = readback.p; to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = footprints[i];
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    VERIFY(SUCCEEDED(list->Close()));
    ID3D12CommandList* batch[]{list.p};
    queue->ExecuteCommandLists(1, batch);
    VERIFY(SUCCEEDED(queue->Signal(fence.p, 1)));
    const auto end = GetTickCount64() + 5000;
    while (fence->GetCompletedValue() < 1 && GetTickCount64() < end) Sleep(1);
    VERIFY(fence->GetCompletedValue() == 1);
    void* mapped{};
    VERIFY(SUCCEEDED(readback->Map(0, nullptr, &mapped)));
    auto* bytes = static_cast<const std::uint8_t*>(mapped);
    for (UINT i = 0; i < layers; ++i) for (int y = 0; y < width; ++y) for (int x = 0; x < width; ++x) {
        const auto offset = footprints[i].Offset + y * footprints[i].Footprint.RowPitch + x * 2;
        const auto value = *reinterpret_cast<const UINT16*>(bytes + offset);
        const bool changed = i < layers-1 && x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
        const UINT expected = changed ? 0x8040u+i : original[i];
        if (value != expected) {
            std::fprintf(stderr, "slice %u pixel (%d,%d) expected %04x got %04x\n", i, x, y, expected, value);
            return 1;
        }
    }
    if (with_stencil) {
        for (UINT i=0; i<layers; ++i) for (UINT y=0; y<width; ++y) for (UINT x=0; x<width; ++x)
            VERIFY(bytes[footprints[layers+i].Offset + y*footprints[layers+i].Footprint.RowPitch + x] == 0x30+i);
        std::printf("Xbox D16S8 depth and stencil: %u depth words and %u stencil bytes exact after UAV round trip\n", width*width*layers,width*width*layers);
    }
    readback->Unmap(0, nullptr);
    if (made_d24) {
        void* parent_mapped{};
        VERIFY(SUCCEEDED(parent_readback->Map(0, nullptr, &parent_mapped)));
        const auto* parent = static_cast<const std::uint8_t*>(parent_mapped);
        for (UINT i = 0; i < layers; ++i) for (int y = 0; y < width; ++y) for (int x = 0; x < width; ++x) {
            const auto word = *reinterpret_cast<const UINT*>(parent + words[i].Offset + y * words[i].Footprint.RowPitch + x * 4);
            const UINT d16 = UINT((UINT64(word & 0xffffffu) * 65535 + 8388607) / 16777215);  // the nearest D16 value
            const bool changed = i < layers-1 && x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
            const UINT expected = changed ? 0x8040u+i : original[i];
            if (d16 != expected) {
                std::fprintf(stderr, "D24 slice %u pixel (%d,%d) word %06x is D16 %04x, expected %04x\n", i, x, y,
                             word & 0xffffffu, d16, expected);
                return 1;
            }
            VERIFY(parent[words[layers+i].Offset + y*words[layers+i].Footprint.RowPitch + x] == 0x30+i);
        }
        parent_readback->Unmap(0, nullptr);
        std::printf("Xbox D16S8 made as D24S8: the depth image holds what the clears and the UAV wrote, %u texels\n",
                    width * width * layers);
    }
    VERIFY(gpu.validation_errors() == 0);
    std::printf("Depth D16 / storage R16_UINT, six layers and %u exact texels\n", width * width * layers);
    return 0;
}
