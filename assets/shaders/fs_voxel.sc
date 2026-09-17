$input v_normal, v_color0

#include "bgfx_shader.sh"

uniform vec4 u_lightDir;
uniform vec4 u_stressTint;

void main()
{
    float diffuse = max(dot(normalize(v_normal), normalize(-u_lightDir.xyz)), 0.0);
    // A small hemispherical term keeps voxel interiors readable without a light probe.
    float ambient = 0.20 + 0.16 * max(v_normal.y, 0.0);
    vec3 lit = v_color0.rgb * (ambient + diffuse * 0.80);
    lit = mix(lit, v_color0.rgb, u_stressTint.y);
    gl_FragColor = vec4(lit, v_color0.a);
}
