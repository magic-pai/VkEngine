#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location=0) in vec2 uv;layout(location=0) out vec4 color;
void main(){vec4 far=g.inverseVP*vec4(uv*2-1,1,1);vec3 d=normalize(far.xyz/far.w-g.camera.xyz);color=vec4(textureLod(specularEnv,rotateEnv(d),0).rgb*g.environment.z,1);}
