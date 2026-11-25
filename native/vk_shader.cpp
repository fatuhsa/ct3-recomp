#include "native/vk_shader.h"

#include "host/log.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

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
    // Detect HLSL helper functions with Texture*/Sampler* parameters, e.g.:
    //   float4 SampleTex(Texture2D tex, SamplerState smp, float2 uv) { ... }
    // Rewrite each call site to a specialized helper that binds global texture/sampler
    // identifiers directly and place specialized helpers after globals so glslang
    // (with ENABLE_OPT=OFF) never emits VUID-06924 opaque-variable stores on ARM Mali.
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

        // Find matching closing brace for the helper body.
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

        // Remove original generic helper definition.
        work.erase(fn_start, pos - fn_start);

        // Replace each call site func_name(arg0, arg1, ...) with a specialized variant.
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

    // Insert specialized helpers just before main/entry function so globals are already declared.
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
