/* Authored NV40 programs. No captured guest shader bytes or GPU inputs. */
#include "rsx_fp_decompiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <d3dcompiler.h>
#endif

#define REQUIRE(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); \
} } while (0)

static void bound_word(u8* destination, u32 value)
{
    value = (value << 16) | (value >> 16);
    destination[0] = (u8)(value >> 24);
    destination[1] = (u8)(value >> 16);
    destination[2] = (u8)(value >> 8);
    destination[3] = (u8)value;
}

/* The 193/210-instruction cases have 64 constants, the last past byte 4096.
 * The final instruction samples unit 7, including after byte 4096 for 210+.
 * The 1024/1025 cases place END exactly at/past the 16 KiB boundary. */
static u32 bound_program(u8* bytes, unsigned instructions, unsigned constants,
                         u32* last_constant)
{
    u32 offset = 0;
    for (unsigned i = 0; i < instructions; ++i) {
        int constant = constants && i && i <= 192 && i % 3 == 0;
        int last = i + 1 == instructions;
        bound_word(bytes + offset, (last ? 0x170e1e01u : 0x01001e00u));
        bound_word(bytes + offset + 4, 0x1c9dc000u | (constant ? 2u : 1u));
        bound_word(bytes + offset + 8, 0);
        bound_word(bytes + offset + 12, 0);
        offset += 16;
        if (constant) {
            *last_constant = offset;
            for (unsigned lane = 0; lane < 4; ++lane)
                bound_word(bytes + offset + lane * 4, 0x3f000000u + i + lane);
            offset += 16;
        }
    }
    return offset;
}

static void bound_compile_hlsl(const char* hlsl)
{
#ifdef _WIN32
    ID3DBlob* shader = NULL;
    ID3DBlob* errors = NULL;
    HRESULT result = D3DCompile(hlsl, strlen(hlsl), "authored-long-fp", NULL,
        NULL, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &shader, &errors);
    if (FAILED(result) && errors)
        fprintf(stderr, "%s\n", (const char*)errors->lpVtbl->GetBufferPointer(errors));
    if (errors) errors->lpVtbl->Release(errors);
    if (shader) shader->lpVtbl->Release(shader);
    REQUIRE(SUCCEEDED(result));
#else
    (void)hlsl;
#endif
}

void check_fp_program_bound(u32 bound)
{
    static u8 program[16384 + 16];
    static u8 canonical[16384];
    static char hlsl[256u * 1024u];
    static rsx_fp_constant_block block;
    const unsigned counts[] = {193, 210, 1024};
    for (unsigned case_index = 0; case_index < 3; ++case_index) {
        unsigned instructions = counts[case_index];
        unsigned constants = instructions == 1024 ? 0 : 64;
        u32 last_constant = 0;
        u32 length = bound_program(program, instructions, constants, &last_constant);
        REQUIRE(length == (instructions + constants) * 16);
        REQUIRE(rsx_fp_program_size(program, bound) == length);
        REQUIRE(rsx_fp_program_size(program, 4096) != length);
        u64 hash = rsx_fp_structural_hash(program, bound, 0);
        REQUIRE(hash != 0);
        REQUIRE(rsx_fp_structural_hash(program, 4096, 0) == 0);
        REQUIRE(rsx_fp_collect_constants(program, bound, &block) == (int)constants);
        REQUIRE(rsx_fp_collect_constants(program, 4096, &block) == -1);
        REQUIRE(rsx_fp_canonical_program(program, bound, canonical, sizeof canonical) == length);
        REQUIRE(rsx_fp_canonical_program(program, 4096, canonical, sizeof canonical) == 0);
        REQUIRE(rsx_fp_texture_units(program, bound) == (1u << 7));
        if (instructions >= 210)
            REQUIRE(rsx_fp_texture_units(program, 4096) == 0);
        u32 collected = 999;
        REQUIRE(rsx_fp_decompile_buffered_ex(program, bound, 0x40, 0,
            hlsl, sizeof hlsl, &collected) == (int)instructions);
        REQUIRE(collected == constants);
        bound_compile_hlsl(hlsl);
        if (constants) {
            REQUIRE(last_constant >= 4096);
            program[last_constant] ^= 0x01;
            REQUIRE(rsx_fp_structural_hash(program, bound, 0) == hash);
        }
    }
    u32 unused = 0;
    REQUIRE(bound_program(program, 1025, 0, &unused) == 16400);
    REQUIRE(rsx_fp_program_size(program, bound) == 0);
    REQUIRE(rsx_fp_structural_hash(program, bound, 0) == 0);
    REQUIRE(rsx_fp_collect_constants(program, bound, &block) == -1);
    puts("PASS authored 4112/4384/16384-byte programs, constants, prefetch, texture mask, truncation controls");
#ifdef _WIN32
    puts("PASS actual FXC ps_5_0/O3 for all three authored programs");
#endif
}

#ifdef FP_PROGRAM_BOUND
int main(int argc, char** argv)
{
    u32 bound = argc > 1 ? (u32)strtoul(argv[1], NULL, 10) : FP_PROGRAM_BOUND;
    check_fp_program_bound(bound);
    return 0;
}
#endif
