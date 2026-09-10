#version 450
layout(set=0,binding=0) uniform sampler2D hdr;
layout(set=0,binding=1) uniform usampler2D objectIds;
layout(push_constant) uniform Push{float exposure;float srgb;uint selected;uint pad;}p;
layout(location=0) in vec2 uv;layout(location=0) out vec4 color;
void main(){vec3 x=texture(hdr,uv).rgb*exp2(p.exposure);x=clamp((x*(2.51*x+.03))/(x*(2.43*x+.59)+.14),0,1);
if(p.selected!=0){ivec2 size=textureSize(objectIds,0);ivec2 pixel=clamp(ivec2(uv*size),ivec2(0),size-1);uint center=texelFetch(objectIds,pixel,0).r;if(center!=p.selected){bool edge=false;for(int y=-2;y<=2;y++)for(int x=-2;x<=2;x++)edge=edge||texelFetch(objectIds,clamp(pixel+ivec2(x,y),ivec2(0),size-1),0).r==p.selected;if(edge)x=vec3(1,.4,.03);}}
if(p.srgb<.5)x=mix(12.92*x,1.055*pow(x,vec3(1./2.4))-.055,step(vec3(.0031308),x));color=vec4(x,1);}
