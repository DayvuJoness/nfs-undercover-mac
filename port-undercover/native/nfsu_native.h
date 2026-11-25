/* nfsu_native.h - native replacements for NFS Undercover's hottest small functions (game.toml [translate] native).
 * funcs.h includes this before it names FN_<addr>, so every direct call, tail call and table case of a replaced
 * address goes to the native function. The untouched translation stays as fn_<addr> for the cases a replacement
 * hands back. */
#pragma once
#include "x86.h"

void nfsu_bounds_vs_frustum(X86 *c);
#define FN_0074a1d0 nfsu_bounds_vs_frustum

/* msvcr80's qsort, for the render thread's draw-list sort (nfsu_native.c). */
void nfsu_qsort(X86 *c);
#define FN_1003cee0 nfsu_qsort

/* The game's sprintf (FUN_004b6e60): one call site skipped (nfsu_native.c). */
void nfsu_sprintf(X86 *c);
#define FN_004b6e60 nfsu_sprintf

/* FUN_00780390, the render thread's on-screen sphere test (nfsu_native.c). */
void nfsu_sphere_visible(X86 *c);
#define FN_00780390 nfsu_sphere_visible

/* FUN_008251b0, one occluder's five planes against a sphere (nfsu_native.c). */
void nfsu_occluder_hides(X86 *c);
#define FN_008251b0 nfsu_occluder_hides

/* FUN_006a3060, the animation spline the main thread evaluates while a scene loads (nfsu_native.c). */
void nfsu_spline(X86 *c);
#define FN_006a3060 nfsu_spline

/* FUN_00463280 and FUN_004636d0, the attribute hash lookups (nfsu_native.c). */
void nfsu_attr_slot(X86 *c);
#define FN_00463280 nfsu_attr_slot
void nfsu_attr_find(X86 *c);
#define FN_004636d0 nfsu_attr_find

/* FUN_006cea20, a car's shift points from its torque curve (nfsu_native.c). */
void nfsu_shift_points(X86 *c);
#define FN_006cea20 nfsu_shift_points

/* FUN_007063c0, the curve with its input and output clamped (nfsu_native.c). */
void nfsu_curve_clamped(X86 *c);
#define FN_007063c0 nfsu_curve_clamped
void nfsu_curve(X86 *c);
#define FN_00706380 nfsu_curve

/* FUN_0076d720, one vertex of a screen quad (the HUD's and menus' sprites and text) (nfsu_native.c). */
void nfsu_vertex_add(X86 *c);
#define FN_0076d720 nfsu_vertex_add

/* FUN_0082a7d0, a scenery instance in a view: the first tests natively (nfsu_native.c). */
void nfsu_scenery_visible(X86 *c);
#define FN_0082a7d0 nfsu_scenery_visible

/* FUN_007674e0 (set a stage's texture) and FUN_00541730 (bind a material's layers): the unchanged cases natively
 * (nfsu_native.c). */
void nfsu_set_texture(X86 *c);
#define FN_007674e0 nfsu_set_texture
void nfsu_material_bind(X86 *c);
#define FN_00541730 nfsu_material_bind

/* FUN_00763ef0, a draw's depth key for the sort (nfsu_native.c). */
void nfsu_depth_key(X86 *c);
#define FN_00763ef0 nfsu_depth_key
