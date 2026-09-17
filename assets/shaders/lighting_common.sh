#ifndef ASPERA_LIGHTING_COMMON
#define ASPERA_LIGHTING_COMMON
#define PI 3.14159265359
vec3 worldPosition(vec2 uv, float depth, mat4 inverseVP, vec4 screen) {
    vec4 p = vec4(uv * 2.0 - 1.0, depth * screen.w - (screen.w - 1.0), 1.0);
    p.y *= screen.z;
    p = mul(inverseVP, p);
    return p.xyz / p.w;
}
vec3 fresnel(vec3 f0, float h) { return f0 + (1.0 - f0) * pow(1.0 - h, 5.0); }
vec3 brdf(vec3 base, float metal, float rough, vec3 n, vec3 v, vec3 l) {
    vec3 h = normalize(v + l);
    float nl = max(dot(n,l), 0.0), nv = max(dot(n,v),0.001), nh=max(dot(n,h),0.0);
    float a = max(rough*rough,0.0025), a2=a*a;
    float d=nh*nh*(a2-1.0)+1.0;
    float D=a2/(PI*d*d);
    float gv=nl*sqrt(nv*nv*(1.0-a2)+a2);
    float gl=nv*sqrt(nl*nl*(1.0-a2)+a2);
    vec3 F=fresnel(mix(vec3(0.04),base,metal),max(dot(v,h),0.0));
    return ((1.0-F)*(1.0-metal)*base/PI + D*F*0.5/max(gv+gl,0.0001))*nl;
}
vec3 skyRadiance(vec3 direction, vec3 skyColor, float skyIntensity, vec3 sunDirection, vec3 sunColor) {
    float y = max(direction.y, 0.0);
    vec3 tint = mix(vec3(0.78,0.83,0.9), skyColor, sqrt(y));
    float halo=pow(max(dot(direction,-sunDirection),0.0),24.0);
    float horizon=1.0-clamp(-sunDirection.y,0.0,1.0);
    tint+=sunColor*halo*(0.25+0.75*horizon);
    return tint * skyIntensity / PI * mix(0.25, 1.0, smoothstep(-0.12,0.18,direction.y));
}
vec3 linearToSrgb(vec3 c) {
    c=max(c,vec3(0.0));
    return mix(12.92*c,1.055*pow(c,vec3(1.0/2.4))-0.055,step(vec3(0.0031308),c));
}
vec3 aces(vec3 c) { return clamp((c*(2.51*c+0.03))/(c*(2.43*c+0.59)+0.14),0.0,1.0); }
#endif
