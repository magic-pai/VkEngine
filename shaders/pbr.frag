#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location=0) in vec3 position;layout(location=1) in vec3 normal;layout(location=2) in vec4 tangent;layout(location=3) in vec2 uv0;layout(location=4) in vec2 uv1;layout(location=5) in vec4 color;layout(location=6) flat in uint objectId;
layout(location=0) out vec4 outputColor;
vec2 uv(int i){return materialUV(i,uv0,uv1);}
float visibility(vec3 n,vec3 l){vec4 p=g.lightVP*vec4(position,1);vec3 q=p.xyz/p.w;q.xy=q.xy*.5+.5;if(any(lessThan(q,vec3(0)))||any(greaterThan(q,vec3(1))))return 1;float bias=max(.00008,.0007*(1-max(dot(n,l),0)));float sum=0;vec2 texel=1./vec2(textureSize(shadowMap,0));for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++)sum+=texture(shadowMap,vec3(q.xy+vec2(x,y)*texel,q.z-bias));return sum/9.;}
void main(){
 vec4 base=mat.baseColor*texture(baseTex,uv(0))*color;if(mat.alpha.y==1&&base.a<mat.alpha.x)discard;
 vec3 n=normalize(normal);
 vec3 mapped=texture(normalTex,uv(1)).xyz*2-1;mapped.xy*=mat.factors.z;
 if(mat.info[1].rotationUV.w>.5){vec3 t=normalize(tangent.xyz-n*dot(n,tangent.xyz));vec3 b=cross(n,t)*tangent.w;
 if(mat.info[1].rotationUV.z>.5||abs(mat.info[1].rotationUV.x)>.00001||any(notEqual(mat.info[1].offsetScale.zw,vec2(1)))){vec3 dp1=dFdx(position),dp2=dFdy(position);vec2 a=dFdx(uv(1)),buv=dFdy(uv(1));float det=a.x*buv.y-a.y*buv.x;if(abs(det)>1e-10){t=normalize((dp1*buv.y-dp2*a.y)/det);t=normalize(t-n*dot(n,t));b=cross(n,t)*sign(det)*sign(dot(cross(dp1,dp2),n));}}
 n=normalize(mat3(t,b,n)*mapped);}
 if(!gl_FrontFacing)n=-n;
 vec4 mr=texture(metalTex,uv(2));bool separate=mat.alpha.z>.5;float metallic=clamp(mat.factors.x*(separate?mr.r:mr.b),0,1);float roughness=clamp(mat.factors.y*(separate?texture(roughTex,uv(7)).r:mr.g),.045,1);
 float ao=mix(1,texture(aoTex,uv(3)).r,mat.factors.w);vec3 emission=mat.emissive.xyz*texture(emissiveTex,uv(4)).rgb;
 if(mat.alpha.w>.5){outputColor=vec4(base.rgb+emission,base.a);return;}
 vec3 v=normalize(g.camera.xyz-position),l=normalize(-g.light.xyz),h=normalize(v+l);float nv=max(dot(n,v),.0001),nl=max(dot(n,l),0),nh=max(dot(n,h),0),vh=max(dot(v,h),0);
 float spec=mat.specular.w*texture(specTex,uv(5)).a;vec3 dielectric=min(vec3(.04)*mat.specular.rgb*texture(specColorTex,uv(6)).rgb,vec3(1))*spec;vec3 f0=mix(dielectric,base.rgb,metallic);vec3 f=f0+(1-f0)*pow(1-vh,5);float a=roughness*roughness,a2=a*a;float den=nh*nh*(a2-1)+1;float D=a2/(PI*den*den);float gv=nl*sqrt(nv*nv*(1-a2)+a2),gl=nv*sqrt(nl*nl*(1-a2)+a2);float G=.5/max(gv+gl,.00001);
 vec3 direct=((1-f)*(1-metallic)*base.rgb/PI+D*G*f)*g.lightColor.rgb*g.light.w*nl*visibility(n,l);
 vec3 fr=f0+(max(vec3(1-roughness),f0)-f0)*pow(1-nv,5);vec3 diffuse=texture(diffuseEnv,rotateEnv(n)).rgb*base.rgb*(1-metallic)*(1-fr);vec3 reflection=reflect(-v,n);vec3 prefiltered=textureLod(specularEnv,rotateEnv(reflection),roughness*8).rgb;vec2 brdf=texture(brdfLut,vec2(nv,roughness)).rg;vec3 ibl=diffuse+prefiltered*(f0*brdf.x+brdf.y);
 vec3 result=direct+ibl*g.environment.z*ao+emission;
 if(objectId==0){vec2 grid=abs(fract(position.xz-.5)-.5)/max(fwidth(position.xz),vec2(.0001));float line=1-min(min(grid.x,grid.y),1);result=mix(result,result*1.3,line*.3);}
 outputColor=vec4(result,mat.alpha.y==2?base.a:1);
}
