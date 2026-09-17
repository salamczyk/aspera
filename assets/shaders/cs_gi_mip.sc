#include "bgfx_compute.sh"
SAMPLER3D(s_giMipSource,0);
IMAGE3D_WO(s_giMipDestination,rgba16f,1);
uniform vec4 u_giMip;
vec4 value(ivec3 p,int lobe){float dim=u_giMip.y;return texture3DLod(s_giMipSource,(vec3(p)+vec3(0.5,float(lobe)*dim+0.5,0.5))/vec3(dim,dim*6.0,dim),0.0);}
NUM_THREADS(4,4,4)
void main(){ivec3 p=ivec3(gl_GlobalInvocationID);int dim=int(u_giMip.x);if(p.x>=dim||p.y>=dim*6||p.z>=dim)return;
    int lobe=p.y/dim;ivec3 node=ivec3(p.x,p.y%dim,p.z)*2;int axis=lobe/2;bool positive=(lobe%2)==0;
    ivec3 direction=axis==0?ivec3(positive?1:-1,0,0):(axis==1?ivec3(0,positive?1:-1,0):ivec3(0,0,positive?1:-1));vec4 sum=vec4(0.0);
    for(int a=0;a<2;++a)for(int b=0;b<2;++b){ivec3 front=axis==0?ivec3(positive?1:0,a,b):(axis==1?ivec3(a,positive?1:0,b):ivec3(a,b,positive?1:0));vec4 first=value(node+front,lobe),second=value(node+front-direction,lobe);sum+=first+second*(1.0-first.a);}
    imageStore(s_giMipDestination,p,sum*0.25);
}
