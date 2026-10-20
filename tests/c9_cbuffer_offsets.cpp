// C9 W1 cbuffer-offset reflection.
//
// `fxc /O3` strips the light members from reflection once they fold into the
// compiled expressions (the struct collapses to one opaque $Globals variable),
// so the member offsets cannot be read from the optimized blob. This probe
// reflects the IDENTICAL A4Params declaration compiled with optimization
// disabled, which preserves the member table, and prints the byte offset and
// size of every field. The Clovis V2 2624-byte total is verified separately on the
// optimized blob by c9_shader_reflect.exe.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <d3dcommon.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Must stay byte-identical to a4_common.hlsli's A4Params field order.
//
// Reflect the fields as direct cbuffer members.  Passing A4Params as a pixel
// shader input is not valid without semantics and does not exercise constant
// buffer packing at all. The Clovis V2 tail is represented as its closed
// 144xfloat4
// binary contract; optimized production assembly is checked separately.
constexpr const char* kSource = R"(
cbuffer A4Constants : register(b0) {
 float4 viewport, outputSize, levels, hits, phase;
 float4 color0,color1,color2,color3;
 float4 material, rect, input0,input1,pointer,tuning;
 float4 lightLevel[2],lightTexture[2];
 float4 lightControl;
 float4 alloyV2[144];
};
float4 psMain(float4 pos : SV_Position) : SV_Target {
 return lightLevel[0]+lightLevel[1]+lightTexture[0]
      + lightTexture[1]+lightControl+alloyV2[0]+alloyV2[143]+viewport+color3;
}
)";

struct Expected { const char* name; UINT offset; UINT size; };

} // namespace

int wmain() {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    // /Od keeps the member table; the layout rules are identical to /O3.
    const HRESULT hr = D3DCompile(kSource, std::strlen(kSource), "a4_params_probe", nullptr,
                                  nullptr, "psMain", "ps_4_0", D3DCOMPILE_DEBUG, 0, &code, &errors);
    if (FAILED(hr) || !code) {
        std::printf("REFLECT-OFFSETS FAIL(compile): %s\n",
                    errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return 2;
    }

    ID3D11ShaderReflection* raw = nullptr;
    if (FAILED(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(),
                          __uuidof(ID3D11ShaderReflection),
                          reinterpret_cast<void**>(&raw))) || !raw) {
        std::printf("REFLECT-OFFSETS FAIL(D3DReflect)\n");
        return 2;
    }

    const Expected expected[] = {
        {"lightLevel",   240, 32},
        {"lightTexture", 272, 32},
        {"lightControl", 304, 16},
        {"alloyV2",      320, 2304},
    };

    D3D11_SHADER_DESC desc{};
    raw->GetDesc(&desc);
    int failures = 0;
    bool sawTotal = false;
    for (UINT b = 0; b < desc.ConstantBuffers; ++b) {
        ID3D11ShaderReflectionConstantBuffer* cb = raw->GetConstantBufferByIndex(b);
        D3D11_SHADER_BUFFER_DESC cbDesc{};
        if (FAILED(cb->GetDesc(&cbDesc))) continue;
        std::printf("cbuffer %s size=%u variables=%u\n", cbDesc.Name, cbDesc.Size,
                    cbDesc.Variables);
        if (cbDesc.Size == 2624) sawTotal = true;
        for (UINT v = 0; v < cbDesc.Variables; ++v) {
            ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
            D3D11_SHADER_VARIABLE_DESC varDesc{};
            if (FAILED(var->GetDesc(&varDesc))) continue;
            const char* name = varDesc.Name ? varDesc.Name : "?";
            bool interesting = false;
            for (const auto& e : expected) {
                if (std::strcmp(name, e.name) != 0) continue;
                interesting = true;
                const bool ok = varDesc.StartOffset == e.offset && varDesc.Size == e.size;
                if (!ok) ++failures;
                std::printf("  member %-13s offset=%u size=%u expected_offset=%u "
                            "expected_size=%u %s\n",
                            name, varDesc.StartOffset, varDesc.Size, e.offset, e.size,
                            ok ? "OK" : "MISMATCH");
            }
            if (!interesting) {
                std::printf("  member %-13s offset=%u size=%u\n", name, varDesc.StartOffset,
                            varDesc.Size);
            }
        }
    }
    raw->Release();
    code->Release();
    if (errors) errors->Release();

    if (!sawTotal) {
        std::printf("REFLECT-OFFSETS MISMATCH (no 2624-byte cbuffer)\n");
        return 1;
    }
    std::printf("REFLECT-OFFSETS %s\n", failures == 0 ? "OK" : "MISMATCH");
    return failures == 0 ? 0 : 1;
}
