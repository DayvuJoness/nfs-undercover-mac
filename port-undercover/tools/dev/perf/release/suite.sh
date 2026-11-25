#!/bin/sh
R=/tmp/rel/rel.sh
# 2: every mod on + heat 5 (pursuit) + live keys: 0 cops off/on, O lights, - camera, 6 unlock off/on
$R allmods 250 "mod_redux=1,mod_prostreet_sky=1,mod_hc_hud=1,mod_photoreal=1,mod_revamped_pursuits=1,mod_police_career=1,mod_addon_cars=1,mod_porsche_pack=1,mod_hard_drivers=3,mod_eo_collectors=1,mod_eo_unlock_all=1,mod_eo_cops=1,mod_eo_heat_hack=1,mod_eo_heat_level=5" "70:0:0.1,95:0:0.1,110:O:0.1,125:O:0.1,140:Minus:0.1,150:Minus:0.1,160:6:0.1,175:6:0.1,185:9:0.1"
# 3: every mod off
$R alloff 200 "mod_redux=0,mod_prostreet_sky=0,mod_hc_hud=0,mod_photoreal=0,mod_revamped_pursuits=0,mod_police_career=0,mod_addon_cars=0,mod_porsche_pack=0,mod_hard_drivers=0,mod_eo_collectors=0,mod_eo_unlock_all=0,mod_eo_cops=0,mod_eo_heat_hack=0" ""
# 4: police mode (pause > police mode) with the autopilot already on, then H (lights / pursuit)
$R police 230 "mod_police_career=1,mod_revamped_pursuits=1" "60:Escape:0.1,62:Down:0.05,62.6:Down:0.05,63.2:Down:0.05,64.5:Return:0.1,90:7:0.1,120:H:0.1,150:H:0.1"
# 6/7: graphics variants (restart-only ones through the shared file)
MS="render_scale=3,hdr_output=1,shadow_softness=0,smoke_resolution=2,colour_filter=2,notch_area=0,startup_logos=0" $R gfx1 200 "motion_blur=3,depth_of_field=2,bloom=2,gloss=2,sharpening=0,menu_fps=3,hud_fill=0,draw_distance=0" ""
MS="render_scale=0,shadow_reflection_detail=2,shadow_softness=3,smoke_resolution=1,colour_filter=1" $R gfx2 200 "motion_blur=1,depth_of_field=0,bloom=0,gloss=0,sharpening=3,menu_fps=1,draw_distance=1,upscaling=0" ""
