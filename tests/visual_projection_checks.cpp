#include "nickel/visual_projection.hpp"
#include <limits>
#include <cstdio>
int main(){
 using nickel::visual::Point;using nickel::visual::project;
 float m[16]={1,0,0,0,0,1,0,0,0,0,0,1,0,0,0,0};float origin[3]={100,200,300},p[3]={100,200,302};Point out{};int failures=0;
 auto check=[&](bool ok,const char* name){if(!ok){std::fprintf(stderr,"%s failed\n",name);++failures;}};
 check(project(m,origin,p,1920,1080,&out)&&out.x==960&&out.y==540,"center translated camera");
 p[0]=101;p[1]=201;check(project(m,origin,p,1920,1080,&out)&&out.x==1440&&out.y==270,"perspective and inverted screen Y");
 p[2]=299;check(!project(m,origin,p,1920,1080,&out),"behind camera rejected");
 p[2]=300;check(!project(m,origin,p,1920,1080,&out),"camera plane rejected");
 p[2]=302;p[0]=std::numeric_limits<float>::quiet_NaN();check(!project(m,origin,p,1920,1080,&out),"NaN rejected");
 p[0]=101;check(!project(m,origin,p,0,1080,&out),"zero viewport rejected");
 check(!project(m,origin,p,std::numeric_limits<float>::infinity(),1080,&out),"infinite viewport rejected");
 Point from{},to{};float a[3]={100,200,299},b[3]={100,200,302};
 check(nickel::visual::project_segment(m,origin,a,b,1920,1080,&from,&to)&&from.x==960&&from.y==540&&to.x==960&&to.y==540,"near crossing is clipped");
 a[2]=298;b[2]=299;check(!nickel::visual::project_segment(m,origin,a,b,1920,1080,&from,&to),"both endpoints behind rejected");
 a[2]=302;b[2]=299;check(nickel::visual::project_segment(m,origin,a,b,1920,1080,&from,&to),"reversed near crossing is clipped");
 return failures?1:0;
}
