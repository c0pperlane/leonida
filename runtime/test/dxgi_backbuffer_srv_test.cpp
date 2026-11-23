// Sample DXGI back buffers through D3D12 SRVs (default, explicit and _SRGB) across Present, ResizeBuffers, and
// swapchain release.
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include "../shims/d3d12_com.h"
#include "../src/shader_library.h"
#include "d3d12_test_shaders.h"
#include "shader_fixture_path.h"
#include "test_temp_dir.h"
#include "vk_test_device.h"
#include "window_station.h"

#include <dxgi1_4.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

using namespace d3d12test;
constexpr UINT width = 8;
constexpr UINT height = 8;
constexpr UINT words = 64;  // cs_set0_sample has 64 invocations per group.
constexpr UINT bytes = words * sizeof(std::uint32_t);
constexpr DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;

struct TestWindow {
    HWND hwnd = nullptr;
    ~TestWindow() { if (hwnd) DestroyWindow(hwnd); }
};

bool wait_fence(ID3D12Fence* fence, UINT64 value) {
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return false;
    const bool ready = SUCCEEDED(fence->SetEventOnCompletion(value, event)) &&
                       WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
    CloseHandle(event);
    return ready;
}

int run(vktest::Device& vk) {
    const auto program = fssm_program({0x43530003, 0xbf810000});
    const auto container = shader_container(program);
    const testfiles::ScratchDir scratch("leonida_dxgi_backbuffer_srv_test");
    const auto& library = scratch.path();
    EXPECT(testfiles::copy_file(vktest::shader_fixture_path(LEONIDA_TEST_CS_SET0_SAMPLE_SPV),
                                library / "cs_set0_sample.spv"));
    {
        std::ofstream index(library / "index.json", std::ios::binary);
        index << "{\"cs_set0_sample\": {\"stage\": \"cs\", \"fssm_sha256\": \"" << leonida::sha256_hex(program)
              << "\", \"bindings\": [0, 1, 2], \"push_constant_bytes\": 144, \"local_size\": [64, 1, 1], "
                 "\"uses_code_base\": false, \"wave64\": false}}";
        EXPECT(index);
    }

    RECT rect{0, 0, width, height};
    EXPECT(AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE));
    TestWindow window{CreateWindowExW(0, L"STATIC", L"Leonida DXGI back buffer SRV", WS_OVERLAPPEDWINDOW,
                                      CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                                      nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)};
    EXPECT(window.hwnd);

    rt::d3d12::VulkanDeviceInfo info{};
    info.get_instance_proc = vk.instance_proc;
    info.instance = vk.instance;
    info.physical = vk.physical;
    info.device = vk.device;
    info.queue = vk.queue;
    info.queue_family = vk.family;
    info.buffer_device_address = true;
    info.bindless = true;
    info.bindless_partially_bound = vk.partially_bound;
    info.presentation = true;
    info.swapchain_maintenance1 = vk.swapchain_maintenance1;
    info.shader_dir = library.string();
    Ref<ID3D12Device> device;
    EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&device.p))));
    if (rt::d3d12::device_push_constants_need_fallback(device.p)) {
        std::printf("SKIP: the sample shader needs 144 push-constant bytes\n");
        return 77;
    }
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12Fence> fence;
    const D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    EXPECT(SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue.p))));
    EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p))));
    Ref<IDXGIFactory4> factory;
    EXPECT(SUCCEEDED(rt::d3d12::create_dxgi_factory(device.p, IID_PPV_ARGS(&factory.p))));
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width;
    sd.Height = height;
    sd.Format = format;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    Ref<IDXGISwapChain1> chain1;
    Ref<IDXGISwapChain3> chain;
    EXPECT(SUCCEEDED(factory->CreateSwapChainForHwnd(queue.p, window.hwnd, &sd, nullptr, nullptr, &chain1.p)));
    EXPECT(SUCCEEDED(chain1->QueryInterface(IID_PPV_ARGS(&chain.p))));

    D3D12_DESCRIPTOR_RANGE view_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_DESCRIPTOR_RANGE sampler_range{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 2};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor = {0, 0};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable = {1, &view_range};
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable = {1, &sampler_range};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    root_desc.Desc_1_0 = {4, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    const auto blob = serialize_root_signature(root_desc);
    EXPECT(!blob.empty());
    Ref<ID3D12RootSignature> root;
    EXPECT(SUCCEEDED(device->CreateRootSignature(0, blob.data(), blob.size(), IID_PPV_ARGS(&root.p))));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root.p;
    pso_desc.CS = {container.data(), container.size()};
    Ref<ID3D12PipelineState> pso;
    EXPECT(SUCCEEDED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso.p))));

    const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
    const auto out_desc = buffer_desc(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto rb_desc = buffer_desc(bytes);
    Ref<ID3D12Resource> output, readback;
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &out_desc,
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                     IID_PPV_ARGS(&output.p))));
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&readback.p))));

    const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    const D3D12_DESCRIPTOR_HEAP_DESC srv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                             D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    const D3D12_DESCRIPTOR_HEAP_DESC smp_desc{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1,
                                             D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Ref<ID3D12DescriptorHeap> rtvs, srvs, samplers;
    EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs.p))));
    EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&srvs.p))));
    EXPECT(SUCCEEDED(device->CreateDescriptorHeap(&smp_desc, IID_PPV_ARGS(&samplers.p))));
    const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
    const auto srv = srvs->GetCPUDescriptorHandleForHeapStart();
    const auto srv_gpu = srvs->GetGPUDescriptorHandleForHeapStart();
    const auto sampler_gpu = samplers->GetGPUDescriptorHandleForHeapStart();
    const auto output_va = output->GetGPUVirtualAddress();
    EXPECT(rtv.ptr && srv.ptr && srv_gpu.ptr && sampler_gpu.ptr && output_va);
    D3D12_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    device->CreateSampler(&sampler, samplers->GetCPUDescriptorHandleForHeapStart());
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;

    UINT64 fence_value = 0;
    unsigned mismatches = 0;
    const auto sample = [&](const char* label, ID3D12Resource* image, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                            std::uint32_t expected, std::uint32_t clear = 0) -> int {
        if (!clear) clear = expected;
        device->CreateRenderTargetView(image, nullptr, rtv);
        device->CreateShaderResourceView(image, desc, srv);
        Ref<ID3D12CommandAllocator> allocator;
        Ref<ID3D12GraphicsCommandList> list;
        EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p))));
        EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr,
                                                   IID_PPV_ARGS(&list.p))));
        const auto barrier = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
            const auto b = transition(resource, before, after);
            list->ResourceBarrier(1, &b);
        };
        // COMMON for the offscreen control and PRESENT for the swapchain are both zero.
        barrier(image, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        float color[4];
        for (UINT i = 0; i < 4; ++i) color[i] = float((clear >> (8 * i)) & 0xffu) / 255.0f;
        list->ClearRenderTargetView(rtv, color, 0, nullptr);
        barrier(image, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ID3D12DescriptorHeap* heaps[] = {srvs.p, samplers.p};
        list->SetDescriptorHeaps(2, heaps);
        list->SetComputeRootSignature(root.p);
        list->SetPipelineState(pso.p);
        const UINT constants[2] = {0, 0};
        list->SetComputeRoot32BitConstants(0, 2, constants, 0);
        list->SetComputeRootUnorderedAccessView(1, output_va);
        list->SetComputeRootDescriptorTable(2, srv_gpu);
        list->SetComputeRootDescriptorTable(3, sampler_gpu);
        list->Dispatch(1, 1, 1);
        barrier(image, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT);
        barrier(output.p, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback.p, 0, output.p, 0, bytes);
        barrier(output.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        EXPECT(SUCCEEDED(list->Close()));
        ID3D12CommandList* lists[] = {list.p};
        queue->ExecuteCommandLists(1, lists);
        EXPECT(rt::d3d12::last_execute_result(queue.p) == leonida::ReplayResult::success);
        EXPECT(SUCCEEDED(queue->Signal(fence.p, ++fence_value)));
        EXPECT(wait_fence(fence.p, fence_value));
        void* data = nullptr;
        const D3D12_RANGE read_range{0, bytes};
        EXPECT(SUCCEEDED(readback->Map(0, &read_range, &data)) && data);
        const auto* pixels = static_cast<const std::uint32_t*>(data);
        const auto matched = static_cast<unsigned>(std::count(pixels, pixels + words, expected));
        if (matched != words) {
            const auto* bad = std::find_if(pixels, pixels + words, [expected](std::uint32_t p) { return p != expected; });
            std::fprintf(stderr, "%s: %u/%u sampled texels matched; texel %td read %08x, wanted %08x\n",
                         label, matched, words, bad - pixels, *bad, expected);
            ++mismatches;
        } else {
            std::printf("%s: %u/%u sampled texels matched %08x\n", label, words, words, expected);
        }
        const D3D12_RANGE no_writes{0, 0};
        readback->Unmap(0, &no_writes);
        return 0;
    };

    // A regular render target uses the same sampler, pipeline, states and readback as the swapchain cases.
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = width;
    texture.Height = height;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 1;
    texture.Format = format;
    texture.SampleDesc.Count = 1;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    Ref<ID3D12Resource> control;
    EXPECT(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&control.p))));
    EXPECT(sample("regular texture", control.p, nullptr, 0xab8a6034) == 0);
    EXPECT(mismatches == 0);
    const auto sample_backbuffer = [&](const char* label, UINT expected_width, const D3D12_SHADER_RESOURCE_VIEW_DESC* d,
                                       std::uint32_t expected, Ref<ID3D12Resource>& backbuffer,
                                       std::uint32_t clear = 0) -> int {
        const UINT index = chain->GetCurrentBackBufferIndex();
        EXPECT(index < sd.BufferCount);
        EXPECT(SUCCEEDED(chain->GetBuffer(index, IID_PPV_ARGS(&backbuffer.p))) && backbuffer.p);
        const auto desc = backbuffer->GetDesc();
        EXPECT(desc.Width == expected_width && desc.Height == height && desc.Format == format);
        std::printf("%s uses back buffer %u\n", label, index);
        return sample(label, backbuffer.p, d, expected, clear);
    };
    {
        Ref<ID3D12Resource> first, second;
        EXPECT(chain->GetCurrentBackBufferIndex() == 0);
        EXPECT(sample_backbuffer("default SRV before Present", width, nullptr, 0xffbe6211, first) == 0);
        EXPECT(SUCCEEDED(chain->Present(0, 0)));
        EXPECT(chain->GetCurrentBackBufferIndex() == 1);
        EXPECT(sample_backbuffer("explicit SRV after Present", width, &view, 0x77294ed1, second) == 0);
        EXPECT(first.p != second.p);
    }

    // An _SRGB view of the UNORM back buffer decodes the cleared bytes to linear: e1 c6 80 -> c0 90 37, alpha as
    // stored (the D3D12 sRGB curve; each decoded value is within 0.05 of a whole step, so packUnorm4x8 is exact).
    D3D12_SHADER_RESOURCE_VIEW_DESC srgb_view = view;
    srgb_view.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    constexpr std::uint32_t srgb_clear = 0x7f80c6e1;
    constexpr std::uint32_t srgb_linear = 0x7f3790c0;
    {
        Ref<ID3D12Resource> third;
        EXPECT(SUCCEEDED(chain->Present(0, 0)));
        EXPECT(sample_backbuffer("sRGB SRV after Present", width, &srgb_view, srgb_linear, third, srgb_clear) == 0);
    }

    EXPECT(SUCCEEDED(chain->ResizeBuffers(0, width * 2, height, DXGI_FORMAT_UNKNOWN, 0)));
    EXPECT(chain->GetCurrentBackBufferIndex() == 0);
    Ref<ID3D12Resource> retained;
    EXPECT(sample_backbuffer("default SRV after ResizeBuffers", width * 2, nullptr, 0x93da234e, retained) == 0);
    chain.reset();
    chain1.reset();
    EXPECT(sample("retained back buffer after swapchain release", retained.p, &view, 0xe173bb36) == 0);
    EXPECT(sample("sRGB SRV of the resized retained back buffer", retained.p, &srgb_view, srgb_linear,
                  srgb_clear) == 0);
    EXPECT(mismatches == 0);
    return 0;
}

}  // namespace

int main() {
    if (!has_interactive_window_station()) {
        std::printf("SKIP: no interactive desktop\n");
        return 77;
    }
    vktest::Device vk;
    vk.request_buffer_device_address = true;
    const auto status = vk.initialize(true);
    if (status == vktest::DeviceStatus::unavailable) return 77;
    EXPECT(status == vktest::DeviceStatus::ready);
    if (!vk.bindless || !vk.buffer_device_address) {
        std::printf("SKIP: the sample shader needs descriptor indexing and buffer device address\n");
        return 77;
    }
    const int result = run(vk);
    EXPECT(vk.validation_errors() == 0);
    return result;
}
