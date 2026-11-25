// shaders_msl.h - the MSL behind every program gpu/shaders.md names. Moved
// verbatim from compositor.mm, performance_overlay.h and d3d_render.mm; the
// slot numbers are the ones shaders.md documents.
#pragma once

namespace gpu::metal {

// "compositor": compositor_vertex / compositor_fragment
inline const char *const kCompositorSource = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Quad { float4 rect; float4 uv; float2 drawable; uint opaque; uint pad; float4 gamma[17]; };
struct Varying { float4 position [[position]]; float2 uv; };
vertex Varying compositor_vertex(uint id [[vertex_id]], constant Quad& q [[buffer(0)]]) {
    const float2 corners[4]={float2(0,0),float2(1,0),float2(0,1),float2(1,1)};
    float2 p=q.rect.xy+corners[id]*q.rect.zw;
    Varying v;
    v.position=float4(p.x/q.drawable.x*2-1,1-p.y/q.drawable.y*2,0,1);
    v.uv=q.uv.xy+corners[id]*q.uv.zw;
    return v;
}
fragment float4 compositor_fragment(Varying v [[stage_in]],
                                    constant Quad& q [[buffer(0)]],
                                    texture2d<float> tex [[texture(0)]]) {
    constexpr sampler nearest(coord::normalized,address::clamp_to_edge,filter::nearest);
    float4 c=tex.sample(nearest,v.uv);
    if(q.opaque) c.a=1;
    if(q.pad==1) { // the game's gamma ramp (SetGammaRamp), piecewise linear over 17 samples
        const float3 x=saturate(c.rgb)*16.0;
        const uint3 i=min(uint3(x),uint3(15));
        const float3 f=x-float3(i);
        c.r=mix(q.gamma[i.r].r,q.gamma[i.r+1].r,f.r);
        c.g=mix(q.gamma[i.g].g,q.gamma[i.g+1].g,f.g);
        c.b=mix(q.gamma[i.b].b,q.gamma[i.b+1].b,f.b);
    }
    return c;
}
// The game's picture, sharpened (compositor_set_sharpening, 2026-11-17): contrast-adaptive sharpening in the
// manner of AMD's CAS - each pixel against its four neighbours, weighted by how much room the local contrast
// leaves, so edges get crisper without halos and flat areas stay as they are. buffer(1).x is the strength,
// 0..1. The game's gamma ramp follows as in compositor_fragment.
fragment float4 compositor_sharp_fragment(Varying v [[stage_in]],
                                          constant Quad& q [[buffer(0)]],
                                          constant float4& sh [[buffer(1)]],
                                          texture2d<float> tex [[texture(0)]]) {
    constexpr sampler nearest(coord::normalized,address::clamp_to_edge,filter::nearest);
    const float2 px=1.0/float2(tex.get_width(),tex.get_height());
    const float3 c=tex.sample(nearest,v.uv).rgb;
    const float3 n=tex.sample(nearest,v.uv+float2(0,-px.y)).rgb;
    const float3 s=tex.sample(nearest,v.uv+float2(0,px.y)).rgb;
    const float3 w=tex.sample(nearest,v.uv+float2(-px.x,0)).rgb;
    const float3 e=tex.sample(nearest,v.uv+float2(px.x,0)).rgb;
    const float3 mn=min(c,min(min(n,s),min(w,e)));
    const float3 mx=max(c,max(max(n,s),max(w,e)));
    const float3 amp=sqrt(saturate(min(mn,1.0-mx)/max(mx,1e-5)));
    const float3 wt=-amp*mix(0.125,0.2,saturate(sh.x));
    // 2026-11-17: the difference is then amplified by 1 + strength (up to 2x at "strong"); at 1:1 with the screen
    // plain CAS was too faint to see.
    const float3 cas=saturate((c+wt*(n+s+w+e))/(1.0+4.0*wt));
    float4 r=float4(saturate(c+(cas-c)*(1.0+saturate(sh.x))),1.0);
    if(q.pad==1) {
        const float3 x=saturate(r.rgb)*16.0;
        const uint3 i=min(uint3(x),uint3(15));
        const float3 f=x-float3(i);
        r.r=mix(q.gamma[i.r].r,q.gamma[i.r+1].r,f.r);
        r.g=mix(q.gamma[i.g].g,q.gamma[i.g+1].g,f.g);
        r.b=mix(q.gamma[i.b].b,q.gamma[i.b+1].b,f.b);
    }
    return r;
}
// Test86 HDR output (RGBA16F linear extended sRGB drawable, 1.0 = SDR white). mode 0: decode the sRGB
// source (UI, menus, movies); 1: the game frame - decode, then where its alpha marks the scene (the core
// mod sets it before the HUD, or the backend's HDR composite does; the HUD clears it) expand the highlights
// above `knee` toward `headroom`, hue kept; 2: the source is already linear (a re-fit of the composed frame).
// 3 (2026-11-18): an RGBA16F game frame whose scene composite marked the scene with alpha 2 (d3d9_shadergen); the
// presenter lifts the marked near-white (see below). The mark is read from texture(1) (the frame as rendered, so a
// MetalFX-enlarged colour in texture(0) keeps it); `sharp` > 0 sharpens the colour first as compositor_sharp_fragment
// does, and the game's gamma ramp applies as in compositor_fragment.
struct Hdr { float mode; float headroom; float knee; float sharp; };
float3 hdr_decode(float3 c) {
    c = max(c, 0.0);
    return select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045);
}
fragment float4 compositor_hdr_fragment(Varying v [[stage_in]],
                                        constant Quad& q [[buffer(0)]],
                                        constant Hdr& h [[buffer(1)]],
                                        texture2d<float> tex [[texture(0)]],
                                        texture2d<float> masktex [[texture(1)]]) {
    constexpr sampler nearest(coord::normalized,address::clamp_to_edge,filter::nearest);
    constexpr sampler smooth(coord::normalized,address::clamp_to_edge,filter::linear);
    float4 c=tex.sample(nearest,v.uv);
    if(h.mode>1.5 && h.mode<2.5) return c;
    const bool lifted=h.mode>2.5;                  // 3: a float frame with the scene marked
    const float mask=h.mode>0.5 && !lifted ? masktex.sample(smooth,v.uv).a : c.a;
    c.rgb=min(c.rgb,1.0);                          // as an 8-bit back buffer would hold it
    if(q.opaque) c.a=1;
    if(h.sharp>0.0) {
        const float2 px=1.0/float2(tex.get_width(),tex.get_height());
        const float3 n=min(tex.sample(nearest,v.uv+float2(0,-px.y)).rgb,1.0);
        const float3 s=min(tex.sample(nearest,v.uv+float2(0,px.y)).rgb,1.0);
        const float3 w=min(tex.sample(nearest,v.uv+float2(-px.x,0)).rgb,1.0);
        const float3 e=min(tex.sample(nearest,v.uv+float2(px.x,0)).rgb,1.0);
        const float3 mn=min(c.rgb,min(min(n,s),min(w,e)));
        const float3 mx=max(c.rgb,max(max(n,s),max(w,e)));
        const float3 amp=sqrt(saturate(min(mn,1.0-mx)/max(mx,1e-5)));
        const float3 wt=-amp*mix(0.125,0.2,saturate(h.sharp));
        const float3 cas=saturate((c.rgb+wt*(n+s+w+e))/(1.0+4.0*wt));
        c.rgb=saturate(c.rgb+(cas-c.rgb)*(1.0+saturate(h.sharp)));
    }
    if(q.pad==1) {
        const float3 x=saturate(c.rgb)*16.0;
        const uint3 i=min(uint3(x),uint3(15));
        const float3 f=x-float3(i);
        c.r=mix(q.gamma[i.r].r,q.gamma[i.r+1].r,f.r);
        c.g=mix(q.gamma[i.g].g,q.gamma[i.g+1].g,f.g);
        c.b=mix(q.gamma[i.b].b,q.gamma[i.b+1].b,f.b);
    }
    if(lifted) {
        // The picture is the game's own (what is above 1.0 only additive light an 8-bit target would have cut,
        // dropped here); where the scene's mark survives (alpha 2 from the composite, down to 1 under the HUD) its
        // near-white is lifted: by the brightest channel y, unchanged up to 0.88, then y + 41.67 (y - 0.88)^2
        // (white -> 1.6 encoded, ~2.95 in light), hue kept; what ends above white is scaled by headroom.
        // The lift is measured on the frame as rendered (texture(1), smoothly filtered), not on the sharpened or
        // MetalFX-enlarged colour: their light fringe along dark edges (leaves against the sky) was lifted into a
        // glowing outline (2026-11-18 report).
        // 2026-11-18 (report: lifted glints flickered at speed): the brightness that decides the lift is the
        // frame's average over about 5 pixels around (centre + 8 taps), so a glint that sparkles from frame to frame
        // or the speed blur's streaks no longer flash through the lift; the mark is read at the centre.
        const float2 rp=2.0/float2(masktex.get_width(),masktex.get_height());
        const float4 ref=masktex.sample(smooth,v.uv);
        float yb=max(max(min(ref.r,1.0),min(ref.g,1.0)),min(ref.b,1.0));
        for(int k=0;k<8;++k) {
            const float an=0.7854*float(k);
            const float3 q=min(masktex.sample(smooth,v.uv+rp*float2(cos(an),sin(an))).rgb,1.0);
            yb+=max(max(q.r,q.g),q.b);
        }
        yb/=9.0;
        const float m=saturate(ref.a-1.0);
        const float3 lin0=hdr_decode(c.rgb);
        // the smaller of the two: a dark leaf pixel next to the sky (bright in the smoothed frame) is not lifted
        const float y=min(yb,max(max(c.r,c.g),c.b));
        // knee (mode 3): the marked scene's brightness against SDR white (the core mod's hdr_world), with colour
        // kept a little fuller as brightness rises (x1.08 saturation at the default), so the world does not look
        // washed out next to its lifted highlights (2026-11-18 report). The HUD and the menus stay at 1.
        const float g=mix(1.0,h.knee,m);
        const float sat=1.0+0.25*(g-1.0);
        float3 lin=lin0;
        if(y>0.88) {
            const float d=y-0.88;
            lin=hdr_decode(c.rgb*((y+41.67*d*d)/y));
            lin=select(lin,1.0+(lin-1.0)*h.headroom,lin>1.0);
            lin=mix(lin0,lin,m);
        }
        const float L=dot(lin,float3(0.2627,0.6780,0.0593));
        lin=max(L+(lin-L)*sat,0.0)*g;
        return float4(lin,1.0);
    }
    float3 lin=hdr_decode(c.rgb);
    if(h.mode>0.5 && mask>0.45 && h.headroom>1.0) {
        const float L=dot(lin,float3(0.2126,0.7152,0.0722));
        const float t=saturate((L-h.knee)/max(1.0-h.knee,1e-3));
        lin*=1.0+(h.headroom-1.0)*t*t;
    }
    return float4(lin,c.a);
}
)MSL";

// "hud": hud_v / hud_f
inline const char *const kHudSource = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; float2 uv; };
vertex V hud_v(uint i [[vertex_id]], constant float4& r [[buffer(0)]]) {
    float2 q = float2(i & 1, i >> 1);
    V v; v.p = float4(r.xy + q * r.zw, 0, 1); v.uv = q; return v;
}
fragment float4 hud_f(V v [[stage_in]], texture2d<float> t [[texture(0)]]) {
    constexpr sampler s(filter::linear);
    return t.sample(s, v.uv);
}
)MSL";

// "d3d", "surface_upload", "guest_readback", "guest_readback_fused", "native_brightness"
inline const char *const kD3DSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct HVertex {
    float x, y, z, w;
    float u, v;
    float r, g, b, a;
    float sr, sg, sb, sa;
};
struct Uniforms {
    float4x4 mvp;
    uint  pretransformed;
    uint  textured;
    uint  texblend;
    uint  alphatest;
    uint  alphafunc;
    float alpharef;
    uint  specular;
    uint  texture_has_alpha;
    uint  fogmode;        // 0 off, 1 vertex, 2 exp, 3 exp2, 4 linear
    float fogstart, fogend, fogdensity;
    float fogr, fogg, fogb;
    float pointsize;
    uint terrain_detail;
};
struct VOut {
    float4 position [[position]];
    float  size [[point_size]];
    float2 uv;
    float4 color;
    float4 spec;
    float  fogdist;
};
// Colour, and the coverage mask that says this pixel was rasterized. A
// discarded fragment writes neither.
struct FOut {
    float4 color    [[color(0)]];
    float  coverage [[color(1)]];
};

vertex VOut d3d_vertex(uint vid [[vertex_id]],
                       const device HVertex* v [[buffer(0)]],
                       constant Uniforms& u [[buffer(1)]]) {
    HVertex s = v[vid];
    VOut o;
    o.position = u.pretransformed ? float4(s.x, s.y, s.z, s.w)
                                  : u.mvp * float4(s.x, s.y, s.z, 1.0);
    o.size = u.pointsize;
    o.uv = float2(s.u, s.v);
    o.color = float4(s.r, s.g, s.b, s.a);
    o.spec = float4(s.sr, s.sg, s.sb, s.sa);
    // Table fog is a function of eye-space distance, which after a standard
    // projection is the clip w.
    o.fogdist = abs(o.position.w);
    return o;
}

// Shade a world fragment using the guest material and optional terrain detail.
// Apply the legacy texture/alpha rules before writing color and coverage attachments.
fragment FOut d3d_fragment(VOut in [[stage_in]],
                           constant Uniforms& u [[buffer(1)]],
                           texture2d<float> tex [[texture(0)]],
                           sampler samp [[sampler(0)]],
                           texture2d<float> detail [[texture(1)]],
                           sampler detail_samp [[sampler(1)]]) {
    float4 c = in.color;
    if (u.textured != 0) {
        float4 t = tex.sample(samp, in.uv);
        if (u.terrain_detail != 0) {
            // An independent material sample supplies real fine structures;
            // enlarging the guest's 16/32 texels cannot create those. The
            // source remains the color/lighting/coastline mask. Blue water
            // fades this land-only layer out, including mixed shore texels.
            float3 chroma = t.rgb / max(max(t.r, t.g), max(t.b, 0.01f));
            float land = 1.0f - smoothstep(0.02f, 0.22f, chroma.b - chroma.r);
            float grass = smoothstep(-0.03f, 0.16f, chroma.g - chroma.r);
            float stone = 1.0f - smoothstep(0.06f, 0.28f, max(chroma.r, chroma.g) - min(chroma.r, min(chroma.g, chroma.b)));
            float sand = smoothstep(0.28f, 0.62f, max(t.r, t.g));
            float4 structure = (detail.sample(detail_samp, in.uv) * 255.0f - 128.0f) / 128.0f;
            float material = mix(structure.a, structure.g, sand);
            material = mix(material, structure.b, stone);
            material = mix(material, structure.r, grass);
            t.rgb = saturate(t.rgb * (1.0f + material * 0.85f * land));
        }
        switch (u.texblend) {
            case 1: case 7:                                   // DECAL, COPY
                c = t; break;
            case 2:                                           // MODULATE
                // The legacy rule: the texture's alpha when it has one,
                // otherwise the diffuse's. Taking the vertex alpha from a
                // vertex that never set it - and plenty of D3DTLVERTEX
                // colours leave it at zero - makes every blended pixel
                // invisible and the whole scene the colour of the clear.
                c = float4(t.rgb * in.color.rgb,
                           u.texture_has_alpha != 0 ? t.a : in.color.a);
                break;
            case 3:                                           // DECALALPHA
                c = float4(mix(in.color.rgb, t.rgb, t.a), in.color.a); break;
            case 5:                                           // DECALMASK
                c = float4(t.rgb, in.color.a); break;
            case 8:                                           // ADD
                c = float4(saturate(t.rgb + in.color.rgb), in.color.a); break;
            case 4:                                           // MODULATEALPHA
                c = float4(t.rgb * in.color.rgb, t.a * in.color.a); break;
            default:
                c = float4(t.rgb * in.color.rgb,
                           u.texture_has_alpha != 0 ? t.a : in.color.a);
                break;
        }
    }
    if (u.specular != 0) c = float4(saturate(c.rgb + in.spec.rgb), c.a);
    if (u.alphatest != 0) {
        bool pass;
        switch (u.alphafunc) {
            case 1: pass = false;                break;   // NEVER
            case 2: pass = c.a <  u.alpharef;    break;   // LESS
            case 3: pass = c.a == u.alpharef;    break;   // EQUAL
            case 4: pass = c.a <= u.alpharef;    break;   // LESSEQUAL
            case 5: pass = c.a >  u.alpharef;    break;   // GREATER
            case 6: pass = c.a != u.alpharef;    break;   // NOTEQUAL
            case 7: pass = c.a >= u.alpharef;    break;   // GREATEREQUAL
            default: pass = true;                break;   // ALWAYS
        }
        if (!pass) discard_fragment();
    }
    if (u.fogmode != 0) {
        // The fog factor is how much of the fragment survives: 1 is clear.
        float f = 1.0;
        float d = in.fogdist;
        switch (u.fogmode) {
            case 1: f = in.spec.a; break;                          // vertex fog
            case 2: f = exp(-u.fogdensity * d); break;             // EXP
            case 3: f = exp(-(u.fogdensity * d) * (u.fogdensity * d)); break;
            default:                                               // LINEAR
                f = (u.fogend - d) / max(u.fogend - u.fogstart, 1e-6);
                break;
        }
        f = saturate(f);
        c = float4(mix(float3(u.fogr, u.fogg, u.fogb), c.rgb, f), c.a);
    }
    FOut o;
    o.color = c;
    o.coverage = 1.0;
    return o;
}
// Reduce guest-visible reads on the GPU; never decompress a 4K texture on
// the CPU merely to select 640x480 pixel centres. Each lane also counts its
// disjoint native cell so existing full-resolution diagnostics stay exact.
kernel void guest_readback_fused(texture2d<float, access::read> color [[texture(0)]],
                           texture2d<float, access::read> coverage [[texture(1)]],
                           device uint2* output [[buffer(0)]],
                           constant uint4* p [[buffer(1)]],
                           uint2 tid [[thread_position_in_grid]]) {
    uint2 span=p[0].zw-p[0].xy;
    if(any(tid>=span)) return;
    uint2 guest=p[0].xy+tid, native_size=p[1].xy, guest_size=p[1].zw;
    uint2 pos=min(((2*guest+1)*native_size)/(2*guest_size),p[2].yz-1);
    uint3 rgb=uint3(round(color.read(pos).rgb*255.0f));
    uint covered=coverage.read(pos).r>0 ? 1:0;
    uint2 lo=(guest*native_size+guest_size-1)/guest_size;
    uint2 hi=((guest+1)*native_size+guest_size-1)/guest_size;
    uint lit=0;
    for(uint y=lo.y;y<hi.y;++y) for(uint x=lo.x;x<hi.x;++x)
        lit+=any(round(color.read(uint2(x,y)).rgb*255.0f)>8.0f) ? 1:0;
    output[p[2].x+tid.y*span.x+tid.x]=uint2(rgb.b|(rgb.g<<8)|(rgb.r<<16)|(covered<<24),lit);
}
// Guest sampling and native brightness reduction have different grids.
// Keeping the sampling lanes short avoids a variable native-cell loop in
// every guest lane. Packing and coverage rules match the fused reference.
kernel void guest_readback(texture2d<float, access::read> color [[texture(0)]],
                           texture2d<float, access::read> coverage [[texture(1)]],
                           device uint2* output [[buffer(0)]],
                           constant uint4* p [[buffer(1)]],
                           uint2 tid [[thread_position_in_grid]]) {
    uint2 span=p[0].zw-p[0].xy;
    if(any(tid>=span)) return;
    uint2 guest=p[0].xy+tid, native_size=p[1].xy, guest_size=p[1].zw;
    uint2 pos=min(((2*guest+1)*native_size)/(2*guest_size),p[2].yz-1);
    uint3 rgb=uint3(round(color.read(pos).rgb*255.0f));
    uint covered=coverage.read(pos).r>0 ? 1:0;
    output[p[2].x+tid.y*span.x+tid.x]=uint2(rgb.b|(rgb.g<<8)|(rgb.r<<16)|(covered<<24),0);
}
// A 16x16 threadgroup covers a 32x32 native tile. Each lane reads four
// adjacent pixels and each SIMD group writes one sum. Padded lanes contribute
// zero but participate in the reduction; no contended global atomic is used.
kernel void native_brightness(texture2d<float, access::read> color [[texture(0)]],
                              device uint* output [[buffer(0)]],
                              constant uint4* p [[buffer(1)]],
                              uint2 tid [[thread_position_in_grid]],
                              uint2 group [[threadgroup_position_in_grid]],
                              uint lane [[thread_index_in_simdgroup]],
                              uint simd_group [[simdgroup_index_in_threadgroup]]) {
    uint2 pos=p[0].xy+tid*2;
    uint lit=0;
    for(uint y=0;y<2;++y) for(uint x=0;x<2;++x) {
        uint2 q=pos+uint2(x,y);
        if(all(q<p[0].zw)) lit+=any(round(color.read(q).rgb*255.0f)>8.0f) ? 1:0;
    }
    uint sum=simd_sum(lit);
    if(lane==0) output[p[1].x+(group.y*p[1].y+group.x)*p[1].z+simd_group]=sum;
}
// Seed a native target directly from an immutable guest-sized buffer. Integer
// channel expansion and floor(x * guest / native) preserve the CPU upload's
// exact pixels without expanding the whole 4K image on the guest thread.
vertex float4 surface_upload_vertex(uint id [[vertex_id]]) {
    float2 corner=float2((id<<1)&2,id&2);
    return float4(corner*2.0f-1.0f,0,1);
}
fragment float4 surface_upload_fragment(float4 position [[position]],
    device const uchar* pixels [[buffer(0)]],
    constant uint4* p [[buffer(1)]], constant uint* palette [[buffer(2)]]) {
    uint2 xy=uint2(position.xy)*p[0].xy/p[0].zw;
    uint offset=xy.y*p[1].x+xy.x*(p[1].y==8 ? 1:(p[1].y<=16 ? 2:4));
    uint3 rgb;
    if(p[1].y==8) {
        uint index=pixels[offset];
        uint c=p[1].z ? palette[index] : index*0x010101u;
        rgb=uint3((c>>16)&255,(c>>8)&255,c&255);
    } else {
        uint value=uint(pixels[offset])|(uint(pixels[offset+1])<<8);
        if(p[1].y>16) value|=(uint(pixels[offset+2])<<16)|(uint(pixels[offset+3])<<24);
        rgb=((uint3(value)&p[2].xyz)>>p[3].xyz)*255/p[4].xyz;
    }
    return float4(float3(rgb)/255.0f,1);
}
)MSL";

} // namespace gpu::metal
