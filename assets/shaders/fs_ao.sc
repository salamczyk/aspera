$input v_texcoord0
#include "bgfx_shader.sh"
#include "lighting_common.sh"
SAMPLER2D(s_depth,0);
SAMPLER2D(s_normal,1);
uniform mat4 u_inverseVP;
uniform vec4 u_screenParams;
uniform vec4 u_aoParams;
void main() {
    float depth=texture2D(s_depth,v_texcoord0).r;
    if(depth>=0.999999) { gl_FragColor=vec4(1.0); return; }
    vec3 p=worldPosition(v_texcoord0,depth,u_inverseVP,u_screenParams);
    vec3 n=normalize(texture2D(s_normal,v_texcoord0).xyz);
    float occlusion=0.0;
    for(int i=0;i<12;++i) {
        float angle=float(i)*2.39996323;
        float radius=(float(i)+1.0)/12.0*u_aoParams.x;
        vec2 uv=v_texcoord0+vec2(cos(angle),sin(angle))*radius/u_screenParams.xy;
        float d=texture2D(s_depth,uv).r;
        vec3 delta=worldPosition(uv,d,u_inverseVP,u_screenParams)-p;
        float distance=length(delta);
        float influence=1.0-smoothstep(0.05,u_aoParams.y,distance);
        occlusion+=max(dot(n,delta/max(distance,0.001))-0.12,0.0)*influence*step(d,0.999998);
    }
    gl_FragColor=vec4(vec3(clamp(1.0-occlusion*0.14,0.25,1.0)),1.0);
}
