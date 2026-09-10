#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location=3) in vec2 uv0;layout(location=4) in vec2 uv1;layout(location=5) in vec4 color;
void main(){float a=mat.baseColor.a*color.a*texture(baseTex,materialUV(0,uv0,uv1)).a;if(mat.alpha.y==1&&a<mat.alpha.x)discard;if(mat.alpha.y==2&&a<.5)discard;}
