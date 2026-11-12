struct type_11 {
    member: f32,
    member_1: u32,
    member_2: f32,
    member_3: f32,
}

struct type_14 {
    member: vec3<f32>,
    member_1: f32,
    member_2: vec3<f32>,
    member_3: f32,
    member_4: vec3<f32>,
    member_5: f32,
    member_6: vec3<f32>,
    member_7: f32,
    member_8: vec3<f32>,
    member_9: f32,
    member_10: vec3<f32>,
    member_11: f32,
    member_12: f32,
    member_13: f32,
    member_14: f32,
    member_15: f32,
    member_16: vec3<f32>,
    member_17: f32,
    member_18: f32,
    member_19: f32,
    member_20: f32,
    member_21: f32,
    member_22: vec2<f32>,
    member_23: f32,
    member_24: f32,
    member_25: vec4<f32>,
    member_26: vec4<f32>,
    member_27: vec4<f32>,
    member_28: vec4<f32>,
    member_29: vec4<f32>,
    member_30: vec4<f32>,
}

struct type_18 {
    member: f32,
    member_1: f32,
    member_2: f32,
    member_3: f32,
    member_4: vec3<f32>,
    member_5: f32,
    member_6: vec3<f32>,
    member_7: f32,
    member_8: vec3<f32>,
    member_9: f32,
    member_10: vec3<f32>,
    member_11: vec3<f32>,
    member_12: vec3<f32>,
    member_13: vec3<f32>,
    member_14: f32,
    member_15: f32,
    member_16: f32,
    member_17: f32,
    member_18: f32,
    member_19: vec2<f32>,
    member_20: f32,
    member_21: f32,
    member_22: f32,
    member_23: vec3<f32>,
    member_24: u32,
    member_25: f32,
    member_26: vec3<f32>,
    member_27: f32,
    member_28: vec3<f32>,
    member_29: f32,
    member_30: vec3<f32>,
    member_31: f32,
}

@group(0) @binding(5)
var<storage> global: type_11;
@group(0) @binding(0)
var<uniform> global_1: type_14;
@group(0) @binding(65)
var global_2: sampler;
@group(0) @binding(1)
var global_3: texture_2d<f32>;
@group(0) @binding(2)
var<uniform> global_4: type_18;
@group(0) @binding(67)
var global_5: sampler;
@group(0) @binding(3)
var global_6: texture_2d<f32>;
var<private> global_7: vec4<f32>;
@group(0) @binding(68)
var global_8: sampler;
@group(0) @binding(4)
var global_9: texture_2d<f32>;
var<private> global_10: vec2<f32>;

fn function_() {
    var phi_5962_: vec3<f32>;
    var phi_5920_: vec3<f32>;
    var phi_5921_: vec3<f32>;
    var phi_5922_: vec3<f32>;
    var phi_5926_: vec3<f32>;
    var phi_5924_: f32;
    var phi_5927_: vec3<f32>;
    var phi_5928_: vec3<f32>;
    var local: bool;
    var local_1: f32;
    var phi_5937_: vec3<f32>;
    var phi_5935_: f32;
    var phi_5938_: vec3<f32>;
    var phi_5939_: vec3<f32>;
    var local_2: bool;
    var local_3: f32;
    var phi_5943_: vec3<f32>;
    var phi_5941_: f32;
    var phi_5944_: vec3<f32>;
    var phi_5945_: vec3<f32>;
    var phi_5946_: vec3<f32>;
    var phi_2913_: bool;
    var phi_5947_: vec2<f32>;
    var phi_5952_: f32;
    var phi_5948_: f32;
    var phi_5958_: vec3<f32>;
    var phi_5954_: vec2<f32>;
    var phi_5953_: vec2<f32>;
    var phi_5955_: f32;
    var phi_5961_: vec3<f32>;
    var phi_5960_: vec3<f32>;
    var phi_5847_: f32;
    var phi_4666_: bool;
    var phi_5843_: f32;
    var phi_5844_: vec3<f32>;
    var phi_5849_: f32;
    var phi_5909_: vec3<f32>;
    var phi_5851_: f32;
    var phi_5908_: vec3<f32>;
    var phi_5850_: f32;
    var phi_5853_: vec3<f32>;
    var phi_5852_: i32;
    var local_4: vec3<f32>;
    var phi_5863_: vec3<f32>;
    var phi_5866_: vec3<f32>;
    var phi_5864_: f32;
    var phi_5867_: vec3<f32>;
    var phi_5869_: vec3<f32>;
    var phi_5868_: i32;
    var local_5: vec3<f32>;
    var phi_5872_: f32;
    var phi_5871_: bool;
    var phi_5898_: f32;
    var phi_5890_: vec3<f32>;
    var phi_5533_: bool;
    var phi_5891_: f32;
    var phi_5892_: vec3<f32>;
    var phi_5900_: vec3<f32>;
    var phi_5901_: vec3<f32>;
    var phi_5902_: vec3<f32>;

    switch bitcast<i32>(0u) {
        default: {
            let _e135 = global_10;
            let _e138 = ((_e135.x * 2f) - 1f);
            let _e142 = (((1f - _e135.y) * 2f) - 1f);
            let _e144 = global_1.member_21;
            let _e146 = global_1.member_20;
            let _e147 = max(_e146, 0.001f);
            if (_e144 <= 0f) {
                switch bitcast<i32>(0u) {
                    default: {
                        let _e151 = global_1.member_23;
                        let _e152 = clamp(_e151, 0f, 1f);
                        let _e154 = global_1.member_7;
                        let _e156 = (_e142 - clamp(_e154, -1f, 1f));
                        let _e159 = clamp(((1f - _e156) * 0.5f), 0f, 1f);
                        let _e162 = global_1.member_28[3u];
                        if (_e162 >= 0.5f) {
                            if (_e159 < 0.5f) {
                                let _e166 = global_1.member_28;
                                let _e169 = global_1.member_29;
                                phi_5962_ = mix(_e166.xyz, _e169.xyz, vec3(smoothstep(0f, 0.5f, _e159)));
                            } else {
                                let _e175 = global_1.member_29;
                                let _e178 = global_1.member_30;
                                phi_5962_ = mix(_e175.xyz, _e178.xyz, vec3(smoothstep(0.5f, 1f, _e159)));
                            }
                            let _e184 = phi_5962_;
                            let _e186 = global_1.member_1;
                            let _e189 = global.member_2;
                            let _e193 = (_e184 * (exp2(_e186) * select(1f, _e189, (_e189 > 0f))));
                            global_7 = vec4<f32>(_e193.x, _e193.y, _e193.z, 1f);
                            break;
                        }
                        let _e199 = global_4.member_30;
                        let _e201 = global_1.member;
                        let _e203 = normalize((_e201 - _e199));
                        let _e205 = global_1.member_6;
                        let _e208 = (_e205 - (_e203 * dot(_e205, _e203)));
                        phi_5920_ = _e208;
                        if (dot(_e208, _e208) < 0.000001f) {
                            let _e212 = global_1.member_4;
                            phi_5920_ = (_e212 - (_e203 * dot(_e212, _e203)));
                        }
                        let _e217 = phi_5920_;
                        phi_5921_ = _e217;
                        if (dot(_e217, _e217) < 0.000001f) {
                            let _e221 = global_1.member_2;
                            phi_5921_ = (_e221 - (_e203 * dot(_e221, _e203)));
                        }
                        let _e226 = phi_5921_;
                        phi_5922_ = _e226;
                        if (dot(_e226, _e226) < 0.000001f) {
                            let _e233 = select(vec3<f32>(1f, 0f, 0f), vec3<f32>(0f, 0f, 1f), vec3((abs(_e203.z) < 0.98f)));
                            phi_5922_ = (_e233 - (_e203 * dot(_e233, _e203)));
                        }
                        let _e238 = phi_5922_;
                        let _e239 = normalize(_e238);
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e243 = global_1.member_26[1u];
                                let _e244 = (_e243 >= 0f);
                                local = _e244;
                                local_1 = _e243;
                                local_2 = _e244;
                                local_3 = _e243;
                                if _e244 {
                                    let _e247 = global_1.member_26[0u];
                                    let _e248 = sin(_e247);
                                    let _e249 = cos(_e247);
                                    let _e266 = textureSampleLevel(global_3, global_2, vec2<f32>(((atan2(((_e248 * _e203.x) + (_e249 * _e203.z)), ((_e249 * _e203.x) - (_e248 * _e203.z))) * 0.15915494f) + 0.5f), (acos(clamp(_e203.y, -1f, 1f)) * 0.31830987f)), 0f);
                                    phi_5928_ = (_e266.xyz * _e243);
                                    break;
                                }
                                let _e275 = normalize(cross(_e203, select(vec3<f32>(0f, 0f, 1f), vec3<f32>(1f, 0f, 0f), vec3((abs(_e203.z) > 0.98f)))));
                                let _e279 = dot(_e203, _e203);
                                let _e289 = ((((asin(clamp(_e279, -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                                let _e295 = (0.5f + ((0.5f * sign(_e289)) * sqrt(abs(_e289))));
                                let _e296 = vec2<f32>(((atan2(dot(_e203, normalize(cross(_e275, _e203))), dot(_e203, _e275)) * 0.15915494f) + 0.5f), _e295);
                                let _e301 = textureSampleLevel(global_3, global_2, vec2<f32>(_e296.x, clamp(_e295, 0.001953125f, 0.9980469f)), 0f);
                                let _e304 = global_4.member_24;
                                let _e305 = (_e304 == 2u);
                                let _e307 = global_4.member_8;
                                let _e309 = global_4.member_9;
                                let _e311 = global_4.member_10;
                                let _e313 = global_4.member_18;
                                let _e315 = global_4.member_23;
                                let _e317 = global_4.member_17;
                                let _e319 = global_4.member_13;
                                let _e322 = clamp(_e152, 0f, 1f);
                                let _e324 = mix((_e307 * max(_e309, 0f)), _e311, vec3(_e322));
                                phi_5926_ = _e301.xyz;
                                if (_e305 && (_e279 < 0f)) {
                                    phi_5926_ = mix(mix(_e324, _e315, vec3(clamp(_e313, 0f, 1f))), _e324, vec3(clamp((-(_e279) * max(_e317, 0.001f)), 0f, 1f)));
                                }
                                let _e337 = phi_5926_;
                                phi_5927_ = _e337;
                                if ((_e152 > 0f) && (_e279 > select(-1f, 0f, _e305))) {
                                    if _e305 {
                                        phi_5924_ = max(_e279, 0f);
                                    } else {
                                        phi_5924_ = abs(_e279);
                                    }
                                    let _e345 = phi_5924_;
                                    let _e353 = fract((((normalize(_e203) * 37f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e358 = (_e353 + vec3(dot(_e353, (_e353.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    phi_5927_ = (_e337 + ((mix(_e319, vec3<f32>(0.000000698f, 0.00000085f, 0.000001163f), vec3(smoothstep(0f, 0.85f, _e345))) * (1f + ((fract(((_e358.x * _e358.y) * _e358.z)) - 0.5f) * 0.05f))) * _e322));
                                }
                                let _e372 = phi_5927_;
                                phi_5928_ = _e372;
                                break;
                            }
                        }
                        let _e374 = phi_5928_;
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e377 = local;
                                if _e377 {
                                    let _e380 = global_1.member_26[0u];
                                    let _e381 = sin(_e380);
                                    let _e382 = cos(_e380);
                                    let _e399 = textureSampleLevel(global_3, global_2, vec2<f32>(((atan2(((_e381 * _e239.x) + (_e382 * _e239.z)), ((_e382 * _e239.x) - (_e381 * _e239.z))) * 0.15915494f) + 0.5f), (acos(clamp(_e239.y, -1f, 1f)) * 0.31830987f)), 0f);
                                    let _e402 = local_1;
                                    phi_5939_ = (_e399.xyz * _e402);
                                    break;
                                }
                                let _e410 = normalize(cross(_e203, select(vec3<f32>(0f, 0f, 1f), vec3<f32>(1f, 0f, 0f), vec3((abs(_e203.z) > 0.98f)))));
                                let _e414 = dot(_e239, _e203);
                                let _e424 = ((((asin(clamp(_e414, -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                                let _e430 = (0.5f + ((0.5f * sign(_e424)) * sqrt(abs(_e424))));
                                let _e431 = vec2<f32>(((atan2(dot(_e239, normalize(cross(_e410, _e203))), dot(_e239, _e410)) * 0.15915494f) + 0.5f), _e430);
                                let _e436 = textureSampleLevel(global_3, global_2, vec2<f32>(_e431.x, clamp(_e430, 0.001953125f, 0.9980469f)), 0f);
                                let _e439 = global_4.member_24;
                                let _e440 = (_e439 == 2u);
                                let _e442 = global_4.member_8;
                                let _e444 = global_4.member_9;
                                let _e446 = global_4.member_10;
                                let _e448 = global_4.member_18;
                                let _e450 = global_4.member_23;
                                let _e452 = global_4.member_17;
                                let _e454 = global_4.member_13;
                                let _e457 = clamp(_e152, 0f, 1f);
                                let _e459 = mix((_e442 * max(_e444, 0f)), _e446, vec3(_e457));
                                phi_5937_ = _e436.xyz;
                                if (_e440 && (_e414 < 0f)) {
                                    phi_5937_ = mix(mix(_e459, _e450, vec3(clamp(_e448, 0f, 1f))), _e459, vec3(clamp((-(_e414) * max(_e452, 0.001f)), 0f, 1f)));
                                }
                                let _e472 = phi_5937_;
                                phi_5938_ = _e472;
                                if ((_e152 > 0f) && (_e414 > select(-1f, 0f, _e440))) {
                                    if _e440 {
                                        phi_5935_ = max(_e414, 0f);
                                    } else {
                                        phi_5935_ = abs(_e414);
                                    }
                                    let _e480 = phi_5935_;
                                    let _e488 = fract((((normalize(_e239) * 37f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e493 = (_e488 + vec3(dot(_e488, (_e488.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    phi_5938_ = (_e472 + ((mix(_e454, vec3<f32>(0.000000698f, 0.00000085f, 0.000001163f), vec3(smoothstep(0f, 0.85f, _e480))) * (1f + ((fract(((_e493.x * _e493.y) * _e493.z)) - 0.5f) * 0.05f))) * _e457));
                                }
                                let _e507 = phi_5938_;
                                phi_5939_ = _e507;
                                break;
                            }
                        }
                        let _e509 = phi_5939_;
                        let _e510 = -(_e203);
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e513 = local_2;
                                if _e513 {
                                    let _e516 = global_1.member_26[0u];
                                    let _e517 = sin(_e516);
                                    let _e518 = cos(_e516);
                                    let _e535 = textureSampleLevel(global_3, global_2, vec2<f32>(((atan2(((_e517 * _e510.x) + (_e518 * _e510.z)), ((_e518 * _e510.x) - (_e517 * _e510.z))) * 0.15915494f) + 0.5f), (acos(clamp(_e510.y, -1f, 1f)) * 0.31830987f)), 0f);
                                    let _e538 = local_3;
                                    phi_5945_ = (_e535.xyz * _e538);
                                    break;
                                }
                                let _e546 = normalize(cross(_e203, select(vec3<f32>(0f, 0f, 1f), vec3<f32>(1f, 0f, 0f), vec3((abs(_e203.z) > 0.98f)))));
                                let _e550 = dot(_e510, _e203);
                                let _e560 = ((((asin(clamp(_e550, -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                                let _e566 = (0.5f + ((0.5f * sign(_e560)) * sqrt(abs(_e560))));
                                let _e567 = vec2<f32>(((atan2(dot(_e510, normalize(cross(_e546, _e203))), dot(_e510, _e546)) * 0.15915494f) + 0.5f), _e566);
                                let _e572 = textureSampleLevel(global_3, global_2, vec2<f32>(_e567.x, clamp(_e566, 0.001953125f, 0.9980469f)), 0f);
                                let _e575 = global_4.member_24;
                                let _e576 = (_e575 == 2u);
                                let _e578 = global_4.member_8;
                                let _e580 = global_4.member_9;
                                let _e582 = global_4.member_10;
                                let _e584 = global_4.member_18;
                                let _e586 = global_4.member_23;
                                let _e588 = global_4.member_17;
                                let _e590 = global_4.member_13;
                                let _e593 = clamp(_e152, 0f, 1f);
                                let _e595 = mix((_e578 * max(_e580, 0f)), _e582, vec3(_e593));
                                phi_5943_ = _e572.xyz;
                                if (_e576 && (_e550 < 0f)) {
                                    phi_5943_ = mix(mix(_e595, _e586, vec3(clamp(_e584, 0f, 1f))), _e595, vec3(clamp((-(_e550) * max(_e588, 0.001f)), 0f, 1f)));
                                }
                                let _e608 = phi_5943_;
                                phi_5944_ = _e608;
                                if ((_e152 > 0f) && (_e550 > select(-1f, 0f, _e576))) {
                                    if _e576 {
                                        phi_5941_ = max(_e550, 0f);
                                    } else {
                                        phi_5941_ = abs(_e550);
                                    }
                                    let _e616 = phi_5941_;
                                    let _e624 = fract((((normalize(_e510) * 37f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e629 = (_e624 + vec3(dot(_e624, (_e624.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    phi_5944_ = (_e608 + ((mix(_e590, vec3<f32>(0.000000698f, 0.00000085f, 0.000001163f), vec3(smoothstep(0f, 0.85f, _e616))) * (1f + ((fract(((_e629.x * _e629.y) * _e629.z)) - 0.5f) * 0.05f))) * _e593));
                                }
                                let _e643 = phi_5944_;
                                phi_5945_ = _e643;
                                break;
                            }
                        }
                        let _e645 = phi_5945_;
                        if (_e159 < 0.5f) {
                            phi_5946_ = mix(_e374, _e509, vec3(smoothstep(0f, 0.5f, _e159)));
                        } else {
                            phi_5946_ = mix(_e509, _e645, vec3(smoothstep(0.5f, 1f, _e159)));
                        }
                        let _e654 = phi_5946_;
                        let _e656 = global_4.member_15;
                        let _e658 = global_4.member_16;
                        let _e667 = mix(_e654, _e509, vec3(clamp((1f - smoothstep(0f, max((mix(_e656, _e658, _e152) * 3f), 0.015f), abs(_e156))), 0f, 1f)));
                        let _e669 = global_1.member_8;
                        let _e670 = normalize(_e669);
                        let _e672 = global_1.member_9;
                        let _e673 = (_e672 > 0f);
                        phi_2913_ = _e673;
                        if _e673 {
                            phi_2913_ = (_e670.y > -0.18f);
                        }
                        let _e677 = phi_2913_;
                        phi_5958_ = _e667;
                        if (_e677 && (_e152 < 0.98f)) {
                            let _e681 = global_1.member_2;
                            let _e682 = normalize(_e681);
                            let _e684 = global_1.member_4;
                            let _e685 = normalize(_e684);
                            let _e688 = global_1.member_3;
                            let _e692 = max(((_e672 * max(_e688, 0.1f)) * 16f), 0.055f);
                            let _e694 = global_1.member_13;
                            if (_e694 > 0.5f) {
                                let _e698 = vec2<f32>(dot(_e670, _e682), dot(_e670, _e685));
                                let _e699 = length(_e698);
                                phi_5947_ = _e698;
                                if (_e699 > 0.82f) {
                                    phi_5947_ = (_e698 * (0.82f / _e699));
                                }
                                let _e704 = phi_5947_;
                                let _e711 = vec2<f32>(((_e138 - (_e704.x / _e147)) * _e147), (_e156 - _e704.y));
                                let _e713 = -(dot(_e711, _e711));
                                let _e714 = (_e692 * _e692);
                                phi_5952_ = exp((_e713 / max(_e714, 0.000001f)));
                                phi_5948_ = exp((_e713 / max((_e714 * 26f), 0.000001f)));
                            } else {
                                let _e728 = vec2<f32>(((_e138 - (dot(_e670, _e682) / _e147)) * _e147), (_e156 - dot(_e670, _e685)));
                                let _e730 = smoothstep(-0.05f, 0.05f, dot(_e670, normalize(_e205)));
                                let _e732 = -(dot(_e728, _e728));
                                let _e733 = (_e692 * _e692);
                                phi_5952_ = (exp((_e732 / max(_e733, 0.000001f))) * _e730);
                                phi_5948_ = (exp((_e732 / max((_e733 * 26f), 0.000001f))) * _e730);
                            }
                            let _e744 = phi_5952_;
                            let _e746 = phi_5948_;
                            let _e748 = smoothstep(-0.05f, 0.05f, _e670.y);
                            let _e749 = smoothstep(-0.006f, 0.006f, _e156);
                            let _e750 = (1f - _e152);
                            let _e754 = global_1.member_16;
                            let _e758 = global_1.member_17;
                            phi_5958_ = ((_e667 + (((mix(_e754, vec3<f32>(1f, 0.48f, 0.18f), vec3((1f - smoothstep(0.04f, 0.55f, _e670.y)))) * ((clamp((_e758 * 0.018f), 0f, 1.2f) * _e748) * _e750)) * _e746) * _e749)) + (((_e754 * (((_e758 * 0.4f) * _e748) * _e750)) * _e744) * _e749));
                        }
                        let _e775 = phi_5958_;
                        let _e777 = global_1.member_10;
                        let _e778 = normalize(_e777);
                        let _e780 = smoothstep(-0.18f, 0.02f, _e778.y);
                        let _e782 = global_1.member_11;
                        phi_5960_ = _e775;
                        if ((_e782 > 0f) && (_e780 > 0f)) {
                            let _e787 = global_1.member_2;
                            let _e788 = normalize(_e787);
                            let _e790 = global_1.member_4;
                            let _e791 = normalize(_e790);
                            let _e794 = vec2<f32>(dot(_e778, _e788), dot(_e778, _e791));
                            let _e796 = global_1.member_13;
                            phi_5953_ = _e794;
                            if (_e796 > 0.5f) {
                                let _e798 = length(_e794);
                                phi_5954_ = _e794;
                                if (_e798 > 0.82f) {
                                    phi_5954_ = (_e794 * (0.82f / _e798));
                                }
                                let _e803 = phi_5954_;
                                phi_5953_ = _e803;
                            }
                            let _e805 = phi_5953_;
                            let _e810 = global_1.member_12;
                            let _e813 = global_1.member_5;
                            let _e816 = max(((_e810 * 6f) * max(_e813, 0.1f)), 0.05f);
                            let _e820 = vec2<f32>(((_e138 - (_e805.x / _e147)) * _e147), (_e156 - _e805.y));
                            let _e823 = smoothstep(-0.006f, 0.006f, _e156);
                            let _e824 = smoothstep(1f, 0.92f, (length(_e820) / _e816));
                            phi_5961_ = _e775;
                            if ((_e824 * _e823) > 0f) {
                                let _e830 = (((_e820 / vec2(_e816)) * 0.5f) + vec2<f32>(0.5f, 0.5f));
                                let _e835 = vec2<f32>(_e830.x, (1f - _e830.y));
                                let _e836 = textureSampleLevel(global_9, global_8, _e835, 0f);
                                let _e840 = dot(_e670, _e778);
                                let _e844 = global_1.member_15;
                                switch bitcast<i32>(0u) {
                                    default: {
                                        let _e848 = (((_e835 * 2f) - vec2<f32>(1f, 1f)) * vec2<f32>(1.1248595f, 1.1248595f));
                                        let _e849 = dot(_e848, _e848);
                                        if (_e849 > 1f) {
                                            phi_5955_ = 0f;
                                            break;
                                        }
                                        phi_5955_ = smoothstep(-0.04f, 0.04f, dot(vec3<f32>(_e848.x, _e848.y, sqrt(max(0f, (1f - _e849)))), normalize(vec3<f32>(dot(_e670, _e788), -(dot(_e670, _e791)), -(_e840)))));
                                        break;
                                    }
                                }
                                let _e861 = phi_5955_;
                                phi_5961_ = (_e775 + (((((mix((vec3<f32>(0.937f, 1.008f, 1.109f) * (0.000055f * (_e840 + 1f))), vec3<f32>(1.2142f, 0.9614f, 0.7521f), vec3(_e861)) * (((_e782 / (0.000064439846f * max((0.5f * (1f - _e840)), 0.001f))) * (dot(_e836.xyz, vec3<f32>(0.2126f, 0.7152f, 0.0722f)) * 2.832059f)) * exp2(_e844))) * _e824) * _e836.w) * _e823) * _e780));
                            }
                            let _e886 = phi_5961_;
                            phi_5960_ = _e886;
                        }
                        let _e888 = phi_5960_;
                        let _e890 = global_1.member_1;
                        let _e893 = global.member_2;
                        let _e897 = (_e888 * (exp2(_e890) * select(1f, _e893, (_e893 > 0f))));
                        global_7 = vec4<f32>(_e897.x, _e897.y, _e897.z, 1f);
                        break;
                    }
                }
                break;
            }
            let _e903 = global_1.member_2;
            let _e906 = global_1.member_4;
            let _e909 = global_1.member_6;
            let _e915 = normalize(vec3<f32>(((_e138 * _e147) * _e144), (_e142 * _e144), -1f));
            let _e925 = normalize((((normalize(_e903) * _e915.x) + (normalize(_e906) * _e915.y)) + (normalize(_e909) * -(_e915.z))));
            let _e928 = global_1.member_28[3u];
            if (_e928 >= 0.5f) {
                let _e931 = max(_e925.y, 0f);
                let _e933 = max(-(_e925.y), 0f);
                let _e937 = global_1.member_28;
                let _e941 = global_1.member_29;
                let _e946 = global_1.member_30;
                let _e951 = global_1.member_1;
                let _e954 = global.member_2;
                let _e958 = ((((_e937.xyz * _e931) + (_e941.xyz * ((1f - _e931) - _e933))) + (_e946.xyz * _e933)) * (exp2(_e951) * select(1f, _e954, (_e954 > 0f))));
                global_7 = vec4<f32>(_e958.x, _e958.y, _e958.z, 1f);
                break;
            }
            let _e965 = global_1.member_26[1u];
            if (_e965 >= 0f) {
                let _e969 = global_1.member_26[0u];
                let _e970 = sin(_e969);
                let _e971 = cos(_e969);
                let _e988 = textureSampleLevel(global_3, global_2, vec2<f32>(((atan2(((_e970 * _e925.x) + (_e971 * _e925.z)), ((_e971 * _e925.x) - (_e970 * _e925.z))) * 0.15915494f) + 0.5f), (acos(clamp(_e925.y, -1f, 1f)) * 0.31830987f)), 0f);
                let _e992 = global_1.member_1;
                let _e995 = global.member_2;
                let _e999 = ((_e988.xyz * _e965) * (exp2(_e992) * select(1f, _e995, (_e995 > 0f))));
                global_7 = vec4<f32>(_e999.x, _e999.y, _e999.z, 1f);
                break;
            }
            let _e1005 = global_4.member_30;
            let _e1007 = global_1.member;
            let _e1008 = (_e1007 - _e1005);
            let _e1009 = length(_e1008);
            let _e1012 = (_e1008 / vec3(max(_e1009, 0.00001f)));
            let _e1013 = dot(_e925, _e1012);
            let _e1015 = global_1.member_8;
            let _e1016 = normalize(_e1015);
            let _e1018 = global_1.member_23;
            let _e1019 = clamp(_e1018, 0f, 1f);
            let _e1026 = normalize(cross(_e1012, select(vec3<f32>(0f, 0f, 1f), vec3<f32>(1f, 0f, 0f), vec3((abs(_e1012.z) > 0.98f)))));
            let _e1028 = normalize(cross(_e1026, _e1012));
            let _e1030 = global_1.member_11;
            let _e1033 = global_1.member_10;
            let _e1034 = normalize(_e1033);
            let _e1036 = global_1.member_12;
            let _e1039 = max(0.000001f, select(0.026f, _e1036, (_e1036 > 0f)));
            let _e1046 = normalize(cross(select(vec3<f32>(1f, 0f, 0f), vec3<f32>(0f, 1f, 0f), vec3((abs(_e1034.y) < 0.99f))), _e1034));
            let _e1047 = cross(_e1034, _e1046);
            let _e1058 = ((((asin(clamp(_e1013, -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
            let _e1064 = (0.5f + ((0.5f * sign(_e1058)) * sqrt(abs(_e1058))));
            let _e1065 = vec2<f32>(((atan2(dot(_e925, _e1028), dot(_e925, _e1026)) * 0.15915494f) + 0.5f), _e1064);
            let _e1070 = textureSampleLevel(global_3, global_2, vec2<f32>(_e1065.x, clamp(_e1064, 0.001953125f, 0.9980469f)), 0f);
            let _e1071 = _e1070.xyz;
            let _e1072 = dot(_e925, _e1016);
            let _e1074 = global_1.member_9;
            phi_5847_ = 0f;
            if ((_e1074 > 0f) && (_e1072 > 0f)) {
                let _e1080 = max(_e1074, 0.000001f);
                let _e1081 = (_e1080 * 0.25f);
                phi_5847_ = (clamp((1f - smoothstep((_e1080 - _e1081), (_e1080 + _e1081), acos(clamp(_e1072, -1f, 1f)))), 0f, 1f) * (1f - _e1019));
            }
            let _e1090 = phi_5847_;
            let _e1092 = (_e1008 / vec3(_e1009));
            let _e1094 = clamp(dot(_e925, _e1092), -1f, 1f);
            let _e1096 = global_4.member;
            let _e1098 = global_4.member_1;
            switch bitcast<i32>(0u) {
                default: {
                    let _e1100 = clamp(_e1009, _e1096, _e1098);
                    let _e1101 = (_e1100 * _e1094);
                    let _e1102 = (_e1094 < 0f);
                    phi_4666_ = _e1102;
                    if _e1102 {
                        phi_4666_ = (((_e1101 * _e1101) - ((_e1100 - _e1096) * (_e1100 + _e1096))) >= 0f);
                    }
                    let _e1110 = phi_4666_;
                    if _e1110 {
                        phi_5844_ = vec3<f32>(0f, 0f, 0f);
                        break;
                    }
                    let _e1111 = textureDimensions(global_6, 0i);
                    let _e1113 = vec2<f32>(vec2<i32>(_e1111));
                    let _e1117 = sqrt(((_e1098 - _e1096) * (_e1098 + _e1096)));
                    let _e1122 = sqrt(max(((_e1100 - _e1096) * (_e1100 + _e1096)), 0f));
                    let _e1123 = (_e1098 - _e1100);
                    let _e1125 = (_e1123 * (_e1098 + _e1100));
                    let _e1129 = sqrt(max((_e1125 + (_e1101 * _e1101)), 0f));
                    if (_e1101 > 0f) {
                        phi_5843_ = (_e1125 / (_e1129 + _e1101));
                    } else {
                        phi_5843_ = (_e1129 - _e1101);
                    }
                    let _e1135 = phi_5843_;
                    let _e1157 = textureSampleLevel(global_6, global_5, vec2<f32>(((0.5f / _e1113.x) + (clamp(((_e1135 - _e1123) / max(((_e1122 + _e1117) - _e1123), 0.001f)), 0f, 1f) * (1f - (1f / _e1113.x)))), ((0.5f / _e1113.y) + (clamp((_e1122 / _e1117), 0f, 1f) * (1f - (1f / _e1113.y))))), 0f);
                    phi_5844_ = _e1157.xyz;
                    break;
                }
            }
            let _e1160 = phi_5844_;
            let _e1162 = global_1.member_1;
            let _e1165 = global.member_2;
            let _e1168 = (exp2(_e1162) * select(1f, _e1165, (_e1165 > 0f)));
            let _e1170 = global_1.member_17;
            let _e1171 = max(_e1074, 0.000001f);
            let _e1183 = global_1.member_16;
            phi_5908_ = vec3<f32>(0f, 0f, 0f);
            phi_5850_ = 0f;
            if (_e1030 > 0f) {
                let _e1187 = dot(_e925, _e1034);
                phi_5909_ = vec3<f32>(0f, 0f, 0f);
                phi_5851_ = 0f;
                if (_e1187 > cos((_e1039 * 1.02f))) {
                    let _e1197 = (_e925 - (_e1034 * _e1187));
                    let _e1206 = vec2<f32>((((dot(_e1197, _e1046) / _e1039) * 0.5f) + 0.5f), (0.5f - ((dot(_e1197, _e1047) / _e1039) * 0.5f)));
                    let _e1207 = textureSampleLevel(global_9, global_8, _e1206, 0f);
                    let _e1211 = dot(_e1016, _e1034);
                    let _e1215 = global_1.member_15;
                    switch bitcast<i32>(0u) {
                        default: {
                            let _e1219 = (((_e1206 * 2f) - vec2<f32>(1f, 1f)) * vec2<f32>(1.1248595f, 1.1248595f));
                            let _e1220 = dot(_e1219, _e1219);
                            if (_e1220 > 1f) {
                                phi_5849_ = 0f;
                                break;
                            }
                            phi_5849_ = smoothstep(-0.04f, 0.04f, dot(vec3<f32>(_e1219.x, _e1219.y, sqrt(max(0f, (1f - _e1220)))), normalize(vec3<f32>(dot(_e1016, _e1046), -(dot(_e1016, _e1047)), -(_e1211)))));
                            break;
                        }
                    }
                    let _e1232 = phi_5849_;
                    let _e1250 = smoothstep(1f, 0.92f, (sqrt(max(0f, (2f * (1f - _e1187)))) / _e1039));
                    phi_5909_ = ((((mix((vec3<f32>(0.937f, 1.008f, 1.109f) * (0.000055f * (_e1211 + 1f))), vec3<f32>(1.2142f, 0.9614f, 0.7521f), vec3(_e1232)) * (((_e1030 / (0.000064439846f * max((0.5f * (1f - _e1211)), 0.001f))) * (dot(_e1207.xyz, vec3<f32>(0.2126f, 0.7152f, 0.0722f)) * 2.832059f)) * exp2(_e1215))) * _e1250) * _e1207.w) * _e1160);
                    phi_5851_ = (_e1250 * _e1207.w);
                }
                let _e1257 = phi_5909_;
                let _e1259 = phi_5851_;
                phi_5908_ = _e1257;
                phi_5850_ = _e1259;
            }
            let _e1261 = phi_5908_;
            let _e1263 = phi_5850_;
            phi_5863_ = _e1071;
            if (_e1263 > 0f) {
                phi_5853_ = vec3<f32>(0f, 0f, 0f);
                phi_5852_ = 0i;
                loop {
                    let _e1266 = phi_5853_;
                    let _e1268 = phi_5852_;
                    local_4 = _e1266;
                    if (_e1268 < 4i) {
                        continue;
                    } else {
                        break;
                    }
                    continuing {
                        let _e1271 = (f32(_e1268) * 1.5707964f);
                        let _e1281 = ((_e1034 * cos(_e1039)) + (((_e1046 * cos(_e1271)) + (_e1047 * sin(_e1271))) * sin(_e1039)));
                        let _e1293 = ((((asin(clamp(dot(_e1281, _e1012), -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                        let _e1299 = (0.5f + ((0.5f * sign(_e1293)) * sqrt(abs(_e1293))));
                        let _e1300 = vec2<f32>(((atan2(dot(_e1281, _e1028), dot(_e1281, _e1026)) * 0.15915494f) + 0.5f), _e1299);
                        let _e1305 = textureSampleLevel(global_3, global_2, vec2<f32>(_e1300.x, clamp(_e1299, 0.001953125f, 0.9980469f)), 0f);
                        phi_5853_ = (_e1266 + _e1305.xyz);
                        phi_5852_ = (_e1268 + 1i);
                    }
                }
                let _e1310 = local_4;
                phi_5863_ = mix(_e1071, (_e1310 * 0.25f), vec3(clamp((_e1263 * dot(_e1160, vec3<f32>(0.2126f, 0.7152f, 0.0722f))), 0f, 1f)));
            }
            let _e1318 = phi_5863_;
            let _e1320 = global_4.member_24;
            let _e1321 = (_e1320 == 2u);
            let _e1323 = global_4.member_8;
            let _e1325 = global_4.member_9;
            let _e1327 = global_4.member_10;
            let _e1329 = global_4.member_18;
            let _e1331 = global_4.member_23;
            let _e1333 = global_4.member_17;
            let _e1335 = global_4.member_13;
            let _e1336 = max(_e1325, 0f);
            let _e1338 = clamp(_e1019, 0f, 1f);
            let _e1340 = mix((_e1323 * _e1336), _e1327, vec3(_e1338));
            phi_5866_ = _e1318;
            if (_e1321 && (_e1013 < 0f)) {
                phi_5866_ = mix(mix(_e1340, _e1331, vec3(clamp(_e1329, 0f, 1f))), _e1340, vec3(clamp((-(_e1013) * max(_e1333, 0.001f)), 0f, 1f)));
            }
            let _e1353 = phi_5866_;
            phi_5867_ = _e1353;
            if ((_e1019 > 0f) && (_e1013 > select(-1f, 0f, _e1321))) {
                if _e1321 {
                    phi_5864_ = max(_e1013, 0f);
                } else {
                    phi_5864_ = abs(_e1013);
                }
                let _e1361 = phi_5864_;
                let _e1369 = fract((((normalize(_e925) * 37f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                let _e1374 = (_e1369 + vec3(dot(_e1369, (_e1369.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                phi_5867_ = (_e1353 + ((mix(_e1335, vec3<f32>(0.000000698f, 0.00000085f, 0.000001163f), vec3(smoothstep(0f, 0.85f, _e1361))) * (1f + ((fract(((_e1374.x * _e1374.y) * _e1374.z)) - 0.5f) * 0.05f))) * _e1338));
            }
            let _e1388 = phi_5867_;
            phi_5901_ = _e1388;
            if ((_e1320 == 1u) && (_e1013 < 0.005f)) {
                phi_5869_ = vec3<f32>(0f, 0f, 0f);
                phi_5868_ = 0i;
                loop {
                    let _e1393 = phi_5869_;
                    let _e1395 = phi_5868_;
                    local_5 = _e1393;
                    if (_e1395 < 16i) {
                        continue;
                    } else {
                        break;
                    }
                    continuing {
                        let _e1397 = f32(_e1395);
                        let _e1399 = ((_e1397 + 0.5f) * 0.0625f);
                        let _e1400 = sqrt(_e1399);
                        let _e1404 = (_e1397 * 2.3999631f);
                        let _e1417 = ((((asin(clamp(sqrt(max((1f - _e1399), 0f)), -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                        let _e1423 = (0.5f + ((0.5f * sign(_e1417)) * sqrt(abs(_e1417))));
                        let _e1424 = vec2<f32>(((atan2((_e1400 * sin(_e1404)), (_e1400 * cos(_e1404))) * 0.15915494f) + 0.5f), _e1423);
                        let _e1429 = textureSampleLevel(global_3, global_2, vec2<f32>(_e1424.x, clamp(_e1423, 0.001953125f, 0.9980469f)), 0f);
                        phi_5869_ = (_e1393 + _e1429.xyz);
                        phi_5868_ = (_e1395 + 1i);
                    }
                }
                let _e1434 = local_5;
                let _e1437 = global_4.member_4;
                let _e1439 = global_4.member_2;
                let _e1441 = global_4.member_25;
                switch bitcast<i32>(0u) {
                    default: {
                        let _e1444 = (_e1009 - _e1096);
                        if (_e1444 <= (_e1096 * 0.0000008f)) {
                            phi_5898_ = 0f;
                            phi_5890_ = select(vec3<f32>(0f, 1f, 0f), _e1092, vec3((_e1009 > 0f)));
                        } else {
                            switch bitcast<i32>(0u) {
                                default: {
                                    let _e1450 = (_e1005 - _e1007);
                                    let _e1451 = dot(_e1450, _e925);
                                    let _e1453 = (_e1450 - (_e925 * _e1451));
                                    let _e1454 = dot(_e1453, _e1453);
                                    let _e1455 = (_e1096 * _e1096);
                                    if (_e1454 > _e1455) {
                                        phi_5872_ = f32();
                                        phi_5871_ = false;
                                        break;
                                    }
                                    phi_5872_ = (_e1451 - sqrt(max((_e1455 - _e1454), 0f)));
                                    phi_5871_ = true;
                                    break;
                                }
                            }
                            let _e1462 = phi_5872_;
                            let _e1464 = phi_5871_;
                            if (!(_e1464) || (_e1462 <= 0f)) {
                                phi_5900_ = _e1388;
                                break;
                            }
                            phi_5898_ = _e1462;
                            phi_5890_ = normalize(((_e1007 + (_e925 * _e1462)) - _e1005));
                        }
                        let _e1473 = phi_5898_;
                        let _e1475 = phi_5890_;
                        let _e1476 = dot(_e1475, _e1016);
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e1478 = clamp(_e1096, _e1096, _e1098);
                                let _e1479 = (_e1478 * _e1476);
                                let _e1480 = (_e1476 < 0f);
                                phi_5533_ = _e1480;
                                if _e1480 {
                                    phi_5533_ = (((_e1479 * _e1479) - ((_e1478 - _e1096) * (_e1478 + _e1096))) >= 0f);
                                }
                                let _e1488 = phi_5533_;
                                if _e1488 {
                                    phi_5892_ = vec3<f32>(0f, 0f, 0f);
                                    break;
                                }
                                let _e1489 = textureDimensions(global_6, 0i);
                                let _e1491 = vec2<f32>(vec2<i32>(_e1489));
                                let _e1495 = sqrt(((_e1098 - _e1096) * (_e1098 + _e1096)));
                                let _e1500 = sqrt(max(((_e1478 - _e1096) * (_e1478 + _e1096)), 0f));
                                let _e1501 = (_e1098 - _e1478);
                                let _e1503 = (_e1501 * (_e1098 + _e1478));
                                let _e1507 = sqrt(max((_e1503 + (_e1479 * _e1479)), 0f));
                                if (_e1479 > 0f) {
                                    phi_5891_ = (_e1503 / (_e1507 + _e1479));
                                } else {
                                    phi_5891_ = (_e1507 - _e1479);
                                }
                                let _e1513 = phi_5891_;
                                let _e1535 = textureSampleLevel(global_6, global_5, vec2<f32>(((0.5f / _e1491.x) + (clamp(((_e1513 - _e1501) / max(((_e1500 + _e1495) - _e1501), 0.001f)), 0f, 1f) * (1f - (1f / _e1491.x)))), ((0.5f / _e1491.y) + (clamp((_e1500 / _e1495), 0f, 1f) * (1f - (1f / _e1491.y))))), 0f);
                                phi_5892_ = _e1535.xyz;
                                break;
                            }
                        }
                        let _e1538 = phi_5892_;
                        phi_5900_ = mix(_e1388, mix(_e1388, ((((((((_e1323 * 0.31830987f) * max(_e1476, 0f)) * _e1538) * _e1183) * _e1170) + (_e1323 * (_e1434 * vec3<f32>(0.0625f, 0.0625f, 0.0625f)))) * _e1336) + (_e1327 * _e1338)), vec3(exp((-(_e1473) * ((dot(_e1437, vec3<f32>(0.299f, 0.587f, 0.114f)) * exp((-(_e1444) / _e1439))) * max(_e1441, 0f)))))), vec3(smoothstep(-0.004f, 0f, -(_e1013))));
                        break;
                    }
                }
                let _e1567 = phi_5900_;
                phi_5901_ = _e1567;
            }
            let _e1569 = phi_5901_;
            phi_5902_ = _e1569;
            if _e1321 {
                let _e1571 = global_4.member_15;
                let _e1573 = global_4.member_16;
                let _e1580 = global_4.member_11;
                let _e1582 = global_4.member_12;
                phi_5902_ = mix(_e1569, mix(_e1580, _e1582, vec3(_e1019)), vec3(clamp((1f - smoothstep(0f, max(mix(_e1571, _e1573, _e1019), 0.00001f), abs(_e1013))), 0f, 1f)));
            }
            let _e1589 = phi_5902_;
            let _e1592 = (((_e1589 + (((_e1183 * min((_e1170 / (3.1415927f * ((_e1171 * _e1171) + ((_e1171 * 0.05f) * (_e1171 * 0.25f))))), (32752f / max(_e1168, 0.000001f)))) * _e1090) * _e1160)) + _e1261) * _e1168);
            global_7 = vec4<f32>(_e1592.x, _e1592.y, _e1592.z, 1f);
            break;
        }
    }
    return;
}

@fragment
fn main(@location(0) param: vec2<f32>) -> @location(0) vec4<f32> {
    global_10 = param;
    function_();
    let _e3 = global_7;
    return _e3;
}
