$input v_texcoord0
#include "bgfx_shader.sh"
#include "lighting_common.sh"
SAMPLER2D(s_base,0);
SAMPLER2D(s_normal,1);
SAMPLER2D(s_depth,2);
SAMPLER2D(s_emission,3);
SAMPLER2D(s_shadow,4);
SAMPLER2D(s_indirect,5);
SAMPLER2D(s_ao,6);
SAMPLER2D(s_tiles,7);
SAMPLERCUBE(s_sky,8);
SAMPLER2D(s_brdfLut,9);
SAMPLER2D(s_voxelDebug,10);
uniform mat4 u_inverseVP;
uniform mat4 u_cameraView;
uniform mat4 u_shadowMatrices[15];
uniform vec4 u_shadowRects[15];
uniform vec4 u_lightData[320];
uniform vec4 u_sunDirection;
uniform vec4 u_sunColor;
uniform vec4 u_skyColor;
uniform vec4 u_eye;
uniform vec4 u_screenParams;
uniform vec4 u_shadowParams;
uniform vec4 u_renderParams;
float visibility(int index, vec3 position, float nl) {
    vec4 q=mul(u_shadowMatrices[index],vec4(position,1.0));
    vec3 p=q.xyz/q.w;
    vec4 bounds=u_shadowRects[index];
    if (p.z<=0.0 || p.z>=1.0 || p.x<bounds.x || p.y<bounds.y || p.x>bounds.z || p.y>bounds.w) return 1.0;
    float bias=0.00012+0.0005*(1.0-nl);
    float sum=0.0;
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) {
        vec2 uv=clamp(p.xy+vec2(float(x),float(y))*u_shadowParams.w,bounds.xy+u_shadowParams.ww,bounds.zw-u_shadowParams.ww);
        sum += step(p.z-bias,texture2DLod(s_shadow,uv,0.0).r);
    }
    return sum/9.0;
}
int pointFace(vec3 d) {
    vec3 a=abs(d);
    if(a.x>=a.y && a.x>=a.z) return d.x>=0.0?0:1;
    if(a.y>=a.z) return d.y>=0.0?2:3;
    return d.z>=0.0?4:5;
}
void main() {
    vec2 uv=v_texcoord0;
    if(u_renderParams.x>5.5) {gl_FragColor=vec4(texture2D(s_voxelDebug,uv).rgb,1.0);return;}
    float depth=texture2D(s_depth,uv).r;
    vec3 p=worldPosition(uv,depth,u_inverseVP,u_screenParams);
    vec3 v=normalize(u_eye.xyz-p);
    if(depth>=0.999999) {
        vec3 d=-v;
        vec3 sky=skyRadiance(d,u_skyColor.rgb,u_skyColor.w,u_sunDirection.xyz,u_sunColor.rgb);
        sky += u_sunColor.rgb * u_sunColor.w * smoothstep(0.9998,0.99995,dot(d,-u_sunDirection.xyz));
        gl_FragColor=vec4(min(sky,vec3(60000.0)),1.0); return;
    }
    vec4 base=texture2D(s_base,uv), normal=texture2D(s_normal,uv);
    vec3 n=normalize(normal.xyz);
    vec3 emission=texture2D(s_emission,uv).rgb;
    float z=mul(u_cameraView,vec4(p,1.0)).z;
    int cascade=z<u_shadowParams.x?0:(z<u_shadowParams.y?1:2);
    float sunVis=1.0;
    float nl=max(dot(n,-u_sunDirection.xyz),0.0);
    vec3 shadowPosition=p+n*u_eye.w*0.25;
    if(z<u_shadowParams.z) {
        sunVis=visibility(cascade,shadowPosition,nl);
        float end=cascade==0?u_shadowParams.x:(cascade==1?u_shadowParams.y:u_shadowParams.z);
        float blend=smoothstep(end*0.9,end,z);
        float next=cascade<2?visibility(cascade+1,shadowPosition,nl):1.0;
        sunVis=mix(sunVis,next,blend);
    }
    vec3 direct=brdf(base.rgb,base.a,normal.a,n,v,-u_sunDirection.xyz)*u_sunColor.rgb*u_sunColor.w*sunVis;
    vec2 tile=floor(uv*u_screenParams.xy/16.0);
    vec2 tileCount=ceil(u_screenParams.xy/16.0);
    vec2 tileDims=vec2(tileCount.x*17.0,tileCount.y);
    float count=texture2DLod(s_tiles,(vec2(tile.x*17.0,tile.y)+0.5)/tileDims,0.0).r;
    for(int k=0;k<64;++k) {
        if(float(k)>=count) break;
        vec4 packed=texture2DLod(s_tiles,(vec2(tile.x*17.0+1.0+floor(float(k)/4.0),tile.y)+0.5)/tileDims,0.0);
        int component=k-int(floor(float(k)/4.0))*4;
        int id=int(component==0?packed.x:(component==1?packed.y:(component==2?packed.z:packed.w)));
        vec4 pos=u_lightData[id*5], color=u_lightData[id*5+1], dir=u_lightData[id*5+2], cone=u_lightData[id*5+3], extra=u_lightData[id*5+4];
        vec3 delta=pos.xyz-p; float distance=length(delta); vec3 l=delta/max(distance,0.0001);
        float edge=clamp(1.0-pow(distance/pos.w,4.0),0.0,1.0);
        float attenuation=edge*edge/max(distance*distance,0.01);
        if(dir.w>0.5) attenuation*=smoothstep(0.0,1.0,clamp((dot(-l,dir.xyz)-cone.y)/max(cone.x-cone.y,0.0001),0.0,1.0));
        float shadow=1.0;
        if(extra.x>=0.0) {
            int face=dir.w>0.5?0:pointFace(p-pos.xyz);
            shadow=visibility(int(extra.x)+face,shadowPosition,max(dot(n,l),0.0));
        }
        direct+=brdf(base.rgb,base.a,normal.a,n,v,l)*color.rgb*color.w*attenuation*shadow;
    }
    // GI is irradiance, already occluded. SSAO affects only unresolved sky fill.
    vec4 indirect=texture2D(s_indirect,uv);
    float ao=texture2D(s_ao,uv).r;
    vec3 f0=mix(vec3(0.04),base.rgb,base.a);
    vec3 F=fresnel(f0,max(dot(n,v),0.0));
    vec3 diffuse=(1.0-F)*(1.0-base.a)*base.rgb/PI;
    vec3 skyDiffuse=u_skyColor.rgb*u_skyColor.w*(0.35+0.65*max(n.y,0.0))*indirect.a*ao;
    vec3 reflected=reflect(-v,n);
    vec3 prefiltered=textureCubeLod(s_sky,reflected,normal.a*7.0).rgb;
    vec2 lut=texture2D(s_brdfLut,vec2(max(dot(n,v),0.001),normal.a)).rg;
    vec3 specular=prefiltered*(f0*lut.x+lut.y)*mix(indirect.a*ao,1.0,normal.a*0.25);
    vec3 result=direct+diffuse*(indirect.rgb+skyDiffuse)+specular+emission;
    if(u_renderParams.x>0.5 && u_renderParams.x<1.5) result=base.rgb*10000.0;
    else if(u_renderParams.x<2.5 && u_renderParams.x>1.5) result=direct;
    else if(u_renderParams.x<3.5 && u_renderParams.x>2.5) result=vec3(sunVis)*10000.0;
    else if(u_renderParams.x<4.5 && u_renderParams.x>3.5) result=diffuse*indirect.rgb;
    else if(u_renderParams.x<5.5 && u_renderParams.x>4.5) result=vec3(ao)*10000.0;
    else if(u_renderParams.x>5.5) result=texture2D(s_voxelDebug,uv).rgb;
    gl_FragColor=vec4(min(result,vec3(60000.0)),1.0);
}
