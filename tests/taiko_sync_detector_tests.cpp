#include "taiko_sync_detector.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
int main(int argc,char** argv) {
 TaikoSyncDetector d;
 unsigned hits=0; double worst=0;
 if(argc==2) {
  FILE* f=fopen(argv[1],"rb"); if(!f)return 2;
  float x; uint64_t i=0;
  while(fread(&x,4,1,f)==1) {
   auto hit=d.sample(x,1000000000ull+i*1000000000ull/48000);++i;
   if(hit){double t=(hit-1000000000ull)/1e9; if(hits<4)printf("peak %.6f\n",t);++hits;}
  }
  fclose(f);printf("encoded hits=%u\n",hits);return hits==180?0:1;
 }
 for(uint64_t i=0;i<4*48000;i++) {
  double t=double(i)/48000, local=t-std::floor(t)-0.5;
  float x=0.15*std::exp(-0.5*std::pow(local/0.003,2))*std::cos(2*3.141592653589793*4000*t);
  auto hit=d.sample(x,1000000000ull+i*1000000000ull/48000);
  if(hit){double error=std::abs(double(hit-1000000000ull)/1e9-(hits+0.5));worst=std::max(worst,error);hits++;}
 }
 if(hits!=4 || worst>0.0011)return 3;
 d.reset();
 for(uint64_t i=0;i<48000;i++)if(d.sample(0.2*std::sin(i*6.283185307179586*1000/48000),i*1000000000ull/48000))return 4;
 printf("synthetic hits=%u worst_error=%.3fms; 1kHz rejected\n",hits,worst*1000);
}
