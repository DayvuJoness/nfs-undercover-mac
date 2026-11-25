// d3dx_math.cpp - D3DX arithmetic shared by the D3DX imports (dx/d3dx9.cpp) and a game's native replacements
// (native/*.c, part of the translated code). It lives in the runtime so every binary that links the translated code
// has it, the test programs included. Built with the kit's usual floating-point flags: the import's results are
// what they were when this expression sat in d3dx9.cpp.
extern "C" void recomp_d3dx_vec3_transform(const float v[3], const float m[16], float out[4]) {
    const float x = v[0], y = v[1], z = v[2];
    for (int j = 0; j < 4; ++j)
        out[j] = x * m[j] + y * m[4 + j] + z * m[8 + j] + m[12 + j];
}

// D3DXVec4Transform's arithmetic, shared the same way (NFS Undercover's native FUN_0076d720 transforms its quad
// vertices with it, 2026-11-21).
extern "C" void recomp_d3dx_vec4_transform(const float v[4], const float m[16], float out[4]) {
    const float in[4] = {v[0], v[1], v[2], v[3]};
    for (int j = 0; j < 4; ++j)
        out[j] = in[0] * m[j] + in[1] * m[4 + j] + in[2] * m[8 + j] + in[3] * m[12 + j];
}
