struct type_5 {
    member: f32,
    member_1: u32,
    member_2: f32,
    member_3: f32,
}

struct type_11 {
    member: u32,
    member_1: u32,
}

struct type_24 {
    member: array<u32>,
}

struct type_30 {
    member: array<atomic<u32>>,
}

@group(0) @binding(3)
var<storage> global: type_5;
@group(0) @binding(66)
var global_1: sampler;
@group(0) @binding(2)
var global_2: texture_2d<f32>;
@group(3) @binding(0)
var<uniform> global_3: type_11;
var<private> global_4: u32;
var<workgroup> global_5: array<atomic<u32>, 512>;
var<private> global_6: vec3<u32>;
@group(0) @binding(64)
var global_7: sampler;
@group(0) @binding(0)
var global_8: texture_2d<f32>;
@group(0) @binding(1)
var<storage, read_write> global_9: type_30;

fn function_() {
    var phi_314_: u32;
    var phi_170_: bool;
    var phi_315_: u32;
    var phi_316_: u32;

    let _e37 = global_4;
    phi_314_ = _e37;
    loop {
        let _e39 = phi_314_;
        if (_e39 < 512u) {
            continue;
        } else {
            break;
        }
        continuing {
            atomicStore((&global_5[_e39]), 0u);
            phi_314_ = (_e39 + 64u);
        }
    }
    workgroupBarrier();
    let _e43 = global_6;
    let _e45 = bitcast<vec2<i32>>(_e43.xy);
    let _e49 = global_3.member;
    let _e50 = (bitcast<u32>(_e45.x) < _e49);
    phi_170_ = _e50;
    if _e50 {
        let _e54 = global_3.member_1;
        phi_170_ = (bitcast<u32>(_e45.y) < _e54);
    }
    let _e57 = phi_170_;
    if _e57 {
        let _e58 = textureLoad(global_8, _e45, 0i);
        let _e63 = global.member_2;
        let _e66 = (dot(max(_e58.xyz, vec3<f32>(0f, 0f, 0f)), vec3<f32>(0.2126f, 0.7152f, 0.0722f)) / select(1f, _e63, (_e63 > 0f)));
        switch bitcast<i32>(0u) {
            default: {
                if (_e66 < 0.00000003f) {
                    phi_315_ = 0u;
                    break;
                }
                phi_315_ = u32(((clamp(((log2(_e66) - -24f) * 0.02631579f), 0f, 1f) * 254f) + 1f));
                break;
            }
        }
        let _e77 = phi_315_;
        let _e78 = textureDimensions(global_2, 0i);
        let _e79 = vec2<i32>(_e78);
        let _e84 = global_3.member_1;
        let _e93 = textureLoad(global_2, clamp(vec2<i32>((((vec2<f32>(_e45) + vec2<f32>(0.5f, 0.5f)) / vec2<f32>(f32(_e49), f32(_e84))) * vec2<f32>(_e79))), vec2<i32>(0i, 0i), (_e79 - vec2<i32>(1i, 1i))), 0i);
        let _e100 = atomicAdd((&global_5[((select(0u, 1u, (_e93.x <= 0f)) * 256u) + _e77)]), 1u);
    }
    workgroupBarrier();
    phi_316_ = _e37;
    loop {
        let _e102 = phi_316_;
        if (_e102 < 512u) {
            let _e105 = atomicLoad((&global_5[_e102]));
            if (_e105 > 0u) {
                let _e109 = atomicAdd((&global_9.member[_e102]), _e105);
            }
            continue;
        } else {
            break;
        }
        continuing {
            phi_316_ = (_e102 + 64u);
        }
    }
    return;
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(local_invocation_index) param: u32, @builtin(global_invocation_id) param_1: vec3<u32>) {
    global_4 = param;
    global_6 = param_1;
    function_();
}
