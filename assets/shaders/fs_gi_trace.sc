$input v_texcoord0
#include "bgfx_shader.sh"
#include "lighting_common.sh"
SAMPLER2D(s_giDepth,0);
SAMPLER2D(s_giNormal,1);
SAMPLER3D(s_giNear0,2);SAMPLER3D(s_giNear1,3);SAMPLER3D(s_giNear2,4);SAMPLER3D(s_giNear3,5);SAMPLER3D(s_giNear4,6);SAMPLER3D(s_giNear5,7);SAMPLER3D(s_giNear6,8);
SAMPLER3D(s_giFar0,9);SAMPLER3D(s_giFar1,10);SAMPLER3D(s_giFar2,11);SAMPLER3D(s_giFar3,12);SAMPLER3D(s_giFar4,13);SAMPLER3D(s_giFar5,14);SAMPLER3D(s_giFar6,15);
uniform mat4 u_giInverseVP;
uniform vec4 u_giScreen;
uniform vec4 u_giResolve;
uniform vec4 u_giOrigin;
uniform vec4 u_giFarOrigin;
vec3 atlas(vec3 q,float lod,int lobe){float dim=max(1.0,u_giResolve.w/exp2(lod+1.0));q=clamp(q,vec3(0.5/dim),vec3(1.0-0.5/dim));return vec3(q.x,(q.y+float(lobe))/6.0,q.z);}
vec4 levelNear(vec3 q,int level,int lobe){vec3 a=atlas(q,float(level),lobe);if(level==0)return texture3D(s_giNear0,a);if(level==1)return texture3D(s_giNear1,a);if(level==2)return texture3D(s_giNear2,a);if(level==3)return texture3D(s_giNear3,a);if(level==4)return texture3D(s_giNear4,a);if(level==5)return texture3D(s_giNear5,a);return texture3D(s_giNear6,a);}
vec4 levelFar(vec3 q,int level,int lobe){vec3 a=atlas(q,float(level),lobe);if(level==0)return texture3D(s_giFar0,a);if(level==1)return texture3D(s_giFar1,a);if(level==2)return texture3D(s_giFar2,a);if(level==3)return texture3D(s_giFar3,a);if(level==4)return texture3D(s_giFar4,a);if(level==5)return texture3D(s_giFar5,a);return texture3D(s_giFar6,a);}
vec4 sampleNearLobe(vec3 q,float diameter,int lobe){float lod=clamp(log2(max(diameter/(2.0*u_giOrigin.w),1.0)),0.0,log2(u_giResolve.w)-1.0);int i=int(floor(lod));return mix(levelNear(q,i,lobe),levelNear(q,min(i+1,6),lobe),fract(lod));}
vec4 sampleFarLobe(vec3 q,float diameter,int lobe){float lod=clamp(log2(max(diameter/(2.0*u_giFarOrigin.w),1.0)),0.0,log2(u_giResolve.w)-1.0);int i=int(floor(lod));return mix(levelFar(q,i,lobe),levelFar(q,min(i+1,6),lobe),fract(lod));}
// Blend the projected X/Y/Z faces. Picking only the dominant face loses all wall
// bounce for a floor's upward cones, because their dominant direction is always Y.
vec4 sampleNear(vec3 q,float diameter,vec3 direction) {
    vec3 w=abs(direction); w/=w.x+w.y+w.z;
    return sampleNearLobe(q,diameter,direction.x>=0.0?0:1)*w.x
         + sampleNearLobe(q,diameter,direction.y>=0.0?2:3)*w.y
         + sampleNearLobe(q,diameter,direction.z>=0.0?4:5)*w.z;
}
vec4 sampleFar(vec3 q,float diameter,vec3 direction) {
    vec3 w=abs(direction); w/=w.x+w.y+w.z;
    return sampleFarLobe(q,diameter,direction.x>=0.0?0:1)*w.x
         + sampleFarLobe(q,diameter,direction.y>=0.0?2:3)*w.y
         + sampleFarLobe(q,diameter,direction.z>=0.0?4:5)*w.z;
}
void main(){float depth=texture2D(s_giDepth,v_texcoord0).r;if(depth>=0.999999){gl_FragColor=vec4(0,0,0,1);return;}
    vec3 n=normalize(texture2D(s_giNormal,v_texcoord0).xyz),world=worldPosition(v_texcoord0,depth,u_giInverseVP,u_giScreen);
    vec3 tangent=normalize(cross(abs(n.y)<0.95?vec3(0,1,0):vec3(1,0,0),n)),bitangent=cross(n,tangent);
    vec3 nearSurface=(world-u_giOrigin.xyz)/(u_giOrigin.w*u_giResolve.w);
    float nearEdge=min(min(min(nearSurface.x,1.0-nearSurface.x),min(nearSurface.y,1.0-nearSurface.y)),min(nearSurface.z,1.0-nearSurface.z));
    // The conservative far cells need a correspondingly larger receiver offset.
    // Starting with the near-cell offset inside a far cell incorrectly occludes the receiver itself.
    float receiverCell=mix(u_giFarOrigin.w,u_giOrigin.w,smoothstep(0.02,0.1,nearEdge));
    vec3 irradiance=vec3(0.0);float sky=0.0;
    for(int c=0;c<4;++c){float angle=float(c)*1.57079632679;vec3 direction=normalize(n*0.8+(tangent*cos(angle)+bitangent*sin(angle))*0.6);vec3 outgoing=-direction;
        float distance=receiverCell,transmittance=1.0;vec3 sum=vec3(0.0);
        for(int step=0;step<24;++step){float diameter=max(2.0*u_giOrigin.w,distance*1.25);vec3 position=world+n*receiverCell+direction*distance;vec3 nearUV=(position-u_giOrigin.xyz)/(u_giOrigin.w*u_giResolve.w),farUV=(position-u_giFarOrigin.xyz)/(u_giFarOrigin.w*u_giResolve.w);
            bool insideNear=all(greaterThanEqual(nearUV,vec3(0.0)))&&all(lessThanEqual(nearUV,vec3(1.0))),insideFar=all(greaterThanEqual(farUV,vec3(0.0)))&&all(lessThanEqual(farUV,vec3(1.0)));if(!insideNear&&!insideFar)break;
            vec4 value=vec4(0.0);if(insideNear){value=sampleNear(nearUV,diameter,outgoing);float edge=min(min(min(nearUV.x,1.0-nearUV.x),min(nearUV.y,1.0-nearUV.y)),min(nearUV.z,1.0-nearUV.z));if(insideFar&&edge<0.1)value=mix(sampleFar(farUV,diameter,outgoing),value,smoothstep(0.02,0.1,edge));}else value=sampleFar(farUV,diameter,outgoing);
            sum+=transmittance*value.rgb;transmittance*=1.0-clamp(value.a,0.0,1.0);if(transmittance<0.005)break;distance+=max(u_giOrigin.w,diameter*0.5);}
        irradiance+=sum*(PI/4.0);sky+=transmittance*0.25;}
    gl_FragColor=vec4(min(irradiance,vec3(60000.0)),sky);
}
