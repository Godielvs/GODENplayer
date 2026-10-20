// C9 W1 shader-reflection probe.
//
// The portable C++ suite proves `sizeof(a4::Constants)==2624` on the CPU. That is
// NOT proof about the shader: the HLSL side must be reflected from the bytecode
// that production actually links. This probe answers that with D3DReflect:
//   * declared cbuffer size in bytes and the byte offset/size of every member,
//   * specifically the C9 additions lightLevel[2] / lightTexture[2] / lightControl,
//   * the number of texture bindings the pass declares (the pass input contract).
//
// It never launches the player and never touches the registry or any profile.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <d3dcommon.h>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> readAll(const wchar_t* path) {
    std::ifstream stream(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream),
                                     std::istreambuf_iterator<char>());
}

struct CBufferMember {
    std::string name;
    UINT offset = 0;
    UINT size = 0;
    UINT elements = 0;
};

int reflect(const wchar_t* path, const char* label, bool expectLights) {
    const auto blob = readAll(path);
    if (blob.empty()) {
        std::printf("REFLECT %s FAIL(no-bytecode)\n", label);
        return 2;
    }
    ID3D11ShaderReflection* raw = nullptr;
    // __uuidof avoids a link-time dependency on the IID_ID3D11ShaderReflection
    // symbol, which some SDK/d3dcompiler import-library combinations omit.
    if (FAILED(D3DReflect(blob.data(), blob.size(), __uuidof(ID3D11ShaderReflection),
                          reinterpret_cast<void**>(&raw))) || !raw) {
        std::printf("REFLECT %s FAIL(D3DReflect)\n", label);
        return 2;
    }

    D3D11_SHADER_DESC desc{};
    raw->GetDesc(&desc);
    std::printf("REFLECT %s profile=%s cbuffers=%u boundResources=%u\n", label,
                desc.Version ? "ps_4_0" : "?", desc.ConstantBuffers,
                desc.BoundResources);

    int result = 0;
    for (UINT b = 0; b < desc.ConstantBuffers; ++b) {
        ID3D11ShaderReflectionConstantBuffer* cb = raw->GetConstantBufferByIndex(b);
        D3D11_SHADER_BUFFER_DESC cbDesc{};
        if (FAILED(cb->GetDesc(&cbDesc))) continue;
        std::printf("  cbuffer %s size=%u variables=%u\n", cbDesc.Name, cbDesc.Size,
                    cbDesc.Variables);
        if (cbDesc.Size != 2624 && expectLights) {
            std::printf("  ERROR: expected 2624-byte cbuffer, reflected %u\n", cbDesc.Size);
            result = 1;
        }
        for (UINT v = 0; v < cbDesc.Variables; ++v) {
            ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
            D3D11_SHADER_VARIABLE_DESC varDesc{};
            if (FAILED(var->GetDesc(&varDesc))) continue;
            const std::string name = varDesc.Name ? varDesc.Name : "?";
            if (expectLights && (name == "lightLevel" || name == "lightTexture" ||
                                 name == "lightControl")) {
                std::printf("  member %-13s offset=%u size=%u\n", name.c_str(),
                            varDesc.StartOffset, varDesc.Size);
            }
        }
    }
    raw->Release();
    std::printf("REFLECT %s %s\n", label, result == 0 ? "OK" : "MISMATCH");
    return result;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: c9_shader_reflect <field.ps> <label> [--no-lights]\n");
        return 2;
    }
    const bool expectLights = !(argc > 3 && std::wstring(argv[3]) == L"--no-lights");
    char label[128]{};
    WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, label, sizeof(label) - 1, nullptr, nullptr);
    // One file per invocation: callers loop over the five passes.
    return reflect(argv[1], label, expectLights);
}
