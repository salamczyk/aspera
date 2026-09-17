#include "bgfx_compute.sh"
BUFFER_RO(s_giBoxes,vec4,0);
BUFFER_RO(s_giRanges,uint,1);
BUFFER_RO(s_giIndices,uint,2);
BUFFER_RO(s_giDirty,uint,3);
BUFFER_RW(s_giOwners,uint,4);
uniform vec4 u_giOrigin;
uniform vec4 u_giDimensions;
bool overlap(vec3 q,vec3 x,vec3 y,vec3 z,float cellHalf) {
    // All 15 separating axes for an OBB against the axis-aligned volume cell.
    vec3 extent=abs(x)+abs(y)+abs(z)+vec3(cellHalf);
    float epsilon=cellHalf*0.0001;
    if(any(greaterThanEqual(abs(q),extent-vec3(epsilon))))return false;
    vec3 axes[3];axes[0]=x;axes[1]=y;axes[2]=z;
    for(int i=0;i<3;++i){vec3 a=normalize(axes[i]);float h=length(axes[i])+cellHalf*(abs(a.x)+abs(a.y)+abs(a.z));if(abs(dot(q,a))>=h-epsilon)return false;
        for(int k=0;k<3;++k){vec3 basis=k==0?vec3(1,0,0):(k==1?vec3(0,1,0):vec3(0,0,1));vec3 c=cross(a,basis);if(dot(c,c)<0.00001)continue;
            float r=abs(dot(x,c))+abs(dot(y,c))+abs(dot(z,c))+cellHalf*(abs(c.x)+abs(c.y)+abs(c.z));if(abs(dot(q,c))>=r-epsilon)return false;}}
    return true;
}
NUM_THREADS(8,8,8)
void main(){
    uint brick=s_giDirty[gl_WorkGroupID.x], n=uint(u_giDimensions.y);
    uvec3 b=uvec3(brick%n,(brick/n)%n,brick/(n*n));uvec3 cell=b*8u+gl_LocalInvocationID;
    vec3 center=u_giOrigin.xyz+(vec3(cell)+0.5)*u_giOrigin.w;
    uint first=s_giRanges[brick*2u],count=s_giRanges[brick*2u+1u],material=0u;
    for(uint j=0u;j<count;++j){uint i=s_giIndices[first+j]*4u;vec4 box=s_giBoxes[i];if(overlap(center-box.xyz,s_giBoxes[i+1u].xyz,s_giBoxes[i+2u].xyz,s_giBoxes[i+3u].xyz,u_giOrigin.w*0.5)){material=uint(box.w+0.5);break;}}
    uint r=uint(u_giDimensions.x);s_giOwners[cell.x+r*(cell.y+r*cell.z)]=material;
}
