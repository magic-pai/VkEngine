const float PI=3.14159265359;
struct Instance { mat4 model; uvec4 meta; };
layout(set=0,binding=0,std140) uniform Globals { mat4 vp; mat4 inverseVP; mat4 lightVP; vec4 camera; vec4 light; vec4 lightColor; vec4 environment; } g;
layout(set=0,binding=1,std430) readonly buffer Instances { Instance instances[]; };
layout(set=0,binding=2) uniform samplerCube specularEnv;
layout(set=0,binding=3) uniform samplerCube diffuseEnv;
layout(set=0,binding=4) uniform sampler2D brdfLut;
layout(set=0,binding=5) uniform sampler2DShadow shadowMap;
struct TextureInfo { vec4 offsetScale; vec4 rotationUV; };
layout(set=1,binding=0,std140) uniform Material { vec4 baseColor; vec4 emissive; vec4 factors; vec4 specular; vec4 alpha; TextureInfo info[8]; } mat;
layout(set=1,binding=1) uniform sampler2D baseTex;
layout(set=1,binding=2) uniform sampler2D normalTex;
layout(set=1,binding=3) uniform sampler2D metalTex;
layout(set=1,binding=4) uniform sampler2D aoTex;
layout(set=1,binding=5) uniform sampler2D emissiveTex;
layout(set=1,binding=6) uniform sampler2D specTex;
layout(set=1,binding=7) uniform sampler2D specColorTex;
layout(set=1,binding=8) uniform sampler2D roughTex;
vec2 materialUV(int i,vec2 uv0,vec2 uv1){TextureInfo t=mat.info[i];vec2 uv=(t.rotationUV.z>.5?uv1:uv0)*t.offsetScale.zw;return mat2(t.rotationUV.y,t.rotationUV.x,-t.rotationUV.x,t.rotationUV.y)*uv+t.offsetScale.xy;}
vec3 rotateEnv(vec3 d){float a=g.environment.y;return vec3(cos(a)*d.x-sin(a)*d.z,d.y,sin(a)*d.x+cos(a)*d.z);}
