/* Authored MOV input encodings exercise the production fragment decompiler. */
#include "rsx_fp_decompiler.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void word(u8 *p, u32 value) {
    value = (value << 16) | (value >> 16);
    p[0] = (u8)(value >> 24);
    p[1] = (u8)(value >> 16);
    p[2] = (u8)(value >> 8);
    p[3] = (u8)value;
}
static void program(u32 input, char *hlsl) {
    u8 code[16] = {0};
    word(code, (1u << 24) | (15u << 9) | (input << 13) | 1u);
    word(code + 4, (0xE4u << 9) | (7u << 18) | 1u);
    assert(rsx_fp_decompile(code, sizeof code, 0x40, hlsl, 65536) == 1);
}
int main(int argc, char **argv) {
    char plain[65536], facing[65536];
    program(4, plain);
    if (argc == 2 && !strcmp(argv[1], "--plain")) {
        fputs(plain, stdout);
        return 0;
    }
    program(14, facing);
    assert(!strstr(plain, "SV_IsFrontFace"));
    assert(strstr(facing, "bool rsx_front : SV_IsFrontFace"));
    assert(strstr(facing, "input.rsx_front ? float4(1,1,1,1) : float4(-1,-1,-1,-1)"));
    puts("PASS production FACING input and conditional system-value declaration");
}
