#include "probe.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace {

// ---------- utilidades de archivo (UTF-8, sin depender de C++/CX) ----------
std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

bool ReadBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? (size_t)size : 0);
    size_t got = out.empty() ? 0 : fread(out.data(), 1, out.size(), f);
    fclose(f);
    return got == out.size() && !out.empty();
}

std::wstring ReadText(const std::wstring& path) {
    std::vector<uint8_t> b;
    if (!ReadBytes(path, b)) return {};
    return FromUtf8(std::string(b.begin(), b.end()));
}

// Se abre y se cierra en cada escritura: si el proceso muere después, el dato ya quedó en disco.
void WriteText(const std::wstring& path, const std::wstring& text, bool append) {
    FILE* f = _wfopen(path.c_str(), append ? L"ab" : L"wb");
    if (!f) return;
    std::string s = ToUtf8(text);
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
}

std::wstring Trim(std::wstring s) {
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == L' ' || s[i] == L'\r' || s[i] == L'\n')) ++i;
    return s.substr(i);
}

std::wstring Hex(uint32_t v) {
    wchar_t buf[16];
    swprintf(buf, 16, L"0x%08X", v);
    return buf;
}

std::wstring Sm(uint32_t v) { // 0x51 -> "5.1"
    wchar_t buf[16];
    swprintf(buf, 16, L"%u.%u", (v >> 4) & 0xF, v & 0xF);
    return buf;
}

// ---------- estado de las pruebas ----------
enum class St { Ok, Fail, Crash, Missing };

struct Runner {
    const ProbeEnv& env;
    std::vector<Line> out;
    std::set<std::wstring> crashed;
    std::map<std::wstring, St> status;

    explicit Runner(const ProbeEnv& e) : env(e) {}

    std::wstring P(const wchar_t* name) const { return env.state_dir + L"\\" + name; }

    void Say(int level, const std::wstring& t) {
        out.push_back({t, level});
        WriteText(P(L"probe_log.txt"), t + L"\r\n", true);
    }

    // Marca el paso en disco ANTES de ejecutarlo. Devuelve false si ya mató al proceso antes.
    bool Enter(const wchar_t* id) {
        if (crashed.count(id)) {
            status[id] = St::Crash;
            Say(LV_BAD, std::wstring(L"[CRASH] ") + id + L": el proceso murió aquí en una ejecución anterior (omitido)");
            return false;
        }
        WriteText(P(L"probe_progress.txt"), id, false);
        return true;
    }
};

bool LoadShader(const ProbeEnv& env, const wchar_t* file, std::vector<uint8_t>& bytes) {
    return ReadBytes(env.assets_dir + L"\\" + file, bytes);
}

HRESULT MakeEmptyRootSig(ID3D12Device* dev, ComPtr<ID3D12RootSignature>& rs) {
    D3D12_ROOT_SIGNATURE_DESC d{};
    d.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) return hr;
    return dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs));
}

void Report(Runner& r, const wchar_t* id, const wchar_t* title, HRESULT hr) {
    if (SUCCEEDED(hr)) {
        r.status[id] = St::Ok;
        r.Say(LV_OK, std::wstring(L"[OK]    ") + title);
    } else {
        r.status[id] = St::Fail;
        r.Say(LV_BAD, std::wstring(L"[FALLA] ") + title + L"  HRESULT=" + Hex((uint32_t)hr));
    }
}

void TestCompute(Runner& r, ID3D12Device* dev, ID3D12RootSignature* rs, const wchar_t* id,
                 const wchar_t* title, const wchar_t* file) {
    if (!r.Enter(id)) return;
    std::vector<uint8_t> cs;
    if (!LoadShader(r.env, file, cs)) {
        r.status[id] = St::Missing;
        r.Say(LV_WARN, std::wstring(L"[?] ") + title + L": falta " + file + L" en el paquete");
        return;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
    d.pRootSignature = rs;
    d.CS.pShaderBytecode = cs.data();
    d.CS.BytecodeLength = cs.size();
    ComPtr<ID3D12PipelineState> pso;
    Report(r, id, title, dev->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso)));
}

void TestGraphics(Runner& r, ID3D12Device* dev, ID3D12RootSignature* rs, const wchar_t* id,
                  const wchar_t* title, const wchar_t* vs_file, const wchar_t* ps_file) {
    if (!r.Enter(id)) return;
    std::vector<uint8_t> vs, ps;
    if (!LoadShader(r.env, vs_file, vs) || !LoadShader(r.env, ps_file, ps)) {
        r.status[id] = St::Missing;
        r.Say(LV_WARN, std::wstring(L"[?] ") + title + L": faltan archivos .cso en el paquete");
        return;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = rs;
    d.VS.pShaderBytecode = vs.data();
    d.VS.BytecodeLength = vs.size();
    d.PS.pShaderBytecode = ps.data();
    d.PS.BytecodeLength = ps.size();
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = 0xFFFFFFFFu;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.DepthStencilState.DepthEnable = FALSE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    Report(r, id, title, dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)));
}


// ---------- compilación de shaders en la propia consola (D3DCompile) ----------
typedef HRESULT(WINAPI* PFN_D3DCOMPILE_)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                         LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

const char kCsSrc[] = "[numthreads(1, 1, 1)] void main() {}";
const char kGfxSrc[] =
    "float4 VSMain(uint id : SV_VertexID) : SV_Position {"
    "  float2 p = float2((id << 1) & 2, id & 2); return float4(p * 2.0 - 1.0, 0.0, 1.0); }"
    "float4 PSMain() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }";

HMODULE LoadCompilerDll() {
#if defined(WINAPI_FAMILY) && (WINAPI_FAMILY == WINAPI_FAMILY_APP)
    return LoadPackagedLibrary(L"d3dcompiler_47.dll", 0); // UWP: solo carga DLLs del paquete
#else
    return LoadLibraryW(L"d3dcompiler_47.dll");
#endif
}

bool CompileHlsl(PFN_D3DCOMPILE_ fn, const char* src, size_t len, const char* entry, const char* target,
                 ComPtr<ID3DBlob>& out, std::wstring& msg) {
    ComPtr<ID3DBlob> err;
    HRESULT hr = fn(src, len, "probe.hlsl", nullptr, nullptr, entry, target, 0, 0, &out, &err);
    if (SUCCEEDED(hr) && out) return true;
    msg = Hex((uint32_t)hr);
    if (err && err->GetBufferSize() > 0) {
        size_t n = err->GetBufferSize() < 140 ? err->GetBufferSize() : 140;
        std::wstring t = FromUtf8(std::string((const char*)err->GetBufferPointer(), n));
        for (auto& c : t) if (c == L'\r' || c == L'\n') c = L' ';
        msg += L" " + t;
    }
    return false;
}

void ComputeFromBytes(Runner& r, ID3D12Device* dev, ID3D12RootSignature* rs, const wchar_t* id,
                      const wchar_t* title, const void* data, size_t size) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
    d.pRootSignature = rs;
    d.CS.pShaderBytecode = data;
    d.CS.BytecodeLength = size;
    ComPtr<ID3D12PipelineState> pso;
    Report(r, id, title, dev->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso)));
}

void GraphicsFromBytes(Runner& r, ID3D12Device* dev, ID3D12RootSignature* rs, const wchar_t* id,
                       const wchar_t* title, const void* vs, size_t vs_size, const void* ps, size_t ps_size) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = rs;
    d.VS.pShaderBytecode = vs;
    d.VS.BytecodeLength = vs_size;
    d.PS.pShaderBytecode = ps;
    d.PS.BytecodeLength = ps_size;
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = 0xFFFFFFFFu;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.DepthStencilState.DepthEnable = FALSE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    Report(r, id, title, dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)));
}

} // namespace

std::vector<Line> RunProbes(const ProbeEnv& env) {
    Runner r(env);

    // ¿La ejecución anterior murió dentro de un paso? Entonces ese paso se omite ahora.
    std::wstring prev = Trim(ReadText(r.P(L"probe_progress.txt")));
    {
        std::wstringstream ss(ReadText(r.P(L"probe_crashed.txt")));
        std::wstring tok;
        while (ss >> tok) r.crashed.insert(tok);
    }
    if (!prev.empty() && prev != L"DONE" && !r.crashed.count(prev)) {
        r.crashed.insert(prev);
        WriteText(r.P(L"probe_crashed.txt"), prev + L"\r\n", true);
    }
    WriteText(r.P(L"probe_progress.txt"), L"START", false);
    WriteText(r.P(L"probe_log.txt"), L"\r\n==== nueva ejecución ====\r\n", true);

    r.Say(LV_TITLE, L"DxilProbe: ¿acepta esta consola shaders DXIL (SM 6.0)?");
    if (!r.crashed.empty()) {
        std::wstring list;
        for (auto& c : r.crashed) list += c + L" ";
        r.Say(LV_WARN, L"Pasos que ya mataron el proceso antes: " + list);
    }

    // ---- 1. Adaptador ----
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (!r.Enter(L"dxgi")) return r.out;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        r.Say(LV_BAD, L"CreateDXGIFactory1 falló");
        return r.out;
    }
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (!adapter && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            adapter = a;
            r.Say(LV_INFO, std::wstring(L"Adaptador: ") + d.Description);
        }
    }
    if (!adapter) {
        r.Say(LV_BAD, L"No hay adaptador por hardware");
        return r.out;
    }

    // ---- 2. Dispositivo D3D12 (feature level mínimo 11_0, como Mesa) ----
    ComPtr<ID3D12Device> dev;
    if (!r.Enter(L"device")) {
        r.Say(LV_BAD, L"El proceso murió creando el dispositivo D3D12");
        return r.out;
    }
    HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
    if (FAILED(hr)) {
        r.Say(LV_BAD, L"D3D12CreateDevice falló: " + Hex((uint32_t)hr));
        return r.out;
    }

    // ---- 3. Capacidades básicas ----
    if (r.Enter(L"caps")) {
        static const D3D_FEATURE_LEVEL lv[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                               D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D12_FEATURE_DATA_FEATURE_LEVELS fl{};
        fl.NumFeatureLevels = 4;
        fl.pFeatureLevelsRequested = lv;
        if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl)))) {
            r.Say(LV_INFO, L"Feature level máximo: " + Hex((uint32_t)fl.MaxSupportedFeatureLevel) +
                               L"  (0xC000 = 12_0)");
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
        if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o)))) {
            r.Say(LV_INFO, L"Resource binding tier: " + std::to_wstring((int)o.ResourceBindingTier));
        }
    }

    // ---- 4. Shader model: se reproduce el bucle de Mesa (d3d12_screen.cpp) ----
    if (r.Enter(L"shader_model")) {
        static const uint32_t req[] = {0x68, 0x67, 0x66, 0x65, 0x64, 0x63, 0x62, 0x61, 0x60, 0x51};
        std::wstring row;
        bool first = true;
        uint32_t mesa_value = 0;
        for (uint32_t q : req) {
            D3D12_FEATURE_DATA_SHADER_MODEL d{};
            d.HighestShaderModel = (D3D_SHADER_MODEL)q;
            HRESULT h = dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &d, sizeof(d));
            if (SUCCEEDED(h)) {
                row += Sm(q) + L"->" + Sm((uint32_t)d.HighestShaderModel) + L"  ";
                if (first && q != 0x51) {
                    first = false;
                    uint32_t v = (uint32_t)d.HighestShaderModel;
                    mesa_value = ((v & 0xf0) << 12) | (v & 0xf);
                }
            } else {
                row += Sm(q) + L":error " + Hex((uint32_t)h) + L"  ";
            }
        }
        r.Say(LV_INFO, L"Shader model (pedido->devuelto): " + row);
        if (first) {
            r.Say(LV_WARN, L"Mesa no obtendría ningún shader model (max_shader_model sin asignar)");
        } else {
            bool ge60 = mesa_value >= 0x60000;
            r.Say(ge60 ? LV_OK : LV_WARN,
                  L"max_shader_model de Mesa = " + Hex(mesa_value) +
                      (ge60 ? L"  (>= 6.0)" : L"  (< 6.0; Mesa no rechaza el dispositivo igualmente)"));
        }
    }

    // ---- 5. Root signature vacía (necesaria para los PSO de prueba) ----
    ComPtr<ID3D12RootSignature> rs;
    if (!r.Enter(L"rootsig")) return r.out;
    hr = MakeEmptyRootSig(dev.Get(), rs);
    if (FAILED(hr)) {
        r.Say(LV_BAD, L"No se pudo crear la root signature: " + Hex((uint32_t)hr));
        return r.out;
    }

    // ---- 6. PSO: primero los de control (DXBC 5.1), después DXIL 6.0 ----
    r.Say(LV_INFO, L"Creación de pipelines (el DXIL es la prueba de riesgo):");
    TestCompute(r, dev.Get(), rs.Get(), L"pso_cs_51", L"Compute  DXBC SM 5.1 (control)", L"cs_5_1.cso");
    TestGraphics(r, dev.Get(), rs.Get(), L"pso_gfx_51", L"Graphics DXBC SM 5.1 (control)", L"vs_5_1.cso", L"ps_5_1.cso");
    TestCompute(r, dev.Get(), rs.Get(), L"pso_cs_60", L"Compute  DXIL SM 6.0", L"cs_6_0.cso");
    TestGraphics(r, dev.Get(), rs.Get(), L"pso_gfx_60", L"Graphics DXIL SM 6.0 (lo que usa Mesa)", L"vs_6_0.cso", L"ps_6_0.cso");

    // ---- 6b. Feature level de D3D11 (lo que usaría ANGLE) ----
    if (r.Enter(L"d3d11_caps")) {
        static const D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                D3D_FEATURE_LEVEL_10_1};
        ComPtr<ID3D11Device> d3d11;
        D3D_FEATURE_LEVEL got{};
        HRESULT h = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, fls, 3,
                                      D3D11_SDK_VERSION, &d3d11, &got, nullptr);
        if (SUCCEEDED(h)) {
            r.Say(LV_INFO, L"D3D11 feature level: " + Hex((uint32_t)got) + L"  (0xB100 = 11_1, 0xB000 = 11_0)");
        } else {
            r.Say(LV_WARN, L"D3D11CreateDevice falló: " + Hex((uint32_t)h));
        }
    }

    // ---- 6c. Compilar HLSL dentro de la consola con D3DCompile ----
    r.Say(LV_INFO, L"Compilación de shaders EN la consola (D3DCompile, SM 5.1):");
    PFN_D3DCOMPILE_ compile_fn = nullptr;
    if (r.Enter(L"dll_load")) {
        HMODULE mod = LoadCompilerDll();
        DWORD le = mod ? 0 : GetLastError();
        if (!mod) {
            r.status[L"dll_load"] = St::Fail;
            r.Say(LV_BAD, L"[FALLA] No carga d3dcompiler_47.dll  (error de Windows " + std::to_wstring((unsigned)le) + L")");
        } else {
            compile_fn = reinterpret_cast<PFN_D3DCOMPILE_>(reinterpret_cast<void*>(GetProcAddress(mod, "D3DCompile")));
            if (!compile_fn) {
                r.status[L"dll_load"] = St::Fail;
                r.Say(LV_BAD, L"[FALLA] d3dcompiler_47.dll cargó pero no exporta D3DCompile");
            } else {
                r.status[L"dll_load"] = St::Ok;
                r.Say(LV_OK, L"[OK]    d3dcompiler_47.dll carga en la consola");
            }
        }
    }
    if (compile_fn && r.Enter(L"compile")) {
        ComPtr<ID3DBlob> cs, vs, ps;
        std::wstring m1, m2, m3;
        bool a = CompileHlsl(compile_fn, kCsSrc, sizeof(kCsSrc) - 1, "main", "cs_5_1", cs, m1);
        bool b = CompileHlsl(compile_fn, kGfxSrc, sizeof(kGfxSrc) - 1, "VSMain", "vs_5_1", vs, m2);
        bool c = CompileHlsl(compile_fn, kGfxSrc, sizeof(kGfxSrc) - 1, "PSMain", "ps_5_1", ps, m3);
        if (a && b && c) {
            r.status[L"compile"] = St::Ok;
            r.Say(LV_OK, L"[OK]    D3DCompile compila cs/vs/ps (SM 5.1) en tiempo de ejecución");
            if (r.Enter(L"pso_rt_cs"))
                ComputeFromBytes(r, dev.Get(), rs.Get(), L"pso_rt_cs", L"Compute  compilado en la consola",
                                 cs->GetBufferPointer(), cs->GetBufferSize());
            if (r.Enter(L"pso_rt_gfx"))
                GraphicsFromBytes(r, dev.Get(), rs.Get(), L"pso_rt_gfx", L"Graphics compilado en la consola",
                                  vs->GetBufferPointer(), vs->GetBufferSize(), ps->GetBufferPointer(),
                                  ps->GetBufferSize());
        } else {
            r.status[L"compile"] = St::Fail;
            r.Say(LV_BAD, L"[FALLA] D3DCompile: cs=" + (a ? std::wstring(L"ok") : m1) + L" vs=" +
                              (b ? std::wstring(L"ok") : m2) + L" ps=" + (c ? std::wstring(L"ok") : m3));
        }
    }

    // ---- 7. Veredicto ----
    auto ok = [&](const wchar_t* id) { return r.status.count(id) && r.status[id] == St::Ok; };
    auto bad = [&](const wchar_t* id) {
        return r.status.count(id) && (r.status[id] == St::Fail || r.status[id] == St::Crash);
    };
    bool control = ok(L"pso_cs_51") && ok(L"pso_gfx_51");
    bool dxil_ok = ok(L"pso_cs_60") && ok(L"pso_gfx_60");
    bool dxil_bad = bad(L"pso_cs_60") || bad(L"pso_gfx_60");

    r.Say(LV_TITLE, L"VEREDICTO");
    if (!control) {
        r.Say(LV_WARN, L"Ni el DXBC 5.1 de control funciona del todo: el problema es más general (mira [FALLA] / [?]).");
    } else if (dxil_ok) {
        r.Say(LV_OK, L"DXIL 6.0 SÍ funciona aquí: el crash de Azahar viene de otra parte, no del formato de shader.");
    } else if (dxil_bad) {
        r.Say(LV_BAD, L"DXBC 5.1 funciona y DXIL 6.0 NO: Mesa (que genera DXIL) no puede dibujar en esta consola.");
        r.Say(LV_BAD, L"Un backend propio tendría que usar DXBC (SM 5.1).");
    } else {
        r.Say(LV_WARN, L"Resultado incompleto: faltan archivos o pasos omitidos.");
    }
    bool rt_ok = ok(L"dll_load") && ok(L"compile") && ok(L"pso_rt_cs") && ok(L"pso_rt_gfx");
    if (rt_ok) {
        r.Say(LV_OK, L"Compilar HLSL en la consola SÍ funciona: backend DXBC propio o ANGLE->D3D11 son viables.");
    } else {
        r.Say(LV_BAD, L"Compilar HLSL en la consola NO funciona: haría falta precompilar shaders (inviable para 3DS).");
    }
    r.Say(LV_INFO, L"Log completo: LocalState\\probe_log.txt (Device Portal > File explorer)");
    if (!r.crashed.empty()) {
        r.Say(LV_INFO, L"Si la app se cerró sola antes, es normal: ábrela de nuevo para ver el resultado.");
    }

    WriteText(r.P(L"probe_progress.txt"), L"DONE", false);
    return r.out;
}
