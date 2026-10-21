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
    member_31: vec4<f32>,
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
    var phi_7883_: vec3<f32>;
    var phi_7832_: vec3<f32>;
    var phi_7833_: vec3<f32>;
    var phi_7834_: vec3<f32>;
    var phi_7838_: vec3<f32>;
    var phi_7836_: f32;
    var phi_7839_: vec3<f32>;
    var phi_7840_: vec3<f32>;
    var local: bool;
    var local_1: f32;
    var phi_7849_: vec3<f32>;
    var phi_7847_: f32;
    var phi_7850_: vec3<f32>;
    var phi_7851_: vec3<f32>;
    var local_2: bool;
    var local_3: f32;
    var phi_7855_: vec3<f32>;
    var phi_7853_: f32;
    var phi_7856_: vec3<f32>;
    var phi_7857_: vec3<f32>;
    var phi_7858_: vec3<f32>;
    var phi_3783_: bool;
    var phi_7859_: vec2<f32>;
    var phi_7864_: f32;
    var phi_7860_: f32;
    var phi_7872_: vec3<f32>;
    var phi_7873_: vec3<f32>;
    var phi_7865_: i32;
    var phi_7878_: vec2<f32>;
    var phi_7877_: vec2<f32>;
    var phi_7871_: vec3<f32>;
    var phi_7867_: vec2<f32>;
    var phi_7866_: vec2<f32>;
    var phi_7868_: f32;
    var phi_7876_: vec3<f32>;
    var phi_7875_: vec3<f32>;
    var phi_7721_: f32;
    var phi_6103_: bool;
    var phi_7717_: f32;
    var phi_7718_: vec3<f32>;
    var phi_7723_: f32;
    var phi_7820_: vec3<f32>;
    var phi_7725_: f32;
    var phi_7819_: vec3<f32>;
    var phi_7724_: f32;
    var phi_7727_: vec3<f32>;
    var phi_7726_: i32;
    var local_4: vec3<f32>;
    var phi_7739_: vec3<f32>;
    var phi_7787_: vec3<f32>;
    var phi_7737_: i32;
    var phi_7786_: vec3<f32>;
    var phi_7743_: vec3<f32>;
    var phi_7741_: f32;
    var phi_7744_: vec3<f32>;
    var phi_7746_: vec3<f32>;
    var phi_7745_: i32;
    var local_5: vec3<f32>;
    var phi_7749_: f32;
    var phi_7748_: bool;
    var phi_7777_: f32;
    var phi_7769_: vec3<f32>;
    var phi_7337_: bool;
    var phi_7770_: f32;
    var phi_7771_: vec3<f32>;
    var phi_7779_: vec3<f32>;
    var phi_7780_: vec3<f32>;
    var phi_7781_: vec3<f32>;
    var local_6: vec3<f32>;
    var local_7: vec3<f32>;
    var local_8: vec3<f32>;

    switch bitcast<i32>(0u) {
        default: {
            let _e200 = global_10;
            let _e203 = ((_e200.x * 2f) - 1f);
            let _e207 = (((1f - _e200.y) * 2f) - 1f);
            let _e209 = global_1.member_21;
            let _e211 = global_1.member_20;
            let _e212 = max(_e211, 0.001f);
            if (_e209 <= 0f) {
                switch bitcast<i32>(0u) {
                    default: {
                        let _e216 = global_1.member_23;
                        let _e217 = clamp(_e216, 0f, 1f);
                        let _e219 = global_1.member_7;
                        let _e221 = (_e207 - clamp(_e219, -1f, 1f));
                        let _e224 = clamp(((1f - _e221) * 0.5f), 0f, 1f);
                        let _e227 = global_1.member_29[3u];
                        if (_e227 >= 0.5f) {
                            if (_e224 < 0.5f) {
                                let _e231 = global_1.member_29;
                                let _e234 = global_1.member_30;
                                phi_7883_ = mix(_e231.xyz, _e234.xyz, vec3(smoothstep(0f, 0.5f, _e224)));
                            } else {
                                let _e240 = global_1.member_30;
                                let _e243 = global_1.member_31;
                                phi_7883_ = mix(_e240.xyz, _e243.xyz, vec3(smoothstep(0.5f, 1f, _e224)));
                            }
                            let _e249 = phi_7883_;
                            let _e251 = global_1.member_1;
                            let _e254 = global.member_2;
                            let _e258 = (_e249 * (exp2(_e251) * select(1f, _e254, (_e254 > 0f))));
                            global_7 = vec4<f32>(_e258.x, _e258.y, _e258.z, 1f);
                            break;
                        }
                        let _e264 = global_4.member;
                        let _e268 = global_1.member;
                        let _e270 = normalize((_e268 - vec3<f32>(0f, -(_e264), 0f)));
                        let _e272 = global_1.member_6;
                        let _e275 = (_e272 - (_e270 * dot(_e272, _e270)));
                        phi_7832_ = _e275;
                        if (dot(_e275, _e275) < 0.000001f) {
                            let _e279 = global_1.member_4;
                            phi_7832_ = (_e279 - (_e270 * dot(_e279, _e270)));
                        }
                        let _e284 = phi_7832_;
                        phi_7833_ = _e284;
                        if (dot(_e284, _e284) < 0.000001f) {
                            let _e288 = global_1.member_2;
                            phi_7833_ = (_e288 - (_e270 * dot(_e288, _e270)));
                        }
                        let _e293 = phi_7833_;
                        phi_7834_ = _e293;
                        if (dot(_e293, _e293) < 0.000001f) {
                            let _e300 = select(vec3<f32>(1f, 0f, 0f), vec3<f32>(0f, 0f, 1f), vec3((abs(_e270.z) < 0.98f)));
                            phi_7834_ = (_e300 - (_e270 * dot(_e300, _e270)));
                        }
                        let _e305 = phi_7834_;
                        let _e306 = normalize(_e305);
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e310 = global_1.member_26[1u];
                                let _e311 = (_e310 >= 0f);
                                local = _e311;
                                local_1 = _e310;
                                local_2 = _e311;
                                local_3 = _e310;
                                if _e311 {
                                    let _e314 = global_1.member_26[0u];
                                    let _e315 = sin(_e314);
                                    let _e316 = cos(_e314);
                                    let _e333 = textureSampleLevel(global_3, global_2, vec2<f32>(((atan2(((_e315 * _e270.x) + (_e316 * _e270.z)), ((_e316 * _e270.x) - (_e315 * _e270.z))) * 0.15915494f) + 0.5f), (acos(clamp(_e270.y, -1f, 1f)) * 0.31830987f)), 0f);
                                    phi_7840_ = (_e333.xyz * _e310);
                                    break;
                                }
                                let _e342 = normalize(cross(_e270, select(vec3<f32>(0f, 0f, 1f), vec3<f32>(1f, 0f, 0f), vec3((abs(_e270.z) > 0.98f)))));
                                let _e346 = dot(_e270, _e270);
                                let _e356 = ((((asin(clamp(_e346, -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                                let _e362 = (0.5f + ((0.5f * sign(_e356)) * sqrt(abs(_e356))));
                                let _e363 = vec2<f32>(((atan2(dot(_e270, normalize(cross(_e342, _e270))), dot(_e270, _e342)) * 0.15915494f) + 0.5f), _e362);
                                let _e368 = textureSampleLevel(global_3, global_2, vec2<f32>(_e363.x, clamp(_e362, 0.001953125f, 0.9980469f)), 0f);
                                let _e371 = global_4.member_24;
                                let _e372 = (_e371 == 2u);
                                let _e374 = global_4.member_8;
                                let _e376 = global_4.member_9;
                                let _e378 = global_4.member_10;
                                let _e380 = global_4.member_18;
                                let _e382 = global_4.member_23;
                                let _e384 = global_4.member_17;
                                let _e386 = global_4.member_13;
                                let _e389 = clamp(_e217, 0f, 1f);
                                let _e391 = mix((_e374 * max(_e376, 0f)), _e378, vec3(_e389));
                                phi_7838_ = _e368.xyz;
                                if (_e372 && (_e346 < 0f)) {
                                    phi_7838_ = mix(mix(_e391, _e382, vec3(clamp(_e380, 0f, 1f))), _e391, vec3(clamp((-(_e346) * max(_e384, 0.001f)), 0f, 1f)));
                                }
                                let _e404 = phi_7838_;
                                phi_7839_ = _e404;
                                if ((_e217 > 0f) && (_e346 > select(-1f, 0f, _e372))) {
                                    if _e372 {
                                        phi_7836_ = max(_e346, 0f);
                                    } else {
                                        phi_7836_ = abs(_e346);
                                    }
                                    let _e412 = phi_7836_;
                                    let _e420 = fract((((normalize(_e270) * 37f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e425 = (_e420 + vec3(dot(_e420, (_e420.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    phi_7839_ = (_e404 + ((mix(_e386, vec3<f32>(0.000000698f, 0.00000085f, 0.000001163f), vec3(smoothstep(0f, 0.85f, _e412))) * (1f + ((fract(((_e425.x * _e425.y) * _e425.z)) - 0.5f) * 0.05f))) * _e389));
                                }
                                let _e439 = phi_7839_;
                                phi_7840_ = _e439;
                                break;
                            }
                        }
                        let _e441 = phi_7840_;
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e444 = local;
                                if _e444 {
                                    let _e447 = global_1.member_26[0u];
                                    let _e448 = sin(_e447);
                                    let _e449 = cos(_e447);
                                    let _e466 = textureSampleLevel(global_3, global_2, vec2<f32>(((atan2(((_e448 * _e306.x) + (_e449 * _e306.z)), ((_e449 * _e306.x) - (_e448 * _e306.z))) * 0.15915494f) + 0.5f), (acos(clamp(_e306.y, -1f, 1f)) * 0.31830987f)), 0f);
                                    let _e469 = local_1;
                                    phi_7851_ = (_e466.xyz * _e469);
                                    break;
                                }
                                let _e477 = normalize(cross(_e270, select(vec3<f32>(0f, 0f, 1f), vec3<f32>(1f, 0f, 0f), vec3((abs(_e270.z) > 0.98f)))));
                                let _e481 = dot(_e306, _e270);
                                let _e491 = ((((asin(clamp(_e481, -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                                let _e497 = (0.5f + ((0.5f * sign(_e491)) * sqrt(abs(_e491))));
                                let _e498 = vec2<f32>(((atan2(dot(_e306, normalize(cross(_e477, _e270))), dot(_e306, _e477)) * 0.15915494f) + 0.5f), _e497);
                                let _e503 = textureSampleLevel(global_3, global_2, vec2<f32>(_e498.x, clamp(_e497, 0.001953125f, 0.9980469f)), 0f);
                                let _e506 = global_4.member_24;
                                let _e507 = (_e506 == 2u);
                                let _e509 = global_4.member_8;
                                let _e511 = global_4.member_9;
                                let _e513 = global_4.member_10;
                                let _e515 = global_4.member_18;
                                let _e517 = global_4.member_23;
                                let _e519 = global_4.member_17;
                                let _e521 = global_4.member_13;
                                let _e524 = clamp(_e217, 0f, 1f);
                                let _e526 = mix((_e509 * max(_e511, 0f)), _e513, vec3(_e524));
                                phi_7849_ = _e503.xyz;
                                if (_e507 && (_e481 < 0f)) {
                                    phi_7849_ = mix(mix(_e526, _e517, vec3(clamp(_e515, 0f, 1f))), _e526, vec3(clamp((-(_e481) * max(_e519, 0.001f)), 0f, 1f)));
                                }
                                let _e539 = phi_7849_;
                                phi_7850_ = _e539;
                                if ((_e217 > 0f) && (_e481 > select(-1f, 0f, _e507))) {
                                    if _e507 {
                                        phi_7847_ = max(_e481, 0f);
                                    } else {
                                        phi_7847_ = abs(_e481);
                                    }
                                    let _e547 = phi_7847_;
                                    let _e555 = fract((((normalize(_e306) * 37f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e560 = (_e555 + vec3(dot(_e555, (_e555.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    phi_7850_ = (_e539 + ((mix(_e521, vec3<f32>(0.000000698f, 0.00000085f, 0.000001163f), vec3(smoothstep(0f, 0.85f, _e547))) * (1f + ((fract(((_e560.x * _e560.y) * _e560.z)) - 0.5f) * 0.05f))) * _e524));
                                }
                                let _e574 = phi_7850_;
                                phi_7851_ = _e574;
                                break;
                            }
                        }
                        let _e576 = phi_7851_;
                        let _e577 = -(_e270);
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e580 = local_2;
                                if _e580 {
                                    let _e583 = global_1.member_26[0u];
                                    let _e584 = sin(_e583);
                                    let _e585 = cos(_e583);
                                    let _e602 = textureSampleLevel(global_3, global_2, vec2<f32>(((atan2(((_e584 * _e577.x) + (_e585 * _e577.z)), ((_e585 * _e577.x) - (_e584 * _e577.z))) * 0.15915494f) + 0.5f), (acos(clamp(_e577.y, -1f, 1f)) * 0.31830987f)), 0f);
                                    let _e605 = local_3;
                                    phi_7857_ = (_e602.xyz * _e605);
                                    break;
                                }
                                let _e613 = normalize(cross(_e270, select(vec3<f32>(0f, 0f, 1f), vec3<f32>(1f, 0f, 0f), vec3((abs(_e270.z) > 0.98f)))));
                                let _e617 = dot(_e577, _e270);
                                let _e627 = ((((asin(clamp(_e617, -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                                let _e633 = (0.5f + ((0.5f * sign(_e627)) * sqrt(abs(_e627))));
                                let _e634 = vec2<f32>(((atan2(dot(_e577, normalize(cross(_e613, _e270))), dot(_e577, _e613)) * 0.15915494f) + 0.5f), _e633);
                                let _e639 = textureSampleLevel(global_3, global_2, vec2<f32>(_e634.x, clamp(_e633, 0.001953125f, 0.9980469f)), 0f);
                                let _e642 = global_4.member_24;
                                let _e643 = (_e642 == 2u);
                                let _e645 = global_4.member_8;
                                let _e647 = global_4.member_9;
                                let _e649 = global_4.member_10;
                                let _e651 = global_4.member_18;
                                let _e653 = global_4.member_23;
                                let _e655 = global_4.member_17;
                                let _e657 = global_4.member_13;
                                let _e660 = clamp(_e217, 0f, 1f);
                                let _e662 = mix((_e645 * max(_e647, 0f)), _e649, vec3(_e660));
                                phi_7855_ = _e639.xyz;
                                if (_e643 && (_e617 < 0f)) {
                                    phi_7855_ = mix(mix(_e662, _e653, vec3(clamp(_e651, 0f, 1f))), _e662, vec3(clamp((-(_e617) * max(_e655, 0.001f)), 0f, 1f)));
                                }
                                let _e675 = phi_7855_;
                                phi_7856_ = _e675;
                                if ((_e217 > 0f) && (_e617 > select(-1f, 0f, _e643))) {
                                    if _e643 {
                                        phi_7853_ = max(_e617, 0f);
                                    } else {
                                        phi_7853_ = abs(_e617);
                                    }
                                    let _e683 = phi_7853_;
                                    let _e691 = fract((((normalize(_e577) * 37f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e696 = (_e691 + vec3(dot(_e691, (_e691.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    phi_7856_ = (_e675 + ((mix(_e657, vec3<f32>(0.000000698f, 0.00000085f, 0.000001163f), vec3(smoothstep(0f, 0.85f, _e683))) * (1f + ((fract(((_e696.x * _e696.y) * _e696.z)) - 0.5f) * 0.05f))) * _e660));
                                }
                                let _e710 = phi_7856_;
                                phi_7857_ = _e710;
                                break;
                            }
                        }
                        let _e712 = phi_7857_;
                        if (_e224 < 0.5f) {
                            phi_7858_ = mix(_e441, _e576, vec3(smoothstep(0f, 0.5f, _e224)));
                        } else {
                            phi_7858_ = mix(_e576, _e712, vec3(smoothstep(0.5f, 1f, _e224)));
                        }
                        let _e721 = phi_7858_;
                        let _e723 = global_4.member_15;
                        let _e725 = global_4.member_16;
                        let _e734 = mix(_e721, _e576, vec3(clamp((1f - smoothstep(0f, max((mix(_e723, _e725, _e217) * 3f), 0.015f), abs(_e221))), 0f, 1f)));
                        let _e736 = global_1.member_8;
                        let _e737 = normalize(_e736);
                        let _e739 = global_1.member_9;
                        let _e740 = (_e739 > 0f);
                        phi_3783_ = _e740;
                        if _e740 {
                            phi_3783_ = (_e737.y > -0.18f);
                        }
                        let _e744 = phi_3783_;
                        phi_7872_ = _e734;
                        if (_e744 && (_e217 < 0.98f)) {
                            let _e748 = global_1.member_2;
                            let _e749 = normalize(_e748);
                            let _e751 = global_1.member_4;
                            let _e752 = normalize(_e751);
                            let _e755 = global_1.member_3;
                            let _e759 = max(((_e739 * max(_e755, 0.1f)) * 16f), 0.055f);
                            let _e761 = global_1.member_13;
                            if (_e761 > 0.5f) {
                                let _e765 = vec2<f32>(dot(_e737, _e749), dot(_e737, _e752));
                                let _e766 = length(_e765);
                                phi_7859_ = _e765;
                                if (_e766 > 0.82f) {
                                    phi_7859_ = (_e765 * (0.82f / _e766));
                                }
                                let _e771 = phi_7859_;
                                let _e778 = vec2<f32>(((_e203 - (_e771.x / _e212)) * _e212), (_e221 - _e771.y));
                                let _e780 = -(dot(_e778, _e778));
                                let _e781 = (_e759 * _e759);
                                phi_7864_ = exp((_e780 / max(_e781, 0.000001f)));
                                phi_7860_ = exp((_e780 / max((_e781 * 26f), 0.000001f)));
                            } else {
                                let _e795 = vec2<f32>(((_e203 - (dot(_e737, _e749) / _e212)) * _e212), (_e221 - dot(_e737, _e752)));
                                let _e797 = smoothstep(-0.05f, 0.05f, dot(_e737, normalize(_e272)));
                                let _e799 = -(dot(_e795, _e795));
                                let _e800 = (_e759 * _e759);
                                phi_7864_ = (exp((_e799 / max(_e800, 0.000001f))) * _e797);
                                phi_7860_ = (exp((_e799 / max((_e800 * 26f), 0.000001f))) * _e797);
                            }
                            let _e811 = phi_7864_;
                            let _e813 = phi_7860_;
                            let _e815 = smoothstep(-0.05f, 0.05f, _e737.y);
                            let _e816 = smoothstep(-0.006f, 0.006f, _e221);
                            let _e817 = (1f - _e217);
                            let _e821 = global_1.member_16;
                            let _e825 = global_1.member_17;
                            phi_7872_ = ((_e734 + (((mix(_e821, vec3<f32>(1f, 0.48f, 0.18f), vec3((1f - smoothstep(0.04f, 0.55f, _e737.y)))) * ((clamp((_e825 * 0.018f), 0f, 1.2f) * _e815) * _e817)) * _e813) * _e816)) + (((_e821 * (((_e825 * 0.4f) * _e815) * _e817)) * _e811) * _e816));
                        }
                        let _e842 = phi_7872_;
                        let _e845 = global_1.member_27[0u];
                        phi_7871_ = _e842;
                        if ((_e845 > 0.5f) && (_e217 > 0.01f)) {
                            let _e850 = global_1.member_2;
                            let _e853 = global_1.member_4;
                            let _e858 = global_1.member_27[1u];
                            let _e862 = global_1.member_27[2u];
                            let _e866 = global_1.member_27[3u];
                            let _e867 = clamp(_e866, 0.2f, 50f);
                            let _e870 = global_1.member_28[0u];
                            let _e871 = clamp(_e870, 0.2f, 4f);
                            let _e874 = global_1.member_28[1u];
                            let _e875 = clamp(_e874, 0.1f, 4f);
                            let _e878 = global_1.member_28[3u];
                            phi_7873_ = _e842;
                            phi_7865_ = 0i;
                            loop {
                                let _e881 = phi_7873_;
                                let _e883 = phi_7865_;
                                local_7 = _e881;
                                if (_e883 < 8i) {
                                    let _e885 = f32(_e883);
                                    let _e891 = global_1.member_24;
                                    let _e898 = (((_e885 * 31.73f) + (floor(((_e891 * 0.017f) + (_e885 * 7.13f))) * 19.19f)) + 3.11f);
                                    let _e902 = fract(((vec3<f32>(_e898, 37f, 9f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e907 = (_e902 + vec3(dot(_e902, (_e902.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    let _e923 = fract(((vec3<f32>(_e898, 2f, 5f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e928 = (_e923 + vec3(dot(_e923, (_e923.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    let _e936 = fract(((_e891 / max((mix(8.5f, 1f, (clamp(_e862, 0f, 4f) * 0.25f)) * mix(0.58f, 1.72f, fract(((_e907.x * _e907.y) * _e907.z)))), 0.1f)) + fract(((_e928.x * _e928.y) * _e928.z))));
                                    let _e940 = fract(((vec3<f32>(_e898, 43f, 17f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e945 = (_e940 + vec3(dot(_e940, (_e940.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    let _e952 = mix(0.55f, 1.65f, fract(((_e945.x * _e945.y) * _e945.z)));
                                    let _e956 = mix(0.42f, 0.13f, clamp(((_e867 * _e952) * 0.25f), 0f, 1f));
                                    let _e965 = fract(((vec3<f32>(_e898, 11f, 7f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e970 = (_e965 + vec3(dot(_e965, (_e965.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    let _e980 = fract(((vec3<f32>(_e898, 41f, 29f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e985 = (_e980 + vec3(dot(_e980, (_e980.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    let _e994 = ((fract(((_e970.x * _e970.y) * _e970.z)) + (0.23f * fract(((_e985.x * _e985.y) * _e985.z)))) * 6.2831855f);
                                    let _e998 = fract(((vec3<f32>(_e898, 13f, 3f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e1003 = (_e998 + vec3(dot(_e998, (_e998.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    let _e1011 = mix(0.12f, 0.96f, pow(fract(((_e1003.x * _e1003.y) * _e1003.z)), 0.72f));
                                    let _e1015 = sqrt(max(0f, (1f - (_e1011 * _e1011))));
                                    let _e1021 = normalize(vec3<f32>((cos(_e994) * _e1015), _e1011, (sin(_e994) * _e1015)));
                                    let _e1023 = vec2<f32>((_e203 * _e212), _e221);
                                    let _e1027 = vec2<f32>((dot(_e1021, normalize(_e850)) / _e212), dot(_e1021, normalize(_e853)));
                                    let _e1029 = global_1.member_13;
                                    phi_7877_ = _e1027;
                                    if (_e1029 > 0.5f) {
                                        let _e1031 = length(_e1027);
                                        phi_7878_ = _e1027;
                                        if (_e1031 > 1.18f) {
                                            phi_7878_ = (_e1027 * (1.18f / _e1031));
                                        }
                                        let _e1036 = phi_7878_;
                                        phi_7877_ = _e1036;
                                    }
                                    let _e1038 = phi_7877_;
                                    let _e1042 = fract(((vec3<f32>(_e898, 71f, 5f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e1047 = (_e1042 + vec3(dot(_e1042, (_e1042.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    let _e1054 = mix(-2.55f, -0.58f, fract(((_e1047.x * _e1047.y) * _e1047.z)));
                                    let _e1059 = normalize(vec2<f32>((cos(_e1054) / _e212), sin(_e1054)));
                                    let _e1068 = (vec2<f32>((_e1038.x * _e212), _e1038.y) + (_e1059 * (((_e936 * 0.58f) * _e867) * _e952)));
                                    let _e1072 = fract(((vec3<f32>(_e898, 53f, 11f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e1077 = (_e1072 + vec3(dot(_e1072, (_e1072.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    let _e1084 = mix(0.55f, 1.75f, fract(((_e1077.x * _e1077.y) * _e1077.z)));
                                    let _e1088 = fract(((vec3<f32>(_e898, 61f, 13f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e1093 = (_e1088 + vec3(dot(_e1088, (_e1088.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    let _e1106 = max((((0.3f * _e871) * _e1084) * smoothstep(0f, (_e956 * 0.55f), _e936)), 0.0001f);
                                    let _e1107 = (_e1023 - _e1068);
                                    let _e1109 = dot(_e1107, -(_e1059));
                                    let _e1110 = clamp(_e1109, 0f, _e1106);
                                    let _e1114 = length((_e1023 - (_e1068 - (_e1059 * _e1110))));
                                    let _e1139 = (_e1107 / vec2((0.03f * clamp(_e878, 0.1f, 4f))));
                                    let _e1140 = dpdx(_e1139);
                                    let _e1141 = dpdy(_e1139);
                                    let _e1146 = (0.11111111f + (0.25f * (dot(_e1140, _e1140) + dot(_e1141, _e1141))));
                                    local_6 = (_e881 + ((((((((vec3<f32>(0.62f, 0.78f, 1f) * ((exp(((-(_e1114) * _e1114) * (105000f / max((_e875 * _e875), 0.01f)))) * exp((-(_e1110) * (20f / max((_e871 * _e1084), 0.2f))))) * (smoothstep(0f, 0.006f, _e1109) * (1f - smoothstep((_e1106 * 0.88f), _e1106, _e1109))))) * 0.85f) + ((vec3<f32>(1f, 0.96f, 0.82f) * (exp((-(dot(_e1139, _e1139)) / _e1146)) / (9f * _e1146))) * 2.4f)) * (smoothstep(0f, 0.035f, _e936) * (1f - smoothstep((_e956 * 0.58f), _e956, _e936)))) * step((_e885 + 0.5f), (clamp(_e858, 0f, 1f) * 8f))) * smoothstep(-0.03f, 0.03f, dot(_e1021, normalize(_e272)))) * _e217) * mix(0.55f, 1.85f, fract(((_e1093.x * _e1093.y) * _e1093.z)))));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e2355 = local_6;
                                    phi_7873_ = _e2355;
                                    phi_7865_ = (_e883 + 1i);
                                }
                            }
                            let _e2361 = local_7;
                            phi_7871_ = _e2361;
                        }
                        let _e1166 = phi_7871_;
                        let _e1168 = global_1.member_10;
                        let _e1169 = normalize(_e1168);
                        let _e1171 = smoothstep(-0.18f, 0.02f, _e1169.y);
                        let _e1173 = global_1.member_11;
                        phi_7875_ = _e1166;
                        if ((_e1173 > 0f) && (_e1171 > 0f)) {
                            let _e1178 = global_1.member_2;
                            let _e1179 = normalize(_e1178);
                            let _e1181 = global_1.member_4;
                            let _e1182 = normalize(_e1181);
                            let _e1185 = vec2<f32>(dot(_e1169, _e1179), dot(_e1169, _e1182));
                            let _e1187 = global_1.member_13;
                            phi_7866_ = _e1185;
                            if (_e1187 > 0.5f) {
                                let _e1189 = length(_e1185);
                                phi_7867_ = _e1185;
                                if (_e1189 > 0.82f) {
                                    phi_7867_ = (_e1185 * (0.82f / _e1189));
                                }
                                let _e1194 = phi_7867_;
                                phi_7866_ = _e1194;
                            }
                            let _e1196 = phi_7866_;
                            let _e1201 = global_1.member_12;
                            let _e1204 = global_1.member_5;
                            let _e1207 = max(((_e1201 * 6f) * max(_e1204, 0.1f)), 0.05f);
                            let _e1211 = vec2<f32>(((_e203 - (_e1196.x / _e212)) * _e212), (_e221 - _e1196.y));
                            let _e1214 = smoothstep(-0.006f, 0.006f, _e221);
                            let _e1215 = smoothstep(1f, 0.92f, (length(_e1211) / _e1207));
                            phi_7876_ = _e1166;
                            if ((_e1215 * _e1214) > 0f) {
                                let _e1221 = (((_e1211 / vec2(_e1207)) * 0.5f) + vec2<f32>(0.5f, 0.5f));
                                let _e1226 = vec2<f32>(_e1221.x, (1f - _e1221.y));
                                let _e1227 = textureSampleLevel(global_9, global_8, _e1226, 0f);
                                let _e1231 = dot(_e737, _e1169);
                                let _e1235 = global_1.member_15;
                                switch bitcast<i32>(0u) {
                                    default: {
                                        let _e1239 = (((_e1226 * 2f) - vec2<f32>(1f, 1f)) * vec2<f32>(1.1248595f, 1.1248595f));
                                        let _e1240 = dot(_e1239, _e1239);
                                        if (_e1240 > 1f) {
                                            phi_7868_ = 0f;
                                            break;
                                        }
                                        phi_7868_ = smoothstep(-0.04f, 0.04f, dot(vec3<f32>(_e1239.x, _e1239.y, sqrt(max(0f, (1f - _e1240)))), normalize(vec3<f32>(dot(_e737, _e1179), -(dot(_e737, _e1182)), -(_e1231)))));
                                        break;
                                    }
                                }
                                let _e1252 = phi_7868_;
                                phi_7876_ = (_e1166 + (((((mix((vec3<f32>(0.937f, 1.008f, 1.109f) * (0.000055f * (_e1231 + 1f))), vec3<f32>(1.2142f, 0.9614f, 0.7521f), vec3(_e1252)) * (((_e1173 / (0.000064439846f * max((0.5f * (1f - _e1231)), 0.001f))) * (dot(_e1227.xyz, vec3<f32>(0.2126f, 0.7152f, 0.0722f)) * 2.832059f)) * exp2(_e1235))) * _e1215) * _e1227.w) * _e1214) * _e1171));
                            }
                            let _e1277 = phi_7876_;
                            phi_7875_ = _e1277;
                        }
                        let _e1279 = phi_7875_;
                        let _e1281 = global_1.member_1;
                        let _e1284 = global.member_2;
                        let _e1288 = (_e1279 * (exp2(_e1281) * select(1f, _e1284, (_e1284 > 0f))));
                        global_7 = vec4<f32>(_e1288.x, _e1288.y, _e1288.z, 1f);
                        break;
                    }
                }
                break;
            }
            let _e1294 = global_1.member_2;
            let _e1295 = normalize(_e1294);
            let _e1297 = global_1.member_4;
            let _e1298 = normalize(_e1297);
            let _e1300 = global_1.member_6;
            let _e1306 = normalize(vec3<f32>(((_e203 * _e212) * _e209), (_e207 * _e209), -1f));
            let _e1316 = normalize((((_e1295 * _e1306.x) + (_e1298 * _e1306.y)) + (normalize(_e1300) * -(_e1306.z))));
            let _e1319 = global_1.member_29[3u];
            if (_e1319 >= 0.5f) {
                let _e1322 = max(_e1316.y, 0f);
                let _e1324 = max(-(_e1316.y), 0f);
                let _e1328 = global_1.member_29;
                let _e1332 = global_1.member_30;
                let _e1337 = global_1.member_31;
                let _e1342 = global_1.member_1;
                let _e1345 = global.member_2;
                let _e1349 = ((((_e1328.xyz * _e1322) + (_e1332.xyz * ((1f - _e1322) - _e1324))) + (_e1337.xyz * _e1324)) * (exp2(_e1342) * select(1f, _e1345, (_e1345 > 0f))));
                global_7 = vec4<f32>(_e1349.x, _e1349.y, _e1349.z, 1f);
                break;
            }
            let _e1356 = global_1.member_26[1u];
            if (_e1356 >= 0f) {
                let _e1360 = global_1.member_26[0u];
                let _e1361 = sin(_e1360);
                let _e1362 = cos(_e1360);
                let _e1379 = textureSampleLevel(global_3, global_2, vec2<f32>(((atan2(((_e1361 * _e1316.x) + (_e1362 * _e1316.z)), ((_e1362 * _e1316.x) - (_e1361 * _e1316.z))) * 0.15915494f) + 0.5f), (acos(clamp(_e1316.y, -1f, 1f)) * 0.31830987f)), 0f);
                let _e1383 = global_1.member_1;
                let _e1386 = global.member_2;
                let _e1390 = ((_e1379.xyz * _e1356) * (exp2(_e1383) * select(1f, _e1386, (_e1386 > 0f))));
                global_7 = vec4<f32>(_e1390.x, _e1390.y, _e1390.z, 1f);
                break;
            }
            let _e1396 = global_4.member;
            let _e1398 = vec3<f32>(0f, -(_e1396), 0f);
            let _e1400 = global_1.member;
            let _e1401 = (_e1400 - _e1398);
            let _e1402 = length(_e1401);
            let _e1405 = (_e1401 / vec3(max(_e1402, 0.00001f)));
            let _e1406 = dot(_e1316, _e1405);
            let _e1408 = global_1.member_8;
            let _e1409 = normalize(_e1408);
            let _e1411 = global_1.member_23;
            let _e1412 = clamp(_e1411, 0f, 1f);
            let _e1419 = normalize(cross(_e1405, select(vec3<f32>(0f, 0f, 1f), vec3<f32>(1f, 0f, 0f), vec3((abs(_e1405.z) > 0.98f)))));
            let _e1421 = normalize(cross(_e1419, _e1405));
            let _e1423 = global_1.member_11;
            let _e1426 = global_1.member_10;
            let _e1427 = normalize(_e1426);
            let _e1429 = global_1.member_12;
            let _e1432 = max(0.000001f, select(0.026f, _e1429, (_e1429 > 0f)));
            let _e1439 = normalize(cross(select(vec3<f32>(1f, 0f, 0f), vec3<f32>(0f, 1f, 0f), vec3((abs(_e1427.y) < 0.99f))), _e1427));
            let _e1440 = cross(_e1427, _e1439);
            let _e1451 = ((((asin(clamp(_e1406, -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
            let _e1457 = (0.5f + ((0.5f * sign(_e1451)) * sqrt(abs(_e1451))));
            let _e1458 = vec2<f32>(((atan2(dot(_e1316, _e1421), dot(_e1316, _e1419)) * 0.15915494f) + 0.5f), _e1457);
            let _e1463 = textureSampleLevel(global_3, global_2, vec2<f32>(_e1458.x, clamp(_e1457, 0.001953125f, 0.9980469f)), 0f);
            let _e1464 = _e1463.xyz;
            let _e1465 = dot(_e1316, _e1409);
            let _e1467 = global_1.member_9;
            phi_7721_ = 0f;
            if ((_e1467 > 0f) && (_e1465 > 0f)) {
                let _e1473 = max(_e1467, 0.000001f);
                let _e1474 = (_e1473 * 0.25f);
                phi_7721_ = (clamp((1f - smoothstep((_e1473 - _e1474), (_e1473 + _e1474), acos(clamp(_e1465, -1f, 1f)))), 0f, 1f) * (1f - _e1412));
            }
            let _e1483 = phi_7721_;
            let _e1485 = (_e1401 / vec3(_e1402));
            let _e1487 = clamp(dot(_e1316, _e1485), -1f, 1f);
            let _e1489 = global_4.member_1;
            switch bitcast<i32>(0u) {
                default: {
                    let _e1491 = clamp(_e1402, _e1396, _e1489);
                    let _e1492 = (_e1491 * _e1487);
                    let _e1493 = (_e1487 < 0f);
                    phi_6103_ = _e1493;
                    if _e1493 {
                        phi_6103_ = (((_e1492 * _e1492) - ((_e1491 - _e1396) * (_e1491 + _e1396))) >= 0f);
                    }
                    let _e1501 = phi_6103_;
                    if _e1501 {
                        phi_7718_ = vec3<f32>(0f, 0f, 0f);
                        break;
                    }
                    let _e1502 = textureDimensions(global_6, 0i);
                    let _e1504 = vec2<f32>(vec2<i32>(_e1502));
                    let _e1508 = sqrt(((_e1489 - _e1396) * (_e1489 + _e1396)));
                    let _e1513 = sqrt(max(((_e1491 - _e1396) * (_e1491 + _e1396)), 0f));
                    let _e1514 = (_e1489 - _e1491);
                    let _e1516 = (_e1514 * (_e1489 + _e1491));
                    let _e1520 = sqrt(max((_e1516 + (_e1492 * _e1492)), 0f));
                    if (_e1492 > 0f) {
                        phi_7717_ = (_e1516 / (_e1520 + _e1492));
                    } else {
                        phi_7717_ = (_e1520 - _e1492);
                    }
                    let _e1526 = phi_7717_;
                    let _e1548 = textureSampleLevel(global_6, global_5, vec2<f32>(((0.5f / _e1504.x) + (clamp(((_e1526 - _e1514) / max(((_e1513 + _e1508) - _e1514), 0.001f)), 0f, 1f) * (1f - (1f / _e1504.x)))), ((0.5f / _e1504.y) + (clamp((_e1513 / _e1508), 0f, 1f) * (1f - (1f / _e1504.y))))), 0f);
                    phi_7718_ = _e1548.xyz;
                    break;
                }
            }
            let _e1551 = phi_7718_;
            let _e1553 = global_1.member_1;
            let _e1556 = global.member_2;
            let _e1559 = (exp2(_e1553) * select(1f, _e1556, (_e1556 > 0f)));
            let _e1561 = global_1.member_17;
            let _e1562 = max(_e1467, 0.000001f);
            let _e1574 = global_1.member_16;
            phi_7819_ = vec3<f32>(0f, 0f, 0f);
            phi_7724_ = 0f;
            if (_e1423 > 0f) {
                let _e1578 = dot(_e1316, _e1427);
                phi_7820_ = vec3<f32>(0f, 0f, 0f);
                phi_7725_ = 0f;
                if (_e1578 > cos((_e1432 * 1.02f))) {
                    let _e1588 = (_e1316 - (_e1427 * _e1578));
                    let _e1597 = vec2<f32>((((dot(_e1588, _e1439) / _e1432) * 0.5f) + 0.5f), (0.5f - ((dot(_e1588, _e1440) / _e1432) * 0.5f)));
                    let _e1598 = textureSampleLevel(global_9, global_8, _e1597, 0f);
                    let _e1602 = dot(_e1409, _e1427);
                    let _e1606 = global_1.member_15;
                    switch bitcast<i32>(0u) {
                        default: {
                            let _e1610 = (((_e1597 * 2f) - vec2<f32>(1f, 1f)) * vec2<f32>(1.1248595f, 1.1248595f));
                            let _e1611 = dot(_e1610, _e1610);
                            if (_e1611 > 1f) {
                                phi_7723_ = 0f;
                                break;
                            }
                            phi_7723_ = smoothstep(-0.04f, 0.04f, dot(vec3<f32>(_e1610.x, _e1610.y, sqrt(max(0f, (1f - _e1611)))), normalize(vec3<f32>(dot(_e1409, _e1439), -(dot(_e1409, _e1440)), -(_e1602)))));
                            break;
                        }
                    }
                    let _e1623 = phi_7723_;
                    let _e1641 = smoothstep(1f, 0.92f, (sqrt(max(0f, (2f * (1f - _e1578)))) / _e1432));
                    phi_7820_ = ((((mix((vec3<f32>(0.937f, 1.008f, 1.109f) * (0.000055f * (_e1602 + 1f))), vec3<f32>(1.2142f, 0.9614f, 0.7521f), vec3(_e1623)) * (((_e1423 / (0.000064439846f * max((0.5f * (1f - _e1602)), 0.001f))) * (dot(_e1598.xyz, vec3<f32>(0.2126f, 0.7152f, 0.0722f)) * 2.832059f)) * exp2(_e1606))) * _e1641) * _e1598.w) * _e1551);
                    phi_7725_ = (_e1641 * _e1598.w);
                }
                let _e1648 = phi_7820_;
                let _e1650 = phi_7725_;
                phi_7819_ = _e1648;
                phi_7724_ = _e1650;
            }
            let _e1652 = phi_7819_;
            let _e1654 = phi_7724_;
            phi_7739_ = _e1464;
            if (_e1654 > 0f) {
                phi_7727_ = vec3<f32>(0f, 0f, 0f);
                phi_7726_ = 0i;
                loop {
                    let _e1657 = phi_7727_;
                    let _e1659 = phi_7726_;
                    local_4 = _e1657;
                    if (_e1659 < 4i) {
                        continue;
                    } else {
                        break;
                    }
                    continuing {
                        let _e1662 = (f32(_e1659) * 1.5707964f);
                        let _e1672 = ((_e1427 * cos(_e1432)) + (((_e1439 * cos(_e1662)) + (_e1440 * sin(_e1662))) * sin(_e1432)));
                        let _e1684 = ((((asin(clamp(dot(_e1672, _e1405), -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                        let _e1690 = (0.5f + ((0.5f * sign(_e1684)) * sqrt(abs(_e1684))));
                        let _e1691 = vec2<f32>(((atan2(dot(_e1672, _e1421), dot(_e1672, _e1419)) * 0.15915494f) + 0.5f), _e1690);
                        let _e1696 = textureSampleLevel(global_3, global_2, vec2<f32>(_e1691.x, clamp(_e1690, 0.001953125f, 0.9980469f)), 0f);
                        phi_7727_ = (_e1657 + _e1696.xyz);
                        phi_7726_ = (_e1659 + 1i);
                    }
                }
                let _e1701 = local_4;
                phi_7739_ = mix(_e1464, (_e1701 * 0.25f), vec3(clamp((_e1654 * dot(_e1551, vec3<f32>(0.2126f, 0.7152f, 0.0722f))), 0f, 1f)));
            }
            let _e1709 = phi_7739_;
            let _e1712 = global_1.member_27[0u];
            phi_7786_ = vec3<f32>(0f, 0f, 0f);
            if ((_e1712 > 0.5f) && (_e1412 > 0.01f)) {
                let _e1718 = global_1.member_27[1u];
                let _e1722 = global_1.member_27[2u];
                let _e1726 = global_1.member_27[3u];
                let _e1727 = clamp(_e1726, 0.2f, 50f);
                let _e1730 = global_1.member_28[0u];
                let _e1731 = clamp(_e1730, 0.2f, 4f);
                let _e1734 = global_1.member_28[1u];
                let _e1735 = clamp(_e1734, 0.1f, 4f);
                let _e1738 = global_1.member_28[2u];
                phi_7787_ = vec3<f32>(0f, 0f, 0f);
                phi_7737_ = 0i;
                loop {
                    let _e1741 = phi_7787_;
                    let _e1743 = phi_7737_;
                    local_8 = _e1741;
                    if (_e1743 < 8i) {
                        continue;
                    } else {
                        break;
                    }
                    continuing {
                        let _e1745 = f32(_e1743);
                        let _e1751 = global_1.member_24;
                        let _e1758 = (((_e1745 * 31.73f) + (floor(((_e1751 * 0.017f) + (_e1745 * 7.13f))) * 19.19f)) + 3.11f);
                        let _e1762 = fract(((vec3<f32>(_e1758, 37f, 9f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                        let _e1767 = (_e1762 + vec3(dot(_e1762, (_e1762.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                        let _e1783 = fract(((vec3<f32>(_e1758, 2f, 5f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                        let _e1788 = (_e1783 + vec3(dot(_e1783, (_e1783.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                        let _e1796 = fract(((_e1751 / max((mix(8.5f, 1f, (clamp(_e1722, 0f, 4f) * 0.25f)) * mix(0.58f, 1.72f, fract(((_e1767.x * _e1767.y) * _e1767.z)))), 0.1f)) + fract(((_e1788.x * _e1788.y) * _e1788.z))));
                        let _e1800 = fract(((vec3<f32>(_e1758, 43f, 17f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                        let _e1805 = (_e1800 + vec3(dot(_e1800, (_e1800.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                        let _e1812 = mix(0.55f, 1.65f, fract(((_e1805.x * _e1805.y) * _e1805.z)));
                        let _e1816 = mix(0.42f, 0.13f, clamp(((_e1727 * _e1812) * 0.25f), 0f, 1f));
                        let _e1825 = fract(((vec3<f32>(_e1758, 11f, 7f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                        let _e1830 = (_e1825 + vec3(dot(_e1825, (_e1825.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                        let _e1840 = fract(((vec3<f32>(_e1758, 41f, 29f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                        let _e1845 = (_e1840 + vec3(dot(_e1840, (_e1840.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                        let _e1854 = ((fract(((_e1830.x * _e1830.y) * _e1830.z)) + (0.23f * fract(((_e1845.x * _e1845.y) * _e1845.z)))) * 6.2831855f);
                        let _e1858 = fract(((vec3<f32>(_e1758, 13f, 3f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                        let _e1863 = (_e1858 + vec3(dot(_e1858, (_e1858.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                        let _e1871 = mix(0.12f, 0.96f, pow(fract(((_e1863.x * _e1863.y) * _e1863.z)), 0.72f));
                        let _e1875 = sqrt(max(0f, (1f - (_e1871 * _e1871))));
                        let _e1881 = normalize(vec3<f32>((cos(_e1854) * _e1875), _e1871, (sin(_e1854) * _e1875)));
                        let _e1885 = fract(((vec3<f32>(_e1758, 17f, 2f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                        let _e1890 = (_e1885 + vec3(dot(_e1885, (_e1885.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                        let _e1901 = fract(((vec3<f32>(_e1758, 23f, 31f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                        let _e1906 = (_e1901 + vec3(dot(_e1901, (_e1901.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                        let _e1917 = fract(((vec3<f32>(_e1758, 19f, 5f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                        let _e1922 = (_e1917 + vec3(dot(_e1917, (_e1917.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                        let _e1931 = normalize(vec3<f32>(mix(-0.9f, 0.9f, fract(((_e1890.x * _e1890.y) * _e1890.z))), mix(-1.35f, -0.45f, fract(((_e1906.x * _e1906.y) * _e1906.z))), mix(-0.9f, 0.9f, fract(((_e1922.x * _e1922.y) * _e1922.z)))));
                        let _e1935 = normalize((_e1931 - (_e1881 * dot(_e1931, _e1881))));
                        let _e1941 = normalize((_e1881 + (_e1935 * (((_e1796 * 0.8f) * _e1727) * _e1812))));
                        let _e1944 = (_e1316 - _e1941);
                        let _e1948 = fract(((vec3<f32>(_e1758, 53f, 11f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                        let _e1953 = (_e1948 + vec3(dot(_e1948, (_e1948.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                        let _e1960 = mix(0.55f, 1.75f, fract(((_e1953.x * _e1953.y) * _e1953.z)));
                        let _e1964 = fract(((vec3<f32>(_e1758, 61f, 13f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                        let _e1969 = (_e1964 + vec3(dot(_e1964, (_e1964.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                        let _e1982 = max((((0.1f * _e1731) * _e1960) * smoothstep(0f, (_e1816 * 0.55f), _e1796)), 0.0001f);
                        let _e1984 = dot(_e1944, -(_e1935));
                        let _e1985 = clamp(_e1984, 0f, _e1982);
                        let _e1989 = abs(dot((_e1944 + (_e1935 * _e1985)), normalize(cross(_e1941, _e1935))));
                        let _e1991 = smoothstep(0.7648422f, 0.9393727f, dot(_e1316, _e1941));
                        let _e2018 = (vec2<f32>(dot(_e1944, _e1295), dot(_e1944, _e1298)) / vec2((0.15f * clamp(_e1738, 0.1f, 4f))));
                        let _e2019 = dpdx(_e2018);
                        let _e2020 = dpdy(_e2018);
                        let _e2025 = (0.11111111f + (0.25f * (dot(_e2019, _e2019) + dot(_e2020, _e2020))));
                        phi_7787_ = (_e1741 + (((((((vec3<f32>(0.62f, 0.78f, 1f) * (((exp(((-(_e1989) * _e1989) * (340000f / max((_e1735 * _e1735), 0.01f)))) * exp((-(_e1985) * (44f / max((_e1731 * _e1960), 0.2f))))) * _e1991) * (smoothstep(0f, 0.002f, _e1984) * (1f - smoothstep((_e1982 * 0.88f), _e1982, _e1984))))) * 0.85f) + ((vec3<f32>(1f, 0.96f, 0.82f) * ((exp((-(dot(_e2018, _e2018)) / _e2025)) / (9f * _e2025)) * _e1991)) * 2.4f)) * (smoothstep(0f, 0.035f, _e1796) * (1f - smoothstep((_e1816 * 0.58f), _e1816, _e1796)))) * step((_e1745 + 0.5f), (clamp(_e1718, 0f, 1f) * 8f))) * _e1412) * mix(0.55f, 1.85f, fract(((_e1969.x * _e1969.y) * _e1969.z)))));
                        phi_7737_ = (_e1743 + 1i);
                    }
                }
                let _e2384 = local_8;
                phi_7786_ = _e2384;
            }
            let _e2045 = phi_7786_;
            let _e2047 = global_4.member_24;
            let _e2048 = (_e2047 == 2u);
            let _e2050 = global_4.member_8;
            let _e2052 = global_4.member_9;
            let _e2054 = global_4.member_10;
            let _e2056 = global_4.member_18;
            let _e2058 = global_4.member_23;
            let _e2060 = global_4.member_17;
            let _e2062 = global_4.member_13;
            let _e2063 = max(_e2052, 0f);
            let _e2065 = clamp(_e1412, 0f, 1f);
            let _e2067 = mix((_e2050 * _e2063), _e2054, vec3(_e2065));
            phi_7743_ = _e1709;
            if (_e2048 && (_e1406 < 0f)) {
                phi_7743_ = mix(mix(_e2067, _e2058, vec3(clamp(_e2056, 0f, 1f))), _e2067, vec3(clamp((-(_e1406) * max(_e2060, 0.001f)), 0f, 1f)));
            }
            let _e2080 = phi_7743_;
            phi_7744_ = _e2080;
            if ((_e1412 > 0f) && (_e1406 > select(-1f, 0f, _e2048))) {
                if _e2048 {
                    phi_7741_ = max(_e1406, 0f);
                } else {
                    phi_7741_ = abs(_e1406);
                }
                let _e2088 = phi_7741_;
                let _e2096 = fract((((normalize(_e1316) * 37f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                let _e2101 = (_e2096 + vec3(dot(_e2096, (_e2096.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                phi_7744_ = (_e2080 + ((mix(_e2062, vec3<f32>(0.000000698f, 0.00000085f, 0.000001163f), vec3(smoothstep(0f, 0.85f, _e2088))) * (1f + ((fract(((_e2101.x * _e2101.y) * _e2101.z)) - 0.5f) * 0.05f))) * _e2065));
            }
            let _e2115 = phi_7744_;
            phi_7780_ = _e2115;
            if ((_e2047 == 1u) && (_e1406 < 0.005f)) {
                phi_7746_ = vec3<f32>(0f, 0f, 0f);
                phi_7745_ = 0i;
                loop {
                    let _e2120 = phi_7746_;
                    let _e2122 = phi_7745_;
                    local_5 = _e2120;
                    if (_e2122 < 16i) {
                        continue;
                    } else {
                        break;
                    }
                    continuing {
                        let _e2124 = f32(_e2122);
                        let _e2126 = ((_e2124 + 0.5f) * 0.0625f);
                        let _e2127 = sqrt(_e2126);
                        let _e2131 = (_e2124 * 2.3999631f);
                        let _e2144 = ((((asin(clamp(sqrt(max((1f - _e2126), 0f)), -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                        let _e2150 = (0.5f + ((0.5f * sign(_e2144)) * sqrt(abs(_e2144))));
                        let _e2151 = vec2<f32>(((atan2((_e2127 * sin(_e2131)), (_e2127 * cos(_e2131))) * 0.15915494f) + 0.5f), _e2150);
                        let _e2156 = textureSampleLevel(global_3, global_2, vec2<f32>(_e2151.x, clamp(_e2150, 0.001953125f, 0.9980469f)), 0f);
                        phi_7746_ = (_e2120 + _e2156.xyz);
                        phi_7745_ = (_e2122 + 1i);
                    }
                }
                let _e2161 = local_5;
                let _e2164 = global_4.member_4;
                let _e2166 = global_4.member_2;
                let _e2168 = global_4.member_25;
                switch bitcast<i32>(0u) {
                    default: {
                        let _e2171 = (_e1402 - _e1396);
                        if (_e2171 <= (_e1396 * 0.0000008f)) {
                            phi_7777_ = 0f;
                            phi_7769_ = select(vec3<f32>(0f, 1f, 0f), _e1485, vec3((_e1402 > 0f)));
                        } else {
                            switch bitcast<i32>(0u) {
                                default: {
                                    let _e2177 = (_e1398 - _e1400);
                                    let _e2178 = dot(_e2177, _e1316);
                                    let _e2180 = (_e2177 - (_e1316 * _e2178));
                                    let _e2181 = dot(_e2180, _e2180);
                                    let _e2182 = (_e1396 * _e1396);
                                    if (_e2181 > _e2182) {
                                        phi_7749_ = f32();
                                        phi_7748_ = false;
                                        break;
                                    }
                                    phi_7749_ = (_e2178 - sqrt(max((_e2182 - _e2181), 0f)));
                                    phi_7748_ = true;
                                    break;
                                }
                            }
                            let _e2189 = phi_7749_;
                            let _e2191 = phi_7748_;
                            if (!(_e2191) || (_e2189 <= 0f)) {
                                phi_7779_ = _e2115;
                                break;
                            }
                            phi_7777_ = _e2189;
                            phi_7769_ = normalize(((_e1400 + (_e1316 * _e2189)) - _e1398));
                        }
                        let _e2200 = phi_7777_;
                        let _e2202 = phi_7769_;
                        let _e2203 = dot(_e2202, _e1409);
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e2205 = clamp(_e1396, _e1396, _e1489);
                                let _e2206 = (_e2205 * _e2203);
                                let _e2207 = (_e2203 < 0f);
                                phi_7337_ = _e2207;
                                if _e2207 {
                                    phi_7337_ = (((_e2206 * _e2206) - ((_e2205 - _e1396) * (_e2205 + _e1396))) >= 0f);
                                }
                                let _e2215 = phi_7337_;
                                if _e2215 {
                                    phi_7771_ = vec3<f32>(0f, 0f, 0f);
                                    break;
                                }
                                let _e2216 = textureDimensions(global_6, 0i);
                                let _e2218 = vec2<f32>(vec2<i32>(_e2216));
                                let _e2222 = sqrt(((_e1489 - _e1396) * (_e1489 + _e1396)));
                                let _e2227 = sqrt(max(((_e2205 - _e1396) * (_e2205 + _e1396)), 0f));
                                let _e2228 = (_e1489 - _e2205);
                                let _e2230 = (_e2228 * (_e1489 + _e2205));
                                let _e2234 = sqrt(max((_e2230 + (_e2206 * _e2206)), 0f));
                                if (_e2206 > 0f) {
                                    phi_7770_ = (_e2230 / (_e2234 + _e2206));
                                } else {
                                    phi_7770_ = (_e2234 - _e2206);
                                }
                                let _e2240 = phi_7770_;
                                let _e2262 = textureSampleLevel(global_6, global_5, vec2<f32>(((0.5f / _e2218.x) + (clamp(((_e2240 - _e2228) / max(((_e2227 + _e2222) - _e2228), 0.001f)), 0f, 1f) * (1f - (1f / _e2218.x)))), ((0.5f / _e2218.y) + (clamp((_e2227 / _e2222), 0f, 1f) * (1f - (1f / _e2218.y))))), 0f);
                                phi_7771_ = _e2262.xyz;
                                break;
                            }
                        }
                        let _e2265 = phi_7771_;
                        phi_7779_ = mix(_e2115, mix(_e2115, ((((((((_e2050 * 0.31830987f) * max(_e2203, 0f)) * _e2265) * _e1574) * _e1561) + (_e2050 * (_e2161 * vec3<f32>(0.0625f, 0.0625f, 0.0625f)))) * _e2063) + (_e2054 * _e2065)), vec3(exp((-(_e2200) * ((dot(_e2164, vec3<f32>(0.299f, 0.587f, 0.114f)) * exp((-(_e2171) / _e2166))) * max(_e2168, 0f)))))), vec3(smoothstep(-0.004f, 0f, -(_e1406))));
                        break;
                    }
                }
                let _e2294 = phi_7779_;
                phi_7780_ = _e2294;
            }
            let _e2296 = phi_7780_;
            phi_7781_ = _e2296;
            if _e2048 {
                let _e2298 = global_4.member_15;
                let _e2300 = global_4.member_16;
                let _e2307 = global_4.member_11;
                let _e2309 = global_4.member_12;
                phi_7781_ = mix(_e2296, mix(_e2307, _e2309, vec3(_e1412)), vec3(clamp((1f - smoothstep(0f, max(mix(_e2298, _e2300, _e1412), 0.00001f), abs(_e1406))), 0f, 1f)));
            }
            let _e2316 = phi_7781_;
            let _e2322 = ((((_e2316 + (_e2045 * (1f - _e1654))) + (((_e1574 * min((_e1561 / (3.1415927f * ((_e1562 * _e1562) + ((_e1562 * 0.05f) * (_e1562 * 0.25f))))), (32752f / max(_e1559, 0.000001f)))) * _e1483) * _e1551)) + _e1652) * _e1559);
            global_7 = vec4<f32>(_e2322.x, _e2322.y, _e2322.z, 1f);
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
