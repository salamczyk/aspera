$input v_texcoord0
#include "bgfx_shader.sh"
SAMPLER2D(s_source,0);
uniform vec4 u_postParams;
void main() {
    vec3 sum=vec3(0.0); float weights=0.0;
    for(int i=-4;i<=4;++i) {
        float weight=exp(-float(i*i)/8.0);
        vec3 color=texture2D(s_source,v_texcoord0+u_postParams.xy*float(i)).rgb;
        if(u_postParams.z>0.0) color=max(color-vec3(u_postParams.z),vec3(0.0));
        sum+=color*weight; weights+=weight;
    }
    gl_FragColor=vec4(sum/weights,1.0);
}
