@group(0) @binding(64)
var global: sampler;
@group(0) @binding(0)
var global_1: texture_2d<f32>;
var<private> global_2: vec3<u32>;
@group(0) @binding(1)
var global_3: texture_storage_2d<r32float,write>;

fn function_() {
    var phi_45_: bool;

    switch bitcast<i32>(0u) {
        default: {
            let _e11 = textureDimensions(global_1, 0i);
            let _e12 = vec2<i32>(_e11);
            let _e13 = global_2;
            let _e15 = bitcast<vec2<i32>>(_e13.xy);
            let _e18 = (_e15.x >= _e12.x);
            phi_45_ = _e18;
            if !(_e18) {
                phi_45_ = (_e15.y >= _e12.y);
            }
            let _e24 = phi_45_;
            if _e24 {
                break;
            }
            let _e25 = textureLoad(global_1, _e15, 0i);
            textureStore(global_3, _e15, vec4<f32>(_e25.x, 0f, 0f, 0f));
            break;
        }
    }
    return;
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) param: vec3<u32>) {
    global_2 = param;
    function_();
}
