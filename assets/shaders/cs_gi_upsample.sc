#include "bgfx_compute.sh"
#include "lighting_common.sh"
SAMPLER2D(s_giFiltered,0);
SAMPLER2D(s_giGeometry,1);
SAMPLER2D(s_giDepth,2);
SAMPLER2D(s_giNormal,3);
IMAGE2D_WO(s_giUpsampled,rgba16f,4);
uniform mat4 u_giInverseVP;
uniform mat4 u_giView;
uniform vec4 u_giScreen;
uniform vec4 u_giResolve;
NUM_THREADS(8,8,1)
void main(){ivec2 pixel=ivec2(gl_GlobalInvocationID.xy);if(any(greaterThanEqual(pixel,ivec2(u_giScreen.xy))))return;vec2 uv=(vec2(pixel)+0.5)/u_giScreen.xy;
    float depth=texture2DLod(s_giDepth,uv,0.0).r;if(depth>=0.999999){imageStore(s_giUpsampled,pixel,vec4(0,0,0,1));return;}
    vec3 n=normalize(texture2DLod(s_giNormal,uv,0.0).xyz),world=worldPosition(uv,depth,u_giInverseVP,u_giScreen);float z=mul(u_giView,vec4(world,1.0)).z;vec2 base=floor(uv*u_giResolve.xy-0.5);vec4 sum=vec4(0.0);float weights=0.0;
    for(int y=0;y<2;++y)for(int x=0;x<2;++x){vec2 sampleUv=(base+vec2(float(x),float(y))+0.5)/u_giResolve.xy;vec4 geometry=texture2DLod(s_giGeometry,sampleUv,0.0);float spatial=max(0.001,1.0-abs((sampleUv.x-uv.x)*u_giResolve.x))*max(0.001,1.0-abs((sampleUv.y-uv.y)*u_giResolve.y));float weight=spatial*pow(max(dot(n,geometry.xyz),0.0),16.0)*exp(-abs(z-geometry.w)/max(0.03,z*0.01));sum+=texture2DLod(s_giFiltered,sampleUv,0.0)*weight;weights+=weight;}
    imageStore(s_giUpsampled,pixel,weights>0.0001?sum/weights:texture2DLod(s_giFiltered,uv,0.0));
}
