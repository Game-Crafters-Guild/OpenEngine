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
    var phi_5980_: vec3<f32>;
    var phi_5938_: vec3<f32>;
    var phi_5939_: vec3<f32>;
    var phi_5940_: vec3<f32>;
    var phi_5944_: vec3<f32>;
    var phi_5942_: f32;
    var phi_5945_: vec3<f32>;
    var phi_5946_: vec3<f32>;
    var local: bool;
    var local_1: f32;
    var phi_5955_: vec3<f32>;
    var phi_5953_: f32;
    var phi_5956_: vec3<f32>;
    var phi_5957_: vec3<f32>;
    var local_2: bool;
    var local_3: f32;
    var phi_5961_: vec3<f32>;
    var phi_5959_: f32;
    var phi_5962_: vec3<f32>;
    var phi_5963_: vec3<f32>;
    var phi_5964_: vec3<f32>;
    var phi_2923_: bool;
    var phi_5965_: vec2<f32>;
    var phi_5970_: f32;
    var phi_5966_: f32;
    var phi_5976_: vec3<f32>;
    var phi_5972_: vec2<f32>;
    var phi_5971_: vec2<f32>;
    var phi_5973_: f32;
    var phi_5979_: vec3<f32>;
    var phi_5978_: vec3<f32>;
    var phi_5865_: f32;
    var phi_4684_: bool;
    var phi_5861_: f32;
    var phi_5862_: vec3<f32>;
    var phi_5867_: f32;
    var phi_5927_: vec3<f32>;
    var phi_5869_: f32;
    var phi_5926_: vec3<f32>;
    var phi_5868_: f32;
    var phi_5871_: vec3<f32>;
    var phi_5870_: i32;
    var local_4: vec3<f32>;
    var phi_5881_: vec3<f32>;
    var phi_5884_: vec3<f32>;
    var phi_5882_: f32;
    var phi_5885_: vec3<f32>;
    var phi_5887_: vec3<f32>;
    var phi_5886_: i32;
    var local_5: vec3<f32>;
    var phi_5890_: f32;
    var phi_5889_: bool;
    var phi_5916_: f32;
    var phi_5908_: vec3<f32>;
    var phi_5551_: bool;
    var phi_5909_: f32;
    var phi_5910_: vec3<f32>;
    var phi_5918_: vec3<f32>;
    var phi_5919_: vec3<f32>;
    var phi_5920_: vec3<f32>;

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
                                phi_5980_ = mix(_e166.xyz, _e169.xyz, vec3(smoothstep(0f, 0.5f, _e159)));
                            } else {
                                let _e175 = global_1.member_29;
                                let _e178 = global_1.member_30;
                                phi_5980_ = mix(_e175.xyz, _e178.xyz, vec3(smoothstep(0.5f, 1f, _e159)));
                            }
                            let _e184 = phi_5980_;
                            let _e186 = global_1.member_1;
                            let _e189 = global.member_2;
                            let _e193 = (_e184 * (exp2(_e186) * select(1f, _e189, (_e189 > 0f))));
                            global_7 = vec4<f32>(_e193.x, _e193.y, _e193.z, 1f);
                            break;
                        }
                        let _e199 = global_4.member;
                        let _e203 = global_1.member;
                        let _e205 = normalize((_e203 - vec3<f32>(0f, -(_e199), 0f)));
                        let _e207 = global_1.member_6;
                        let _e210 = (_e207 - (_e205 * dot(_e207, _e205)));
                        phi_5938_ = _e210;
                        if (dot(_e210, _e210) < 0.000001f) {
                            let _e214 = global_1.member_4;
                            phi_5938_ = (_e214 - (_e205 * dot(_e214, _e205)));
                        }
                        let _e219 = phi_5938_;
                        phi_5939_ = _e219;
                        if (dot(_e219, _e219) < 0.000001f) {
                            let _e223 = global_1.member_2;
                            phi_5939_ = (_e223 - (_e205 * dot(_e223, _e205)));
                        }
                        let _e228 = phi_5939_;
                        phi_5940_ = _e228;
                        if (dot(_e228, _e228) < 0.000001f) {
                            let _e235 = select(vec3<f32>(1f, 0f, 0f), vec3<f32>(0f, 0f, 1f), vec3((abs(_e205.z) < 0.98f)));
                            phi_5940_ = (_e235 - (_e205 * dot(_e235, _e205)));
                        }
                        let _e240 = phi_5940_;
                        let _e241 = normalize(_e240);
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e245 = global_1.member_26[1u];
                                let _e246 = (_e245 >= 0f);
                                local = _e246;
                                local_1 = _e245;
                                local_2 = _e246;
                                local_3 = _e245;
                                if _e246 {
                                    let _e249 = global_1.member_26[0u];
                                    let _e250 = sin(_e249);
                                    let _e251 = cos(_e249);
                                    let _e268 = textureSampleLevel(global_3, global_2, vec2<f32>(((atan2(((_e250 * _e205.x) + (_e251 * _e205.z)), ((_e251 * _e205.x) - (_e250 * _e205.z))) * 0.15915494f) + 0.5f), (acos(clamp(_e205.y, -1f, 1f)) * 0.31830987f)), 0f);
                                    phi_5946_ = (_e268.xyz * _e245);
                                    break;
                                }
                                let _e277 = normalize(cross(_e205, select(vec3<f32>(0f, 0f, 1f), vec3<f32>(1f, 0f, 0f), vec3((abs(_e205.z) > 0.98f)))));
                                let _e281 = dot(_e205, _e205);
                                let _e291 = ((((asin(clamp(_e281, -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                                let _e297 = (0.5f + ((0.5f * sign(_e291)) * sqrt(abs(_e291))));
                                let _e298 = vec2<f32>(((atan2(dot(_e205, normalize(cross(_e277, _e205))), dot(_e205, _e277)) * 0.15915494f) + 0.5f), _e297);
                                let _e303 = textureSampleLevel(global_3, global_2, vec2<f32>(_e298.x, clamp(_e297, 0.001953125f, 0.9980469f)), 0f);
                                let _e306 = global_4.member_24;
                                let _e307 = (_e306 == 2u);
                                let _e309 = global_4.member_8;
                                let _e311 = global_4.member_9;
                                let _e313 = global_4.member_10;
                                let _e315 = global_4.member_18;
                                let _e317 = global_4.member_23;
                                let _e319 = global_4.member_17;
                                let _e321 = global_4.member_13;
                                let _e324 = clamp(_e152, 0f, 1f);
                                let _e326 = mix((_e309 * max(_e311, 0f)), _e313, vec3(_e324));
                                phi_5944_ = _e303.xyz;
                                if (_e307 && (_e281 < 0f)) {
                                    phi_5944_ = mix(mix(_e326, _e317, vec3(clamp(_e315, 0f, 1f))), _e326, vec3(clamp((-(_e281) * max(_e319, 0.001f)), 0f, 1f)));
                                }
                                let _e339 = phi_5944_;
                                phi_5945_ = _e339;
                                if ((_e152 > 0f) && (_e281 > select(-1f, 0f, _e307))) {
                                    if _e307 {
                                        phi_5942_ = max(_e281, 0f);
                                    } else {
                                        phi_5942_ = abs(_e281);
                                    }
                                    let _e347 = phi_5942_;
                                    let _e355 = fract((((normalize(_e205) * 37f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e360 = (_e355 + vec3(dot(_e355, (_e355.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    phi_5945_ = (_e339 + ((mix(_e321, vec3<f32>(0.000000698f, 0.00000085f, 0.000001163f), vec3(smoothstep(0f, 0.85f, _e347))) * (1f + ((fract(((_e360.x * _e360.y) * _e360.z)) - 0.5f) * 0.05f))) * _e324));
                                }
                                let _e374 = phi_5945_;
                                phi_5946_ = _e374;
                                break;
                            }
                        }
                        let _e376 = phi_5946_;
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e379 = local;
                                if _e379 {
                                    let _e382 = global_1.member_26[0u];
                                    let _e383 = sin(_e382);
                                    let _e384 = cos(_e382);
                                    let _e401 = textureSampleLevel(global_3, global_2, vec2<f32>(((atan2(((_e383 * _e241.x) + (_e384 * _e241.z)), ((_e384 * _e241.x) - (_e383 * _e241.z))) * 0.15915494f) + 0.5f), (acos(clamp(_e241.y, -1f, 1f)) * 0.31830987f)), 0f);
                                    let _e404 = local_1;
                                    phi_5957_ = (_e401.xyz * _e404);
                                    break;
                                }
                                let _e412 = normalize(cross(_e205, select(vec3<f32>(0f, 0f, 1f), vec3<f32>(1f, 0f, 0f), vec3((abs(_e205.z) > 0.98f)))));
                                let _e416 = dot(_e241, _e205);
                                let _e426 = ((((asin(clamp(_e416, -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                                let _e432 = (0.5f + ((0.5f * sign(_e426)) * sqrt(abs(_e426))));
                                let _e433 = vec2<f32>(((atan2(dot(_e241, normalize(cross(_e412, _e205))), dot(_e241, _e412)) * 0.15915494f) + 0.5f), _e432);
                                let _e438 = textureSampleLevel(global_3, global_2, vec2<f32>(_e433.x, clamp(_e432, 0.001953125f, 0.9980469f)), 0f);
                                let _e441 = global_4.member_24;
                                let _e442 = (_e441 == 2u);
                                let _e444 = global_4.member_8;
                                let _e446 = global_4.member_9;
                                let _e448 = global_4.member_10;
                                let _e450 = global_4.member_18;
                                let _e452 = global_4.member_23;
                                let _e454 = global_4.member_17;
                                let _e456 = global_4.member_13;
                                let _e459 = clamp(_e152, 0f, 1f);
                                let _e461 = mix((_e444 * max(_e446, 0f)), _e448, vec3(_e459));
                                phi_5955_ = _e438.xyz;
                                if (_e442 && (_e416 < 0f)) {
                                    phi_5955_ = mix(mix(_e461, _e452, vec3(clamp(_e450, 0f, 1f))), _e461, vec3(clamp((-(_e416) * max(_e454, 0.001f)), 0f, 1f)));
                                }
                                let _e474 = phi_5955_;
                                phi_5956_ = _e474;
                                if ((_e152 > 0f) && (_e416 > select(-1f, 0f, _e442))) {
                                    if _e442 {
                                        phi_5953_ = max(_e416, 0f);
                                    } else {
                                        phi_5953_ = abs(_e416);
                                    }
                                    let _e482 = phi_5953_;
                                    let _e490 = fract((((normalize(_e241) * 37f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e495 = (_e490 + vec3(dot(_e490, (_e490.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    phi_5956_ = (_e474 + ((mix(_e456, vec3<f32>(0.000000698f, 0.00000085f, 0.000001163f), vec3(smoothstep(0f, 0.85f, _e482))) * (1f + ((fract(((_e495.x * _e495.y) * _e495.z)) - 0.5f) * 0.05f))) * _e459));
                                }
                                let _e509 = phi_5956_;
                                phi_5957_ = _e509;
                                break;
                            }
                        }
                        let _e511 = phi_5957_;
                        let _e512 = -(_e205);
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e515 = local_2;
                                if _e515 {
                                    let _e518 = global_1.member_26[0u];
                                    let _e519 = sin(_e518);
                                    let _e520 = cos(_e518);
                                    let _e537 = textureSampleLevel(global_3, global_2, vec2<f32>(((atan2(((_e519 * _e512.x) + (_e520 * _e512.z)), ((_e520 * _e512.x) - (_e519 * _e512.z))) * 0.15915494f) + 0.5f), (acos(clamp(_e512.y, -1f, 1f)) * 0.31830987f)), 0f);
                                    let _e540 = local_3;
                                    phi_5963_ = (_e537.xyz * _e540);
                                    break;
                                }
                                let _e548 = normalize(cross(_e205, select(vec3<f32>(0f, 0f, 1f), vec3<f32>(1f, 0f, 0f), vec3((abs(_e205.z) > 0.98f)))));
                                let _e552 = dot(_e512, _e205);
                                let _e562 = ((((asin(clamp(_e552, -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                                let _e568 = (0.5f + ((0.5f * sign(_e562)) * sqrt(abs(_e562))));
                                let _e569 = vec2<f32>(((atan2(dot(_e512, normalize(cross(_e548, _e205))), dot(_e512, _e548)) * 0.15915494f) + 0.5f), _e568);
                                let _e574 = textureSampleLevel(global_3, global_2, vec2<f32>(_e569.x, clamp(_e568, 0.001953125f, 0.9980469f)), 0f);
                                let _e577 = global_4.member_24;
                                let _e578 = (_e577 == 2u);
                                let _e580 = global_4.member_8;
                                let _e582 = global_4.member_9;
                                let _e584 = global_4.member_10;
                                let _e586 = global_4.member_18;
                                let _e588 = global_4.member_23;
                                let _e590 = global_4.member_17;
                                let _e592 = global_4.member_13;
                                let _e595 = clamp(_e152, 0f, 1f);
                                let _e597 = mix((_e580 * max(_e582, 0f)), _e584, vec3(_e595));
                                phi_5961_ = _e574.xyz;
                                if (_e578 && (_e552 < 0f)) {
                                    phi_5961_ = mix(mix(_e597, _e588, vec3(clamp(_e586, 0f, 1f))), _e597, vec3(clamp((-(_e552) * max(_e590, 0.001f)), 0f, 1f)));
                                }
                                let _e610 = phi_5961_;
                                phi_5962_ = _e610;
                                if ((_e152 > 0f) && (_e552 > select(-1f, 0f, _e578))) {
                                    if _e578 {
                                        phi_5959_ = max(_e552, 0f);
                                    } else {
                                        phi_5959_ = abs(_e552);
                                    }
                                    let _e618 = phi_5959_;
                                    let _e626 = fract((((normalize(_e512) * 37f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e631 = (_e626 + vec3(dot(_e626, (_e626.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    phi_5962_ = (_e610 + ((mix(_e592, vec3<f32>(0.000000698f, 0.00000085f, 0.000001163f), vec3(smoothstep(0f, 0.85f, _e618))) * (1f + ((fract(((_e631.x * _e631.y) * _e631.z)) - 0.5f) * 0.05f))) * _e595));
                                }
                                let _e645 = phi_5962_;
                                phi_5963_ = _e645;
                                break;
                            }
                        }
                        let _e647 = phi_5963_;
                        if (_e159 < 0.5f) {
                            phi_5964_ = mix(_e376, _e511, vec3(smoothstep(0f, 0.5f, _e159)));
                        } else {
                            phi_5964_ = mix(_e511, _e647, vec3(smoothstep(0.5f, 1f, _e159)));
                        }
                        let _e656 = phi_5964_;
                        let _e658 = global_4.member_15;
                        let _e660 = global_4.member_16;
                        let _e669 = mix(_e656, _e511, vec3(clamp((1f - smoothstep(0f, max((mix(_e658, _e660, _e152) * 3f), 0.015f), abs(_e156))), 0f, 1f)));
                        let _e671 = global_1.member_8;
                        let _e672 = normalize(_e671);
                        let _e674 = global_1.member_9;
                        let _e675 = (_e674 > 0f);
                        phi_2923_ = _e675;
                        if _e675 {
                            phi_2923_ = (_e672.y > -0.18f);
                        }
                        let _e679 = phi_2923_;
                        phi_5976_ = _e669;
                        if (_e679 && (_e152 < 0.98f)) {
                            let _e683 = global_1.member_2;
                            let _e684 = normalize(_e683);
                            let _e686 = global_1.member_4;
                            let _e687 = normalize(_e686);
                            let _e690 = global_1.member_3;
                            let _e694 = max(((_e674 * max(_e690, 0.1f)) * 16f), 0.055f);
                            let _e696 = global_1.member_13;
                            if (_e696 > 0.5f) {
                                let _e700 = vec2<f32>(dot(_e672, _e684), dot(_e672, _e687));
                                let _e701 = length(_e700);
                                phi_5965_ = _e700;
                                if (_e701 > 0.82f) {
                                    phi_5965_ = (_e700 * (0.82f / _e701));
                                }
                                let _e706 = phi_5965_;
                                let _e713 = vec2<f32>(((_e138 - (_e706.x / _e147)) * _e147), (_e156 - _e706.y));
                                let _e715 = -(dot(_e713, _e713));
                                let _e716 = (_e694 * _e694);
                                phi_5970_ = exp((_e715 / max(_e716, 0.000001f)));
                                phi_5966_ = exp((_e715 / max((_e716 * 26f), 0.000001f)));
                            } else {
                                let _e730 = vec2<f32>(((_e138 - (dot(_e672, _e684) / _e147)) * _e147), (_e156 - dot(_e672, _e687)));
                                let _e732 = smoothstep(-0.05f, 0.05f, dot(_e672, normalize(_e207)));
                                let _e734 = -(dot(_e730, _e730));
                                let _e735 = (_e694 * _e694);
                                phi_5970_ = (exp((_e734 / max(_e735, 0.000001f))) * _e732);
                                phi_5966_ = (exp((_e734 / max((_e735 * 26f), 0.000001f))) * _e732);
                            }
                            let _e746 = phi_5970_;
                            let _e748 = phi_5966_;
                            let _e750 = smoothstep(-0.05f, 0.05f, _e672.y);
                            let _e751 = smoothstep(-0.006f, 0.006f, _e156);
                            let _e752 = (1f - _e152);
                            let _e756 = global_1.member_16;
                            let _e760 = global_1.member_17;
                            phi_5976_ = ((_e669 + (((mix(_e756, vec3<f32>(1f, 0.48f, 0.18f), vec3((1f - smoothstep(0.04f, 0.55f, _e672.y)))) * ((clamp((_e760 * 0.018f), 0f, 1.2f) * _e750) * _e752)) * _e748) * _e751)) + (((_e756 * (((_e760 * 0.4f) * _e750) * _e752)) * _e746) * _e751));
                        }
                        let _e777 = phi_5976_;
                        let _e779 = global_1.member_10;
                        let _e780 = normalize(_e779);
                        let _e782 = smoothstep(-0.18f, 0.02f, _e780.y);
                        let _e784 = global_1.member_11;
                        phi_5978_ = _e777;
                        if ((_e784 > 0f) && (_e782 > 0f)) {
                            let _e789 = global_1.member_2;
                            let _e790 = normalize(_e789);
                            let _e792 = global_1.member_4;
                            let _e793 = normalize(_e792);
                            let _e796 = vec2<f32>(dot(_e780, _e790), dot(_e780, _e793));
                            let _e798 = global_1.member_13;
                            phi_5971_ = _e796;
                            if (_e798 > 0.5f) {
                                let _e800 = length(_e796);
                                phi_5972_ = _e796;
                                if (_e800 > 0.82f) {
                                    phi_5972_ = (_e796 * (0.82f / _e800));
                                }
                                let _e805 = phi_5972_;
                                phi_5971_ = _e805;
                            }
                            let _e807 = phi_5971_;
                            let _e812 = global_1.member_12;
                            let _e815 = global_1.member_5;
                            let _e818 = max(((_e812 * 6f) * max(_e815, 0.1f)), 0.05f);
                            let _e822 = vec2<f32>(((_e138 - (_e807.x / _e147)) * _e147), (_e156 - _e807.y));
                            let _e825 = smoothstep(-0.006f, 0.006f, _e156);
                            let _e826 = smoothstep(1f, 0.92f, (length(_e822) / _e818));
                            phi_5979_ = _e777;
                            if ((_e826 * _e825) > 0f) {
                                let _e832 = (((_e822 / vec2(_e818)) * 0.5f) + vec2<f32>(0.5f, 0.5f));
                                let _e837 = vec2<f32>(_e832.x, (1f - _e832.y));
                                let _e838 = textureSampleLevel(global_9, global_8, _e837, 0f);
                                let _e842 = dot(_e672, _e780);
                                let _e846 = global_1.member_15;
                                switch bitcast<i32>(0u) {
                                    default: {
                                        let _e850 = (((_e837 * 2f) - vec2<f32>(1f, 1f)) * vec2<f32>(1.1248595f, 1.1248595f));
                                        let _e851 = dot(_e850, _e850);
                                        if (_e851 > 1f) {
                                            phi_5973_ = 0f;
                                            break;
                                        }
                                        phi_5973_ = smoothstep(-0.04f, 0.04f, dot(vec3<f32>(_e850.x, _e850.y, sqrt(max(0f, (1f - _e851)))), normalize(vec3<f32>(dot(_e672, _e790), -(dot(_e672, _e793)), -(_e842)))));
                                        break;
                                    }
                                }
                                let _e863 = phi_5973_;
                                phi_5979_ = (_e777 + (((((mix((vec3<f32>(0.937f, 1.008f, 1.109f) * (0.000055f * (_e842 + 1f))), vec3<f32>(1.2142f, 0.9614f, 0.7521f), vec3(_e863)) * (((_e784 / (0.000064439846f * max((0.5f * (1f - _e842)), 0.001f))) * (dot(_e838.xyz, vec3<f32>(0.2126f, 0.7152f, 0.0722f)) * 2.832059f)) * exp2(_e846))) * _e826) * _e838.w) * _e825) * _e782));
                            }
                            let _e888 = phi_5979_;
                            phi_5978_ = _e888;
                        }
                        let _e890 = phi_5978_;
                        let _e892 = global_1.member_1;
                        let _e895 = global.member_2;
                        let _e899 = (_e890 * (exp2(_e892) * select(1f, _e895, (_e895 > 0f))));
                        global_7 = vec4<f32>(_e899.x, _e899.y, _e899.z, 1f);
                        break;
                    }
                }
                break;
            }
            let _e905 = global_1.member_2;
            let _e908 = global_1.member_4;
            let _e911 = global_1.member_6;
            let _e917 = normalize(vec3<f32>(((_e138 * _e147) * _e144), (_e142 * _e144), -1f));
            let _e927 = normalize((((normalize(_e905) * _e917.x) + (normalize(_e908) * _e917.y)) + (normalize(_e911) * -(_e917.z))));
            let _e930 = global_1.member_28[3u];
            if (_e930 >= 0.5f) {
                let _e933 = max(_e927.y, 0f);
                let _e935 = max(-(_e927.y), 0f);
                let _e939 = global_1.member_28;
                let _e943 = global_1.member_29;
                let _e948 = global_1.member_30;
                let _e953 = global_1.member_1;
                let _e956 = global.member_2;
                let _e960 = ((((_e939.xyz * _e933) + (_e943.xyz * ((1f - _e933) - _e935))) + (_e948.xyz * _e935)) * (exp2(_e953) * select(1f, _e956, (_e956 > 0f))));
                global_7 = vec4<f32>(_e960.x, _e960.y, _e960.z, 1f);
                break;
            }
            let _e967 = global_1.member_26[1u];
            if (_e967 >= 0f) {
                let _e971 = global_1.member_26[0u];
                let _e972 = sin(_e971);
                let _e973 = cos(_e971);
                let _e990 = textureSampleLevel(global_3, global_2, vec2<f32>(((atan2(((_e972 * _e927.x) + (_e973 * _e927.z)), ((_e973 * _e927.x) - (_e972 * _e927.z))) * 0.15915494f) + 0.5f), (acos(clamp(_e927.y, -1f, 1f)) * 0.31830987f)), 0f);
                let _e994 = global_1.member_1;
                let _e997 = global.member_2;
                let _e1001 = ((_e990.xyz * _e967) * (exp2(_e994) * select(1f, _e997, (_e997 > 0f))));
                global_7 = vec4<f32>(_e1001.x, _e1001.y, _e1001.z, 1f);
                break;
            }
            let _e1007 = global_4.member;
            let _e1009 = vec3<f32>(0f, -(_e1007), 0f);
            let _e1011 = global_1.member;
            let _e1012 = (_e1011 - _e1009);
            let _e1013 = length(_e1012);
            let _e1016 = (_e1012 / vec3(max(_e1013, 0.00001f)));
            let _e1017 = dot(_e927, _e1016);
            let _e1019 = global_1.member_8;
            let _e1020 = normalize(_e1019);
            let _e1022 = global_1.member_23;
            let _e1023 = clamp(_e1022, 0f, 1f);
            let _e1030 = normalize(cross(_e1016, select(vec3<f32>(0f, 0f, 1f), vec3<f32>(1f, 0f, 0f), vec3((abs(_e1016.z) > 0.98f)))));
            let _e1032 = normalize(cross(_e1030, _e1016));
            let _e1034 = global_1.member_11;
            let _e1037 = global_1.member_10;
            let _e1038 = normalize(_e1037);
            let _e1040 = global_1.member_12;
            let _e1043 = max(0.000001f, select(0.026f, _e1040, (_e1040 > 0f)));
            let _e1050 = normalize(cross(select(vec3<f32>(1f, 0f, 0f), vec3<f32>(0f, 1f, 0f), vec3((abs(_e1038.y) < 0.99f))), _e1038));
            let _e1051 = cross(_e1038, _e1050);
            let _e1062 = ((((asin(clamp(_e1017, -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
            let _e1068 = (0.5f + ((0.5f * sign(_e1062)) * sqrt(abs(_e1062))));
            let _e1069 = vec2<f32>(((atan2(dot(_e927, _e1032), dot(_e927, _e1030)) * 0.15915494f) + 0.5f), _e1068);
            let _e1074 = textureSampleLevel(global_3, global_2, vec2<f32>(_e1069.x, clamp(_e1068, 0.001953125f, 0.9980469f)), 0f);
            let _e1075 = _e1074.xyz;
            let _e1076 = dot(_e927, _e1020);
            let _e1078 = global_1.member_9;
            phi_5865_ = 0f;
            if ((_e1078 > 0f) && (_e1076 > 0f)) {
                let _e1084 = max(_e1078, 0.000001f);
                let _e1085 = (_e1084 * 0.25f);
                phi_5865_ = (clamp((1f - smoothstep((_e1084 - _e1085), (_e1084 + _e1085), acos(clamp(_e1076, -1f, 1f)))), 0f, 1f) * (1f - _e1023));
            }
            let _e1094 = phi_5865_;
            let _e1096 = (_e1012 / vec3(_e1013));
            let _e1098 = clamp(dot(_e927, _e1096), -1f, 1f);
            let _e1100 = global_4.member_1;
            switch bitcast<i32>(0u) {
                default: {
                    let _e1102 = clamp(_e1013, _e1007, _e1100);
                    let _e1103 = (_e1102 * _e1098);
                    let _e1104 = (_e1098 < 0f);
                    phi_4684_ = _e1104;
                    if _e1104 {
                        phi_4684_ = (((_e1103 * _e1103) - ((_e1102 - _e1007) * (_e1102 + _e1007))) >= 0f);
                    }
                    let _e1112 = phi_4684_;
                    if _e1112 {
                        phi_5862_ = vec3<f32>(0f, 0f, 0f);
                        break;
                    }
                    let _e1113 = textureDimensions(global_6, 0i);
                    let _e1115 = vec2<f32>(vec2<i32>(_e1113));
                    let _e1119 = sqrt(((_e1100 - _e1007) * (_e1100 + _e1007)));
                    let _e1124 = sqrt(max(((_e1102 - _e1007) * (_e1102 + _e1007)), 0f));
                    let _e1125 = (_e1100 - _e1102);
                    let _e1127 = (_e1125 * (_e1100 + _e1102));
                    let _e1131 = sqrt(max((_e1127 + (_e1103 * _e1103)), 0f));
                    if (_e1103 > 0f) {
                        phi_5861_ = (_e1127 / (_e1131 + _e1103));
                    } else {
                        phi_5861_ = (_e1131 - _e1103);
                    }
                    let _e1137 = phi_5861_;
                    let _e1159 = textureSampleLevel(global_6, global_5, vec2<f32>(((0.5f / _e1115.x) + (clamp(((_e1137 - _e1125) / max(((_e1124 + _e1119) - _e1125), 0.001f)), 0f, 1f) * (1f - (1f / _e1115.x)))), ((0.5f / _e1115.y) + (clamp((_e1124 / _e1119), 0f, 1f) * (1f - (1f / _e1115.y))))), 0f);
                    phi_5862_ = _e1159.xyz;
                    break;
                }
            }
            let _e1162 = phi_5862_;
            let _e1164 = global_1.member_1;
            let _e1167 = global.member_2;
            let _e1170 = (exp2(_e1164) * select(1f, _e1167, (_e1167 > 0f)));
            let _e1172 = global_1.member_17;
            let _e1173 = max(_e1078, 0.000001f);
            let _e1185 = global_1.member_16;
            phi_5926_ = vec3<f32>(0f, 0f, 0f);
            phi_5868_ = 0f;
            if (_e1034 > 0f) {
                let _e1189 = dot(_e927, _e1038);
                phi_5927_ = vec3<f32>(0f, 0f, 0f);
                phi_5869_ = 0f;
                if (_e1189 > cos((_e1043 * 1.02f))) {
                    let _e1199 = (_e927 - (_e1038 * _e1189));
                    let _e1208 = vec2<f32>((((dot(_e1199, _e1050) / _e1043) * 0.5f) + 0.5f), (0.5f - ((dot(_e1199, _e1051) / _e1043) * 0.5f)));
                    let _e1209 = textureSampleLevel(global_9, global_8, _e1208, 0f);
                    let _e1213 = dot(_e1020, _e1038);
                    let _e1217 = global_1.member_15;
                    switch bitcast<i32>(0u) {
                        default: {
                            let _e1221 = (((_e1208 * 2f) - vec2<f32>(1f, 1f)) * vec2<f32>(1.1248595f, 1.1248595f));
                            let _e1222 = dot(_e1221, _e1221);
                            if (_e1222 > 1f) {
                                phi_5867_ = 0f;
                                break;
                            }
                            phi_5867_ = smoothstep(-0.04f, 0.04f, dot(vec3<f32>(_e1221.x, _e1221.y, sqrt(max(0f, (1f - _e1222)))), normalize(vec3<f32>(dot(_e1020, _e1050), -(dot(_e1020, _e1051)), -(_e1213)))));
                            break;
                        }
                    }
                    let _e1234 = phi_5867_;
                    let _e1252 = smoothstep(1f, 0.92f, (sqrt(max(0f, (2f * (1f - _e1189)))) / _e1043));
                    phi_5927_ = ((((mix((vec3<f32>(0.937f, 1.008f, 1.109f) * (0.000055f * (_e1213 + 1f))), vec3<f32>(1.2142f, 0.9614f, 0.7521f), vec3(_e1234)) * (((_e1034 / (0.000064439846f * max((0.5f * (1f - _e1213)), 0.001f))) * (dot(_e1209.xyz, vec3<f32>(0.2126f, 0.7152f, 0.0722f)) * 2.832059f)) * exp2(_e1217))) * _e1252) * _e1209.w) * _e1162);
                    phi_5869_ = (_e1252 * _e1209.w);
                }
                let _e1259 = phi_5927_;
                let _e1261 = phi_5869_;
                phi_5926_ = _e1259;
                phi_5868_ = _e1261;
            }
            let _e1263 = phi_5926_;
            let _e1265 = phi_5868_;
            phi_5881_ = _e1075;
            if (_e1265 > 0f) {
                phi_5871_ = vec3<f32>(0f, 0f, 0f);
                phi_5870_ = 0i;
                loop {
                    let _e1268 = phi_5871_;
                    let _e1270 = phi_5870_;
                    local_4 = _e1268;
                    if (_e1270 < 4i) {
                        continue;
                    } else {
                        break;
                    }
                    continuing {
                        let _e1273 = (f32(_e1270) * 1.5707964f);
                        let _e1283 = ((_e1038 * cos(_e1043)) + (((_e1050 * cos(_e1273)) + (_e1051 * sin(_e1273))) * sin(_e1043)));
                        let _e1295 = ((((asin(clamp(dot(_e1283, _e1016), -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                        let _e1301 = (0.5f + ((0.5f * sign(_e1295)) * sqrt(abs(_e1295))));
                        let _e1302 = vec2<f32>(((atan2(dot(_e1283, _e1032), dot(_e1283, _e1030)) * 0.15915494f) + 0.5f), _e1301);
                        let _e1307 = textureSampleLevel(global_3, global_2, vec2<f32>(_e1302.x, clamp(_e1301, 0.001953125f, 0.9980469f)), 0f);
                        phi_5871_ = (_e1268 + _e1307.xyz);
                        phi_5870_ = (_e1270 + 1i);
                    }
                }
                let _e1312 = local_4;
                phi_5881_ = mix(_e1075, (_e1312 * 0.25f), vec3(clamp((_e1265 * dot(_e1162, vec3<f32>(0.2126f, 0.7152f, 0.0722f))), 0f, 1f)));
            }
            let _e1320 = phi_5881_;
            let _e1322 = global_4.member_24;
            let _e1323 = (_e1322 == 2u);
            let _e1325 = global_4.member_8;
            let _e1327 = global_4.member_9;
            let _e1329 = global_4.member_10;
            let _e1331 = global_4.member_18;
            let _e1333 = global_4.member_23;
            let _e1335 = global_4.member_17;
            let _e1337 = global_4.member_13;
            let _e1338 = max(_e1327, 0f);
            let _e1340 = clamp(_e1023, 0f, 1f);
            let _e1342 = mix((_e1325 * _e1338), _e1329, vec3(_e1340));
            phi_5884_ = _e1320;
            if (_e1323 && (_e1017 < 0f)) {
                phi_5884_ = mix(mix(_e1342, _e1333, vec3(clamp(_e1331, 0f, 1f))), _e1342, vec3(clamp((-(_e1017) * max(_e1335, 0.001f)), 0f, 1f)));
            }
            let _e1355 = phi_5884_;
            phi_5885_ = _e1355;
            if ((_e1023 > 0f) && (_e1017 > select(-1f, 0f, _e1323))) {
                if _e1323 {
                    phi_5882_ = max(_e1017, 0f);
                } else {
                    phi_5882_ = abs(_e1017);
                }
                let _e1363 = phi_5882_;
                let _e1371 = fract((((normalize(_e927) * 37f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                let _e1376 = (_e1371 + vec3(dot(_e1371, (_e1371.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                phi_5885_ = (_e1355 + ((mix(_e1337, vec3<f32>(0.000000698f, 0.00000085f, 0.000001163f), vec3(smoothstep(0f, 0.85f, _e1363))) * (1f + ((fract(((_e1376.x * _e1376.y) * _e1376.z)) - 0.5f) * 0.05f))) * _e1340));
            }
            let _e1390 = phi_5885_;
            phi_5919_ = _e1390;
            if ((_e1322 == 1u) && (_e1017 < 0.005f)) {
                phi_5887_ = vec3<f32>(0f, 0f, 0f);
                phi_5886_ = 0i;
                loop {
                    let _e1395 = phi_5887_;
                    let _e1397 = phi_5886_;
                    local_5 = _e1395;
                    if (_e1397 < 16i) {
                        continue;
                    } else {
                        break;
                    }
                    continuing {
                        let _e1399 = f32(_e1397);
                        let _e1401 = ((_e1399 + 0.5f) * 0.0625f);
                        let _e1402 = sqrt(_e1401);
                        let _e1406 = (_e1399 * 2.3999631f);
                        let _e1419 = ((((asin(clamp(sqrt(max((1f - _e1401), 0f)), -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                        let _e1425 = (0.5f + ((0.5f * sign(_e1419)) * sqrt(abs(_e1419))));
                        let _e1426 = vec2<f32>(((atan2((_e1402 * sin(_e1406)), (_e1402 * cos(_e1406))) * 0.15915494f) + 0.5f), _e1425);
                        let _e1431 = textureSampleLevel(global_3, global_2, vec2<f32>(_e1426.x, clamp(_e1425, 0.001953125f, 0.9980469f)), 0f);
                        phi_5887_ = (_e1395 + _e1431.xyz);
                        phi_5886_ = (_e1397 + 1i);
                    }
                }
                let _e1436 = local_5;
                let _e1439 = global_4.member_4;
                let _e1441 = global_4.member_2;
                let _e1443 = global_4.member_25;
                switch bitcast<i32>(0u) {
                    default: {
                        let _e1446 = (_e1013 - _e1007);
                        if (_e1446 <= (_e1007 * 0.0000008f)) {
                            phi_5916_ = 0f;
                            phi_5908_ = select(vec3<f32>(0f, 1f, 0f), _e1096, vec3((_e1013 > 0f)));
                        } else {
                            switch bitcast<i32>(0u) {
                                default: {
                                    let _e1452 = (_e1009 - _e1011);
                                    let _e1453 = dot(_e1452, _e927);
                                    let _e1455 = (_e1452 - (_e927 * _e1453));
                                    let _e1456 = dot(_e1455, _e1455);
                                    let _e1457 = (_e1007 * _e1007);
                                    if (_e1456 > _e1457) {
                                        phi_5890_ = f32();
                                        phi_5889_ = false;
                                        break;
                                    }
                                    phi_5890_ = (_e1453 - sqrt(max((_e1457 - _e1456), 0f)));
                                    phi_5889_ = true;
                                    break;
                                }
                            }
                            let _e1464 = phi_5890_;
                            let _e1466 = phi_5889_;
                            if (!(_e1466) || (_e1464 <= 0f)) {
                                phi_5918_ = _e1390;
                                break;
                            }
                            phi_5916_ = _e1464;
                            phi_5908_ = normalize(((_e1011 + (_e927 * _e1464)) - _e1009));
                        }
                        let _e1475 = phi_5916_;
                        let _e1477 = phi_5908_;
                        let _e1478 = dot(_e1477, _e1020);
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e1480 = clamp(_e1007, _e1007, _e1100);
                                let _e1481 = (_e1480 * _e1478);
                                let _e1482 = (_e1478 < 0f);
                                phi_5551_ = _e1482;
                                if _e1482 {
                                    phi_5551_ = (((_e1481 * _e1481) - ((_e1480 - _e1007) * (_e1480 + _e1007))) >= 0f);
                                }
                                let _e1490 = phi_5551_;
                                if _e1490 {
                                    phi_5910_ = vec3<f32>(0f, 0f, 0f);
                                    break;
                                }
                                let _e1491 = textureDimensions(global_6, 0i);
                                let _e1493 = vec2<f32>(vec2<i32>(_e1491));
                                let _e1497 = sqrt(((_e1100 - _e1007) * (_e1100 + _e1007)));
                                let _e1502 = sqrt(max(((_e1480 - _e1007) * (_e1480 + _e1007)), 0f));
                                let _e1503 = (_e1100 - _e1480);
                                let _e1505 = (_e1503 * (_e1100 + _e1480));
                                let _e1509 = sqrt(max((_e1505 + (_e1481 * _e1481)), 0f));
                                if (_e1481 > 0f) {
                                    phi_5909_ = (_e1505 / (_e1509 + _e1481));
                                } else {
                                    phi_5909_ = (_e1509 - _e1481);
                                }
                                let _e1515 = phi_5909_;
                                let _e1537 = textureSampleLevel(global_6, global_5, vec2<f32>(((0.5f / _e1493.x) + (clamp(((_e1515 - _e1503) / max(((_e1502 + _e1497) - _e1503), 0.001f)), 0f, 1f) * (1f - (1f / _e1493.x)))), ((0.5f / _e1493.y) + (clamp((_e1502 / _e1497), 0f, 1f) * (1f - (1f / _e1493.y))))), 0f);
                                phi_5910_ = _e1537.xyz;
                                break;
                            }
                        }
                        let _e1540 = phi_5910_;
                        phi_5918_ = mix(_e1390, mix(_e1390, ((((((((_e1325 * 0.31830987f) * max(_e1478, 0f)) * _e1540) * _e1185) * _e1172) + (_e1325 * (_e1436 * vec3<f32>(0.0625f, 0.0625f, 0.0625f)))) * _e1338) + (_e1329 * _e1340)), vec3(exp((-(_e1475) * ((dot(_e1439, vec3<f32>(0.299f, 0.587f, 0.114f)) * exp((-(_e1446) / _e1441))) * max(_e1443, 0f)))))), vec3(smoothstep(-0.004f, 0f, -(_e1017))));
                        break;
                    }
                }
                let _e1569 = phi_5918_;
                phi_5919_ = _e1569;
            }
            let _e1571 = phi_5919_;
            phi_5920_ = _e1571;
            if _e1323 {
                let _e1573 = global_4.member_15;
                let _e1575 = global_4.member_16;
                let _e1582 = global_4.member_11;
                let _e1584 = global_4.member_12;
                phi_5920_ = mix(_e1571, mix(_e1582, _e1584, vec3(_e1023)), vec3(clamp((1f - smoothstep(0f, max(mix(_e1573, _e1575, _e1023), 0.00001f), abs(_e1017))), 0f, 1f)));
            }
            let _e1591 = phi_5920_;
            let _e1594 = (((_e1591 + (((_e1185 * min((_e1172 / (3.1415927f * ((_e1173 * _e1173) + ((_e1173 * 0.05f) * (_e1173 * 0.25f))))), (32752f / max(_e1170, 0.000001f)))) * _e1094) * _e1162)) + _e1263) * _e1170);
            global_7 = vec4<f32>(_e1594.x, _e1594.y, _e1594.z, 1f);
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
