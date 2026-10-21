// The fields of the volumetric fog's parameter block (FogGpuParams in
// VolumetricFogRenderer.cpp mirrors them, std140). Declared by the VolumetricFog
// module's passes (volumetric_fog_common.glsl) and by the forward passes that fog
// what they draw (volumetric_fog_forward.glsl); the includer writes the block
// around them.
    mat4 uInvViewProj;
    mat4 uView;
    vec4 uCameraPos;
    vec4 uSunDirection;
    vec4 uSunColor;
    vec4 uAmbientColor; // rgb=flat-ambient fallback tint, w=IBL intensity scale
    vec4 uAlbedoDensity;
    vec4 uEmissionAnisotropy;
    vec4 uHeightParams; // x=baseHeight, y=heightFalloff, z=localLightHistoryPressure, w=isGlobal
    vec4 uLocalVolumeCenterShape; // xyz=centerWS, w=shape (0 box, 1 sphere, 2 capsule, 3 cylinder)
    vec4 uLocalVolumeAxisXHalfExtent; // xyz=axisX, w=halfExtentX
    vec4 uLocalVolumeAxisYHalfExtent; // xyz=axisY, w=halfExtentY
    vec4 uLocalVolumeAxisZHalfExtent; // xyz=axisZ, w=halfExtentZ
    vec4 uLocalVolumeParams; // x=blendDistance, y=valid, z=skyFade (far fog->sky feather)
    vec4 uNoiseParams;
    vec4 uNoiseVelocityTime;
    vec4 uNoiseChannelWeights;
    vec4 uDensityParams; // x=global noise threshold, y=threshold softness, z=orthographic, w=iblEnabled
    vec4 uVolumeParams;
    vec4 uGridParams;
    vec4 uJitterParams; // x=frame, y=historyValid, z=renderWidth, w=renderHeight
    vec4 uFogExtraParams; // x=compositeDepthBias, y=shadowBias, z=reserved, w=emissionGridActive
    mat4 uPrevViewProj;
    vec4 uPrevCameraPos;
