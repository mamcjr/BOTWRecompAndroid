// Conservative linear SPIR-V post-passes for Mali-G925 (Phase 1):
// 1. spirv_mali_cleanup: strictly intra-block vecN temp promotion to OpCompositeExtract,
//    intra-block scalar StorageClassFunction store-to-load forwarding, dead same-block store removal,
//    OpBitcast identity/round-trip folding, pure SSA DCE, and dead StorageClassFunction OpVariable /
//    orphan OpName/OpDecorate removal (FS/CS). Full state reset on every OpLabel/OpFunctionCall.
// 2. spirv_mali_demote_to_terminate: converts OpDemoteToHelperInvocation to
//    OpTerminateInvocation when no downstream derivative/implicit-LOD/subgroup/helper op exists,
//    truncating block tails and updating all function OpPhi nodes cleanly (FS on Vulkan 1.3).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    MALI_SPV_OP_NOP = 0,
    MALI_SPV_OP_UNDEF = 1,
    MALI_SPV_OP_NAME = 5,
    MALI_SPV_OP_MEMBER_NAME = 6,
    MALI_SPV_OP_LINE = 8,
    MALI_SPV_OP_EXTENSION = 10,
    MALI_SPV_OP_EXT_INST_IMPORT = 11,
    MALI_SPV_OP_EXT_INST = 12,
    MALI_SPV_OP_ENTRY_POINT = 15,
    MALI_SPV_OP_CAPABILITY = 17,
    MALI_SPV_OP_TYPE_VOID = 19,
    MALI_SPV_OP_TYPE_BOOL = 20,
    MALI_SPV_OP_TYPE_INT = 21,
    MALI_SPV_OP_TYPE_FLOAT = 22,
    MALI_SPV_OP_TYPE_VECTOR = 23,
    MALI_SPV_OP_TYPE_POINTER = 32,
    MALI_SPV_OP_TYPE_FUNCTION = 33,
    MALI_SPV_OP_CONSTANT_TRUE = 41,
    MALI_SPV_OP_CONSTANT_FALSE = 42,
    MALI_SPV_OP_CONSTANT = 43,
    MALI_SPV_OP_FUNCTION = 54,
    MALI_SPV_OP_FUNCTION_PARAM = 55,
    MALI_SPV_OP_FUNCTION_END = 56,
    MALI_SPV_OP_FUNCTION_CALL = 57,
    MALI_SPV_OP_VARIABLE = 59,
    MALI_SPV_OP_LOAD = 61,
    MALI_SPV_OP_STORE = 62,
    MALI_SPV_OP_COPY_MEMORY = 63,
    MALI_SPV_OP_ACCESS_CHAIN = 65,
    MALI_SPV_OP_IN_BOUNDS_ACCESS_CHAIN = 66,
    MALI_SPV_OP_PTR_ACCESS_CHAIN = 67,
    MALI_SPV_OP_ARRAY_LENGTH = 68,
    MALI_SPV_OP_DECORATE = 71,
    MALI_SPV_OP_MEMBER_DECORATE = 72,
    MALI_SPV_OP_GROUP_DECORATE = 74,
    MALI_SPV_OP_VECTOR_EXTRACT_DYNAMIC = 77,
    MALI_SPV_OP_VECTOR_INSERT_DYNAMIC = 78,
    MALI_SPV_OP_VECTOR_SHUFFLE = 79,
    MALI_SPV_OP_COMPOSITE_CONSTRUCT = 80,
    MALI_SPV_OP_COMPOSITE_EXTRACT = 81,
    MALI_SPV_OP_COMPOSITE_INSERT = 82,
    MALI_SPV_OP_COPY_OBJECT = 83,
    MALI_SPV_OP_TRANSPOSE = 84,
    MALI_SPV_OP_SAMPLED_IMAGE = 86,
    MALI_SPV_OP_IMAGE_WRITE = 99,
    MALI_SPV_OP_IMAGE = 100,
    MALI_SPV_OP_IMAGE_QUERY_FORMAT = 101,
    MALI_SPV_OP_IMAGE_TEXEL_POINTER = 102,
    MALI_SPV_OP_IMAGE_QUERY_SIZE_LOD = 103,
    MALI_SPV_OP_IMAGE_QUERY_SIZE = 104,
    MALI_SPV_OP_IMAGE_QUERY_LOD = 105,
    MALI_SPV_OP_IMAGE_QUERY_LEVELS = 106,
    MALI_SPV_OP_IMAGE_QUERY_SAMPLES = 107,
    MALI_SPV_OP_BITCAST = 124,
    MALI_SPV_OP_CONTROL_BARRIER = 224,
    MALI_SPV_OP_MEMORY_BARRIER = 225,
    MALI_SPV_OP_PHI = 245,
    MALI_SPV_OP_LOOP_MERGE = 246,
    MALI_SPV_OP_SELECTION_MERGE = 247,
    MALI_SPV_OP_LABEL = 248,
    MALI_SPV_OP_BRANCH = 249,
    MALI_SPV_OP_BRANCH_CONDITIONAL = 250,
    MALI_SPV_OP_SWITCH = 251,
    MALI_SPV_OP_KILL = 252,
    MALI_SPV_OP_RETURN = 253,
    MALI_SPV_OP_RETURN_VALUE = 254,
    MALI_SPV_OP_UNREACHABLE = 255,
    MALI_SPV_OP_LIFETIME_START = 256,
    MALI_SPV_OP_LIFETIME_STOP = 257,
    MALI_SPV_OP_NO_LINE = 317,
    MALI_SPV_OP_DECORATE_ID = 332,
    MALI_SPV_OP_TERMINATE_INVOCATION = 4416,
    MALI_SPV_OP_IS_HELPER_INVOCATION = 4417,
    MALI_SPV_OP_CAP_DEMOTE = 5379,
    MALI_SPV_OP_DEMOTE_TO_HELPER = 5380,
    MALI_SPV_OP_IS_HELPER_INVOCATION_EXT = 5381,
    MALI_SPV_OP_DECORATE_STRING = 5632,
    MALI_SPV_OP_MEMBER_DECORATE_STRING = 5633,
};

typedef struct {
    uint32_t offset;
    uint32_t wc;
    uint16_t op;
    uint8_t dead;
} MaliSpvInsn;

static inline bool mali_spv_validate_stream(const uint32_t *w, size_t nw, size_t *out_ninsns) {
    if (!w || nw <= 5 || w[0] != 0x07230203u || w[3] == 0 || w[4] != 0) return false;
    size_t n = 0;
    for (size_t pos = 5; pos < nw;) {
        uint32_t wc = w[pos] >> 16;
        if (!wc || pos + wc > nw) return false;
        n++;
        pos += wc;
    }
    if (out_ninsns) *out_ninsns = n;
    return true;
}

static inline bool mali_spv_op_has_result(uint16_t op, bool *has_res_ty) {
    bool rt = true;
    switch (op) {
    case MALI_SPV_OP_UNDEF:
    case MALI_SPV_OP_EXT_INST:
    case MALI_SPV_OP_FUNCTION:
    case MALI_SPV_OP_FUNCTION_PARAM:
    case MALI_SPV_OP_FUNCTION_CALL:
    case MALI_SPV_OP_VARIABLE:
    case MALI_SPV_OP_LOAD:
    case MALI_SPV_OP_ACCESS_CHAIN:
    case MALI_SPV_OP_IN_BOUNDS_ACCESS_CHAIN:
    case MALI_SPV_OP_PTR_ACCESS_CHAIN:
    case MALI_SPV_OP_ARRAY_LENGTH:
    case MALI_SPV_OP_VECTOR_EXTRACT_DYNAMIC:
    case MALI_SPV_OP_VECTOR_INSERT_DYNAMIC:
    case MALI_SPV_OP_VECTOR_SHUFFLE:
    case MALI_SPV_OP_COMPOSITE_CONSTRUCT:
    case MALI_SPV_OP_COMPOSITE_EXTRACT:
    case MALI_SPV_OP_COMPOSITE_INSERT:
    case MALI_SPV_OP_COPY_OBJECT:
    case MALI_SPV_OP_TRANSPOSE:
    case MALI_SPV_OP_SAMPLED_IMAGE:
    case MALI_SPV_OP_IMAGE:
    case MALI_SPV_OP_IMAGE_TEXEL_POINTER:
    case MALI_SPV_OP_IMAGE_QUERY_FORMAT:
    case MALI_SPV_OP_IMAGE_QUERY_SIZE_LOD:
    case MALI_SPV_OP_IMAGE_QUERY_SIZE:
    case MALI_SPV_OP_IMAGE_QUERY_LOD:
    case MALI_SPV_OP_IMAGE_QUERY_LEVELS:
    case MALI_SPV_OP_IMAGE_QUERY_SAMPLES:
    case MALI_SPV_OP_PHI:
    case MALI_SPV_OP_IS_HELPER_INVOCATION:
    case MALI_SPV_OP_IS_HELPER_INVOCATION_EXT:
        break;
    case MALI_SPV_OP_LABEL:
        rt = false;
        break;
    default:
        if ((op >= 87 && op <= 98) || (op >= 109 && op <= 215) ||
            op == 227 || (op >= 229 && op <= 242) || (op >= 261 && op <= 271) ||
            (op >= 305 && op <= 320 && op != 317 && op != 319) ||
            (op >= 333 && op <= 366) || op == 4431 || (op >= 5000 && op <= 5007) || op == 5296 ||
            (op >= 19 && op <= 33) || (op >= 41 && op <= 52) || op == 11) {
            if (op == 11 || (op >= 19 && op <= 33)) rt = false;
            break;
        }
        return false;
    }
    if (has_res_ty) *has_res_ty = rt;
    return true;
}

static inline uint32_t mali_spv_result_id(const uint32_t *w, uint32_t wc, uint16_t op, uint32_t *out_res_ty) {
    bool has_rt = false;
    if (!mali_spv_op_has_result(op, &has_rt)) return 0;
    if (has_rt) {
        if (wc < 3) return 0;
        if (out_res_ty) *out_res_ty = w[1];
        return w[2];
    }
    if (wc < 2) return 0;
    if (out_res_ty) *out_res_ty = 0;
    return w[1];
}

// Visit every <id> operand word (excluding result_type / result_id) of a function-body instruction.
// Returns false if the opcode's operand layout is unrecognized so callers fail safe.
typedef void (*MaliSpvIdVisitor)(uint32_t *slot, uint32_t idx_in_insn, void *user);

static inline bool mali_spv_visit_fn_operands(uint32_t *w, uint32_t wc, const uint8_t *int_words, uint32_t bound,
                                              MaliSpvIdVisitor fn, void *user) {
    uint16_t op = (uint16_t)(w[0] & 0xFFFFu);
    switch (op) {
    case MALI_SPV_OP_NOP:
    case MALI_SPV_OP_LINE:
    case MALI_SPV_OP_NO_LINE:
    case MALI_SPV_OP_LABEL:
    case MALI_SPV_OP_FUNCTION_PARAM:
    case MALI_SPV_OP_FUNCTION_END:
    case MALI_SPV_OP_KILL:
    case MALI_SPV_OP_RETURN:
    case MALI_SPV_OP_UNREACHABLE:
    case MALI_SPV_OP_TERMINATE_INVOCATION:
    case MALI_SPV_OP_DEMOTE_TO_HELPER:
    case MALI_SPV_OP_IS_HELPER_INVOCATION:
    case MALI_SPV_OP_IS_HELPER_INVOCATION_EXT:
    case MALI_SPV_OP_UNDEF:
        return true;
    case MALI_SPV_OP_FUNCTION:
        if (wc < 5) return false;
        fn(&w[4], 4, user);
        return true;
    case MALI_SPV_OP_VARIABLE:
        if (wc < 4) return false;
        if (wc >= 5) fn(&w[4], 4, user);
        return true;
    case MALI_SPV_OP_LOAD:
        if (wc < 4) return false;
        fn(&w[3], 3, user);
        return true;
    case MALI_SPV_OP_STORE:
        if (wc < 3) return false;
        fn(&w[1], 1, user);
        fn(&w[2], 2, user);
        return true;
    case MALI_SPV_OP_COPY_MEMORY:
        if (wc < 3) return false;
        fn(&w[1], 1, user);
        fn(&w[2], 2, user);
        return true;
    case MALI_SPV_OP_ARRAY_LENGTH:
        if (wc < 5) return false;
        fn(&w[3], 3, user);
        return true;
    case MALI_SPV_OP_VECTOR_SHUFFLE:
        if (wc < 5) return false;
        fn(&w[3], 3, user);
        fn(&w[4], 4, user);
        return true;
    case MALI_SPV_OP_COMPOSITE_EXTRACT:
        if (wc < 4) return false;
        fn(&w[3], 3, user);
        return true;
    case MALI_SPV_OP_COMPOSITE_INSERT:
        if (wc < 5) return false;
        fn(&w[3], 3, user);
        fn(&w[4], 4, user);
        return true;
    case MALI_SPV_OP_EXT_INST:
        if (wc < 5) return false;
        fn(&w[3], 3, user);
        for (uint32_t k = 5; k < wc; k++) fn(&w[k], k, user);
        return true;
    case MALI_SPV_OP_LOOP_MERGE:
        if (wc < 4) return false;
        fn(&w[1], 1, user);
        fn(&w[2], 2, user);
        return true;
    case MALI_SPV_OP_SELECTION_MERGE:
        if (wc < 3) return false;
        fn(&w[1], 1, user);
        return true;
    case MALI_SPV_OP_BRANCH:
    case MALI_SPV_OP_RETURN_VALUE:
        if (wc < 2) return false;
        fn(&w[1], 1, user);
        return true;
    case MALI_SPV_OP_BRANCH_CONDITIONAL:
        if (wc < 4) return false;
        for (uint32_t k = 1; k <= 3; k++) fn(&w[k], k, user);
        return true;
    case MALI_SPV_OP_SWITCH: {
        if (wc < 3 || !int_words || w[1] >= bound) return false;
        fn(&w[1], 1, user);
        fn(&w[2], 2, user);
        uint32_t lw = int_words[w[1]] ? int_words[w[1]] : 1u;
        for (uint32_t k = 3; k + lw < wc; k += lw + 1u) fn(&w[k + lw], k + lw, user);
        return true;
    }
    case MALI_SPV_OP_LIFETIME_START:
    case MALI_SPV_OP_LIFETIME_STOP:
        if (wc < 2) return false;
        fn(&w[1], 1, user);
        return true;
    case MALI_SPV_OP_CONTROL_BARRIER:
    case MALI_SPV_OP_MEMORY_BARRIER:
        for (uint32_t k = 1; k < wc; k++) fn(&w[k], k, user);
        return true;
    case MALI_SPV_OP_IMAGE_WRITE:
        if (wc < 4) return false;
        fn(&w[1], 1, user);
        fn(&w[2], 2, user);
        fn(&w[3], 3, user);
        for (uint32_t k = 5; k < wc; k++) fn(&w[k], k, user);
        return true;
    case 228:  // OpAtomicStore: %Pointer %Memory %Semantics %Value (no result)
        if (wc < 5) return false;
        for (uint32_t k = 1; k < wc; k++) fn(&w[k], k, user);
        return true;
    case 319:  // OpAtomicFlagClear: %Pointer %Memory %Semantics (no result)
        if (wc < 4) return false;
        for (uint32_t k = 1; k < wc; k++) fn(&w[k], k, user);
        return true;
    case 87: case 88: case 91: case 92: case 95: case 98:
    case 305: case 306: case 309: case 310: case 313: case 320:
        if (wc < 5) return false;
        fn(&w[3], 3, user);
        fn(&w[4], 4, user);
        for (uint32_t k = 6; k < wc; k++) fn(&w[k], k, user);
        return true;
    case 89: case 90: case 93: case 94: case 96: case 97:
    case 307: case 308: case 311: case 312: case 314: case 315:
        if (wc < 6) return false;
        fn(&w[3], 3, user);
        fn(&w[4], 4, user);
        fn(&w[5], 5, user);
        for (uint32_t k = 7; k < wc; k++) fn(&w[k], k, user);
        return true;
    case MALI_SPV_OP_FUNCTION_CALL:
    case MALI_SPV_OP_ACCESS_CHAIN:
    case MALI_SPV_OP_IN_BOUNDS_ACCESS_CHAIN:
    case MALI_SPV_OP_PTR_ACCESS_CHAIN:
    case MALI_SPV_OP_VECTOR_EXTRACT_DYNAMIC:
    case MALI_SPV_OP_VECTOR_INSERT_DYNAMIC:
    case MALI_SPV_OP_COMPOSITE_CONSTRUCT:
    case MALI_SPV_OP_COPY_OBJECT:
    case MALI_SPV_OP_TRANSPOSE:
    case MALI_SPV_OP_SAMPLED_IMAGE:
    case MALI_SPV_OP_IMAGE:
    case MALI_SPV_OP_IMAGE_TEXEL_POINTER:
    case MALI_SPV_OP_IMAGE_QUERY_FORMAT:
    case MALI_SPV_OP_IMAGE_QUERY_SIZE_LOD:
    case MALI_SPV_OP_IMAGE_QUERY_SIZE:
    case MALI_SPV_OP_IMAGE_QUERY_LOD:
    case MALI_SPV_OP_IMAGE_QUERY_LEVELS:
    case MALI_SPV_OP_IMAGE_QUERY_SAMPLES:
    case 316:  // OpImageSparseTexelsResident
    case MALI_SPV_OP_PHI:
        for (uint32_t k = 3; k < wc; k++) fn(&w[k], k, user);
        return true;
    default:
        if ((op >= 109 && op <= 215) || op == 227 || (op >= 229 && op <= 242) ||
            (op >= 261 && op <= 263) || op == 318 ||
            (op >= 333 && op <= 341) || (op >= 343 && op <= 348) ||
            op == 365 || op == 366 || op == 4431 || op == 5296) {
            for (uint32_t k = 3; k < wc; k++) fn(&w[k], k, user);
            return true;
        }
        if (op == 342 || (op >= 349 && op <= 364) || (op >= 264 && op <= 271) ||
            (op >= 5000 && op <= 5007)) {  // w[4] is GroupOperation literal
            if (wc < 5) return false;
            fn(&w[3], 3, user);
            for (uint32_t k = 5; k < wc; k++) fn(&w[k], k, user);
            return true;
        }
        return false;
    }
}

typedef struct {
    const uint32_t *alias_id;
    uint32_t bound;
} MaliSpvAliasCtx;

static inline uint32_t mali_spv_resolve(const uint32_t *alias_id, uint32_t bound, uint32_t id) {
    for (int step = 0; step < 64 && id < bound && alias_id[id]; step++) id = alias_id[id];
    return id;
}

static inline void mali_spv_remap_cb(uint32_t *slot, uint32_t idx_in_insn, void *user) {
    (void)idx_in_insn;
    MaliSpvAliasCtx *ctx = (MaliSpvAliasCtx *)user;
    *slot = mali_spv_resolve(ctx->alias_id, ctx->bound, *slot);
}

typedef struct {
    uint8_t *pinned;
    uint32_t bound;
} MaliSpvPinCtx;

static inline void mali_spv_pin_var_cb(uint32_t *slot, uint32_t idx_in_insn, void *user) {
    (void)idx_in_insn;
    MaliSpvPinCtx *ctx = (MaliSpvPinCtx *)user;
    if (*slot && *slot < ctx->bound) ctx->pinned[*slot] = 1;
}

typedef struct {
    uint32_t *use_count;
    uint32_t *var_read_count;
    uint32_t *var_store_count;
    const uint8_t *var_scalar_clean;
    uint32_t bound;
    uint16_t cur_op;
} MaliSpvUseCtx;

static inline void mali_spv_use_cb(uint32_t *slot, uint32_t idx_in_insn, void *user) {
    MaliSpvUseCtx *ctx = (MaliSpvUseCtx *)user;
    uint32_t id = *slot;
    if (!id || id >= ctx->bound) return;
    if (ctx->cur_op == MALI_SPV_OP_STORE && idx_in_insn == 1 && ctx->var_scalar_clean[id]) {
        ctx->var_store_count[id]++;
        return;
    }
    if (ctx->cur_op == MALI_SPV_OP_LOAD && idx_in_insn == 3 && ctx->var_scalar_clean[id]) {
        ctx->var_read_count[id]++;
    }
    ctx->use_count[id]++;
}

static inline bool mali_spv_is_pure_dce_op(uint16_t op, const uint32_t *w, uint32_t wc, uint32_t glsl_ext_id,
                                           const uint8_t *id_safe_load_sc, uint32_t bound) {
    switch (op) {
    case MALI_SPV_OP_LOAD:
        if (wc < 4 || w[3] >= bound) return false;
        if (wc >= 5 && (w[4] & 1u)) return false;  // Volatile
        return id_safe_load_sc[w[3]] != 0;
    case MALI_SPV_OP_EXT_INST:
        if (!glsl_ext_id || wc < 5 || w[3] != glsl_ext_id) return false;
        // Modf (35) and Frexp (51) write through a pointer operand in addition to returning a value.
        return w[4] != 35u && w[4] != 51u;
    case MALI_SPV_OP_ACCESS_CHAIN:
    case MALI_SPV_OP_IN_BOUNDS_ACCESS_CHAIN:
    case MALI_SPV_OP_VECTOR_EXTRACT_DYNAMIC:
    case MALI_SPV_OP_VECTOR_INSERT_DYNAMIC:
    case MALI_SPV_OP_VECTOR_SHUFFLE:
    case MALI_SPV_OP_COMPOSITE_CONSTRUCT:
    case MALI_SPV_OP_COMPOSITE_EXTRACT:
    case MALI_SPV_OP_COMPOSITE_INSERT:
    case MALI_SPV_OP_COPY_OBJECT:
    case MALI_SPV_OP_TRANSPOSE:
    case MALI_SPV_OP_BITCAST:
    case MALI_SPV_OP_PHI:
        return true;
    default:
        return op >= 109 && op <= 205;
    }
}

typedef struct {
    uint8_t *cand_vec;
    uint32_t *ac_base;
    uint32_t bound;
    uint16_t cur_op;
    bool cur_volatile;
    uint32_t cur_res;
} MaliSpvVecCheckCtx;

static inline void mali_spv_vec_check_cb(uint32_t *slot, uint32_t idx_in_insn, void *user) {
    MaliSpvVecCheckCtx *ctx = (MaliSpvVecCheckCtx *)user;
    uint32_t id = *slot;
    if (!id || id >= ctx->bound) return;
    if (ctx->cur_op == MALI_SPV_OP_STORE && idx_in_insn == 1 && !ctx->cur_volatile && ctx->cand_vec[id]) return;
    if ((ctx->cur_op == MALI_SPV_OP_ACCESS_CHAIN || ctx->cur_op == MALI_SPV_OP_IN_BOUNDS_ACCESS_CHAIN) &&
        idx_in_insn == 3 && ctx->cur_res < ctx->bound && ctx->ac_base[ctx->cur_res] == id) return;
    if (ctx->cur_op == MALI_SPV_OP_LOAD && idx_in_insn == 3 && !ctx->cur_volatile && ctx->ac_base[id]) return;
    if (ctx->cand_vec[id]) ctx->cand_vec[id] = 0;
    if (ctx->ac_base[id] && ctx->ac_base[id] < ctx->bound) ctx->cand_vec[ctx->ac_base[id]] = 0;
}

// Promote intra-block whole-vector temp stores (`vec4 t = texture(...); rX = t[c];`) to OpCompositeExtract
// so spirv_mali_cleanup eliminates the temporary vec4 variable and chains results directly.
static inline bool spirv_mali_promote_vec_temps(uint32_t **io_words, size_t *io_nwords) {
    if (!io_words || !*io_words || !io_nwords) return false;
    uint32_t *w = *io_words;
    size_t nw = *io_nwords, ninsns = 0;
    if (!mali_spv_validate_stream(w, nw, &ninsns) || ninsns == 0) return false;
    uint32_t bound = w[3];

    MaliSpvInsn *insns = (MaliSpvInsn *)calloc(ninsns, sizeof *insns);
    uint8_t *ty_scalar = (uint8_t *)calloc(bound, 1);
    uint32_t *vec_elem = (uint32_t *)calloc(bound, sizeof *vec_elem);
    uint8_t *vec_len = (uint8_t *)calloc(bound, 1);
    uint32_t *ptr_pointee = (uint32_t *)calloc(bound, sizeof *ptr_pointee);
    uint8_t *ptr_is_fn = (uint8_t *)calloc(bound, 1);
    uint8_t *u32_ty = (uint8_t *)calloc(bound, 1);
    uint8_t *u32_ok = (uint8_t *)calloc(bound, 1);
    uint32_t *u32_val = (uint32_t *)calloc(bound, sizeof *u32_val);
    uint8_t *int_ty_words = (uint8_t *)calloc(bound, 1);
    uint8_t *int_val_words = (uint8_t *)calloc(bound, 1);
    uint32_t *val_type = (uint32_t *)calloc(bound, sizeof *val_type);
    uint8_t *id_has_dec = (uint8_t *)calloc(bound, 1);
    uint8_t *cand_vec = (uint8_t *)calloc(bound, 1);
    uint32_t *ac_base = (uint32_t *)calloc(bound, sizeof *ac_base);
    uint32_t *ac_idx = (uint32_t *)calloc(bound, sizeof *ac_idx);
    uint32_t *cur_vec_val = (uint32_t *)calloc(bound, sizeof *cur_vec_val);
    uint32_t *dirty_vecs = (uint32_t *)malloc(bound * sizeof *dirty_vecs);
    uint8_t *in_dirty = (uint8_t *)calloc(bound, 1);
    uint32_t *rw_vec = (uint32_t *)calloc(ninsns, sizeof *rw_vec);
    uint32_t *rw_comp = (uint32_t *)calloc(ninsns, sizeof *rw_comp);
    uint8_t *vec_promoted_any = (uint8_t *)calloc(bound, 1);
    uint8_t *vec_unpromoted_load = (uint8_t *)calloc(bound, 1);
    uint8_t *dead_vec = (uint8_t *)calloc(bound, 1);
    uint8_t *dead_vec_id = (uint8_t *)calloc(bound, 1);
    if (!insns || !ty_scalar || !vec_elem || !vec_len || !ptr_pointee || !ptr_is_fn || !u32_ty ||
        !u32_ok || !u32_val || !int_ty_words || !int_val_words || !val_type || !id_has_dec ||
        !cand_vec || !ac_base || !ac_idx || !cur_vec_val || !dirty_vecs || !in_dirty || !rw_vec || !rw_comp ||
        !vec_promoted_any || !vec_unpromoted_load || !dead_vec || !dead_vec_id) {
        free(insns); free(ty_scalar); free(vec_elem); free(vec_len); free(ptr_pointee); free(ptr_is_fn);
        free(u32_ty); free(u32_ok); free(u32_val); free(int_ty_words); free(int_val_words);
        free(val_type); free(id_has_dec); free(cand_vec); free(ac_base); free(ac_idx);
        free(cur_vec_val); free(dirty_vecs); free(in_dirty); free(rw_vec); free(rw_comp);
        free(vec_promoted_any); free(vec_unpromoted_load); free(dead_vec); free(dead_vec_id);
        return false;
    }

    size_t pos = 5, idx = 0;
    for (; pos < nw; idx++) {
        uint32_t wc = w[pos] >> 16;
        uint16_t op = (uint16_t)(w[pos] & 0xFFFFu);
        insns[idx].offset = (uint32_t)pos;
        insns[idx].wc = wc;
        insns[idx].op = op;
        if (op == MALI_SPV_OP_TYPE_BOOL && wc >= 2 && w[pos + 1] < bound) ty_scalar[w[pos + 1]] = 1;
        else if (op == MALI_SPV_OP_TYPE_INT && wc >= 4 && w[pos + 1] < bound) {
            ty_scalar[w[pos + 1]] = 1;
            int_ty_words[w[pos + 1]] = w[pos + 2] > 32 ? 2u : 1u;
            if (w[pos + 2] == 32u) u32_ty[w[pos + 1]] = 1;
        } else if (op == MALI_SPV_OP_TYPE_FLOAT && wc >= 3 && w[pos + 1] < bound) ty_scalar[w[pos + 1]] = 1;
        else if (op == MALI_SPV_OP_TYPE_VECTOR && wc >= 4 && w[pos + 1] < bound && w[pos + 2] < bound) {
            if (ty_scalar[w[pos + 2]] && w[pos + 3] >= 2u && w[pos + 3] <= 4u) {
                vec_elem[w[pos + 1]] = w[pos + 2];
                vec_len[w[pos + 1]] = (uint8_t)w[pos + 3];
            }
        } else if (op == MALI_SPV_OP_TYPE_POINTER && wc >= 4 && w[pos + 1] < bound) {
            ptr_pointee[w[pos + 1]] = w[pos + 3];
            if (w[pos + 2] == 7u) ptr_is_fn[w[pos + 1]] = 1;
        } else if (op == MALI_SPV_OP_CONSTANT && wc == 4 && w[pos + 1] < bound && w[pos + 2] < bound && u32_ty[w[pos + 1]]) {
            u32_ok[w[pos + 2]] = 1;
            u32_val[w[pos + 2]] = w[pos + 3];
        } else if (op == MALI_SPV_OP_DECORATE || op == MALI_SPV_OP_DECORATE_ID || op == MALI_SPV_OP_DECORATE_STRING) {
            if (wc >= 2 && w[pos + 1] < bound) id_has_dec[w[pos + 1]] = 1;
        } else if (op == MALI_SPV_OP_GROUP_DECORATE) {
            for (uint32_t k = 2; k < wc; k++)
                if (w[pos + k] < bound) id_has_dec[w[pos + k]] = 1;
        }
        uint32_t rty = 0, rid = mali_spv_result_id(w + pos, wc, op, &rty);
        if (rid && rid < bound && rty < bound) {
            val_type[rid] = rty;
            if (int_ty_words[rty]) int_val_words[rid] = int_ty_words[rty];
        }
        pos += wc;
    }

    bool in_fn = false, ok = true;
    for (size_t i = 0; i < ninsns; i++) {
        uint16_t op = insns[i].op;
        uint32_t wc = insns[i].wc;
        uint32_t *iw = w + insns[i].offset;
        if (op == MALI_SPV_OP_FUNCTION) { in_fn = true; continue; }
        if (op == MALI_SPV_OP_FUNCTION_END) { in_fn = false; continue; }
        if (!in_fn) continue;
        if (op == MALI_SPV_OP_VARIABLE && wc == 4 && iw[3] == 7u && iw[1] < bound && iw[2] < bound && ptr_is_fn[iw[1]]) {
            uint32_t vty = ptr_pointee[iw[1]];
            if (vty < bound && vec_len[vty]) cand_vec[iw[2]] = 1;
        } else if ((op == MALI_SPV_OP_ACCESS_CHAIN || op == MALI_SPV_OP_IN_BOUNDS_ACCESS_CHAIN) && wc == 5) {
            uint32_t ty = iw[1], res = iw[2], base = iw[3], cidx = iw[4];
            if (ty < bound && res < bound && base < bound && cidx < bound &&
                ptr_is_fn[ty] && cand_vec[base] && u32_ok[cidx] && !id_has_dec[res] && val_type[base] < bound) {
                uint32_t vty = ptr_pointee[val_type[base]];
                if (vty < bound && u32_val[cidx] < vec_len[vty] && ptr_pointee[ty] == vec_elem[vty]) {
                    ac_base[res] = base;
                    ac_idx[res] = u32_val[cidx];
                }
            }
        }
    }

    MaliSpvVecCheckCtx vctx = {.cand_vec = cand_vec, .ac_base = ac_base, .bound = bound};
    in_fn = false;
    for (size_t i = 0; i < ninsns; i++) {
        uint16_t op = insns[i].op;
        uint32_t wc = insns[i].wc;
        uint32_t *iw = w + insns[i].offset;
        if (op == MALI_SPV_OP_FUNCTION) { in_fn = true; continue; }
        if (op == MALI_SPV_OP_FUNCTION_END) { in_fn = false; continue; }
        if (!in_fn || op == MALI_SPV_OP_VARIABLE) continue;
        vctx.cur_op = op;
        vctx.cur_res = mali_spv_result_id(iw, wc, op, NULL);
        vctx.cur_volatile = (op == MALI_SPV_OP_LOAD && wc >= 5 && (iw[4] & 1u)) ||
                            (op == MALI_SPV_OP_STORE && wc >= 4 && (iw[3] & 1u));
        if (op == MALI_SPV_OP_STORE && wc >= 3 && iw[1] < bound && cand_vec[iw[1]]) {
            uint32_t pty = val_type[iw[1]];
            uint32_t vty = pty < bound ? ptr_pointee[pty] : 0;
            if (iw[2] >= bound || !vty || val_type[iw[2]] != vty || vctx.cur_volatile) cand_vec[iw[1]] = 0;
        } else if (op == MALI_SPV_OP_LOAD && wc >= 4 && iw[3] < bound && ac_base[iw[3]] && iw[2] < bound && id_has_dec[iw[2]]) {
            if (ac_base[iw[3]] < bound) cand_vec[ac_base[iw[3]]] = 0;
        }
        if (!mali_spv_visit_fn_operands(iw, wc, int_val_words, bound, mali_spv_vec_check_cb, &vctx)) {
            ok = false;
            break;
        }
    }

    size_t rw_count = 0;
    if (ok) {
        in_fn = false;
        size_t ndirty = 0;
        for (size_t i = 0; i < ninsns; i++) {
            uint16_t op = insns[i].op;
            uint32_t wc = insns[i].wc;
            uint32_t *iw = w + insns[i].offset;
            if (op == MALI_SPV_OP_FUNCTION) {
                in_fn = true;
                ndirty = 0;
                continue;
            }
            if (!in_fn) continue;
            if (op == MALI_SPV_OP_LABEL || op == MALI_SPV_OP_FUNCTION_CALL || op == MALI_SPV_OP_FUNCTION_END) {
                for (size_t d = 0; d < ndirty; d++) {
                    uint32_t vid = dirty_vecs[d];
                    cur_vec_val[vid] = 0;
                    in_dirty[vid] = 0;
                }
                ndirty = 0;
                if (op == MALI_SPV_OP_FUNCTION_END) in_fn = false;
                continue;
            }
            if (op == MALI_SPV_OP_STORE && wc >= 3 && iw[1] < bound && cand_vec[iw[1]]) {
                uint32_t v = iw[1];
                if (!in_dirty[v]) { in_dirty[v] = 1; dirty_vecs[ndirty++] = v; }
                cur_vec_val[v] = iw[2];
            } else if (op == MALI_SPV_OP_LOAD && wc >= 4 && iw[3] < bound) {
                uint32_t base = ac_base[iw[3]];
                if (base && base < bound && cand_vec[base]) {
                    if (cur_vec_val[base]) {
                        rw_vec[i] = cur_vec_val[base];
                        rw_comp[i] = ac_idx[iw[3]];
                        rw_count++;
                        vec_promoted_any[base] = 1;
                    } else {
                        vec_unpromoted_load[base] = 1;
                    }
                }
            }
        }
    }

    bool changed = false;
    if (ok && rw_count > 0) {
        for (uint32_t v = 0; v < bound; v++) {
            if (cand_vec[v] && vec_promoted_any[v] && !vec_unpromoted_load[v]) {
                dead_vec[v] = 1;
                dead_vec_id[v] = 1;
            }
        }
        for (uint32_t r = 0; r < bound; r++) {
            if (ac_base[r] && ac_base[r] < bound && dead_vec[ac_base[r]]) dead_vec_id[r] = 1;
        }
        uint32_t *nw_buf = (uint32_t *)malloc((nw + rw_count) * sizeof(uint32_t));
        if (nw_buf) {
            memcpy(nw_buf, w, 5 * sizeof(uint32_t));
            size_t o = 5;
            for (size_t i = 0; i < ninsns; i++) {
                uint16_t op = insns[i].op;
                uint32_t wc = insns[i].wc;
                uint32_t *iw = w + insns[i].offset;
                if (rw_vec[i]) {
                    nw_buf[o++] = (5u << 16) | MALI_SPV_OP_COMPOSITE_EXTRACT;
                    nw_buf[o++] = iw[1];
                    nw_buf[o++] = iw[2];
                    nw_buf[o++] = rw_vec[i];
                    nw_buf[o++] = rw_comp[i];
                } else if (op == MALI_SPV_OP_VARIABLE && wc >= 4 && iw[2] < bound && dead_vec[iw[2]]) {
                } else if (op == MALI_SPV_OP_STORE && wc >= 3 && iw[1] < bound && dead_vec[iw[1]]) {
                } else if ((op == MALI_SPV_OP_ACCESS_CHAIN || op == MALI_SPV_OP_IN_BOUNDS_ACCESS_CHAIN) &&
                           wc >= 4 && iw[2] < bound && dead_vec_id[iw[2]]) {
                } else if ((op == MALI_SPV_OP_NAME || op == MALI_SPV_OP_MEMBER_NAME || op == MALI_SPV_OP_DECORATE ||
                            op == MALI_SPV_OP_MEMBER_DECORATE || op == MALI_SPV_OP_DECORATE_ID ||
                            op == MALI_SPV_OP_DECORATE_STRING || op == MALI_SPV_OP_MEMBER_DECORATE_STRING) &&
                           wc >= 2 && iw[1] < bound && dead_vec_id[iw[1]]) {
                } else {
                    memcpy(nw_buf + o, iw, wc * sizeof(uint32_t));
                    o += wc;
                }
            }
            free(w);
            *io_words = nw_buf;
            *io_nwords = o;
            changed = true;
        }
    }

    free(insns); free(ty_scalar); free(vec_elem); free(vec_len); free(ptr_pointee); free(ptr_is_fn);
    free(u32_ty); free(u32_ok); free(u32_val); free(int_ty_words); free(int_val_words);
    free(val_type); free(id_has_dec); free(cand_vec); free(ac_base); free(ac_idx);
    free(cur_vec_val); free(dirty_vecs); free(in_dirty); free(rw_vec); free(rw_comp);
    free(vec_promoted_any); free(vec_unpromoted_load); free(dead_vec); free(dead_vec_id);
    return changed;
}

static inline void mali_spv_prune_decorations(uint32_t *w, MaliSpvInsn *insns, size_t ninsns,
                                              uint32_t bound, const uint8_t *dead_id) {
    for (size_t i = 0; i < ninsns; i++) {
        uint16_t op = insns[i].op;
        uint32_t wc = insns[i].wc;
        uint32_t *iw = w + insns[i].offset;
        if ((op == MALI_SPV_OP_NAME || op == MALI_SPV_OP_MEMBER_NAME || op == MALI_SPV_OP_DECORATE ||
             op == MALI_SPV_OP_MEMBER_DECORATE || op == MALI_SPV_OP_DECORATE_ID ||
             op == MALI_SPV_OP_DECORATE_STRING || op == MALI_SPV_OP_MEMBER_DECORATE_STRING) &&
            wc >= 2 && iw[1] < bound && dead_id[iw[1]]) {
            insns[i].dead = 1;
        } else if (op == MALI_SPV_OP_GROUP_DECORATE && wc >= 2) {
            uint32_t dst = 2;
            for (uint32_t k = 2; k < wc; k++) {
                if (iw[k] >= bound || !dead_id[iw[k]]) iw[dst++] = iw[k];
            }
            if (dst == 2) insns[i].dead = 1;
            else if (dst != wc) {
                insns[i].wc = dst;
                iw[0] = (dst << 16) | MALI_SPV_OP_GROUP_DECORATE;
            }
        }
    }
}

static inline bool spirv_mali_cleanup(uint32_t **io_words, size_t *io_nwords) {
    if (!io_words || !*io_words || !io_nwords) return false;
    bool vec_promoted = spirv_mali_promote_vec_temps(io_words, io_nwords);
    uint32_t *w = *io_words;
    size_t nw = *io_nwords, ninsns = 0;
    if (!mali_spv_validate_stream(w, nw, &ninsns) || ninsns == 0) return vec_promoted;

    uint32_t bound = w[3];
    MaliSpvInsn *insns = (MaliSpvInsn *)calloc(ninsns, sizeof *insns);
    uint32_t *val_type = (uint32_t *)calloc(bound, sizeof *val_type);
    int32_t *def_insn = (int32_t *)malloc(bound * sizeof *def_insn);
    uint8_t *id_has_decorate = (uint8_t *)calloc(bound, 1);
    uint8_t *var_pinned = (uint8_t *)calloc(bound, 1);
    uint8_t *var_scalar_clean = (uint8_t *)calloc(bound, 1);
    uint32_t *var_pointee_ty = (uint32_t *)calloc(bound, sizeof *var_pointee_ty);
    uint8_t *ty_is_scalar = (uint8_t *)calloc(bound, 1);
    uint32_t *ptr_pointee = (uint32_t *)calloc(bound, sizeof *ptr_pointee);
    uint8_t *ptr_safe_load = (uint8_t *)calloc(bound, 1);
    uint8_t *id_safe_load_sc = (uint8_t *)calloc(bound, 1);
    uint8_t *int_ty_words = (uint8_t *)calloc(bound, 1);
    uint8_t *int_val_words = (uint8_t *)calloc(bound, 1);
    uint32_t *alias_id = (uint32_t *)calloc(bound, sizeof *alias_id);
    uint32_t *cur_val = (uint32_t *)calloc(bound, sizeof *cur_val);
    int32_t *last_store = (int32_t *)malloc(bound * sizeof *last_store);
    uint32_t *use_count = (uint32_t *)calloc(bound, sizeof *use_count);
    uint32_t *var_read_count = (uint32_t *)calloc(bound, sizeof *var_read_count);
    uint32_t *var_store_count = (uint32_t *)calloc(bound, sizeof *var_store_count);
    uint8_t *dead_id = (uint8_t *)calloc(bound, 1);
    uint32_t *dirty_vars = (uint32_t *)malloc(bound * sizeof *dirty_vars);
    uint8_t *in_dirty = (uint8_t *)calloc(bound, 1);
    uint8_t *def_in_preamble = (uint8_t *)calloc(bound, 1);
    if (!insns || !val_type || !def_insn || !id_has_decorate || !var_pinned || !var_scalar_clean ||
        !var_pointee_ty || !ty_is_scalar || !ptr_pointee || !ptr_safe_load || !id_safe_load_sc ||
        !int_ty_words || !int_val_words || !alias_id || !cur_val || !last_store || !use_count ||
        !var_read_count || !var_store_count || !dead_id || !dirty_vars || !in_dirty || !def_in_preamble) {
        free(insns); free(val_type); free(def_insn); free(id_has_decorate); free(var_pinned);
        free(var_scalar_clean); free(var_pointee_ty); free(ty_is_scalar); free(ptr_pointee);
        free(ptr_safe_load); free(id_safe_load_sc); free(int_ty_words); free(int_val_words);
        free(alias_id); free(cur_val); free(last_store); free(use_count); free(var_read_count);
        free(var_store_count); free(dead_id); free(dirty_vars); free(in_dirty); free(def_in_preamble);
        return vec_promoted;
    }
    for (uint32_t i = 0; i < bound; i++) {
        def_insn[i] = -1;
        last_store[i] = -1;
    }

    uint32_t glsl_ext_id = 0;
    MaliSpvPinCtx pctx = {.pinned = var_pinned, .bound = bound};
    size_t pos = 5, idx = 0;
    bool in_fn = false, ok = true;
    for (; pos < nw; idx++) {
        uint32_t wc = w[pos] >> 16;
        uint16_t op = (uint16_t)(w[pos] & 0xFFFFu);
        insns[idx].offset = (uint32_t)pos;
        insns[idx].wc = wc;
        insns[idx].op = op;

        if (op == MALI_SPV_OP_EXT_INST_IMPORT && wc >= 6) {
            if (!strncmp((const char *)&w[pos + 2], "GLSL.std.450", 13)) glsl_ext_id = w[pos + 1];
        } else if (op == MALI_SPV_OP_TYPE_BOOL && wc >= 2 && w[pos + 1] < bound) {
            ty_is_scalar[w[pos + 1]] = 1;
        } else if (op == MALI_SPV_OP_TYPE_INT && wc >= 4 && w[pos + 1] < bound) {
            ty_is_scalar[w[pos + 1]] = 1;
            int_ty_words[w[pos + 1]] = w[pos + 2] > 32 ? 2u : 1u;
        } else if (op == MALI_SPV_OP_TYPE_FLOAT && wc >= 3 && w[pos + 1] < bound) {
            ty_is_scalar[w[pos + 1]] = 1;
        } else if (op == MALI_SPV_OP_TYPE_POINTER && wc >= 4 && w[pos + 1] < bound) {
            ptr_pointee[w[pos + 1]] = w[pos + 3];
            uint32_t sc = w[pos + 2];
            if (sc == 0 || sc == 1 || sc == 2 || sc == 6 || sc == 7 || sc == 9) ptr_safe_load[w[pos + 1]] = 1;
        } else if (op == MALI_SPV_OP_DECORATE || op == MALI_SPV_OP_DECORATE_ID || op == MALI_SPV_OP_DECORATE_STRING) {
            if (wc >= 2 && w[pos + 1] < bound) id_has_decorate[w[pos + 1]] = 1;
        } else if (op == MALI_SPV_OP_GROUP_DECORATE) {
            for (uint32_t k = 2; k < wc; k++)
                if (w[pos + k] < bound) id_has_decorate[w[pos + k]] = 1;
        }

        if (op == MALI_SPV_OP_FUNCTION) in_fn = true;
        else if (op == MALI_SPV_OP_FUNCTION_END) in_fn = false;

        uint32_t res_ty = 0, res_id = mali_spv_result_id(w + pos, wc, op, &res_ty);
        if (res_id && res_id < bound && res_ty < bound) {
            val_type[res_id] = res_ty;
            def_insn[res_id] = (int32_t)idx;
            if (!in_fn) def_in_preamble[res_id] = 1;
            if (int_ty_words[res_ty]) int_val_words[res_id] = int_ty_words[res_ty];
            if (ptr_safe_load[res_ty]) id_safe_load_sc[res_id] = 1;
        } else if (res_id && res_id < bound) {
            def_insn[res_id] = (int32_t)idx;
            if (!in_fn) def_in_preamble[res_id] = 1;
        }
        if (in_fn && op != MALI_SPV_OP_FUNCTION) {
            if (op == MALI_SPV_OP_LOAD) {
                if (wc >= 5 && (w[pos + 4] & 1u) && w[pos + 3] < bound) var_pinned[w[pos + 3]] = 1;
            } else if (op == MALI_SPV_OP_STORE) {
                if (wc >= 4 && (w[pos + 3] & 1u) && w[pos + 1] < bound) var_pinned[w[pos + 1]] = 1;
                if (wc >= 3 && w[pos + 2] < bound) var_pinned[w[pos + 2]] = 1;
            } else if (op != MALI_SPV_OP_VARIABLE) {
                if (!mali_spv_visit_fn_operands(w + pos, wc, int_val_words, bound, mali_spv_pin_var_cb, &pctx)) {
                    ok = false;
                    break;
                }
            }
        }
        pos += wc;
    }

    bool any_change = vec_promoted;
    if (ok) {
        for (size_t i = 0; i < ninsns; i++) {
            if (insns[i].op != MALI_SPV_OP_VARIABLE || insns[i].wc < 4) continue;
            uint32_t *iw = w + insns[i].offset;
            if (iw[3] != 7u) continue;  // StorageClassFunction
            uint32_t ty = iw[1], vid = iw[2];
            if (ty < bound && vid < bound && ptr_pointee[ty] < bound && ty_is_scalar[ptr_pointee[ty]] && !var_pinned[vid]) {
                var_scalar_clean[vid] = 1;
                var_pointee_ty[vid] = ptr_pointee[ty];
            }
        }

        // Strictly intra-block scalar forwarding + OpBitcast round-trip/identity folding.
        MaliSpvAliasCtx actx = {.alias_id = alias_id, .bound = bound};
        size_t ndirty = 0;
        in_fn = false;
        size_t cur_blk_start = 0;
        for (size_t i = 0; i < ninsns; i++) {
            uint16_t op = insns[i].op;
            uint32_t wc = insns[i].wc;
            uint32_t *iw = w + insns[i].offset;
            if (op == MALI_SPV_OP_FUNCTION) {
                in_fn = true;
                ndirty = 0;
                cur_blk_start = i + 1;
                continue;
            }
            if (!in_fn) continue;
            if (op == MALI_SPV_OP_LABEL || op == MALI_SPV_OP_FUNCTION_CALL || op == MALI_SPV_OP_FUNCTION_END) {
                for (size_t d = 0; d < ndirty; d++) {
                    uint32_t vid = dirty_vars[d];
                    cur_val[vid] = 0;
                    last_store[vid] = -1;
                    in_dirty[vid] = 0;
                }
                ndirty = 0;
                if (op == MALI_SPV_OP_LABEL) {
                    cur_blk_start = i;
                    continue;
                }
                if (op == MALI_SPV_OP_FUNCTION_END) {
                    in_fn = false;
                    continue;
                }
                // OpFunctionCall still remaps its operand IDs after resetting forwarding state.
            }
            mali_spv_visit_fn_operands(iw, wc, int_val_words, bound, mali_spv_remap_cb, &actx);

            if (op == MALI_SPV_OP_STORE && wc >= 3) {
                uint32_t ptr = iw[1], val = iw[2];
                if (ptr < bound && var_scalar_clean[ptr] && val < bound && val_type[val] == var_pointee_ty[ptr]) {
                    if (last_store[ptr] >= 0) {
                        insns[last_store[ptr]].dead = 1;
                        any_change = true;
                    }
                    if (!in_dirty[ptr]) { in_dirty[ptr] = 1; dirty_vars[ndirty++] = ptr; }
                    cur_val[ptr] = val;
                    last_store[ptr] = (int32_t)i;
                }
            } else if (op == MALI_SPV_OP_LOAD && wc >= 4) {
                uint32_t ty = iw[1], res = iw[2], ptr = iw[3];
                if (ptr < bound && var_scalar_clean[ptr] && res < bound) {
                    uint32_t fwd = cur_val[ptr] ? mali_spv_resolve(alias_id, bound, cur_val[ptr]) : 0;
                    bool same_blk_dom = fwd && fwd < bound && val_type[fwd] == ty && !id_has_decorate[res] &&
                                        (def_in_preamble[fwd] || def_insn[fwd] < 0 || (size_t)def_insn[fwd] >= cur_blk_start);
                    if (same_blk_dom) {
                        alias_id[res] = fwd;
                        insns[i].dead = 1;
                        dead_id[res] = 1;
                        any_change = true;
                    } else {
                        if (!in_dirty[ptr]) { in_dirty[ptr] = 1; dirty_vars[ndirty++] = ptr; }
                        cur_val[ptr] = res;
                        last_store[ptr] = -1;
                    }
                }
            } else if (op == MALI_SPV_OP_BITCAST && wc >= 4) {
                uint32_t ty = iw[1], res = iw[2], src = iw[3];
                if (res < bound && src < bound && !id_has_decorate[res]) {
                    if (val_type[src] == ty) {
                        alias_id[res] = src;
                        insns[i].dead = 1;
                        dead_id[res] = 1;
                        any_change = true;
                    } else if (def_insn[src] >= 0 && !insns[def_insn[src]].dead &&
                               insns[def_insn[src]].op == MALI_SPV_OP_BITCAST) {
                        uint32_t inner = mali_spv_resolve(alias_id, bound, w[insns[def_insn[src]].offset + 3]);
                        if (inner < bound && val_type[inner] == ty) {
                            alias_id[res] = inner;
                            insns[i].dead = 1;
                            dead_id[res] = 1;
                            any_change = true;
                        }
                    }
                }
            }
        }

        // Propagate any alias references inside OpPhi back-edges or cross-block uses across the module.
        in_fn = false;
        for (size_t i = 0; i < ninsns; i++) {
            if (insns[i].dead) continue;
            if (insns[i].op == MALI_SPV_OP_FUNCTION) { in_fn = true; continue; }
            if (insns[i].op == MALI_SPV_OP_FUNCTION_END) { in_fn = false; continue; }
            if (in_fn) mali_spv_visit_fn_operands(w + insns[i].offset, insns[i].wc, int_val_words, bound, mali_spv_remap_cb, &actx);
        }

        // Iterative fixpoint pure-SSA DCE + write-only scalar StorageClassFunction variable elimination.
        for (int iter = 0; iter < 64; iter++) {
            memset(use_count, 0, bound * sizeof *use_count);
            memset(var_read_count, 0, bound * sizeof *var_read_count);
            memset(var_store_count, 0, bound * sizeof *var_store_count);
            MaliSpvUseCtx uctx = {
                .use_count = use_count,
                .var_read_count = var_read_count,
                .var_store_count = var_store_count,
                .var_scalar_clean = var_scalar_clean,
                .bound = bound,
            };
            in_fn = false;
            for (size_t i = 0; i < ninsns; i++) {
                if (insns[i].dead) continue;
                uint16_t op = insns[i].op;
                if (op == MALI_SPV_OP_FUNCTION) { in_fn = true; continue; }
                if (op == MALI_SPV_OP_FUNCTION_END) { in_fn = false; continue; }
                if (!in_fn || op == MALI_SPV_OP_VARIABLE) continue;
                uctx.cur_op = op;
                mali_spv_visit_fn_operands(w + insns[i].offset, insns[i].wc, int_val_words, bound, mali_spv_use_cb, &uctx);
            }
            bool round_changed = false;
            in_fn = false;
            for (size_t i = 0; i < ninsns; i++) {
                if (insns[i].dead) continue;
                uint16_t op = insns[i].op;
                uint32_t *iw = w + insns[i].offset;
                if (op == MALI_SPV_OP_FUNCTION) { in_fn = true; continue; }
                if (op == MALI_SPV_OP_FUNCTION_END) { in_fn = false; continue; }
                if (!in_fn) continue;
                if (op == MALI_SPV_OP_STORE && insns[i].wc >= 3) {
                    uint32_t ptr = iw[1];
                    if (ptr < bound && var_scalar_clean[ptr] && var_read_count[ptr] == 0) {
                        insns[i].dead = 1;
                        round_changed = true;
                    }
                } else if (op == MALI_SPV_OP_VARIABLE && insns[i].wc >= 4 && iw[3] == 7u) {
                    uint32_t vid = iw[2];
                    if (vid < bound && var_scalar_clean[vid] && use_count[vid] == 0 &&
                        var_read_count[vid] == 0 && var_store_count[vid] == 0) {
                        insns[i].dead = 1;
                        dead_id[vid] = 1;
                        round_changed = true;
                    }
                } else if (mali_spv_is_pure_dce_op(op, iw, insns[i].wc, glsl_ext_id, id_safe_load_sc, bound)) {
                    uint32_t rid = mali_spv_result_id(iw, insns[i].wc, op, NULL);
                    if (rid && rid < bound && use_count[rid] == 0) {
                        insns[i].dead = 1;
                        dead_id[rid] = 1;
                        round_changed = true;
                    }
                }
            }
            if (!round_changed) break;
            any_change = true;
        }

        if (any_change) {
            mali_spv_prune_decorations(w, insns, ninsns, bound, dead_id);
            size_t out_pos = 5;
            for (size_t i = 0; i < ninsns; i++) {
                if (insns[i].dead) continue;
                memmove(w + out_pos, w + insns[i].offset, insns[i].wc * sizeof(uint32_t));
                out_pos += insns[i].wc;
            }
            *io_nwords = out_pos;
        }
    }

    free(insns); free(val_type); free(def_insn); free(id_has_decorate); free(var_pinned);
    free(var_scalar_clean); free(var_pointee_ty); free(ty_is_scalar); free(ptr_pointee);
    free(ptr_safe_load); free(id_safe_load_sc); free(int_ty_words); free(int_val_words);
    free(alias_id); free(cur_val); free(last_store); free(use_count); free(var_read_count);
    free(var_store_count); free(dead_id); free(dirty_vars); free(in_dirty); free(def_in_preamble);
    return any_change;
}

static inline bool mali_spv_is_taint_op(uint16_t op) {
    switch (op) {
    case 87: case 89: case 91: case 93:
    case 305: case 307: case 309: case 311:
    case MALI_SPV_OP_IMAGE_QUERY_LOD:
    case MALI_SPV_OP_CONTROL_BARRIER:
    case MALI_SPV_OP_IS_HELPER_INVOCATION:
    case MALI_SPV_OP_IS_HELPER_INVOCATION_EXT:
    case 4431:  // OpGroupNonUniformRotateKHR
    case 5296:  // OpGroupNonUniformPartitionNV
        return true;
    default:
        // All derivative ops (207..215), OpGroup* (261..271), GroupNonUniform* (333..366), Group*AMD (5000..5007).
        return (op >= 207 && op <= 215) || (op >= 261 && op <= 271) ||
               (op >= 333 && op <= 366) || (op >= 5000 && op <= 5007);
    }
}

typedef struct {
    uint32_t label_id;
    size_t start_insn;
    size_t end_insn;  // exclusive
    int succ[64];
    int nsucc;
    bool has_dyn_succ;
    bool has_local_taint;
} MaliSpvBlk;

typedef struct {
    size_t start_insn;
    size_t end_insn;
    uint32_t fn_id;
    bool has_taint;
    int blk_start, blk_end;
} MaliSpvFn;

typedef struct {
    const uint8_t *forbidden_id;
    uint32_t bound;
    bool hit;
} MaliSpvForbiddenCtx;

static inline void mali_spv_check_forbidden_cb(uint32_t *slot, uint32_t idx_in_insn, void *user) {
    (void)idx_in_insn;
    MaliSpvForbiddenCtx *ctx = (MaliSpvForbiddenCtx *)user;
    if (*slot && *slot < ctx->bound && ctx->forbidden_id[*slot]) ctx->hit = true;
}

static inline bool spirv_mali_demote_to_terminate(uint32_t **io_words, size_t *io_nwords) {
    if (!io_words || !*io_words || !io_nwords) return false;
    uint32_t *w = *io_words;
    size_t nw = *io_nwords, ninsns = 0;
    if (!mali_spv_validate_stream(w, nw, &ninsns) || ninsns == 0) return false;
    if (w[1] < 0x00010600u) return false;  // OpTerminateInvocation requires SPIR-V 1.6 (Vulkan 1.3)

    bool is_fragment = false;
    uint32_t entry_fn_id = 0;
    size_t demote_total = 0;
    uint32_t bound = w[3];
    for (size_t pos = 5; pos < nw;) {
        uint32_t wc = w[pos] >> 16;
        uint16_t op = (uint16_t)(w[pos] & 0xFFFFu);
        if (op == MALI_SPV_OP_ENTRY_POINT && wc >= 4 && w[pos + 1] == 4u) {
            is_fragment = true;
            entry_fn_id = w[pos + 2];
        }
        if (op == MALI_SPV_OP_DEMOTE_TO_HELPER) demote_total++;
        pos += wc;
    }
    if (!is_fragment || !entry_fn_id || entry_fn_id >= bound || demote_total == 0) return false;

    MaliSpvInsn *insns = (MaliSpvInsn *)calloc(ninsns, sizeof *insns);
    int32_t *lbl_to_blk = (int32_t *)malloc(bound * sizeof *lbl_to_blk);
    int32_t *fn_id_to_idx = (int32_t *)malloc(bound * sizeof *fn_id_to_idx);
    uint8_t *int_ty_words = (uint8_t *)calloc(bound, 1);
    uint8_t *int_val_words = (uint8_t *)calloc(bound, 1);
    uint8_t *const_bool_val = (uint8_t *)calloc(bound, 1);
    uint8_t *forbidden = (uint8_t *)calloc(bound, 1);
    uint8_t *dead_id = (uint8_t *)calloc(bound, 1);
    if (!insns || !lbl_to_blk || !fn_id_to_idx || !int_ty_words || !int_val_words || !const_bool_val || !forbidden || !dead_id) {
        free(insns); free(lbl_to_blk); free(fn_id_to_idx); free(int_ty_words); free(int_val_words); free(const_bool_val); free(forbidden); free(dead_id);
        return false;
    }
    for (uint32_t i = 0; i < bound; i++) { lbl_to_blk[i] = -1; fn_id_to_idx[i] = -1; }

    size_t nfn = 0, nblk = 0;
    for (size_t pos = 5, idx = 0; pos < nw; idx++, pos += (w[pos] >> 16)) {
        uint32_t wc = w[pos] >> 16;
        uint16_t op = (uint16_t)(w[pos] & 0xFFFFu);
        insns[idx].offset = (uint32_t)pos;
        insns[idx].wc = wc;
        insns[idx].op = op;
        if (op == MALI_SPV_OP_TYPE_INT && wc >= 4 && w[pos + 1] < bound) int_ty_words[w[pos + 1]] = w[pos + 2] > 32 ? 2u : 1u;
        if (op == MALI_SPV_OP_CONSTANT_FALSE && wc >= 3 && w[pos + 2] < bound) const_bool_val[w[pos + 2]] = 1u;
        if (op == MALI_SPV_OP_CONSTANT_TRUE && wc >= 3 && w[pos + 2] < bound) const_bool_val[w[pos + 2]] = 2u;
        uint32_t rty = 0, rid = mali_spv_result_id(w + pos, wc, op, &rty);
        if (rid && rid < bound && rty < bound && int_ty_words[rty]) int_val_words[rid] = int_ty_words[rty];
        if (op == MALI_SPV_OP_FUNCTION) nfn++;
        else if (op == MALI_SPV_OP_LABEL) nblk++;
    }
    if (!nfn || !nblk) {
        free(insns); free(lbl_to_blk); free(fn_id_to_idx); free(int_ty_words); free(int_val_words); free(const_bool_val); free(forbidden); free(dead_id);
        return false;
    }

    MaliSpvFn *fns = (MaliSpvFn *)calloc(nfn, sizeof *fns);
    MaliSpvBlk *blks = (MaliSpvBlk *)calloc(nblk, sizeof *blks);
    int *bfs_q = (int *)malloc(nblk * sizeof *bfs_q);
    uint8_t *bfs_vis = (uint8_t *)calloc(nblk, 1);
    if (!fns || !blks || !bfs_q || !bfs_vis) {
        free(fns); free(blks); free(bfs_q); free(bfs_vis);
        free(insns); free(lbl_to_blk); free(fn_id_to_idx); free(int_ty_words); free(int_val_words); free(const_bool_val); free(forbidden); free(dead_id);
        return false;
    }

    int next_fn = 0, cur_fn = -1, cur_blk = -1;
    for (size_t i = 0; i < ninsns; i++) {
        uint16_t op = insns[i].op;
        uint32_t wc = insns[i].wc;
        uint32_t *iw = w + insns[i].offset;
        if (op == MALI_SPV_OP_FUNCTION) {
            cur_fn = next_fn++;
            fns[cur_fn].start_insn = i;
            fns[cur_fn].fn_id = wc >= 3 ? iw[2] : 0;
            fns[cur_fn].blk_start = cur_blk + 1;
            if (fns[cur_fn].fn_id < bound) fn_id_to_idx[fns[cur_fn].fn_id] = cur_fn;
        } else if (op == MALI_SPV_OP_FUNCTION_END && cur_fn >= 0) {
            if (cur_blk >= fns[cur_fn].blk_start) blks[cur_blk].end_insn = i;
            fns[cur_fn].end_insn = i + 1;
            fns[cur_fn].blk_end = cur_blk + 1;
            cur_fn = -1;
        } else if (op == MALI_SPV_OP_LABEL && cur_fn >= 0 && wc >= 2) {
            if (cur_blk >= fns[cur_fn].blk_start) blks[cur_blk].end_insn = i;
            cur_blk++;
            blks[cur_blk].label_id = iw[1];
            blks[cur_blk].start_insn = i;
            if (iw[1] < bound) lbl_to_blk[iw[1]] = cur_blk;
        }
        if (cur_blk >= 0 && cur_fn >= 0 && mali_spv_is_taint_op(op)) {
            blks[cur_blk].has_local_taint = true;
            fns[cur_fn].has_taint = true;
        }
    }

    // Transitive call-graph propagation for has_taint across functions.
    for (bool chg = true; chg;) {
        chg = false;
        for (size_t f = 0; f < nfn; f++) {
            for (size_t i = fns[f].start_insn; i < fns[f].end_insn; i++) {
                if (insns[i].op != MALI_SPV_OP_FUNCTION_CALL || insns[i].wc < 4) continue;
                uint32_t callee = w[insns[i].offset + 3];
                if (callee < bound && fn_id_to_idx[callee] >= 0 && fns[fn_id_to_idx[callee]].has_taint && !fns[f].has_taint) {
                    fns[f].has_taint = true;
                    chg = true;
                }
            }
        }
    }

    // Build block successors (pruning statically dead constant-bool branch edges such as do {} while (false)).
    for (size_t b = 0; b < nblk; b++) {
        for (size_t i = blks[b].start_insn; i < blks[b].end_insn; i++) {
            uint16_t op = insns[i].op;
            uint32_t wc = insns[i].wc;
            uint32_t *iw = w + insns[i].offset;
            if (op == MALI_SPV_OP_FUNCTION_CALL && wc >= 4 && iw[3] < bound && fn_id_to_idx[iw[3]] >= 0 &&
                fns[fn_id_to_idx[iw[3]]].has_taint) {
                blks[b].has_local_taint = true;
            }
            if (op == MALI_SPV_OP_BRANCH && wc >= 2 && iw[1] < bound && lbl_to_blk[iw[1]] >= 0) {
                if (blks[b].nsucc < 64) blks[b].succ[blks[b].nsucc++] = lbl_to_blk[iw[1]];
                else blks[b].has_dyn_succ = true;
            } else if (op == MALI_SPV_OP_BRANCH_CONDITIONAL && wc >= 4) {
                uint8_t cb = iw[1] < bound ? const_bool_val[iw[1]] : 0;
                int k0 = (cb == 1u) ? 3 : 2;
                int k1 = (cb == 2u) ? 2 : 3;
                for (int k = k0; k <= k1; k++) {
                    if (iw[k] < bound && lbl_to_blk[iw[k]] >= 0) {
                        if (blks[b].nsucc < 64) blks[b].succ[blks[b].nsucc++] = lbl_to_blk[iw[k]];
                        else blks[b].has_dyn_succ = true;
                    }
                }
            } else if (op == MALI_SPV_OP_SWITCH && wc >= 3) {
                if (iw[2] < bound && lbl_to_blk[iw[2]] >= 0 && blks[b].nsucc < 64) blks[b].succ[blks[b].nsucc++] = lbl_to_blk[iw[2]];
                uint32_t lw = (iw[1] < bound && int_val_words[iw[1]]) ? int_val_words[iw[1]] : 1u;
                for (uint32_t k = 3; k + lw < wc; k += lw + 1u) {
                    uint32_t tgt = iw[k + lw];
                    if (tgt < bound && lbl_to_blk[tgt] >= 0) {
                        if (blks[b].nsucc < 64) blks[b].succ[blks[b].nsucc++] = lbl_to_blk[tgt];
                        else blks[b].has_dyn_succ = true;
                    }
                }
            }
        }
    }

    size_t converted = 0;
    for (size_t f = 0; f < nfn; f++) {
        if (fns[f].fn_id != entry_fn_id) continue;
        for (int b = fns[f].blk_start; b < fns[f].blk_end; b++) {
            size_t demote_idx = (size_t)-1;
            bool has_merge = false;
            for (size_t i = blks[b].start_insn; i < blks[b].end_insn; i++) {
                uint16_t op = insns[i].op;
                if (op == MALI_SPV_OP_LOOP_MERGE || op == MALI_SPV_OP_SELECTION_MERGE) {
                    has_merge = true;
                    break;
                }
                if (op == MALI_SPV_OP_DEMOTE_TO_HELPER && demote_idx == (size_t)-1) demote_idx = i;
            }
            if (has_merge || demote_idx == (size_t)-1 || blks[b].has_dyn_succ) continue;

            // 1. Check same block after demote_idx for downstream taint.
            bool tainted = false;
            for (size_t i = demote_idx + 1; i < blks[b].end_insn; i++) {
                uint16_t op = insns[i].op;
                if (mali_spv_is_taint_op(op)) { tainted = true; break; }
                if (op == MALI_SPV_OP_FUNCTION_CALL && insns[i].wc >= 4) {
                    uint32_t cid = w[insns[i].offset + 3];
                    if (cid >= bound || fn_id_to_idx[cid] < 0 || fns[fn_id_to_idx[cid]].has_taint) { tainted = true; break; }
                }
            }
            if (tainted) continue;

            // 2. BFS successor blocks for downstream taint.
            memset(bfs_vis, 0, nblk);
            int qh = 0, qt = 0;
            for (int s = 0; s < blks[b].nsucc; s++) {
                int sb = blks[b].succ[s];
                if (!bfs_vis[sb]) { bfs_vis[sb] = 1; bfs_q[qt++] = sb; }
            }
            while (qh < qt) {
                int ub = bfs_q[qh++];
                if (blks[ub].has_local_taint || blks[ub].has_dyn_succ) { tainted = true; break; }
                for (int s = 0; s < blks[ub].nsucc; s++) {
                    int vb = blks[ub].succ[s];
                    if (!bfs_vis[vb]) { bfs_vis[vb] = 1; bfs_q[qt++] = vb; }
                }
            }
            if (tainted) continue;

            // 3. Verify truncating blks[b] after demote_idx won't empty ANY OpPhi in the function
            //    or orphan any SSA result ID referenced outside blks[b]'s deleted tail (excluding stripped OpPhi pairs).
            bool safe = true;
            uint32_t blk_lbl = blks[b].label_id;
            for (size_t i = demote_idx + 1; i < blks[b].end_insn; i++) {
                uint32_t rid = mali_spv_result_id(w + insns[i].offset, insns[i].wc, insns[i].op, NULL);
                if (rid && rid < bound) forbidden[rid] = 1;
            }
            for (int sb = fns[f].blk_start; sb < fns[f].blk_end && safe; sb++) {
                if (sb == b) continue;
                for (size_t i = blks[sb].start_insn + 1; i < blks[sb].end_insn; i++) {
                    if (insns[i].dead) continue;
                    if (insns[i].op == MALI_SPV_OP_LINE || insns[i].op == MALI_SPV_OP_NO_LINE || insns[i].op == MALI_SPV_OP_NOP) continue;
                    if (insns[i].op != MALI_SPV_OP_PHI) break;
                    uint32_t *iw = w + insns[i].offset;
                    uint32_t keep_pairs = 0;
                    for (uint32_t k = 3; k + 1 < insns[i].wc; k += 2) {
                        if (iw[k + 1] != blk_lbl) keep_pairs++;
                    }
                    if (keep_pairs == 0) { safe = false; break; }
                }
            }
            if (safe) {
                MaliSpvForbiddenCtx fctx = {.forbidden_id = forbidden, .bound = bound, .hit = false};
                for (size_t i = fns[f].start_insn; i < fns[f].end_insn && !fctx.hit; i++) {
                    if (insns[i].dead) continue;
                    if (i >= demote_idx + 1 && i < blks[b].end_insn) continue;
                    if (insns[i].op == MALI_SPV_OP_PHI) {
                        uint32_t *iw = w + insns[i].offset;
                        for (uint32_t k = 3; k + 1 < insns[i].wc; k += 2) {
                            if (iw[k + 1] != blk_lbl && iw[k] < bound && forbidden[iw[k]]) {
                                fctx.hit = true;
                                break;
                            }
                        }
                        continue;
                    }
                    if (!mali_spv_visit_fn_operands(w + insns[i].offset, insns[i].wc, int_val_words, bound,
                                                    mali_spv_check_forbidden_cb, &fctx)) {
                        fctx.hit = true;
                    }
                }
                if (fctx.hit) safe = false;
            }
            for (size_t i = demote_idx + 1; i < blks[b].end_insn; i++) {
                uint32_t rid = mali_spv_result_id(w + insns[i].offset, insns[i].wc, insns[i].op, NULL);
                if (rid && rid < bound) forbidden[rid] = 0;
            }
            if (!safe) continue;

            // Commit conversion: rewrite OpDemoteToHelperInvocation -> OpTerminateInvocation,
            // truncate block tail, strip incoming (val, blk_lbl) pairs across all OpPhis in fns[f].
            w[insns[demote_idx].offset] = (1u << 16) | MALI_SPV_OP_TERMINATE_INVOCATION;
            insns[demote_idx].op = MALI_SPV_OP_TERMINATE_INVOCATION;
            for (size_t i = demote_idx + 1; i < blks[b].end_insn; i++) {
                insns[i].dead = 1;
                uint32_t rid = mali_spv_result_id(w + insns[i].offset, insns[i].wc, insns[i].op, NULL);
                if (rid && rid < bound) dead_id[rid] = 1;
            }
            for (int sb = fns[f].blk_start; sb < fns[f].blk_end; sb++) {
                if (sb == b) continue;
                for (size_t i = blks[sb].start_insn + 1; i < blks[sb].end_insn; i++) {
                    if (insns[i].dead) continue;
                    if (insns[i].op == MALI_SPV_OP_LINE || insns[i].op == MALI_SPV_OP_NO_LINE || insns[i].op == MALI_SPV_OP_NOP) continue;
                    if (insns[i].op != MALI_SPV_OP_PHI) break;
                    uint32_t *iw = w + insns[i].offset;
                    uint32_t dst = 3;
                    for (uint32_t k = 3; k + 1 < insns[i].wc; k += 2) {
                        if (iw[k + 1] != blk_lbl) {
                            iw[dst] = iw[k];
                            iw[dst + 1] = iw[k + 1];
                            dst += 2;
                        }
                    }
                    if (dst != insns[i].wc) {
                        insns[i].wc = dst;
                        iw[0] = (dst << 16) | MALI_SPV_OP_PHI;
                    }
                }
            }
            blks[b].end_insn = demote_idx + 1;
            blks[b].nsucc = 0;
            converted++;
        }
    }

    if (converted > 0) {
        mali_spv_prune_decorations(w, insns, ninsns, bound, dead_id);
        size_t demote_ext_remaining = 0;
        for (size_t i = 0; i < ninsns; i++) {
            if (!insns[i].dead && (insns[i].op == MALI_SPV_OP_DEMOTE_TO_HELPER ||
                                   insns[i].op == MALI_SPV_OP_IS_HELPER_INVOCATION_EXT)) {
                demote_ext_remaining++;
            }
        }
        if (demote_ext_remaining == 0) {
            for (size_t i = 0; i < ninsns; i++) {
                uint16_t op = insns[i].op;
                uint32_t *iw = w + insns[i].offset;
                if (op == MALI_SPV_OP_CAPABILITY && insns[i].wc >= 2 && iw[1] == MALI_SPV_OP_CAP_DEMOTE) {
                    insns[i].dead = 1;
                } else if (op == MALI_SPV_OP_EXTENSION && insns[i].wc >= 10 &&
                           !memcmp(&iw[1], "SPV_EXT_demote_to_helper_invocation", 36)) {
                    insns[i].dead = 1;
                }
            }
        }
        size_t out_pos = 5;
        for (size_t i = 0; i < ninsns; i++) {
            if (insns[i].dead) continue;
            memmove(w + out_pos, w + insns[i].offset, insns[i].wc * sizeof(uint32_t));
            out_pos += insns[i].wc;
        }
        *io_nwords = out_pos;
    }

    free(fns); free(blks); free(bfs_q); free(bfs_vis);
    free(insns); free(lbl_to_blk); free(fn_id_to_idx); free(int_ty_words); free(int_val_words); free(const_bool_val); free(forbidden); free(dead_id);
    return converted > 0;
}
