$input a_position, a_normal, a_texcoord1
$output v_normal, v_previous, v_current, v_material
#include "bgfx_shader.sh"
uniform mat4 u_previousMvp;
void main() {
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
    v_current = gl_Position;
    v_previous = mul(u_previousMvp, vec4(a_position, 1.0));
    v_normal = normalize(mul((mat3)u_model[0], a_normal));
    v_material = a_texcoord1;
}
