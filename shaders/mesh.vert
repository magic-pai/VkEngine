#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location=0) in vec3 inPosition;layout(location=1) in vec3 inNormal;layout(location=2) in vec4 inTangent;layout(location=3) in vec2 inUV0;layout(location=4) in vec2 inUV1;layout(location=5) in vec4 inColor;
layout(location=0) out vec3 position;layout(location=1) out vec3 normal;layout(location=2) out vec4 tangent;layout(location=3) out vec2 uv0;layout(location=4) out vec2 uv1;layout(location=5) out vec4 color;layout(location=6) flat out uint objectId;
layout(push_constant) uniform Push{int mode;} push;
void main(){Instance instance=instances[gl_InstanceIndex];vec4 world=instance.model*vec4(inPosition,1);position=world.xyz;normal=normalize(transpose(inverse(mat3(instance.model)))*inNormal);tangent=vec4(normalize(mat3(instance.model)*inTangent.xyz),inTangent.w*sign(determinant(mat3(instance.model))));uv0=inUV0;uv1=inUV1;color=inColor;objectId=instance.meta.x;gl_Position=(push.mode==1?g.lightVP:g.vp)*world;}
