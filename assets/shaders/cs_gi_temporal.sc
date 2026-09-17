#include "bgfx_compute.sh"
#include "lighting_common.sh"
SAMPLER2D(s_giCurrent,0);
SAMPLER2D(s_giHistory,1);
SAMPLER2D(s_giPreviousGeometry,2);
SAMPLER2D(s_giDepth,3);
SAMPLER2D(s_giNormal,4);
SAMPLER2D(s_giMotion,5);
IMAGE2D_WO(s_giHistoryOut,rgba16f,6);
IMAGE2D_WO(s_giGeometryOut,rgba32f,7);
uniform mat4 u_giInverseVP;
uniform mat4 u_giPreviousVP;
uniform mat4 u_giView;
uniform vec4 u_giScreen;
uniform vec4 u_giResolve;
NUM_THREADS(8,8,1)
void main(){ivec2 pixel=ivec2(gl_GlobalInvocationID.xy);if(any(greaterThanEqual(pixel,ivec2(u_giResolve.xy))))return;
    vec2 uv=(vec2(pixel)+0.5)/u_giResolve.xy;float depth=texture2DLod(s_giDepth,uv,0.0).r;vec4 value=texture2DLod(s_giCurrent,uv,0.0);vec3 n=normalize(texture2DLod(s_giNormal,uv,0.0).xyz);vec3 world=worldPosition(uv,depth,u_giInverseVP,u_giScreen);float z=mul(u_giView,vec4(world,1.0)).z;
    if(depth>=0.999999){imageStore(s_giHistoryOut,pixel,vec4(0,0,0,1));imageStore(s_giGeometryOut,pixel,vec4(0,0,0,10000));return;}
    vec2 previousUv=uv-texture2DLod(s_giMotion,uv,0.0).xy;float weight=0.0;
    if(u_giResolve.z<0.5&&all(greaterThan(previousUv,vec2(0.0)))&&all(lessThan(previousUv,vec2(1.0)))){vec4 geometry=texture2DLod(s_giPreviousGeometry,previousUv,0.0);float expectedZ=mul(u_giPreviousVP,vec4(world,1.0)).w;if(dot(n,geometry.xyz)>0.92&&abs(expectedZ-geometry.w)<max(0.03,abs(expectedZ)*0.015))weight=0.75;}
    vec4 minimum=value,maximum=value;
    for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){vec4 sample=texture2DLod(s_giCurrent,uv+vec2(float(x),float(y))/u_giResolve.xy,0.0);minimum=min(minimum,sample);maximum=max(maximum,sample);}
    if(weight>0.0)value=mix(value,clamp(texture2DLod(s_giHistory,previousUv,0.0),minimum,maximum),weight);
    imageStore(s_giHistoryOut,pixel,value);imageStore(s_giGeometryOut,pixel,vec4(n,z));
}
