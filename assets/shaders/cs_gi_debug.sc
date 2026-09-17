#include "bgfx_compute.sh"
BUFFER_RO(s_giOwners,uint,0);
IMAGE2D_WO(s_giDebugImage,rgba16f,1);
uniform vec4 u_giDebug;
NUM_THREADS(8,8,1)
void main(){uvec2 pixel=gl_GlobalInvocationID.xy;if(any(greaterThanEqual(pixel,uvec2(u_giDebug.xy))))return;uint r=uint(u_giDebug.z);uvec2 cell=min(uvec2((vec2(pixel)+0.5)/u_giDebug.xy*float(r)),uvec2(r-1u));uint m=s_giOwners[cell.x+r*(uint(u_giDebug.w)+r*cell.y)];vec3 color=m==0u?vec3(0.02,0.03,0.04):vec3(fract(float(m)*0.37),fract(float(m)*0.61),fract(float(m)*0.83));imageStore(s_giDebugImage,ivec2(pixel),vec4(color*10000.0,1.0));}
