// The HeightFogParams UBO field list: std140, 256 bytes, mirrored on the C++ side by
// HeightFogParamsUBO (HeightFogParamsUBO.h). Included inside the block declaration so the layout
// is written once:
//   layout(set = 0, binding = N, std140) uniform HeightFogParamsBlock
//   {
//   #include "height_fog_params_fields.glsl"
//   } HeightFogParams;
// height_fog_term.glsl reads the block under the instance name HeightFogParams.
float intensity;
float density;
float minDistance;
float smoothLength;
float baseHeight;
float transitionLength;
float maxDistance;
float gradientStrength;
float emissiveR;
float emissiveG;
float emissiveB;
float sunIntensity;
float sunDirX;
float sunDirY;
float sunDirZ;
float phase;
float sunColorR;
float sunColorG;
float sunColorB;
float phaseWeight0;
float phaseWeight1;
float skyPower;
float skyFillStart;
float skyFillEnd;
float axisX;
float axisY;
float axisZ;
float noiseScale;
float gradientLowR;
float gradientLowG;
float gradientLowB;
float noiseStrength;
float gradientHighR;
float gradientHighG;
float gradientHighB;
float noiseContrast;
float noiseVelX;
float noiseVelY;
float noiseVelZ;
float noiseTime;
int flags;
int axisMode;
int gradientMode;
float maxOpacity;
int layerMode;
float horizonHeightOffset;
float horizonHeightBlendStart;
float horizonHeightBlendEnd;
float noiseMin;
float noiseMax;
float noiseFadeStart;
float noiseFadeEnd;
float skyHorizonOffset;
float skyBottomStrength;
int colorSource;
float skyHorizonScale;
float skyNightAmount;
float skyAzimuthOffset;
float skyHorizonColorR;
float skyHorizonColorG;
float skyHorizonColorB;
float _pad0;
float _pad1;
float _pad2;
