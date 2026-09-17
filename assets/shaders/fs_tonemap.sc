$input v_texcoord0
#include "bgfx_shader.sh"
#include "lighting_common.sh"
SAMPLER2D(s_source,0);
SAMPLER2D(s_bloom,1);
uniform vec4 u_postParams;
void main() {
    vec3 color=texture2D(s_source,v_texcoord0).rgb;
    color+=texture2D(s_bloom,v_texcoord0).rgb*u_postParams.y;
    color=linearToSrgb(aces(color*u_postParams.x));
    // FXAA-style edge resolve on display luminance, keeping the voxel silhouette.
    vec2 pixel=u_postParams.zw;
    vec3 north=linearToSrgb(aces(texture2D(s_source,v_texcoord0+vec2(0.0,-pixel.y)).rgb*u_postParams.x));
    vec3 south=linearToSrgb(aces(texture2D(s_source,v_texcoord0+vec2(0.0,pixel.y)).rgb*u_postParams.x));
    vec3 east=linearToSrgb(aces(texture2D(s_source,v_texcoord0+vec2(pixel.x,0.0)).rgb*u_postParams.x));
    vec3 west=linearToSrgb(aces(texture2D(s_source,v_texcoord0+vec2(-pixel.x,0.0)).rgb*u_postParams.x));
    vec3 weights=vec3(0.299,0.587,0.114);
    float range=max(max(dot(north,weights),dot(south,weights)),max(dot(east,weights),dot(west,weights)))-min(min(dot(north,weights),dot(south,weights)),min(dot(east,weights),dot(west,weights)));
    color=mix(color,(north+south+east+west)*0.25,clamp((range-0.15)*0.3,0.0,0.15));
    gl_FragColor=vec4(color,1.0);
}
