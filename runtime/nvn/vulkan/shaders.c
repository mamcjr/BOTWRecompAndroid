// GLSL -> SPIR-V (shaderc), descriptor layouts from the SPIR-V itself, translated NVN programs, built-in shaders.
#include <pthread.h>
#include <shaderc/shaderc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "shaders.h"
#include "shader_cache.h"

void vk_shaders_init(void) {  // (compilers are per thread: check shaderc works at all)
    shaderc_compiler_t c = shaderc_compiler_initialize();
    if (!c) FATAL("shaderc_compiler_initialize failed\n");
    shaderc_compiler_release(c);
}

static shaderc_shader_kind kind_of(int stage) {
    return stage == STAGE_VERTEX ? shaderc_vertex_shader : stage == STAGE_FRAGMENT ? shaderc_fragment_shader : shaderc_compute_shader;
}

// SPIR-V disk cache: cache/spirv/<hash of stage + source + compile mode>.spv, so a known shader costs a file read instead of a
// GLSL compile (the driver's pipeline cache only covers the step after this one).
#ifndef __ANDROID__  // (Android: shader_cache.h vk_shader_source_hash, which also keys LARECOMP_VK_OPT)
static uint64_t src_hash(const char *src, int stage) {
    uint64_t h = (0xcbf29ce484222325ull ^ (uint64_t)stage) + 3;  // + version of the post-processing (no_contraction)
    for (const char *p = src; *p; p++) { h ^= (uint8_t)*p; h *= 0x100000001B3ull; }
    return h;
}
#endif

static _Atomic uint64_t spirv_misses;  // (hle/benchmark.c: shaders not in the disk cache so far)
uint64_t vk_spirv_misses(void) { return spirv_misses; }

static uint32_t *cache_load(uint64_t h, size_t *nwords) {
    char path[64];
    snprintf(path, sizeof path, "cache/spirv/%016llx.spv", (unsigned long long)h);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint32_t *d = n > 20 && n <= 16 * 1024 * 1024 && n % 4 == 0 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    if (d && !vk_shader_cache_framed(d, (size_t)n / 4)) {
        LOG("[shader-cache] ignoring malformed entry %016llx; recompiling\n", (unsigned long long)h);
        free(d); d = NULL;
    }
    if (d) *nwords = (size_t)n / 4;
    return d;
}

static pthread_once_t cache_dirs_once = PTHREAD_ONCE_INIT;
static void cache_dirs_init(void) { mkdir("cache", 0755); mkdir("cache/spirv", 0755); }
static void cache_store(uint64_t h, const uint32_t *spv, size_t nwords) {
    pthread_once(&cache_dirs_once, cache_dirs_init);
    char path[64], tmp[80];
    snprintf(path, sizeof path, "cache/spirv/%016llx.spv", (unsigned long long)h);
    snprintf(tmp, sizeof tmp, "%s.%llx", path, (unsigned long long)(uintptr_t)&cache_dirs_once ^ (unsigned long long)(uintptr_t)spv);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(spv, 4, nwords, f) == nwords;
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(tmp, path) != 0) remove(tmp);  // (another thread may have stored it first)
}

// Vertex shaders: every float add/sub/mul/div/fma gets NoContraction, so no compiler fuses or splits them. Depth
// pre-pass and colour pass are different shaders computing the same position, and the colour pass tests EQUAL:
// any difference in fusing leaves holes (foliage flickered black). GLSL's `precise gl_Position` only covers what
// glslang traces back from the position, which the translated register code defeats.
static bool float_arith(const uint32_t *w, uint32_t wc, uint32_t op) {
    switch (op) {
    case 127: case 129: case 131: case 133: case 136:  // FNegate FAdd FSub FMul FDiv
    case 142: case 143: case 144: case 145: case 146: case 148:  // vector/matrix products, Dot
        return true;
    case 12: return wc >= 5 && w[4] == 50;  // ExtInst GLSL.std.450 Fma
    default: return false;
    }
}

static uint32_t *no_contraction(uint32_t *spv, size_t *nwords) {
    enum { OP_DECORATE = 71, DEC_NO_CONTRACTION = 42 };
    size_t n = *nwords, insert = 0, count = 0;
    uint32_t bound = spv[3];
    uint8_t *done = calloc(bound, 1);  // already decorated (glslang does it for what it traces from precise)
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (!insert && op >= 19 && op <= 39) insert = i;  // first type declaration: end of the annotations
        if (op == OP_DECORATE && wc >= 3 && spv[i + 2] == DEC_NO_CONTRACTION && spv[i + 1] < bound) done[spv[i + 1]] = 1;
        i += wc;
    }
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) count++;
        i += wc;
    }
    if (!insert || !count) { free(done); return spv; }
    uint32_t *out = malloc((n + count * 3) * 4);
    memcpy(out, spv, insert * 4);
    size_t o = insert;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) {
            done[spv[i + 2]] = 1;
            out[o++] = 3u << 16 | OP_DECORATE;
            out[o++] = spv[i + 2];  // result id
            out[o++] = DEC_NO_CONTRACTION;
        }
        i += wc;
    }
    memcpy(out + o, spv + insert, (n - insert) * 4);
    *nwords = o + (n - insert);
    free(done);
    free(spv);
    return out;
}

// SPIR-V words of src, or NULL (error logged with the source). *nwords set on success. Thread-safe: one shaderc
// compiler per thread (a shared one behind a lock serialised all the pipeline workers).
// No spirv-opt by default: it rewrote the depth pre-pass and colour-pass vertex shaders differently enough that
// their depths stopped matching (EQUAL test: black holes in foliage), and it was most of the compile time; the
// driver optimises anyway. LARECOMP_VK_OPT=1 turns it back on (experiments).
// Debug names (OpSource, OpSourceExtension, OpName, OpMemberName, OpString, OpLine, OpNoLine, OpModuleProcessed):
// glslang writes one for every register variable, ~16 % of a translated shader's SPIR-V, read by no driver
// (SavageWindRecomp's).
static void strip_names(uint32_t *spv, size_t *nwords) {
    size_t o = 5;
    for (size_t i = 5; i < *nwords;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc || i + wc > *nwords) return;  // (malformed: left as it is from here)
        bool name = op == 3 || op == 4 || op == 5 || op == 6 || op == 7 || op == 8 || op == 317 || op == 330;
        if (!name) { memmove(spv + o, spv + i, wc * 4); o += wc; }
        i += wc;
    }
    *nwords = o;
}

uint32_t *vk_glsl_to_spirv(const char *src, int stage, const char *name, size_t *nwords) {
    bool optimized = getenv("LARECOMP_VK_OPT") != NULL;
#ifdef __ANDROID__
    uint64_t h = vk_shader_source_hash(src, stage, optimized);
    uint32_t api = g_vk.api ? g_vk.api : VK_API_VERSION_1_3;  // (0: no device yet - as 1.3)
    if (api < VK_API_VERSION_1_3) h ^= (uint64_t)api << 32;  // (older SPIR-V for a device below 1.3, below)
#else
    uint64_t h = src_hash(src, stage);
#endif
    bool nocache = getenv("LARECOMP_VK_NO_SPIRV_CACHE") != NULL;
    uint32_t *out = nocache ? NULL : cache_load(h, nwords);
    if (!out) spirv_misses++;
    if (out) return out;
    static __thread shaderc_compiler_t comp;
    if (!comp) comp = shaderc_compiler_initialize();
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
#ifdef __ANDROID__  // (SPIR-V 1.6 for 1.3; 1.5 / 1.3 for a device below it, device.c COMPAT_EXT)
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan,
                                           api >= VK_API_VERSION_1_3 ? shaderc_env_version_vulkan_1_3
                                           : api >= VK_API_VERSION_1_2 ? shaderc_env_version_vulkan_1_2
                                                                            : shaderc_env_version_vulkan_1_1);
#else
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
#endif
    shaderc_compile_options_set_optimization_level(o, optimized ? shaderc_optimization_level_performance
                                                                                  : shaderc_optimization_level_zero);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(comp, src, strlen(src), kind_of(stage), name, "main", o);
    shaderc_compile_options_release(o);
    if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {
        static int nlog;
        if (nlog++ < 20) LOG("[shader] %s: GLSL compile failed:\n%s\n%s\n", name, shaderc_result_get_error_message(r), src);
    } else {
        size_t n = shaderc_result_get_length(r);
        out = malloc(n);
        memcpy(out, shaderc_result_get_bytes(r), n);
        *nwords = n / 4;
        strip_names(out, nwords);
        if (stage == STAGE_VERTEX) out = no_contraction(out, nwords);
        if (!nocache) cache_store(h, out, *nwords);
    }
    shaderc_result_release(r);
    return out;
}

VkShaderModule vk_module(const uint32_t *spv, size_t nwords) {
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nwords * 4, .pCode = spv};
    VkShaderModule m;
    VkResult result = vkCreateShaderModule(g_vk.dev, &ci, NULL, &m);
    if (result != VK_SUCCESS) LOG("[shader] vkCreateShaderModule failed: %d (%zu words)\n", result, nwords);
    return result == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

VkShaderModule vk_compile_glsl(const char *src, int stage, const char *name) {
    size_t n;
    uint32_t *spv = vk_glsl_to_spirv(src, stage, name, &n);
    if (!spv) return VK_NULL_HANDLE;
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    return m;
}

// ================================================================== reflection
// Just enough SPIR-V parsing to list the descriptor bindings a module declares (set, binding, type).
enum { OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLER = 26, OP_TYPE_SAMPLED_IMAGE = 27, OP_TYPE_ARRAY = 28, OP_TYPE_RUNTIME_ARRAY = 29,
       OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_VARIABLE = 59, OP_DECORATE = 71 };
enum { DEC_BLOCK = 2, DEC_BUFFER_BLOCK = 3, DEC_BINDING = 33, DEC_SET = 34 };
enum { SC_UNIFORM_CONSTANT = 0, SC_UNIFORM = 2, SC_STORAGE_BUFFER = 12 };

int vk_reflect(const uint32_t *spv, size_t n, VkBindingInfo *out, int max) {
    if (n < 5 || spv[0] != 0x07230203) return 0;
    uint32_t bound = spv[3];
    int32_t *binding = malloc(bound * sizeof *binding), *set = malloc(bound * sizeof *set);
    uint8_t *flags = calloc(bound, 1);    // 1 Block, 2 BufferBlock
    uint32_t *op = calloc(bound, sizeof *op), *ref = calloc(bound, sizeof *ref), *sc = calloc(bound, sizeof *sc);
    for (uint32_t i = 0; i < bound; i++) binding[i] = set[i] = -1;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, o = spv[i] & 0xFFFF;
        if (!wc) break;
        const uint32_t *w = spv + i;
        if (o == OP_DECORATE && wc >= 3 && w[1] < bound) {
            if (w[2] == DEC_BINDING && wc >= 4) binding[w[1]] = (int32_t)w[3];
            else if (w[2] == DEC_SET && wc >= 4) set[w[1]] = (int32_t)w[3];
            else if (w[2] == DEC_BLOCK) flags[w[1]] |= 1;
            else if (w[2] == DEC_BUFFER_BLOCK) flags[w[1]] |= 2;
        } else if ((o == OP_TYPE_IMAGE || o == OP_TYPE_SAMPLER || o == OP_TYPE_SAMPLED_IMAGE || o == OP_TYPE_STRUCT) && wc >= 2 && w[1] < bound) {
            op[w[1]] = o;
        } else if ((o == OP_TYPE_ARRAY || o == OP_TYPE_RUNTIME_ARRAY) && wc >= 3 && w[1] < bound) {
            op[w[1]] = o;
            ref[w[1]] = w[2];
        } else if (o == OP_TYPE_POINTER && wc >= 4 && w[1] < bound) {
            op[w[1]] = o;
            sc[w[1]] = w[2];
            ref[w[1]] = w[3];
        } else if (o == OP_VARIABLE && wc >= 4 && w[2] < bound && w[1] < bound) {
            op[w[2]] = o;
            ref[w[2]] = w[1];  // pointer type
            sc[w[2]] = w[3];
        }
        i += wc;
    }
    int cnt = 0;
    for (uint32_t id = 0; id < bound && cnt < max; id++) {
        if (op[id] != OP_VARIABLE || binding[id] < 0) continue;
        uint32_t t = ref[ref[id]];  // pointee
        while (t < bound && (op[t] == OP_TYPE_ARRAY || op[t] == OP_TYPE_RUNTIME_ARRAY)) t = ref[t];
        VkDescriptorType dt;
        if (t >= bound) continue;
        if (op[t] == OP_TYPE_SAMPLED_IMAGE) dt = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        else if (op[t] == OP_TYPE_IMAGE) dt = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        else if (op[t] == OP_TYPE_SAMPLER) dt = VK_DESCRIPTOR_TYPE_SAMPLER;
        else if (op[t] == OP_TYPE_STRUCT && (sc[id] == SC_STORAGE_BUFFER || (flags[t] & 2))) dt = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else if (op[t] == OP_TYPE_STRUCT && sc[id] == SC_UNIFORM) dt = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else continue;
        out[cnt++] = (VkBindingInfo){(uint32_t)(set[id] < 0 ? 0 : set[id]), (uint32_t)binding[id], dt};
    }
    free(binding); free(set); free(flags); free(op); free(ref); free(sc);
    return cnt;
}

VkDescriptorSetLayout vk_set_layout(const VkBindingInfo *b, int n, uint32_t set, VkShaderStageFlags stages) {
    VkDescriptorSetLayoutBinding lb[96];
    uint32_t k = 0;
    for (int i = 0; i < n && k < 96; i++)
        if (b[i].set == set) lb[k++] = (VkDescriptorSetLayoutBinding){b[i].binding, b[i].type, 1, stages, NULL};
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = k, .pBindings = lb};
    VkDescriptorSetLayout l;
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

// ================================================================== built-in shaders
static const char *builtin_src(const char *name, int *stage) {
    if (!strcmp(name, "present.vert")) {
        *stage = STAGE_VERTEX;
        // pv[0].x: flip (lower-left origin), pv[1]: uv crop (origin, size). Fullscreen triangle; Vulkan NDC y points down.
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 a; vec4 crop; } pc;\n"
               "layout(location = 0) out vec2 uv;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "  vec2 u = vec2(p.x, pc.a.x > 0.5 ? 1.0 - p.y : p.y);\n"
               "  uv = pc.crop.xy + u * pc.crop.zw;\n"
               "}\n";
    }
    if (!strcmp(name, "present.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(location = 0) in vec2 uv;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() { o = vec4(texture(t, uv).rgb, 1.0); }\n";
    }
    if (!strcmp(name, "present_a.frag")) {  // (debug pass dumps: alpha kept)
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(location = 0) in vec2 uv;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() { o = texture(t, uv); }\n";
    }
    // ---- post passes before the present (Video tab "Anti-aliasing" / "Upscaler"): exec.c vk_present_draw.
    // Each draws a fullscreen triangle into an image the size of its output; push constants: two vec4.
    if (!strcmp(name, "post.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "}\n";
    }
    // FXAA (Video tab "Anti-aliasing"), one pass in the manner of FXAA 3.11's quality path (Timothy Lottes, NVIDIA):
    // the luma contrast around the pixel picks it; the edge's direction is the stronger of the row / column second
    // differences; the edge is walked both ways until its luma leaves the edge's mean, and the pixel is moved across it
    // by how near it is to the nearer end (or by the 3x3 neighbourhood's sub-pixel blend, whichever is more). a = (1 /
    // size, size).
    if (!strcmp(name, "fxaa.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "float lu(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
               "vec3 at(vec2 uv) { return textureLod(t0, uv, 0.0).rgb; }\n"
               "float la(vec2 uv) { return lu(at(uv)); }\n"
               "const float steps[10] = float[](1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);\n"
               "void main() {\n"
               "  vec2 px = pc.a.xy, uv = gl_FragCoord.xy * px;\n"
               "  vec3 cM = at(uv);\n"
               "  float lM = lu(cM), lN = la(uv + vec2(0.0, -px.y)), lS = la(uv + vec2(0.0, px.y));\n"
               "  float lE = la(uv + vec2(px.x, 0.0)), lW = la(uv + vec2(-px.x, 0.0));\n"
               "  float mx = max(lM, max(max(lN, lS), max(lE, lW))), mn = min(lM, min(min(lN, lS), min(lE, lW)));\n"
               "  float range = mx - mn;\n"
               "  if (range < max(0.0312, mx * 0.125)) { o = vec4(cM, 1.0); return; }\n"
               "  float lNW = la(uv - px), lSE = la(uv + px);\n"
               "  float lNE = la(uv + vec2(px.x, -px.y)), lSW = la(uv + vec2(-px.x, px.y));\n"
               "  float sub = (2.0 * (lN + lS + lE + lW) + lNW + lNE + lSW + lSE) / 12.0;\n"
               "  float subBlend = smoothstep(0.0, 1.0, clamp(abs(sub - lM) / range, 0.0, 1.0));\n"
               "  subBlend = subBlend * subBlend * 0.75;\n"
               "  float hor = 2.0 * abs(lN + lS - 2.0 * lM) + abs(lNE + lSE - 2.0 * lE) + abs(lNW + lSW - 2.0 * lW);\n"
               "  float ver = 2.0 * abs(lE + lW - 2.0 * lM) + abs(lNE + lNW - 2.0 * lN) + abs(lSE + lSW - 2.0 * lS);\n"
               "  bool isH = hor >= ver;\n"
               "  float pL = isH ? lS : lE, nL = isH ? lN : lW;\n"
               "  float pG = abs(pL - lM), nG = abs(nL - lM);\n"
               "  float stepLen = isH ? px.y : px.x, opp = pL, grad = pG;\n"
               "  if (pG < nG) { stepLen = -stepLen; opp = nL; grad = nG; }\n"
               "  vec2 uvE = uv + (isH ? vec2(0.0, stepLen * 0.5) : vec2(stepLen * 0.5, 0.0));\n"
               "  vec2 es = isH ? vec2(px.x, 0.0) : vec2(0.0, px.y);\n"
               "  float edgeL = (lM + opp) * 0.5, thr = grad * 0.25;\n"
               "  vec2 puv = uvE + es, nuv = uvE - es;\n"
               "  float pD = la(puv) - edgeL, nD = la(nuv) - edgeL;\n"
               "  bool pEnd = abs(pD) >= thr, nEnd = abs(nD) >= thr;\n"
               "  for (int i = 1; i < 10 && !(pEnd && nEnd); i++) {\n"
               "    if (!pEnd) { puv += es * steps[i]; pD = la(puv) - edgeL; pEnd = abs(pD) >= thr; }\n"
               "    if (!nEnd) { nuv -= es * steps[i]; nD = la(nuv) - edgeL; nEnd = abs(nD) >= thr; }\n"
               "  }\n"
               "  float pDist = isH ? puv.x - uv.x : puv.y - uv.y, nDist = isH ? uv.x - nuv.x : uv.y - nuv.y;\n"
               "  float shortest = min(pDist, nDist);\n"
               "  bool sgn = pDist <= nDist ? pD >= 0.0 : nD >= 0.0;\n"
               "  float edgeBlend = sgn == (lM - edgeL >= 0.0) ? 0.0 : 0.5 - shortest / (pDist + nDist);\n"
               "  float f = max(subBlend, edgeBlend);\n"
               "  o = vec4(at(uv + (isH ? vec2(0.0, stepLen * f) : vec2(stepLen * f, 0.0))), 1.0);\n"
               "}\n";
    }
    // SMAA 1x (third_party/smaa, runtime/nvn/smaa_data.c): the reference SMAA.hlsl in its GLSL mode, High preset,
    // with an entry point per pass. a = SMAA_RT_METRICS (1 / size, size). The vertex-stage offset helpers run per
    // pixel here. Luma edges (t0 = colour) -> blending weights (t0 = edges, t1 = area, t2 = search) -> neighbourhood
    // blending (t0 = colour, t1 = weights). Vulkan's texture origin is top-left, as SMAA expects (no flips).
    if (!strncmp(name, "smaa_", 5)) {
        *stage = STAGE_FRAGMENT;
        static char *src[3];
        int k = !strcmp(name, "smaa_edge.frag") ? 0 : !strcmp(name, "smaa_weight.frag") ? 1 : !strcmp(name, "smaa_blend.frag") ? 2 : -1;
        if (k < 0) return NULL;
        if (!src[k]) {
            extern const char smaa_hlsl_src[];
            static const char *head =
                "#version 450\n"
                "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
                "#define SMAA_RT_METRICS pc.a\n"
                "#define SMAA_GLSL_4\n"
                "#define SMAA_PRESET_HIGH\n";
            static const char *mains[3] = {
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy;\n"
                "  vec4 off[3];\n"
                "  SMAAEdgeDetectionVS(uv, off);\n"
                "  o = vec4(SMAALumaEdgeDetectionPS(uv, off, t0), 0.0, 0.0);\n"
                "}\n",
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
                "layout(set = 0, binding = 2) uniform sampler2D t2;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy, pix;\n"
                "  vec4 off[3];\n"
                "  SMAABlendingWeightCalculationVS(uv, pix, off);\n"
                "  o = SMAABlendingWeightCalculationPS(uv, pix, off, t0, t1, t2, vec4(0.0));\n"
                "}\n",
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy;\n"
                "  vec4 off;\n"
                "  SMAANeighborhoodBlendingVS(uv, off);\n"
                "  o = vec4(SMAANeighborhoodBlendingPS(uv, off, t0, t1).rgb, 1.0);\n"
                "}\n"};
            size_t n = strlen(head) + strlen(smaa_hlsl_src) + strlen(mains[k]) + 1;
            src[k] = malloc(n);
            snprintf(src[k], n, "%s%s%s", head, smaa_hlsl_src, mains[k]);
        }
        return src[k];
    }
    // AMD FidelityFX Super Resolution 1 (MIT), EASU: edge-adaptive upscale. a = (in / out scale xy, offset zw),
    // b.xy = input size (alpha: bilinear, for the 3D scene's composite: exec.c scene_upscale). 12 taps around the
    // source pixel, direction / edge length from luma, Lanczos-2-like weights stretched along the edge, clamped to the
    // nearest 4 (no ringing).
    if (!strcmp(name, "easu.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb; }\n"
               "float lum(vec3 c) { return c.g + 0.5 * (c.r + c.b); }\n"
               "void setf(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {\n"
               "  float dx = lD - lB, dy = lE - lA;\n"
               "  float lx = clamp(abs(dx) / max(max(abs(lD - lC), abs(lC - lB)), 1.0 / 65536.0), 0.0, 1.0);\n"
               "  float ly = clamp(abs(dy) / max(max(abs(lE - lC), abs(lC - lA)), 1.0 / 65536.0), 0.0, 1.0);\n"
               "  dir += vec2(dx, dy) * w;\n"
               "  len += (lx * lx + ly * ly) * w;\n"
               "}\n"
               "void tapf(inout vec3 ac, inout float aw, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec3 c) {\n"
               "  vec2 v = vec2(dot(off, dir), dot(off, vec2(-dir.y, dir.x))) * len2;\n"
               "  float d2 = min(dot(v, v), clp);\n"
               "  float wb = 0.4 * d2 - 1.0, wa = lob * d2 - 1.0;\n"
               "  wb *= wb; wa *= wa;\n"
               "  float w = (1.5625 * wb - 0.5625) * wa;\n"
               "  ac += c * w; aw += w;\n"
               "}\n"
               "void main() {\n"
               "  vec2 pp = floor(gl_FragCoord.xy) * pc.a.xy + pc.a.zw;\n"
               "  float alpha = texture(t, (pp + 0.5) / pc.b.xy).a;\n"
               "  vec2 fpf = floor(pp); pp -= fpf;\n"
               "  ivec2 fp = ivec2(fpf);\n"
               "  //    b c\n"
               "  //  e f g h\n"
               "  //  i j k l\n"
               "  //    n o\n"
               "  vec3 bC = tap(fp + ivec2(0, -1)), cC = tap(fp + ivec2(1, -1));\n"
               "  vec3 eC = tap(fp + ivec2(-1, 0)), fC = tap(fp), gC = tap(fp + ivec2(1, 0)), hC = tap(fp + ivec2(2, 0));\n"
               "  vec3 iC = tap(fp + ivec2(-1, 1)), jC = tap(fp + ivec2(0, 1)), kC = tap(fp + ivec2(1, 1)), lC = tap(fp + ivec2(2, 1));\n"
               "  vec3 nC = tap(fp + ivec2(0, 2)), oC = tap(fp + ivec2(1, 2));\n"
               "  float bL = lum(bC), cL = lum(cC), eL = lum(eC), fL = lum(fC), gL = lum(gC), hL = lum(hC);\n"
               "  float iL = lum(iC), jL = lum(jC), kL = lum(kC), lL = lum(lC), nL = lum(nC), oL = lum(oC);\n"
               "  vec2 dir = vec2(0.0);\n"
               "  float len = 0.0;\n"
               "  setf(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);\n"
               "  setf(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);\n"
               "  setf(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);\n"
               "  setf(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);\n"
               "  float dr = dot(dir, dir);\n"
               "  bool zro = dr < 1.0 / 32768.0;\n"
               "  dir = zro ? vec2(1.0, 0.0) : dir * inversesqrt(dr);\n"
               "  len = len * 0.5; len *= len;\n"
               "  float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));\n"
               "  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
               "  float lob = 0.5 - 0.29 * len, clp = 1.0 / lob;\n"
               "  vec3 mn = min(min(fC, gC), min(jC, kC)), mx = max(max(fC, gC), max(jC, kC));\n"
               "  vec3 ac = vec3(0.0);\n"
               "  float aw = 0.0;\n"
               "  tapf(ac, aw, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, bC);\n"
               "  tapf(ac, aw, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, cC);\n"
               "  tapf(ac, aw, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, iC);\n"
               "  tapf(ac, aw, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, jC);\n"
               "  tapf(ac, aw, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, fC);\n"
               "  tapf(ac, aw, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, eC);\n"
               "  tapf(ac, aw, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, kC);\n"
               "  tapf(ac, aw, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, lC);\n"
               "  tapf(ac, aw, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, hC);\n"
               "  tapf(ac, aw, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, gC);\n"
               "  tapf(ac, aw, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, oC);\n"
               "  tapf(ac, aw, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, nC);\n"
               // (weights summing to ~0 on a saturated patch: NaN, and min / max of a NaN may give black - a black dash
               // by a white rock in the scene at 50%; the nearest texel there)
               "  o = vec4(aw > 1.0 / 65536.0 ? min(mx, max(mn, ac / aw)) : fC, alpha);\n"
               "}\n";
    }
    // Snapdragon Game Super Resolution 1 (sgsr/v1/include/glsl/sgsr1_shader_mobile_edge_direction.frag of
    // github.com/SnapdragonStudios/snapdragon-gsr: Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights
    // reserved. SPDX-License-Identifier: BSD-3-Clause), its default mode (RGBA, luma from green, no edge direction):
    // one pass, a bilinear sample sharpened where the 12 texels around it show an edge (Lanczos-2-like weights from
    // their deviation from the mean). a = (1 / input size, input size), b.xy = output size. Alpha: the bilinear
    // sample's (the 3D scene's composite: exec.c scene_upscale), where SGSR writes 1. Its gathers and its bilinear
    // sample are texelFetch here (Vulkan's own rules: texel centers, (i0,j1) (i1,j1) (i1,j0) (i0,j0), clamp): on the 3D
    // scene's A2B10G10R10 image textureGather and the sampler gave Turnip (Adreno 830) a blurred, speckled image, the
    // same shader through the sampler was right on the RGBA8 final image.
    if (!strcmp(name, "sgsr1.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D ps0;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "const float EdgeThreshold = 8.0 / 255.0, EdgeSharpness = 2.0;\n"
               "vec4 tx(ivec2 p) { return texelFetch(ps0, clamp(p, ivec2(0), ivec2(pc.a.zw) - 1), 0); }\n"
               "vec4 bilinear(vec2 uv) {\n"
               "  vec2 x = uv * pc.a.zw - 0.5, f = fract(x);\n"
               "  ivec2 i = ivec2(floor(x));\n"
               "  return mix(mix(tx(i), tx(i + ivec2(1, 0)), f.x), mix(tx(i + ivec2(0, 1)), tx(i + ivec2(1, 1)), f.x), f.y);\n"
               "}\n"
               "vec4 gatherG(vec2 uv) {\n"
               "  ivec2 i = ivec2(floor(uv * pc.a.zw - 0.5));\n"
               "  return vec4(tx(i + ivec2(0, 1)).g, tx(i + ivec2(1, 1)).g, tx(i + ivec2(1, 0)).g, tx(i).g);\n"
               "}\n"
               "float fastLanczos2(float x) { float wA = x - 4.0; float wB = x * wA - wA; wA *= wA; return wB * wA; }\n"
               "vec2 weightY(float dx, float dy, float c, float std) {\n"
               "  float x = (dx * dx + dy * dy) * 0.55 + clamp(abs(c) * std, 0.0, 1.0);\n"
               "  float w = fastLanczos2(x);\n"
               "  return vec2(w, w * c);\n"
               "}\n"
               "void main() {\n"
               "  vec4 vi = pc.a;\n"
               "  vec2 uv = gl_FragCoord.xy / pc.b.xy;\n"
               "  vec4 color = bilinear(uv);\n"
               "  float alpha = color.w;\n"
               "  vec2 imgCoord = uv * vi.zw + vec2(-0.5, 0.5);\n"
               "  vec2 imgCoordPixel = floor(imgCoord);\n"
               "  vec2 coord = imgCoordPixel * vi.xy;\n"
               "  vec2 pl = imgCoord - imgCoordPixel;\n"
               "  vec4 left = gatherG(coord);\n"
               "  float edgeVote = abs(left.z - left.y) + abs(color.y - left.y) + abs(color.y - left.z);\n"
               "  if (edgeVote > EdgeThreshold) {\n"
               "    coord.x += vi.x;\n"
               "    vec4 right = gatherG(coord + vec2(vi.x, 0.0));\n"
               "    vec4 upDown;\n"
               "    upDown.xy = gatherG(coord + vec2(0.0, -vi.y)).wz;\n"
               "    upDown.zw = gatherG(coord + vec2(0.0, vi.y)).yx;\n"
               "    float mean = (left.y + left.z + right.x + right.w) * 0.25;\n"
               "    left -= vec4(mean); right -= vec4(mean); upDown -= vec4(mean);\n"
               "    color.w = color.y - mean;\n"
               "    float sum = abs(left.x) + abs(left.y) + abs(left.z) + abs(left.w) + abs(right.x) + abs(right.y) +\n"
               "                abs(right.z) + abs(right.w) + abs(upDown.x) + abs(upDown.y) + abs(upDown.z) + abs(upDown.w);\n"
               "    float sumMean = 1.014185e+01 / sum;\n"
               "    float std = sumMean * sumMean;\n"
               "    vec2 aWY = weightY(pl.x, pl.y + 1.0, upDown.x, std);\n"
               "    aWY += weightY(pl.x - 1.0, pl.y + 1.0, upDown.y, std);\n"
               "    aWY += weightY(pl.x - 1.0, pl.y - 2.0, upDown.z, std);\n"
               "    aWY += weightY(pl.x, pl.y - 2.0, upDown.w, std);\n"
               "    aWY += weightY(pl.x + 1.0, pl.y - 1.0, left.x, std);\n"
               "    aWY += weightY(pl.x, pl.y - 1.0, left.y, std);\n"
               "    aWY += weightY(pl.x, pl.y, left.z, std);\n"
               "    aWY += weightY(pl.x + 1.0, pl.y, left.w, std);\n"
               "    aWY += weightY(pl.x - 1.0, pl.y - 1.0, right.x, std);\n"
               "    aWY += weightY(pl.x - 2.0, pl.y - 1.0, right.y, std);\n"
               "    aWY += weightY(pl.x - 2.0, pl.y, right.z, std);\n"
               "    aWY += weightY(pl.x - 1.0, pl.y, right.w, std);\n"
               "    float finalY = aWY.y / aWY.x;\n"
               "    float maxY = max(max(left.y, left.z), max(right.x, right.w));\n"
               "    float minY = min(min(left.y, left.z), min(right.x, right.w));\n"
               "    float deltaY = clamp(EdgeSharpness * finalY, minY, maxY) - color.w;\n"
               "    deltaY = clamp(deltaY, -23.0 / 255.0, 23.0 / 255.0);\n"  // (smooth high contrast input)
               "    color.xyz = clamp(color.xyz + deltaY, 0.0, 1.0);\n"
               "  }\n"
               "  o = vec4(color.xyz, alpha);\n"
               "}\n";
    }
    // FSR 1 RCAS: contrast-adaptive sharpening of the EASU output (same size). a.x = sharpness (exp2(-stops)),
    // b.xy = image size. The lobe is limited so the result stays inside the neighbours' range; noise is damped.
    if (!strcmp(name, "rcas.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb; }\n"
               "float lum(vec3 c) { return c.g + 0.5 * (c.r + c.b); }\n"
               "void main() {\n"
               "  ivec2 ip = ivec2(gl_FragCoord.xy);\n"
               "  //    b\n"
               "  //  d e f\n"
               "  //    h\n"
               "  vec3 b = tap(ip + ivec2(0, -1)), d = tap(ip + ivec2(-1, 0)), e = tap(ip), f = tap(ip + ivec2(1, 0)), h = tap(ip + ivec2(0, 1));\n"
               "  float bL = lum(b), dL = lum(d), eL = lum(e), fL = lum(f), hL = lum(h);\n"
               "  float nz = 0.25 * (bL + dL + fL + hL) - eL;\n"
               "  float rng = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);\n"
               "  nz = 1.0 - 0.5 * clamp(abs(nz) / max(rng, 1.0 / 65536.0), 0.0, 1.0);\n"
               "  vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));\n"
               "  vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1.0 / 65536.0));\n"
               "  vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65536.0));\n"
               "  vec3 lobeRGB = max(-hitMin, hitMax);\n"
               "  float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * pc.a.x * nz;\n"
               "  o = vec4((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), texelFetch(t, ip, 0).a);\n"
               "}\n";
    }
    if (!strcmp(name, "clear.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 c; vec4 d; } pc;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, pc.d.x, 1.0);\n"
               "}\n";
    }
    // clear.frag.<f|i|u><index>: writes the push-constant colour to colour target <index>
    if (!strncmp(name, "clear.frag.", 11)) {
        *stage = STAGE_FRAGMENT;
        static __thread char buf[512];
        char k = name[11];
        int idx = atoi(name + 12);
        const char *ty = k == 'i' ? "ivec4" : k == 'u' ? "uvec4" : "vec4";
        const char *cv = k == 'i' ? "floatBitsToInt(pc.c)" : k == 'u' ? "floatBitsToUint(pc.c)" : "pc.c";
        snprintf(buf, sizeof buf,
                 "#version 450\n"
                 "layout(push_constant) uniform P { vec4 c; vec4 d; } pc;\n"
                 "layout(location = %d) out %s o;\n"
                 "void main() { o = %s; }\n", idx, ty, cv);
        return buf;
    }
    if (!strcmp(name, "clear.frag.none")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\nvoid main() {}\n";
    }
    return NULL;
}

VkShaderModule vk_builtin_shader(const char *name) {
    static struct { char name[32]; VkShaderModule m; } cache[64];
    static int n;
    for (int i = 0; i < n; i++) if (!strcmp(cache[i].name, name)) return cache[i].m;
    int stage = 0;
    const char *src = builtin_src(name, &stage);
    if (!src) FATAL("no built-in shader %s\n", name);
    VkShaderModule m = vk_compile_glsl(src, stage, name);
    if (!m) FATAL("built-in shader %s failed to compile\n", name);
    if (n < 64) { snprintf(cache[n].name, sizeof cache[n].name, "%s", name); cache[n++].m = m; }
    return m;
}
