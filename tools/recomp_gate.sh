#!/bin/sh
# Run the generated recompiler across every snapshot pair in the project.
# The pairs were captured for hand-porting; they gate anything that claims
# to reproduce a routine, so generated and hand-written C face the same bar.
R=re/pipeline/road
pass=0; fail=0
while read -r a b name; do
    [ -z "$name" ] && continue
    out=$(timeout 300 ./build/recomp_verify "$R/$a" "$R/$b" "$name" 2>&1 | tail -1)
    case "$out" in
        *EXACT*) pass=$((pass+1)) ;;
        *) fail=$((fail+1)); echo "$out" ;;
    esac
done <<'PAIRS'
st_3_212f1a st_4_212f1e road_interpolate
ph_0_211e78 ph_6_211e98 CHAIN_car
st_1_212f12 st_8_212f2e CHAIN_road
fr_0_212cea fr_1_212e3c race_frame_begin
fp_0_212e58 fp_1_212e78 race_frame_publish
lt_0_211948 lt_1_21194c car_frame_latch
in_0_21186a in_1_21186e input_read
ph_0_211e78 ph_1_211e7c car_update
ph_1_211e7c ph_2_211e80 car_checkpoint
ph_2_211e80 ph_3_211e84 car_clock
ph_3_211e84 ph_4_211e88 car_distance
ph_4_211e88 ph_5_211e94 car_shape
ph_5_211e94 ph_6_211e98 car_tick
sd_0_2151b4 sd_1_2151b8 scen_next_a2
sd_1_2151b8 sd_2_2151bc scen_next_a0
sd_2_2151bc sd_3_2151c0 scen_next_table
sd_3_2151c0 sd_4_2151c4 scen_sort
sd_4_2151c4 sd_5_2151c8 scen_next_a1
sh_0_21508a sh_1_2151b4 scen_prepare
sp_0_2169dc sp_1_2169de span_fill
sl_0_215f08 sl_1_215f0c scen_shape_ptr
pj_0_215f00 pj_1_215f04 scen_project
st_1_212f12 st_2_212f16 road_sky
st_2_212f16 st_3_212f1a road_keyframes
st_4_212f1e st_5_212f22 road_band_bounds
st_5_212f22 st_6_212f26 road_perspective
st_6_212f26 st_7_212f2a road_blitqueue
st_7_212f2a st_8_212f2e road_bands
PAIRS
echo "recomp gate: $pass EXACT, $fail FAIL"
[ "$fail" = 0 ]
