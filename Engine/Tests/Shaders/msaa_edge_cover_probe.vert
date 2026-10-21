#version 450
// Test-only: one triangle over the half of the render target on the negative clip-space y side, so a
// horizontal silhouette edge runs through the middle of each pixel. Under the engine's flipped viewport
// that half holds samples 2 and 3 of the standard 4x pattern and leaves samples 0 and 1 uncovered.
void main()
{
    const vec2 corners[3] = vec2[3](vec2(-4.0, 0.0), vec2(4.0, 0.0), vec2(0.0, -4.0));
    gl_Position = vec4(corners[gl_VertexIndex], 0.0, 1.0);
}
