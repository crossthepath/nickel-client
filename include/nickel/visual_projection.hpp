#pragma once
#include <cmath>
namespace nickel::visual {
struct Point { float x,y; };
// Column-major camera transform; rejects non-finite input and points behind the near plane.
inline bool project(const float* matrix,const float* origin,const float* position,float width,float height,Point* out) {
 if(!matrix||!origin||!position||!out||!std::isfinite(width)||!std::isfinite(height)||width<=0||height<=0)return false;
 const float x=position[0]-origin[0],y=position[1]-origin[1],z=position[2]-origin[2];
 const float w=matrix[3]*x+matrix[7]*y+matrix[11]*z+matrix[15];
 if(!std::isfinite(w)||w<.05f)return false;
 const Point result{width*.5f+(matrix[0]*x+matrix[4]*y+matrix[8]*z+matrix[12])/w*width*.5f,height*.5f-(matrix[1]*x+matrix[5]*y+matrix[9]*z+matrix[13])/w*height*.5f};
 if(!std::isfinite(result.x)||!std::isfinite(result.y)||std::fabs(result.x)>width*16||std::fabs(result.y)>height*16)return false;
 *out=result;return true;
}
inline bool project_segment(const float* matrix,const float* origin,const float* a,const float* b,float width,float height,Point* from,Point* to) {
 if(!matrix||!origin||!a||!b||!from||!to)return false;
 float first[3]={a[0],a[1],a[2]},last[3]={b[0],b[1],b[2]};
 auto depth=[&](const float* p){return matrix[3]*(p[0]-origin[0])+matrix[7]*(p[1]-origin[1])+matrix[11]*(p[2]-origin[2])+matrix[15];};
 const float wa=depth(first),wb=depth(last);if(!std::isfinite(wa)||!std::isfinite(wb)||(wa<.05f&&wb<.05f))return false;
 // Slightly inside the near plane so rounding does not reject the clipped endpoint.
 if(wa<.05f||wb<.05f){const bool clipFirst=wa<.05f;const float t=(.05001f-wa)/(wb-wa);float* clipped=clipFirst?first:last;for(int i=0;i<3;++i)clipped[i]=a[i]+(b[i]-a[i])*t;}
 return project(matrix,origin,first,width,height,from)&&project(matrix,origin,last,width,height,to);
}

}
