// Read-only depth-stencil views in draws (RT-DSV-READ-ONLY-LAYOUT): a depth texture bound through a DSV made with
// D3D12_DSV_FLAG_READ_ONLY_DEPTH (and _STENCIL) is depth-tested against while a pixel shader samples the same texture
// through set 0, the resource in DEPTH_READ | PIXEL_SHADER_RESOURCE, as D3D12 allows.
//
// Each case clears the depth to 0.875 and the stencil to 0, writes depth 0.5 into the left half through a writable
// DSV, moves the depth plane to DEPTH_READ | PIXEL_SHADER_RESOURCE, and draws a full-screen triangle at 0.75 through
// the read-only DSV with a LESS test, depth writes on in the PSO and a stencil INCR_SAT on pass. The pixel shader
// writes the depth it samples under the pixel to an R32_FLOAT target. Then:
//   - the target holds 0.875, the sampled depth, in the right half, where the test passes, and its clear value in
//     the left, where it fails;
//   - the depth plane still holds 0.5 and 0.875: D3D12 writes no depth through a read-only DSV, whatever the PSO says;
//   - the stencil plane counts the passing pixels where only depth is read-only, and stays 0 where both are.
// The cases: D32_FLOAT with READ_ONLY_DEPTH, and D32_FLOAT_S8X24_UINT with READ_ONLY_DEPTH (its stencil plane left in
// DEPTH_WRITE) and with both flags. Each runs on a layer with separate depth/stencil layouts and on one without (not
// the one whose planes are in different states: see the SKIP below).
// Set LEONIDA_VK_VALIDATION=1 to run under VK_LAYER_KHRONOS_validation (0 errors required).
//
// The shaders are GLSL written to the translator's contract (test/glsl/vs_depth.vert, ps_read_only_dsv.frag). No game
// data.
#include "shader_fixture_path.h"
#include "test_temp_dir.h"
#include "vk_test_device.h"
#include "d3d12_test_shaders.h"
#include "../shims/d3d12_com.h"
#include "../shims/d3d12_xs_device.h"
#include "../src/shader_library.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if !defined(LEONIDA_TEST_VS_DEPTH_SPV) || !defined(LEONIDA_TEST_PS_READ_ONLY_DSV_SPV)
#error "the compiled test shaders must be named"
#endif

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

using namespace d3d12test;
using XboxHeapHandleFn = std::uint64_t(WINAPI*)(void* heap);  // heap slots 9/10: handle in RAX

namespace {

constexpr UINT side = 32, half = side / 2;
constexpr float cleared = 0.875f, written = 0.5f, drawn = 0.75f, target_clear = -1.0f;
int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

std::uint32_t f2u(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

struct Case {
    const char* name;
    DXGI_FORMAT format;      // the resource and DSV format
    DXGI_FORMAT srv_format;  // its depth plane as an SRV
    D3D12_DSV_FLAGS flags;
};

struct Harness {
    ID3D12Device* device = nullptr;
    ID3D12RootSignature* root = nullptr;
    std::vector<std::uint8_t> vs, ps;
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12Fence> fence;
    UINT64 fence_value = 0;

    // Runs one case and checks what it left; false only when the list could not be built or run.
    bool run(const Case& c, const std::string& label, std::string& error) {
        const bool stencil = c.format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        const bool stencil_read_only = (c.flags & D3D12_DSV_FLAG_READ_ONLY_STENCIL) != 0;
        const UINT planes = stencil ? 2 : 1;
        const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};

        D3D12_RESOURCE_DESC depth_desc{};
        depth_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depth_desc.Width = depth_desc.Height = side;
        depth_desc.DepthOrArraySize = 1;
        depth_desc.MipLevels = 1;
        depth_desc.Format = c.format;
        depth_desc.SampleDesc.Count = 1;
        depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_RESOURCE_DESC target_desc = depth_desc;
        target_desc.Format = DXGI_FORMAT_R32_FLOAT;
        target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        Ref<ID3D12Resource> depth, target, depth_readback, target_readback;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT depth_foot[2]{}, target_foot{};
        UINT64 depth_bytes = 0, target_bytes = 0;
        device->GetCopyableFootprints(&depth_desc, 0, planes, 0, depth_foot, nullptr, nullptr, &depth_bytes);
        device->GetCopyableFootprints(&target_desc, 0, 1, 0, &target_foot, nullptr, nullptr, &target_bytes);
        const D3D12_RESOURCE_DESC depth_readback_desc = buffer_desc(depth_bytes);
        const D3D12_RESOURCE_DESC target_readback_desc = buffer_desc(target_bytes);
        if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &depth_desc,
                                                   D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&depth.p))) ||
            FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                                   D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target.p))) ||
            FAILED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &depth_readback_desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&depth_readback.p))) ||
            FAILED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &target_readback_desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&target_readback.p)))) {
            error = "CreateCommittedResource failed";
            return false;
        }

        // Views: a writable DSV and the read-only one, the target's RTV, the depth plane's SRV and a POINT sampler.
        const D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        const D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        const D3D12_DESCRIPTOR_HEAP_DESC view_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        const D3D12_DESCRIPTOR_HEAP_DESC sampler_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1,
                                                           D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        Ref<ID3D12DescriptorHeap> dsv_heap, rtv_heap, view_heap, sampler_heap;
        if (FAILED(device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&dsv_heap.p))) ||
            FAILED(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap.p))) ||
            FAILED(device->CreateDescriptorHeap(&view_heap_desc, IID_PPV_ARGS(&view_heap.p))) ||
            FAILED(device->CreateDescriptorHeap(&sampler_heap_desc, IID_PPV_ARGS(&sampler_heap.p)))) {
            error = "CreateDescriptorHeap failed";
            return false;
        }
        const auto cpu = [](ID3D12DescriptorHeap* heap) {
            return std::uint64_t(reinterpret_cast<XboxHeapHandleFn>(slot(heap, 9))(heap));
        };
        const auto gpu = [](ID3D12DescriptorHeap* heap) {
            return std::uint64_t(reinterpret_cast<XboxHeapHandleFn>(slot(heap, 10))(heap));
        };
        const UINT dsv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        const D3D12_CPU_DESCRIPTOR_HANDLE writable_dsv{SIZE_T(cpu(dsv_heap.p))};
        const D3D12_CPU_DESCRIPTOR_HANDLE read_only_dsv{SIZE_T(cpu(dsv_heap.p) + dsv_step)};
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv{SIZE_T(cpu(rtv_heap.p))};
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = c.format;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(depth.p, &dsv, writable_dsv);
        dsv.Flags = c.flags;
        device->CreateDepthStencilView(depth.p, &dsv, read_only_dsv);
        device->CreateRenderTargetView(target.p, nullptr, rtv);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = c.srv_format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(depth.p, &srv, D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(cpu(view_heap.p))});
        D3D12_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        device->CreateSampler(&sampler, D3D12_CPU_DESCRIPTOR_HANDLE{SIZE_T(cpu(sampler_heap.p))});

        // The PSOs: the writing pre-pass has no pixel shader; the read-only draw writes depth and stencil in its
        // state, which the read-only DSV must mask.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root;
        desc.VS = {vs.data(), vs.size()};
        desc.SampleMask = 0xffffffff;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.DepthStencilState.DepthEnable = TRUE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.DSVFormat = c.format;
        desc.SampleDesc.Count = 1;
        Ref<ID3D12PipelineState> prepass, sampling;
        if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&prepass.p)))) {
            error = "CreateGraphicsPipelineState (pre-pass) failed";
            return false;
        }
        desc.PS = {ps.data(), ps.size()};
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        if (stencil) {
            desc.DepthStencilState.StencilEnable = TRUE;
            desc.DepthStencilState.StencilReadMask = 0xff;
            desc.DepthStencilState.StencilWriteMask = 0xff;
            const D3D12_DEPTH_STENCILOP_DESC count{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                                  D3D12_STENCIL_OP_INCR_SAT, D3D12_COMPARISON_FUNC_ALWAYS};
            desc.DepthStencilState.FrontFace = desc.DepthStencilState.BackFace = count;
        }
        if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&sampling.p)))) {
            error = "CreateGraphicsPipelineState (sampling) failed";
            return false;
        }

        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.p, prepass.p))) {
            error = "list reset failed";
            return false;
        }
        const auto clear_flags = D3D12_CLEAR_FLAGS(D3D12_CLEAR_FLAG_DEPTH | (stencil ? D3D12_CLEAR_FLAG_STENCIL : 0));
        list->ClearDepthStencilView(writable_dsv, clear_flags, cleared, 0, 0, nullptr);
        const float clear_color[4] = {target_clear, 0, 0, 0};
        list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
        list->SetGraphicsRootSignature(root);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VIEWPORT viewport{0, 0, float(side), float(side), 0, 1};
        list->RSSetViewports(1, &viewport);
        const D3D12_RECT left{0, 0, LONG(half), LONG(side)}, whole{0, 0, LONG(side), LONG(side)};
        list->RSSetScissorRects(1, &left);
        const std::uint32_t prepass_constants[3] = {f2u(written), 0, 0};
        list->SetGraphicsRoot32BitConstants(0, 3, prepass_constants, 0);
        list->OMSetRenderTargets(0, nullptr, FALSE, &writable_dsv);
        list->DrawInstanced(3, 1, 0, 0);

        // The depth plane becomes readable by the depth test and the pixel shader at once; a stencil plane the draw
        // writes stays in DEPTH_WRITE.
        constexpr auto read_state = D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_DEPTH_READ |
                                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        const bool split = stencil && !stencil_read_only;
        D3D12_RESOURCE_BARRIER to_read = transition(depth.p, D3D12_RESOURCE_STATE_DEPTH_WRITE, read_state);
        if (split) to_read.Transition.Subresource = 0;
        list->ResourceBarrier(1, &to_read);

        ID3D12DescriptorHeap* heaps[] = {view_heap.p, sampler_heap.p};
        list->SetDescriptorHeaps(2, heaps);
        list->SetPipelineState(sampling.p);
        list->SetGraphicsRootSignature(root);
        list->SetGraphicsRootDescriptorTable(1, D3D12_GPU_DESCRIPTOR_HANDLE{gpu(view_heap.p)});
        list->SetGraphicsRootDescriptorTable(2, D3D12_GPU_DESCRIPTOR_HANDLE{gpu(sampler_heap.p)});
        const std::uint32_t draw_constants[3] = {f2u(drawn), 0, 0};
        list->SetGraphicsRoot32BitConstants(0, 3, draw_constants, 0);
        list->RSSetScissorRects(1, &whole);
        list->OMSetRenderTargets(1, &rtv, FALSE, &read_only_dsv);
        list->DrawInstanced(3, 1, 0, 0);

        D3D12_RESOURCE_BARRIER to_copy[3] = {
            transition(depth.p, read_state, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(target.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(depth.p, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE)};
        if (split) {
            to_copy[0].Transition.Subresource = 0;
            to_copy[2].Transition.Subresource = 1;
        }
        list->ResourceBarrier(split ? 3 : 2, to_copy);
        for (UINT plane = 0; plane < planes; ++plane) {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = depth_readback.p;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = depth_foot[plane];
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = depth.p;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = plane;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = target_readback.p;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = target_foot;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = target.p;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        if (list->Close() != S_OK) {
            error = "Close failed";
            return false;
        }
        ID3D12CommandList* lists[] = {list.p};
        queue->ExecuteCommandLists(1, lists);
        if (rt::d3d12::last_execute_result(queue.p) != leonida::ReplayResult::success) {
            error = "the layer refused the list";
            return false;
        }
        if (FAILED(queue->Signal(fence.p, ++fence_value)) || FAILED(fence->SetEventOnCompletion(fence_value, nullptr))) {
            error = "fence wait failed";
            return false;
        }

        std::uint8_t* depth_bytes_ptr = nullptr;
        std::uint8_t* target_bytes_ptr = nullptr;
        if (FAILED(depth_readback->Map(0, nullptr, reinterpret_cast<void**>(&depth_bytes_ptr))) ||
            FAILED(target_readback->Map(0, nullptr, reinterpret_cast<void**>(&target_bytes_ptr)))) {
            error = "Map failed";
            return false;
        }
        int wrong_target = 0, wrong_depth = 0, wrong_stencil = 0;
        float first_target = 0, first_depth = 0;
        unsigned first_stencil = 0;
        for (UINT y = 0; y < side; ++y)
            for (UINT x = 0; x < side; ++x) {
                const bool passes = x >= half;
                float got_target, got_depth;
                std::memcpy(&got_target, target_bytes_ptr + target_foot.Offset + y * target_foot.Footprint.RowPitch + x * 4, 4);
                std::memcpy(&got_depth, depth_bytes_ptr + depth_foot[0].Offset + y * depth_foot[0].Footprint.RowPitch + x * 4, 4);
                if (got_target != (passes ? cleared : target_clear) && !wrong_target++) first_target = got_target;
                if (got_depth != (passes ? cleared : written) && !wrong_depth++) first_depth = got_depth;
                if (stencil) {
                    const unsigned got_stencil =
                        depth_bytes_ptr[depth_foot[1].Offset + y * depth_foot[1].Footprint.RowPitch + x];
                    const unsigned want_stencil = passes && !stencil_read_only ? 1u : 0u;
                    if (got_stencil != want_stencil && !wrong_stencil++) first_stencil = got_stencil;
                }
            }
        depth_readback->Unmap(0, nullptr);
        target_readback->Unmap(0, nullptr);
        char text[256];
        std::snprintf(text, sizeof(text), "%s, %s: sampled depth where the test passes, clear value where it fails "
                      "(%d of %u wrong, first %g)", c.name, label.c_str(), wrong_target, side * side, first_target);
        check(wrong_target == 0, text);
        std::snprintf(text, sizeof(text), "%s, %s: depth not written through the read-only DSV (%d of %u wrong, first %g)",
                      c.name, label.c_str(), wrong_depth, side * side, first_depth);
        check(wrong_depth == 0, text);
        if (stencil) {
            std::snprintf(text, sizeof(text), "%s, %s: stencil %s (%d of %u wrong, first %u)", c.name, label.c_str(),
                          stencil_read_only ? "not written through the read-only DSV" : "counts the passing pixels",
                          wrong_stencil, side * side, first_stencil);
            check(wrong_stencil == 0, text);
        }
        return true;
    }
};

}  // namespace

int main() {
    vktest::Device d;
    const auto device_status = d.initialize();
    if (device_status == vktest::DeviceStatus::unavailable) {
        std::fprintf(stderr, "SKIP: no usable Vulkan 1.3 device\n");
        return 77;
    }
    EXPECT(device_status == vktest::DeviceStatus::ready);
    if (!d.bindless) {
        std::printf("SKIP: the device lacks the descriptor indexing features of the bindless set 0\n");
        return 77;
    }

    const auto vs_program = fssm_program({0x56530411, 0xbf810000});
    const auto ps_program = fssm_program({0x50530411, 0xbf810000});
    const testfiles::ScratchDir scratch("leonida_read_only_dsv_test");
    const auto& library = scratch.path();
    EXPECT(testfiles::copy_file(vktest::shader_fixture_path(LEONIDA_TEST_VS_DEPTH_SPV), library / "vs_depth.spv"));
    EXPECT(testfiles::copy_file(vktest::shader_fixture_path(LEONIDA_TEST_PS_READ_ONLY_DSV_SPV),
                                library / "ps_read_only_dsv.spv"));
    {
        std::ofstream index(library / "index.json", std::ios::binary);
        index << "{\"vs_depth\": {\"stage\": \"vs\", \"fssm_sha256\": \"" << leonida::sha256_hex(vs_program)
              << "\", \"bindings\": [], \"push_constant_bytes\": 144, \"input_signature\": []},\n"
                 " \"ps_read_only_dsv\": {\"stage\": \"ps\", \"fssm_sha256\": \"" << leonida::sha256_hex(ps_program)
              << "\", \"bindings\": [0, 1, 2], \"push_constant_bytes\": 144, \"outputs\": [0]}}";
    }

    const Case cases[] = {
        {"D32_FLOAT, READ_ONLY_DEPTH", DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT, D3D12_DSV_FLAG_READ_ONLY_DEPTH},
        {"D32_FLOAT_S8X24_UINT, READ_ONLY_DEPTH", DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
         DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, D3D12_DSV_FLAG_READ_ONLY_DEPTH},
        {"D32_FLOAT_S8X24_UINT, READ_ONLY_DEPTH | READ_ONLY_STENCIL", DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
         DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,
         D3D12_DSV_FLAGS(D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL)},
    };
    for (const bool separate : {true, false}) {
        const std::string label = separate ? "separate depth/stencil layouts" : "combined depth/stencil layouts";
        rt::d3d12::VulkanDeviceInfo info{};
        info.get_instance_proc = d.instance_proc;
        info.instance = d.instance;
        info.physical = d.physical;
        info.device = d.device;
        info.queue = d.queue;
        info.queue_family = d.family;
        info.buffer_device_address = true;
        info.bindless = d.bindless;
        info.bindless_partially_bound = d.partially_bound;
        info.image_view_min_lod = d.image_view_min_lod;
        info.subgroup = d.subgroup;
        info.features = d.enabled;
        info.separate_depth_stencil_layouts = separate;
        info.shader_dir = library.string();
        ID3D12Device* inner = nullptr;
        EXPECT(SUCCEEDED(rt::d3d12::create_vulkan_device(info, IID_PPV_ARGS(&inner))));
        Ref<ID3D12Device> device;
        EXPECT(SUCCEEDED(rt::d3d12_xs::create_device_proxy(inner, __uuidof(ID3D12Device),
                                                           reinterpret_cast<void**>(&device.p),
                                                           /*console_lists=*/false)));
        rt::d3d12::set_device_outer(inner, device.p);
        if (rt::d3d12::device_push_constants_need_fallback(inner)) {
            std::printf("SKIP: the PSOs need 144 push-constant bytes and this GPU has %u\n",
                        rt::d3d12::device_max_push_constants_size(inner));
            return 77;
        }

        // Root signature, in the console graphics layout from s8: [0] three constants (s8, s9: vs_depth.vert's depth
        // and slope; s10: the view slot's offset in dwords), [1] the table of views (s11), [2] the table of
        // samplers (s12).
        D3D12_DESCRIPTOR_RANGE view_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_DESCRIPTOR_RANGE sampler_range{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, 3};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &view_range};
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = {1, &sampler_range};
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        root_desc.Desc_1_0 = {3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        const auto root_blob = serialize_root_signature(root_desc);
        EXPECT(!root_blob.empty());
        Ref<ID3D12RootSignature> root;
        EXPECT(SUCCEEDED(device->CreateRootSignature(0, root_blob.data(), root_blob.size(), IID_PPV_ARGS(&root.p))));

        Harness h;
        h.device = device.p;
        h.root = root.p;
        h.vs = shader_container(vs_program);
        h.ps = shader_container(ps_program);
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        EXPECT(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&h.queue.p))));
        EXPECT(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&h.allocator.p))));
        EXPECT(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, h.allocator.p, nullptr,
                                                   IID_PPV_ARGS(&h.list.p))));
        EXPECT(SUCCEEDED(h.list->Close()));
        EXPECT(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&h.fence.p))));
        for (const Case& c : cases) {
            if (!separate && c.format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT &&
                !(c.flags & D3D12_DSV_FLAG_READ_ONLY_STENCIL)) {
                // The planes are in different states here (depth read, stencil written), and without separate
                // layouts a barrier on one plane moves both with only its own plane's source scope, so the
                // synchronization layer sees the stencil written by the draw race the depth plane's barrier. Not a
                // read-only DSV matter; the layer the runtime makes always has separate layouts.
                std::printf("SKIP %s, %s: per-plane states need separate layouts\n", c.name, label.c_str());
                continue;
            }
            std::string error;
            EXPECT(h.run(c, label, error) || (std::fprintf(stderr, "%s, %s: %s\n", c.name, label.c_str(),
                                                           error.c_str()), false));
        }
    }
    if (d.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", d.validation_errors());
        ++g_failures;
    }
    std::printf("%d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
