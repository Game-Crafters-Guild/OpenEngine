struct type_16 {
    member: array<mat4x4<f32>, 4>,
    member_1: vec4<f32>,
    member_2: vec4<f32>,
}

struct type_19 {
    member: vec4<f32>,
    member_1: vec4<f32>,
}

struct type_23 {
    member: f32,
}

var<private> global: u32;
var<workgroup> global_1: array<f32, 64>;
var<workgroup> global_2: array<f32, 64>;
@group(0) @binding(1)
var<uniform> global_3: type_16;
@group(0) @binding(64)
var global_4: sampler_comparison;
@group(0) @binding(0)
var global_5: texture_depth_2d_array;
@group(3) @binding(0)
var<uniform> global_6: type_19;
@group(0) @binding(2)
var<storage, read_write> global_7: type_23;

fn function_() {
    var phi_1311_: f32;
    var phi_1310_: f32;
    var phi_884_: bool;
    var phi_890_: bool;
    var phi_896_: bool;
    var phi_902_: bool;
    var phi_958_: bool;
    var phi_964_: bool;
    var phi_970_: bool;
    var phi_976_: bool;
    var phi_1314_: f32;
    var phi_1313_: f32;
    var phi_1312_: i32;
    var phi_1123_: bool;
    var phi_1129_: bool;
    var phi_1135_: bool;
    var phi_1141_: bool;
    var phi_1197_: bool;
    var phi_1203_: bool;
    var phi_1209_: bool;
    var phi_1215_: bool;
    var phi_1319_: f32;
    var phi_1334_: f32;
    var phi_1335_: f32;
    var phi_1339_: f32;
    var phi_1336_: i32;
    var phi_1341_: f32;
    var phi_1340_: f32;
    var phi_1337_: i32;
    var local: f32;
    var local_1: f32;
    var local_2: f32;
    var local_3: f32;
    var local_4: f32;
    var local_5: f32;
    var local_6: f32;

    let _e43 = global_3.member_2[2u];
    let _e46 = min(i32((_e43 + 0.5f)), 4i);
    let _e47 = global;
    let _e48 = bitcast<i32>(_e47);
    let _e49 = (_e48 / 16i);
    let _e53 = global_3.member[min(_e49, 3i)];
    let _e55 = global_6.member;
    let _e57 = global_6.member_1;
    let _e58 = _e57.xyz;
    let _e61 = global_6.member_1[3u];
    let _e64 = global_3.member_2[0u];
    let _e71 = (_e48 - (i32(floor((f32(_e48) / f32(16i)))) * 16i));
    if (_e71 == 0i) {
        phi_1311_ = 0f;
        phi_1310_ = 0f;
    } else {
        let _e73 = (_e71 - 1i);
        let _e74 = (_e73 / 5i);
        let _e77 = f32(_e74);
        phi_1311_ = sqrt(((_e77 + 0.5f) * 0.33333334f));
        phi_1310_ = (((f32((_e73 - (_e74 * 5i))) + 0.5f) * 1.2566371f) + (_e77 * 2.3999631f));
    }
    let _e87 = phi_1311_;
    let _e89 = phi_1310_;
    let _e93 = (vec2<f32>(cos(_e89), sin(_e89)) * _e87);
    let _e95 = (1f - (_e87 * _e87));
    phi_1335_ = 1f;
    if (_e49 < _e46) {
        let _e101 = (_e53 * vec4<f32>(_e55.x, _e55.y, _e55.z, 1f));
        let _e104 = max(_e101.w, 0.000001f);
        let _e106 = ((_e101.z / _e104) + _e64);
        let _e107 = (_e101.w > 0f);
        let _e118 = normalize(cross(select(vec3<f32>(1f, 0f, 0f), vec3<f32>(0f, 1f, 0f), vec3((abs(_e57.y) < 0.99f))), _e58));
        let _e123 = (_e53 * vec4<f32>(_e57.x, _e57.y, _e57.z, 0f));
        let _e124 = (_e118 * _e61);
        let _e130 = (cross(_e58, _e118) * _e61);
        let _e137 = (_e101 + (_e123 * 0.25f));
        let _e142 = (_e137.xyz / vec3(max(_e137.w, 0.000001f)));
        let _e145 = ((_e142.xy * 0.5f) + vec2<f32>(0.5f, 0.5f));
        let _e147 = (1f - _e145.y);
        let _e152 = (_e142.z + _e64);
        let _e153 = (_e137.w > 0f);
        phi_884_ = _e153;
        if _e153 {
            phi_884_ = (_e145.x >= 0f);
        }
        let _e157 = phi_884_;
        phi_890_ = _e157;
        if _e157 {
            phi_890_ = (_e145.x <= 1f);
        }
        let _e161 = phi_890_;
        phi_896_ = _e161;
        if _e161 {
            phi_896_ = (_e147 >= 0f);
        }
        let _e164 = phi_896_;
        phi_902_ = _e164;
        if _e164 {
            phi_902_ = (_e147 <= 1f);
        }
        let _e167 = phi_902_;
        let _e170 = clamp(vec2<f32>(_e145.x, _e147), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
        let _e171 = f32(_e49);
        let _e172 = clamp(_e152, 0f, 1f);
        let _e175 = vec4<f32>(_e170.x, _e170.y, _e171, _e172);
        let _e181 = textureSampleCompareLevel(global_5, global_4, vec2<f32>(_e175.x, _e175.y), i32(_e175.z), _e172);
        let _e185 = (_e101.xyz / vec3(_e104));
        let _e188 = ((_e185.xy * 0.5f) + vec2<f32>(0.5f, 0.5f));
        let _e190 = (1f - _e188.y);
        let _e195 = (_e185.z + _e64);
        phi_958_ = _e107;
        if _e107 {
            phi_958_ = (_e188.x >= 0f);
        }
        let _e199 = phi_958_;
        phi_964_ = _e199;
        if _e199 {
            phi_964_ = (_e188.x <= 1f);
        }
        let _e203 = phi_964_;
        phi_970_ = _e203;
        if _e203 {
            phi_970_ = (_e190 >= 0f);
        }
        let _e206 = phi_970_;
        phi_976_ = _e206;
        if _e206 {
            phi_976_ = (_e190 <= 1f);
        }
        let _e209 = phi_976_;
        let _e212 = clamp(vec2<f32>(_e188.x, _e190), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
        let _e213 = clamp(_e195, 0f, 1f);
        let _e216 = vec4<f32>(_e212.x, _e212.y, _e171, _e213);
        let _e222 = textureSampleCompareLevel(global_5, global_4, vec2<f32>(_e216.x, _e216.y), i32(_e216.z), _e213);
        switch bitcast<i32>(0u) {
            default: {
                if !(((_e107 && (_e106 >= 0f)) && (_e106 <= 1f))) {
                    phi_1334_ = 1f;
                    break;
                }
                phi_1314_ = 0.25f;
                phi_1313_ = (1f - max((select(1f, _e181, (_e167 && (_e152 < 1f))) - select(1f, _e222, (_e209 && (_e195 < 1f)))), 0f));
                phi_1312_ = 1i;
                loop {
                    let _e235 = phi_1314_;
                    let _e237 = phi_1313_;
                    let _e239 = phi_1312_;
                    local_5 = _e237;
                    if (_e239 < 7i) {
                        let _e241 = (_e235 * 4f);
                        let _e244 = (_e101 + ((((_e53 * vec4<f32>(_e124.x, _e124.y, _e124.z, 0f)) * _e93.x) + ((_e53 * vec4<f32>(_e130.x, _e130.y, _e130.z, 0f)) * _e93.y)) * (_e235 * 2f)));
                        let _e246 = (_e244 + (_e123 * _e235));
                        let _e251 = (_e246.xyz / vec3(max(_e246.w, 0.000001f)));
                        let _e254 = ((_e251.xy * 0.5f) + vec2<f32>(0.5f, 0.5f));
                        let _e256 = (1f - _e254.y);
                        let _e261 = (_e251.z + _e64);
                        let _e262 = (_e246.w > 0f);
                        local_3 = _e241;
                        phi_1123_ = _e262;
                        if _e262 {
                            phi_1123_ = (_e254.x >= 0f);
                        }
                        let _e266 = phi_1123_;
                        phi_1129_ = _e266;
                        if _e266 {
                            phi_1129_ = (_e254.x <= 1f);
                        }
                        let _e270 = phi_1129_;
                        phi_1135_ = _e270;
                        if _e270 {
                            phi_1135_ = (_e256 >= 0f);
                        }
                        let _e273 = phi_1135_;
                        phi_1141_ = _e273;
                        if _e273 {
                            phi_1141_ = (_e256 <= 1f);
                        }
                        let _e276 = phi_1141_;
                        let _e279 = clamp(vec2<f32>(_e254.x, _e256), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
                        let _e280 = clamp(_e261, 0f, 1f);
                        let _e283 = vec4<f32>(_e279.x, _e279.y, _e171, _e280);
                        let _e289 = textureSampleCompareLevel(global_5, global_4, vec2<f32>(_e283.x, _e283.y), i32(_e283.z), _e280);
                        if (_e239 == 6i) {
                            phi_1319_ = 1f;
                        } else {
                            let _e293 = (_e244 + (_e123 * _e241));
                            let _e298 = (_e293.xyz / vec3(max(_e293.w, 0.000001f)));
                            let _e301 = ((_e298.xy * 0.5f) + vec2<f32>(0.5f, 0.5f));
                            let _e303 = (1f - _e301.y);
                            let _e308 = (_e298.z + _e64);
                            let _e309 = (_e293.w > 0f);
                            phi_1197_ = _e309;
                            if _e309 {
                                phi_1197_ = (_e301.x >= 0f);
                            }
                            let _e313 = phi_1197_;
                            phi_1203_ = _e313;
                            if _e313 {
                                phi_1203_ = (_e301.x <= 1f);
                            }
                            let _e317 = phi_1203_;
                            phi_1209_ = _e317;
                            if _e317 {
                                phi_1209_ = (_e303 >= 0f);
                            }
                            let _e320 = phi_1209_;
                            phi_1215_ = _e320;
                            if _e320 {
                                phi_1215_ = (_e303 <= 1f);
                            }
                            let _e323 = phi_1215_;
                            let _e326 = clamp(vec2<f32>(_e301.x, _e303), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
                            let _e327 = clamp(_e308, 0f, 1f);
                            let _e330 = vec4<f32>(_e326.x, _e326.y, _e171, _e327);
                            let _e336 = textureSampleCompareLevel(global_5, global_4, vec2<f32>(_e330.x, _e330.y), i32(_e330.z), _e327);
                            phi_1319_ = select(1f, _e336, (_e323 && (_e308 < 1f)));
                        }
                        let _e339 = phi_1319_;
                        local_4 = (_e237 * (1f - max((_e339 - select(1f, _e289, (_e276 && (_e261 < 1f)))), 0f)));
                        continue;
                    } else {
                        break;
                    }
                    continuing {
                        let _e398 = local_3;
                        phi_1314_ = _e398;
                        let _e401 = local_4;
                        phi_1313_ = _e401;
                        phi_1312_ = (_e239 + 1i);
                    }
                }
                let _e414 = local_5;
                phi_1334_ = _e414;
                break;
            }
        }
        let _e346 = phi_1334_;
        phi_1335_ = _e346;
    }
    let _e348 = phi_1335_;
    global_1[_e48] = (_e348 * _e95);
    global_2[_e48] = _e95;
    workgroupBarrier();
    phi_1339_ = 1f;
    phi_1336_ = 0i;
    loop {
        let _e353 = phi_1339_;
        let _e355 = phi_1336_;
        local_2 = _e353;
        if (_e355 < 4i) {
            if (_e355 >= _e46) {
                break;
            }
            phi_1341_ = 0f;
            phi_1340_ = 0f;
            phi_1337_ = 0i;
            loop {
                let _e359 = phi_1341_;
                let _e361 = phi_1340_;
                let _e363 = phi_1337_;
                local = _e359;
                local_1 = _e361;
                if (_e363 < 16i) {
                    continue;
                } else {
                    break;
                }
                continuing {
                    let _e366 = ((_e355 * 16i) + _e363);
                    let _e368 = global_1[_e366];
                    let _e371 = global_2[_e366];
                    phi_1341_ = (_e359 + _e371);
                    phi_1340_ = (_e361 + _e368);
                    phi_1337_ = (_e363 + 1i);
                }
            }
            let _e375 = local;
            let _e378 = local_1;
            local_6 = min(_e353, (_e378 / max(_e375, 0.000001f)));
            continue;
        } else {
            break;
        }
        continuing {
            let _e418 = local_6;
            phi_1339_ = _e418;
            phi_1336_ = (_e355 + 1i);
        }
    }
    if (_e47 == 0u) {
        let _e385 = local_2;
        global_7.member = _e385;
    }
    return;
}

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(local_invocation_index) param: u32) {
    global = param;
    function_();
}
