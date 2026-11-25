#include "native/vk_shader.h"

#include "host/log.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>

#ifdef CT3_ENABLE_XBE_SDK
#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include "core/log.h"
#endif

namespace fs = std::filesystem;

namespace ct2::vk {

namespace {

constexpr uint32_t kPreloadTagShader = 1;
constexpr uint32_t kPreloadTagPso = 2;

uint64_t Fnv1a64(const void* data, size_t len, uint64_t seed = 14695981039346656037ull) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> SplitArgs(const std::string& s) {
    std::vector<std::string> out;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')') --depth;
        else if (s[i] == ',' && depth == 0) {
            out.push_back(Trim(s.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (start < s.size()) {
        std::string tail = Trim(s.substr(start));
        if (!tail.empty()) out.push_back(std::move(tail));
    }
    return out;
}

bool IsOpaqueParamType(const std::string& decl) {
    return decl.find("Texture") != std::string::npos ||
           decl.find("Sampler") != std::string::npos;
}

std::string ExtractParamName(const std::string& decl) {
    const std::string t = Trim(decl);
    const size_t sp = t.find_last_of(" \t");
    return (sp == std::string::npos) ? t : Trim(t.substr(sp + 1));
}

std::string ReplaceIdentifier(const std::string& src, const std::string& ident,
                              const std::string& replacement) {
    if (ident.empty()) return src;
    const std::regex re("\\b" + ident + "\\b");
    return std::regex_replace(src, re, replacement);
}

} // namespace

size_t PsoKeyHash::operator()(const PsoKey& k) const {
    return static_cast<size_t>(Fnv1a64(&k, sizeof(k)));
}

ShaderCache::ShaderCache(std::string cache_dir) : cache_dir_(std::move(cache_dir)) {
    std::error_code ec;
    fs::create_directories(cache_dir_, ec);
}

uint64_t ShaderCache::HashSource(const std::string& text) {
    return Fnv1a64(text.data(), text.size());
}

std::string ShaderCache::Specialize(const std::string& hlsl_source) {
    static const std::regex kFuncHeader(
        R"((float[1-4]?|half[1-4]?|int[1-4]?|uint[1-4]?|void)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*(?:Texture|Sampler)[^)]*)\)\s*\{)");

    std::string work = hlsl_source;
    std::ostringstream specialized_defs;
    int spec_counter = 0;

    std::smatch m;
    while (std::regex_search(work, m, kFuncHeader)) {
        const std::string ret_type = m[1].str();
        const std::string func_name = m[2].str();
        const std::string params_raw = m[3].str();
        const size_t fn_start = static_cast<size_t>(m.position(0));
        const size_t body_open = fn_start + m.length(0) - 1;

        int depth = 1;
        size_t pos = body_open + 1;
        while (pos < work.size() && depth > 0) {
            if (work[pos] == '{') ++depth;
            else if (work[pos] == '}') --depth;
            ++pos;
        }
        if (depth != 0) break;

        const std::string body = work.substr(body_open + 1, pos - body_open - 2);
        const auto params = SplitArgs(params_raw);

        work.erase(fn_start, pos - fn_start);

        const std::regex call_re("\\b" + func_name + R"(\s*\(([^;{}]*?)\))");
        std::string rewritten;
        std::sregex_iterator it(work.begin(), work.end(), call_re);
        std::sregex_iterator end;
        size_t last = 0;
        for (; it != end; ++it) {
            const auto& cm = *it;
            rewritten.append(work, last, static_cast<size_t>(cm.position(0)) - last);
            const auto args = SplitArgs(cm[1].str());
            if (args.size() == params.size()) {
                const std::string spec_name =
                    func_name + "_spec_" + std::to_string(spec_counter++);
                std::string spec_body = body;
                std::vector<std::string> non_opaque_params;
                std::vector<std::string> non_opaque_args;

                for (size_t i = 0; i < params.size(); ++i) {
                    if (IsOpaqueParamType(params[i])) {
                        spec_body = ReplaceIdentifier(spec_body, ExtractParamName(params[i]),
                                                      args[i]);
                    } else {
                        non_opaque_params.push_back(params[i]);
                        non_opaque_args.push_back(args[i]);
                    }
                }

                specialized_defs << ret_type << " " << spec_name << "(";
                for (size_t i = 0; i < non_opaque_params.size(); ++i) {
                    if (i) specialized_defs << ", ";
                    specialized_defs << non_opaque_params[i];
                }
                specialized_defs << ") {" << spec_body << "}\n";

                rewritten += spec_name + "(";
                for (size_t i = 0; i < non_opaque_args.size(); ++i) {
                    if (i) rewritten += ", ";
                    rewritten += non_opaque_args[i];
                }
                rewritten += ")";
            } else {
                rewritten += cm[0].str();
            }
            last = static_cast<size_t>(cm.position(0) + cm.length(0));
        }
        rewritten.append(work, last, std::string::npos);
        work = std::move(rewritten);
    }

    const std::string defs = specialized_defs.str();
    if (defs.empty()) return hlsl_source;

    const size_t entry_pos = work.find("main(");
    if (entry_pos != std::string::npos) {
        const size_t line_start = work.rfind('\n', entry_pos);
        const size_t insert_at = (line_start == std::string::npos) ? 0 : (line_start + 1);
        work.insert(insert_at, "\n// Specialized Mali-safe helpers (VUID-06924)\n" + defs + "\n");
    } else {
        work += "\n// Specialized Mali-safe helpers (VUID-06924)\n" + defs;
    }
    return work;
}

std::string ShaderCache::SpirvPath(uint64_t source_hash) const {
    char name[64];
    std::snprintf(name, sizeof(name), "%016llx_%u.spv",
                  static_cast<unsigned long long>(source_hash), kShaderCacheVersion);
    return (fs::path(cache_dir_) / name).string();
}

std::string ShaderCache::PreloadPath() const {
    return (fs::path(cache_dir_) / "preload.bin").string();
}

bool ShaderCache::LoadSpirv(uint64_t source_hash, std::vector<uint32_t>& out_words) const {
    std::ifstream in(SpirvPath(source_hash), std::ios::binary | std::ios::ate);
    if (!in) return false;
    const auto sz = in.tellg();
    if (sz <= 0 || (static_cast<size_t>(sz) % sizeof(uint32_t)) != 0) return false;
    out_words.resize(static_cast<size_t>(sz) / sizeof(uint32_t));
    in.seekg(0, std::ios::beg);
    return static_cast<bool>(in.read(reinterpret_cast<char*>(out_words.data()), sz));
}

bool ShaderCache::SaveSpirvAtomic(uint64_t source_hash,
                                  const std::vector<uint32_t>& words) const {
    if (words.empty()) return false;
    std::error_code ec;
    fs::create_directories(cache_dir_, ec);

    const std::string final_path = SpirvPath(source_hash);
    const std::string tmp_path = final_path + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(words.data()),
                  static_cast<std::streamsize>(words.size() * sizeof(uint32_t)));
        if (!out.good()) return false;
    }
    fs::rename(tmp_path, final_path, ec);
    return !ec;
}

void ShaderCache::RecordShaderForPreload(uint64_t shader_key, uint64_t source_hash) {
    std::lock_guard<std::mutex> lock(preload_mu_);
    if (!recorded_shaders_.insert(shader_key).second) return;

    std::ofstream out(PreloadPath(), std::ios::binary | std::ios::app);
    if (!out) return;
    const uint32_t tag = kPreloadTagShader;
    PreloadShaderRec rec{shader_key, source_hash};
    out.write(reinterpret_cast<const char*>(&tag), sizeof(tag));
    out.write(reinterpret_cast<const char*>(&rec), sizeof(rec));
}

void ShaderCache::RecordPsoForPreload(const PsoKey& key) {
    std::lock_guard<std::mutex> lock(preload_mu_);
    if (!recorded_psos_.insert(key).second) return;

    std::ofstream out(PreloadPath(), std::ios::binary | std::ios::app);
    if (!out) return;
    const uint32_t tag = kPreloadTagPso;
    out.write(reinterpret_cast<const char*>(&tag), sizeof(tag));
    out.write(reinterpret_cast<const char*>(&key), sizeof(key));
}

PreloadManifest ShaderCache::LoadPreloadManifest() const {
    PreloadManifest manifest;
    std::ifstream in(PreloadPath(), std::ios::binary);
    if (!in) return manifest;

    uint32_t tag = 0;
    while (in.read(reinterpret_cast<char*>(&tag), sizeof(tag))) {
        if (tag == kPreloadTagShader) {
            PreloadShaderRec rec{};
            if (!in.read(reinterpret_cast<char*>(&rec), sizeof(rec))) break;
            manifest.shaders.push_back(rec);
        } else if (tag == kPreloadTagPso) {
            PsoKey key{};
            if (!in.read(reinterpret_cast<char*>(&key), sizeof(key))) break;
            manifest.psos.push_back(key);
        } else {
            break;
        }
    }
    return manifest;
}

bool ShaderCache::ClaimPsoBuild(const PsoKey& key) {
    std::lock_guard<std::mutex> lock(pre_lock_);
    return pre_claimed_.insert(key).second;
}

} // namespace ct2::vk

#ifdef CT3_ENABLE_XBE_SDK
namespace vkshader {

namespace {

fs::path g_dir;
std::mutex g_lock;
constexpr uint32_t kCacheVersion = ct2::vk::kShaderCacheVersion;

// Pre-specialize opaque Texture*/Sampler* helper args for ARM Mali (VUID-06924),
// rewrite fxc 1.#INF, and bind ATTRn inputs to Vulkan [[vk::location(n)]].
std::string Adapt(const std::string& hlsl) {
    std::string s = ct2::vk::ShaderCache::Specialize(hlsl);
    for (size_t at; (at = s.find("1.#INF")) != std::string::npos;) {
        s.replace(at, 6, "asfloat(0x7F800000u)");
    }
    static const std::regex attr(R"((\n[ \t]*)(\w+ v\d+ : ATTR(\d+);))");
    return std::regex_replace(s, attr, "$1[[vk::location($3)]] $2");
}

fs::path CachePath(uint64_t hash) {
    char name[40];
    std::snprintf(name, sizeof(name), "%016llx_%u.spv",
                  static_cast<unsigned long long>(hash), kCacheVersion);
    return g_dir / name;
}

bool Load(uint64_t hash, std::vector<uint32_t>& spirv) {
    if (g_dir.empty()) return false;
    std::ifstream f(CachePath(hash), std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize n = f.tellg();
    if (n <= 0 || n % 4) return false;
    spirv.resize(size_t(n) / 4);
    f.seekg(0);
    return bool(f.read(reinterpret_cast<char*>(spirv.data()), n));
}

void Store(uint64_t hash, const std::vector<uint32_t>& spirv) {
    if (g_dir.empty()) return;
    fs::path p = CachePath(hash), tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return;
        f.write(reinterpret_cast<const char*>(spirv.data()), std::streamsize(spirv.size() * 4));
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
}

} // namespace

void Init(const fs::path& cache_dir) {
    std::lock_guard<std::mutex> lock(g_lock);
    glslang::InitializeProcess();
    std::error_code ec;
    fs::create_directories(cache_dir, ec);
    g_dir = ec ? fs::path() : cache_dir;
}

bool Cached(uint64_t hash, std::vector<uint32_t>& spirv) {
    return Load(hash, spirv);
}

bool Compile(const std::string& hlsl, const char* entry, Stage stage, uint64_t hash, std::vector<uint32_t>& spirv) {
    if (Load(hash, spirv)) return true;
    std::lock_guard<std::mutex> lock(g_lock);
    if (Load(hash, spirv)) return true;

    EShLanguage lang = stage == Stage::Vertex ? EShLangVertex : EShLangFragment;
    std::string text = Adapt(hlsl);
    const char* src = text.c_str();
    glslang::TShader sh(lang);
    sh.setStrings(&src, 1);
    sh.setEntryPoint("main");
    sh.setSourceEntryPoint(entry);
    sh.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    sh.setHlslIoMapping(true);
    sh.setAutoMapBindings(true);
    sh.setAutoMapLocations(true);
    sh.setShiftBinding(glslang::EResUbo, kBindingConstants);
    sh.setShiftBinding(glslang::EResTexture, kBindingTextures);
    sh.setShiftBinding(glslang::EResSampler, kBindingSamplers);

    auto messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl | EShMsgHlslOffsets);
    if (!sh.parse(GetDefaultResources(), 100, false, messages)) {
        LOGE("vulkan: %s %s does not compile: %s", stage == Stage::Vertex ? "vertex" : "pixel", entry, sh.getInfoLog());
        LOGE("%s", text.c_str());
        return false;
    }
    glslang::TProgram program;
    program.addShader(&sh);
    if (!program.link(messages) || !program.mapIO()) {
        LOGE("vulkan: %s does not link: %s", entry, program.getInfoLog());
        return false;
    }
    glslang::SpvOptions options;
    options.disableOptimizer = true;
    options.validate = false;
    spirv.clear();
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &options);
    if (spirv.empty()) return false;
    Store(hash, spirv);
    return true;
}

} // namespace vkshader
#endif // CT3_ENABLE_XBE_SDK
