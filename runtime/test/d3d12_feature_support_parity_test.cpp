// CheckFeatureSupport answers the Vulkan-backed device gives differently from Windows D3D12 (the default adapter, or
// WARP with PROBE_WARP=1), compared row by row:
//  - SHADER_MODEL asked with values past the highest defined model: the HRESULT and the model written back;
//  - FORMAT_SUPPORT's CAST_WITHIN_BIT_LAYOUT bit, on every format both answer;
//  - MULTISAMPLE_QUALITY_LEVELS with the tiled-resource flag on the stencil-less depth formats (controls).
// No game data.
#include "host_wine.h"
#include "../shims/d3d12_com.h"
#include <dxgi1_4.h>

#include <cstdio>

namespace {
template<class T>
struct Ref {
    T* p = nullptr;
    ~Ref() {
        if (p) p->Release();
    }
};

const char* g_host = "";
unsigned g_rows = 0;
unsigned g_differ = 0;

void report(const char* what, const char* name, HRESULT my_hr, UINT mine, HRESULT host_hr, UINT theirs) {
    const bool same = my_hr == host_hr && mine == theirs;
    ++g_rows;
    if (!same) ++g_differ;
    std::printf("%-18s %-26s layer 0x%08lx 0x%08x, %s 0x%08lx 0x%08x%s\n", what, name,
                static_cast<unsigned long>(my_hr), mine, g_host, static_cast<unsigned long>(host_hr), theirs,
                same ? "" : "  DIFFERS");
}

HRESULT shader_model(ID3D12Device* device, UINT requested, UINT& answer) {
    D3D12_FEATURE_DATA_SHADER_MODEL data{};
    data.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(requested);
    const HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &data, sizeof(data));
    answer = SUCCEEDED(hr) ? static_cast<UINT>(data.HighestShaderModel) : 0;
    return hr;
}

HRESULT format_support(ID3D12Device* device, DXGI_FORMAT format, UINT& support1) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT data{};
    data.Format = format;
    const HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &data, sizeof(data));
    support1 = SUCCEEDED(hr) ? static_cast<UINT>(data.Support1) : 0;
    return hr;
}

HRESULT tiled_quality(ID3D12Device* device, DXGI_FORMAT format, UINT samples, UINT& levels) {
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS data{};
    data.Format = format;
    data.SampleCount = samples;
    data.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_TILED_RESOURCE;
    const HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &data, sizeof(data));
    levels = SUCCEEDED(hr) ? data.NumQualityLevels : 0;
    return hr;
}
}  // namespace

int main() {
    Ref<ID3D12Device> ours;
    if (FAILED(rt::d3d12::create_vulkan_device_standalone(IID_PPV_ARGS(&ours.p))) || !ours.p) {
        std::printf("SKIP: no Vulkan device\n");
        return 77;
    }
    if (hostwine::host_is_wine()) {
        std::printf("SKIP: the host comparison is skipped under Wine\n");
        return 77;
    }
    char env[4] = {};
    const bool warp = GetEnvironmentVariableA("PROBE_WARP", env, sizeof(env)) == 1 && env[0] == '1';
    Ref<IDXGIFactory4> factory;
    Ref<IDXGIAdapter> adapter;
    Ref<ID3D12Device> host;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory.p))) ||
        (warp && FAILED(factory.p->EnumWarpAdapter(IID_PPV_ARGS(&adapter.p)))) ||
        FAILED(D3D12CreateDevice(adapter.p, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&host.p)))) {
        std::printf("SKIP: no host D3D12 device\n");
        return 77;
    }
    g_host = warp ? "WARP" : "host";

    // Values below 5.1 are left out: 0x5f crashed the host runtime (see the SHADER_MODEL case in the layer).
    const UINT models[] = {0x51, 0x60, 0x65, 0x69, 0x6a, 0x6f, 0x70, 0x71, 0x7f, 0x80, 0x99, 0xa0, 0xff, 0x100};
    for (UINT requested : models) {
        UINT mine = 0, theirs = 0;
        const HRESULT my_hr = shader_model(ours.p, requested, mine);
        const HRESULT host_hr = shader_model(host.p, requested, theirs);
        char name[16];
        std::snprintf(name, sizeof(name), "0x%x", requested);
        // The model written back is each device's own highest (the layer's is lower): compare it only where the
        // request is at or below the layer's, so the clamp itself is not counted as a difference.
        if (SUCCEEDED(my_hr) && SUCCEEDED(host_hr) && mine < requested) theirs = mine;
        report("SHADER_MODEL", name, my_hr, mine, host_hr, theirs);
    }

    // The cast bit on every format both devices answer; a refusal on either side is another question.
    for (UINT f = 1; f <= 191; ++f) {
        const DXGI_FORMAT format = static_cast<DXGI_FORMAT>(f);
        UINT mine = 0, theirs = 0;
        const HRESULT my_hr = format_support(ours.p, format, mine);
        const HRESULT host_hr = format_support(host.p, format, theirs);
        if (FAILED(my_hr) || FAILED(host_hr)) continue;
        mine &= D3D12_FORMAT_SUPPORT1_CAST_WITHIN_BIT_LAYOUT;
        theirs &= D3D12_FORMAT_SUPPORT1_CAST_WITHIN_BIT_LAYOUT;
        char name[16];
        std::snprintf(name, sizeof(name), "format %u", f);
        report("CAST_WITHIN_BIT", name, my_hr, mine, host_hr, theirs);
    }

    // Controls: tiled MSAA quality of the stencil-less depth formats, which the layer already answers as the host.
    // (The stencil-bearing ones and R8G8_B8G8/G8R8_G8B8 are #4533's.)
    const struct {
        const char* name;
        DXGI_FORMAT format;
    } depth[] = {{"D32_FLOAT", DXGI_FORMAT_D32_FLOAT}, {"D16_UNORM", DXGI_FORMAT_D16_UNORM}};
    for (const auto& row : depth) {
        for (UINT samples : {1u, 4u}) {
            UINT mine = 0, theirs = 0;
            const HRESULT my_hr = tiled_quality(ours.p, row.format, samples, mine);
            const HRESULT host_hr = tiled_quality(host.p, row.format, samples, theirs);
            char name[40];
            std::snprintf(name, sizeof(name), "%s x%u", row.name, samples);
            report("TILED_QUALITY", name, my_hr, mine, host_hr, theirs);
        }
    }

    std::printf("feature support parity against %s: %u rows compared, %u differ\n", g_host, g_rows, g_differ);
    return g_differ ? 1 : 0;
}
