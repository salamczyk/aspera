#include "bgfx_compute.sh"
BUFFER_RO(s_giOwners,uint,0);
SAMPLER2D(s_giPalette,1);
SAMPLER2D(s_giShadow,2);
IMAGE3D_WO(s_giRadiance,rgba16f,3);
uniform vec4 u_giOrigin;
uniform vec4 u_giDimensions;
uniform vec4 u_giSunDirection;
uniform vec4 u_giSun;
uniform mat4 u_giView;
uniform mat4 u_giShadowMatrices[15];
uniform vec4 u_giShadowRects[15];
uniform vec4 u_giShadowParams;
uniform vec4 u_giLights[320];
uint owner(ivec3 p){int r=int(u_giDimensions.x);if(any(lessThan(p,ivec3(0)))||any(greaterThanEqual(p,ivec3(r))))return 0u;return s_giOwners[uint(p.x+r*(p.y+r*p.z))];}
float shadow(int id,vec3 p,float nl){vec4 q=mul(u_giShadowMatrices[id],vec4(p,1));q.xyz/=q.w;vec4 bounds=u_giShadowRects[id];if(q.z<=0.0||q.z>=1.0||any(lessThan(q.xy,bounds.xy))||any(greaterThan(q.xy,bounds.zw)))return 1.0;return step(q.z-0.00015-0.0005*(1.0-nl),texture2DLod(s_giShadow,q.xy,0.0).r);}
int pointFace(vec3 d){vec3 a=abs(d);if(a.x>=a.y&&a.x>=a.z)return d.x>=0.0?0:1;if(a.y>=a.z)return d.y>=0.0?2:3;return d.z>=0.0?4:5;}
vec4 surface(ivec3 cell,ivec3 axis){
    uint material=owner(cell);if(material==0u)return vec4(0.0);
    if(owner(cell+axis)!=0u)return vec4(0.0,0.0,0.0,1.0);
    float x=(float(material)+0.5)/4096.0;
    vec4 base=texture2DLod(s_giPalette,vec2(x,0.5/3.0),0.0),emission=texture2DLod(s_giPalette,vec2(x,1.5/3.0),0.0);
    vec3 n=vec3(axis),p=u_giOrigin.xyz+(vec3(cell)+0.5+n*0.51)*u_giOrigin.w;
    vec3 l=-u_giSunDirection.xyz;float nl=max(dot(n,l),0.0),z=mul(u_giView,vec4(p,1.0)).z;
    int cascade=z<u_giShadowParams.x?0:(z<u_giShadowParams.y?1:2);
    float visibility=z<u_giShadowParams.z?shadow(cascade,p,nl):1.0;
    vec3 irradiance=u_giSun.rgb*u_giSun.w*nl*visibility;
    for(int k=0;k<64;++k){if(float(k)>=u_giDimensions.y)break;vec4 position=u_giLights[k*5],color=u_giLights[k*5+1],direction=u_giLights[k*5+2],cone=u_giLights[k*5+3],extra=u_giLights[k*5+4];
        vec3 delta=position.xyz-p;float distance=length(delta);if(distance>=position.w)continue;l=delta/max(distance,0.001);nl=max(dot(n,l),0.0);if(nl<=0.0)continue;
        float edge=clamp(1.0-pow(distance/position.w,4.0),0.0,1.0);float attenuation=edge*edge/max(distance*distance,0.01);if(direction.w>0.5)attenuation*=smoothstep(0.0,1.0,clamp((dot(-l,direction.xyz)-cone.y)/max(cone.x-cone.y,0.0001),0.0,1.0));
        visibility=1.0;if(extra.x>=0.0)visibility=shadow(int(extra.x)+(direction.w>0.5?0:pointFace(p-position.xyz)),p,nl);
        irradiance+=color.rgb*color.w*nl*attenuation*visibility;
    }
    vec3 radiance=base.rgb*(1.0-base.a)*irradiance/3.14159265359+emission.rgb*emission.a;
    return vec4(min(radiance,vec3(60000.0)),1.0);
}
NUM_THREADS(4,4,4)
void main(){
    ivec3 p=ivec3(gl_GlobalInvocationID);int dim=int(u_giDimensions.x)/2;if(p.x>=dim||p.y>=dim*6||p.z>=dim)return;
    int lobe=p.y/dim;ivec3 node=ivec3(p.x,p.y%dim,p.z)*2;int axis=lobe/2;bool positive=(lobe%2)==0;
    ivec3 direction=axis==0?ivec3(positive?1:-1,0,0):(axis==1?ivec3(0,positive?1:-1,0):ivec3(0,0,positive?1:-1));
    vec4 sum=vec4(0.0);
    for(int a=0;a<2;++a)for(int b=0;b<2;++b){ivec3 front=axis==0?ivec3(positive?1:0,a,b):(axis==1?ivec3(a,positive?1:0,b):ivec3(a,b,positive?1:0));ivec3 back=front-direction;
        vec4 first=surface(node+front,direction),second=surface(node+back,direction);sum+=first+second*(1.0-first.a);}
    imageStore(s_giRadiance,p,sum*0.25);
}
