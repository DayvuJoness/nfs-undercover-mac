#!/bin/sh
R=/tmp/rel/rel.sh
$R allmods2 200 "mod_redux=1,mod_prostreet_sky=1,mod_hc_hud=1,mod_photoreal=1,mod_revamped_pursuits=1,mod_police_career=1,mod_addon_cars=1,mod_porsche_pack=1,mod_hard_drivers=3,mod_eo_collectors=1,mod_eo_unlock_all=1,mod_eo_cops=0" "120:6:0.1,140:6:0.1"
$R police2 240 "mod_police_career=1,mod_revamped_pursuits=1" "60:Escape:0.1,62:Down:0.05,62.6:Down:0.05,63.2:Down:0.05,64.5:Return:0.1,75:Return:0.1,82:Return:0.1,88:Return:0.1,95:Return:0.1,110:7:0.1,140:H:0.1,170:H:0.1,190:Escape:0.1,195:Escape:0.1"
$R race 300 "mod_hard_drivers=3,mod_eo_cops=0" "60:Tab:0.1,75:Tab:0.1,90:Tab:0.1,105:Tab:0.1,120:Tab:0.1,135:Return:0.1,150:Tab:0.1,165:Return:0.1,180:Return:0.1,200:7:0.1"
