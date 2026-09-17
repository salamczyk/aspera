$input v_normal, v_previous, v_current, v_material
#include "bgfx_shader.sh"
SAMPLER2D(s_palette, 0);
uniform vec4 u_screenParams;
void main() {
    float x = (floor(v_material + 0.5) + 0.5) / 4096.0;
    vec4 base = texture2DLod(s_palette, vec2(x, 0.5 / 3.0), 0.0);
    vec4 emission = texture2DLod(s_palette, vec2(x, 1.5 / 3.0), 0.0);
    float roughness = texture2DLod(s_palette, vec2(x, 2.5 / 3.0), 0.0).x;
    vec2 current = v_current.xy / max(v_current.w, 0.0001);
    vec2 previous = v_previous.xy / max(v_previous.w, 0.0001);
    vec2 velocity = (current - previous) * vec2(0.5, 0.5 * u_screenParams.z);
    gl_FragData[0] = base;
    gl_FragData[1] = vec4(normalize(v_normal), roughness);
    gl_FragData[2] = vec4(min(emission.rgb * emission.a, vec3(60000.0)), 1.0);
    gl_FragData[3] = vec4(velocity, 0.0, 0.0);
}
