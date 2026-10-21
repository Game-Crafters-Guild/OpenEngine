struct type_3 {
    member: f32,
    member_1: u32,
    member_2: f32,
    member_3: f32,
}

struct type_10 {
    member: vec4<u32>,
    member_1: array<vec2<u32>>,
}

struct type_13 {
    member: vec4<u32>,
}

struct type_23 {
    member: mat4x4<f32>,
    member_1: mat4x4<f32>,
    member_2: vec4<f32>,
    member_3: vec4<f32>,
}

struct type_30 {
    member: array<u32>,
}

struct type_33 {
    member: vec4<u32>,
    member_1: vec4<f32>,
    member_2: vec4<f32>,
    member_3: vec4<f32>,
    member_4: vec4<f32>,
    member_5: vec4<f32>,
    member_6: vec4<f32>,
    member_7: vec4<f32>,
    member_8: vec4<i32>,
}

struct type_35 {
    member: vec4<u32>,
    member_1: array<type_33>,
}

@group(0) @binding(6)
var<storage> global: type_3;
@group(0) @binding(1)
var<storage> global_1: type_10;
@group(0) @binding(4)
var<uniform> global_2: type_13;
var<private> global_3: vec4<f32>;
@group(0) @binding(64)
var global_4: sampler;
@group(0) @binding(0)
var global_5: texture_2d<f32>;
@group(0) @binding(5)
var<uniform> global_6: type_23;
var<private> global_7: vec4<f32>;
@group(0) @binding(2)
var<storage> global_8: type_30;
@group(0) @binding(3)
var<storage> global_9: type_35;

fn function_() {
    var phi_219_: bool;
    var phi_610_: f32;
    var phi_616_: vec3<f32>;
    var phi_614_: vec3<f32>;
    var phi_613_: u32;
    var phi_617_: f32;
    var phi_623_: vec3<f32>;
    var phi_622_: vec3<f32>;
    var local: vec3<f32>;

    switch bitcast<i32>(0u) {
        default: {
            let _e34 = global_1.member[0u];
            let _e35 = max(_e34, 1u);
            let _e38 = global_1.member[1u];
            let _e39 = max(_e38, 1u);
            let _e42 = global_1.member[2u];
            let _e46 = global_1.member[3u];
            let _e49 = global_2.member;
            let _e51 = max(_e49.xy, vec2<u32>(1u, 1u));
            let _e58 = vec2<f32>(f32((_e35 * _e51.x)), f32((_e39 * _e51.y)));
            let _e59 = global_3;
            let _e60 = _e59.xy;
            let _e63 = vec2<u32>(clamp(_e60, vec2<f32>(0f, 0f), (_e58 - vec2<f32>(1f, 1f))));
            let _e65 = textureLoad(global_5, bitcast<vec2<i32>>(_e63), 0i);
            let _e69 = (((_e60 / _e58) * 2f) - vec2<f32>(1f, 1f));
            let _e71 = global_6.member;
            let _e75 = (_e71 * vec4<f32>(_e69.x, _e69.y, _e65.x, 1f));
            let _e80 = (_e75.xyz / vec3(max(_e75.w, 0.000001f)));
            let _e86 = vec4<f32>(_e80.x, vec4<f32>().y, vec4<f32>().z, vec4<f32>().w);
            let _e92 = vec4<f32>(_e86.x, _e80.y, _e86.z, _e86.w);
            let _e99 = vec4<f32>(_e92.x, _e92.y, _e80.z, _e92.w).xyz;
            let _e100 = dpdx(_e99);
            let _e101 = dpdy(_e99);
            let _e103 = normalize(cross(_e100, _e101));
            if (_e65.x <= 0.000001f) {
                global_7 = vec4<f32>(0f, 0f, 0f, 0f);
                break;
            }
            let _e105 = (_e63 / _e51);
            let _e107 = (_e105.x >= _e35);
            phi_219_ = _e107;
            if !(_e107) {
                phi_219_ = (_e105.y >= _e39);
            }
            let _e112 = phi_219_;
            if _e112 {
                global_7 = vec4<f32>(0f, 0f, 0f, 0f);
                break;
            }
            let _e115 = global_6.member_2[0u];
            let _e116 = max(_e115, 0.0001f);
            let _e119 = global_6.member_2[1u];
            phi_610_ = _e119;
            if (_e119 <= (_e116 + 0.0001f)) {
                phi_610_ = (_e116 + 1000f);
            }
            let _e124 = phi_610_;
            let _e133 = global_6.member_2[2u];
            let _e150 = global_1.member_1[((_e105.x + (_e105.y * _e35)) + (u32(floor((clamp((log((((_e116 * _e124) / max(((_e65.x * (_e124 - _e116)) + _e116), 0.000001f)) / _e116)) * _e133), 0f, 0.999999f) * f32(max(_e42, 1u))))) * (_e35 * _e39)))];
            phi_616_ = _e103;
            if (dot(_e103, -(normalize(_e99))) < 0f) {
                phi_616_ = -(_e103);
            }
            let _e160 = phi_616_;
            phi_614_ = vec3<f32>(0f, 0f, 0f);
            phi_613_ = 0u;
            loop {
                let _e162 = phi_614_;
                let _e164 = phi_613_;
                local = _e162;
                if (_e164 < min(_e150.y, max(_e46, 1u))) {
                    let _e169 = global_8.member[(_e150.x + _e164)];
                    let _e172 = global_9.member[0u];
                    if (_e169 >= _e172) {
                        phi_622_ = _e162;
                        continue;
                    }
                    let _e176 = global_9.member_1[_e169];
                    if (_e176.member.w == 0u) {
                        phi_622_ = _e162;
                        continue;
                    }
                    let _e185 = _e176.member_3.xyz;
                    if (_e176.member.x == 0u) {
                        let _e190 = normalize(_e176.member_2.xyz);
                        let _e192 = global_6.member_1;
                        phi_623_ = (_e162 + (_e185 * (_e176.member_2.w * max(dot(_e160, normalize(-(normalize((_e192 * vec4<f32>(_e190.x, _e190.y, _e190.z, 0f)).xyz)))), 0f))));
                    } else {
                        let _e210 = global_6.member_1;
                        let _e217 = ((_e210 * vec4<f32>(_e176.member_1.x, _e176.member_1.y, _e176.member_1.z, 1f)).xyz - _e99);
                        let _e218 = length(_e217);
                        let _e221 = (_e217 / vec3(max(_e218, 0.000001f)));
                        let _e226 = clamp((1f - (_e218 / max(_e176.member_1.w, 0.001f))), 0f, 1f);
                        phi_617_ = 1f;
                        if (_e176.member.x == 2u) {
                            let _e229 = normalize(_e176.member_2.xyz);
                            phi_617_ = smoothstep(_e176.member_5.x, _e176.member_4.w, dot(normalize(-(_e221)), normalize(normalize((_e210 * vec4<f32>(_e229.x, _e229.y, _e229.z, 0f)).xyz))));
                        }
                        let _e245 = phi_617_;
                        phi_623_ = (_e162 + (_e185 * ((((_e176.member_2.w * max(dot(_e160, _e221), 0f)) * _e226) * _e226) * _e245)));
                    }
                    let _e253 = phi_623_;
                    phi_622_ = _e253;
                    continue;
                } else {
                    break;
                }
                continuing {
                    let _e255 = phi_622_;
                    phi_614_ = _e255;
                    phi_613_ = (_e164 + bitcast<u32>(1i));
                }
            }
            let _e259 = global.member_2;
            let _e263 = local;
            let _e264 = (_e263 * select(1f, _e259, (_e259 > 0f)));
            global_7 = vec4<f32>(_e264.x, _e264.y, _e264.z, 1f);
            break;
        }
    }
    return;
}

@fragment
fn main(@builtin(position) param: vec4<f32>) -> @location(0) vec4<f32> {
    global_3 = param;
    function_();
    let _e3 = global_7;
    return _e3;
}
