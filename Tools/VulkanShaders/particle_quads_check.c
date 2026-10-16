// GPUPARTICLES: host check of the quad record maths. gcc -O2 -ffp-contract=off particle_quads_check.c -lm && ./a.out
// Stock BuildVertex corner math vs GxParticleQuad expansion ((cx*A + cy*B) + C), float, no contraction.
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
static const float co[4][2] = {{-1,1},{-1,-1},{1,1},{1,-1}};
static const float tc[4][2] = {{0,0},{0,1},{1,0},{1,1}};
static float R(void){ return (float)(rand()/(double)RAND_MAX*200.0-100.0); }
typedef struct { float c[3], a[3], b[3], u0, v0, u1, v1; } Q;
static void expand(const Q* q, int i, float p[3], float t[2]) {
    for (int k=0;k<3;k++){ volatile float s = co[i][0]*q->a[k] + co[i][1]*q->b[k]; p[k] = s + q->c[k]; }
    t[0] = i<2 ? q->u0 : q->u1; t[1] = (i&1)==0 ? q->v0 : q->v1;
}
int main(void){
    int exact[5]={0}, total[5]={0}; double maxerr[5]={0};
    for (int n=0;n<100000;n++){
        float c[3]={R(),R(),R()}, sx=fabsf(R())/10, sy=fabsf(R())/10, w=0.25f, h=0.125f, u=R()/100, v=R()/100;
        float ref[4][3], rt[4][2]; Q q; int mode = n%5;
        float u0=tc[0][0]*w+u, v0=tc[0][1]*h+v, u1=tc[3][0]*w+u, v1=tc[3][1]*h+v;
        memcpy(q.c,c,12); q.u0=u0;q.v0=v0;q.u1=u1;q.v1=v1;
        if (mode==0){ // flat, no spin
            for(int i=0;i<4;i++){ ref[i][0]=co[i][0]*sx+c[0]; ref[i][1]=co[i][1]*sy+c[1]; ref[i][2]=c[2]; rt[i][0]=tc[i][0]*w+u; rt[i][1]=tc[i][1]*h+v; }
            float a[3]={sx,0,0}, b[3]={0,sy,0}; memcpy(q.a,a,12); memcpy(q.b,b,12);
        } else if (mode==1){ // billboard, no spin
            float A0=R()/100,A1=R()/100,A2=R()/100,B0=R()/100,B1=R()/100,B2=R()/100;
            for(int i=0;i<4;i++){ float ox=co[i][0]*sx, oy=co[i][1]*sy; volatile float x=B0*oy+ox*A0, y=oy*B1+ox*A1, z=oy*B2+ox*A2; ref[i][0]=x+c[0]; ref[i][1]=y+c[1]; ref[i][2]=z+c[2]; rt[i][0]=tc[i][0]*w+u; rt[i][1]=tc[i][1]*h+v; }
            float a[3]={sx*A0,sx*A1,sx*A2}, b[3]={sy*B0,sy*B1,sy*B2}; memcpy(q.a,a,12); memcpy(q.b,b,12);
        } else if (mode==2){ // velocity aligned
            float ax=R()/100, ay=R()/100;
            for(int i=0;i<4;i++){ float ox=co[i][0]*sx, oy=co[i][1]*sy; volatile float x=oy*-ay+ox*ax, y=ox*ay+oy*ax; ref[i][0]=x+c[0]; ref[i][1]=y+c[1]; ref[i][2]=c[2]; rt[i][0]=tc[i][0]*w+u; rt[i][1]=tc[i][1]*h+v; }
            float a[3]={sx*ax,sx*ay,0}, b[3]={sy*-ay,sy*ax,0}; memcpy(q.a,a,12); memcpy(q.b,b,12);
        } else if (mode==3){ // spin flat
            float ang=R(), sa=sinf(ang), ca=cosf(ang), cX=ca*sx, cY=ca*sy, sX=sx*sa, sY=sy*sa;
            float P[4][3]={{c[0]-cX-sY, c[1]-sX+cY, c[2]},{c[0]-cX+sY, c[1]-sX-cY, c[2]},{cX+c[0]-sY, cY+sX+c[1], c[2]},{cX+sY+c[0], sX+c[1]-cY, c[2]}};
            float T[4][2]={{u,v},{u,v+h},{w+u,v},{u+w,v+h}};
            memcpy(ref,P,sizeof P); memcpy(rt,T,sizeof T);
            float a[3]={ca*sx,sx*sa,0}, b[3]={-(sy*sa),ca*sy,0}; memcpy(q.a,a,12); memcpy(q.b,b,12); q.u0=u; q.v0=v; q.u1=w+u; q.v1=v+h;
        } else { // tail
            float t[3]={R()/10,R()/10,R()/10}; float fl=t[0]*t[0]+t[1]*t[1], inv=1.0f/sqrtf(fl), px=t[0]*(sx*inv), py=t[1]*(inv*sy);
            float e[3]={t[0]+c[0],t[1]+c[1],t[2]+c[2]};
            float P[4][3]={{c[0]-py,c[1]+px,c[2]},{py+c[0],c[1]-px,c[2]},{e[0]-py,px+e[1],e[2]},{e[0]+py,e[1]-px,e[2]}};
            memcpy(ref,P,sizeof P); for(int i=0;i<4;i++){rt[i][0]=w*tc[i][0]+u; rt[i][1]=h*tc[i][1]+v;}
            float hf[3]={t[0]*0.5f,t[1]*0.5f,t[2]*0.5f}; float cc[3]={c[0]+hf[0],c[1]+hf[1],c[2]+hf[2]}; float b[3]={-py,px,0};
            memcpy(q.c,cc,12); memcpy(q.a,hf,12); memcpy(q.b,b,12);
        }
        for(int i=0;i<4;i++){ float p[3],t2[2]; expand(&q,i,p,t2); total[mode]++;
            if (!memcmp(p,ref[i],12) && !memcmp(t2,rt[i],8)) exact[mode]++;
            if (memcmp(t2,rt[i],8)) { printf("texcoord mismatch mode %d corner %d\n",mode,i); return 1; }
            for(int k=0;k<3;k++){ double d=fabs(p[k]-ref[i][k]); if(d>maxerr[mode]) maxerr[mode]=d; } }
    }
    const char* names[5]={"flat","billboard","velocity","spin-flat","tail"};
    for(int m=0;m<5;m++) printf("%-10s bit-exact %6.2f%%  max abs err %g\n",names[m],100.0*exact[m]/total[m],maxerr[m]);
    // flat, billboard and velocity must be bit-exact; the rest within float rounding of ~100-unit coordinates
    if (exact[0]!=total[0]||exact[1]!=total[1]||exact[2]!=total[2]) return 1;
    for(int m=3;m<5;m++) if (maxerr[m] > 1e-4) return 1;
    return 0;
}
