// Test dekodera na PC:
//   g++ -std=c++17 -I components/parkside_bus -o /tmp/test_decoder tests/test_decoder.cpp && /tmp/test_decoder tests/captured_frames.txt
// Format captured_frames.txt: "oczekiwana ramka hex|odcinki w us" (z logu parkside_bus).
#include "frame_decoder.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace esphome::parkside_bus;
static std::string hex(const DecodeResult &r){ if(r.error!=DecodeError::NONE) return std::string("ERR:")+decode_error_str(r.error); std::string s; char b[8]; for(int i=0;i<r.nbytes;i++){snprintf(b,8,i?" %02X":"%02X",r.bytes[i]); s+=b;} return s; }
int main(int argc, char **argv){
  FILE*f=fopen(argc>1?argv[1]:"tests/captured_frames.txt","r"); if(!f){perror("captured_frames"); return 2;} char line[1024]; int ok=0,bad=0,okn=0; float tmin=1e9,tmax=0;
  while(fgets(line,sizeof line,f)){ char*bar=strchr(line,'|'); *bar=0; std::string exp=line; std::vector<uint32_t> r; for(char*p=strtok(bar+1,",\n");p;p=strtok(nullptr,",\n")) r.push_back(atoi(p));
    auto a=decode_frame(r.data(),r.size(),0,1000,40,true); auto n=decode_frame(r.data(),r.size(),0,1000,40,false);
    if(hex(a)==exp) ok++; else {bad++; printf("MISMATCH exp=%s got=%s\n",exp.c_str(),hex(a).c_str());}
    if(hex(n)==exp) okn++;
    if(a.bit_time_us<tmin)tmin=a.bit_time_us; if(a.bit_time_us>tmax)tmax=a.bit_time_us; }
  printf("real frames: auto OK %d / %d, nominal-only OK %d; estimated bit time %.1f..%.1f us\n",ok,ok+bad,okn,tmin,tmax);
  int rc = bad ? 1 : 0;
  // synthetic: per battery a stream = 5 normal frames (calibration) then mixed frames incl. long runs
  std::vector<std::vector<uint8_t>> F={{0,0x80,0,0x01,0xFF},{0,0x88,0x33,0x44,0x77},{0,0x00,0x01,0xFF,0xFE},{0,0x88,0xCC,0x06,0x3C},{0,0xF0,0x0F,0x00,0x81}};
  auto mk=[&](const std::vector<uint8_t>&fr,float clk,int jit){ std::vector<int> bits; for(auto x:fr) for(int i=7;i>=0;i--) bits.push_back((x>>i)&1);
      std::vector<uint32_t> edges; int lv=1; double t=0; for(size_t i=0;i<bits.size();i++){ if(bits[i]!=lv){ edges.push_back((uint32_t)(t+(jit?(rand()%(2*jit+1)-jit):0)+100000)); lv=bits[i]; } t+=1000*clk; } if(lv==0) edges.push_back((uint32_t)(t+100000));
      std::vector<uint32_t> runs; for(size_t i=1;i<edges.size();i++) runs.push_back(edges[i]-edges[i-1]); return runs; };
  for(int jit: {50,150}) for(float clk: {0.92f,0.955f,0.96f,1.0f,1.005f,1.04f,1.08f}){ int s_ok=0,f_ok=0,n_ok=0,tot=0; srand(7);
    for(int bat=0;bat<40;bat++){ float est=0; for(int k=0;k<5;k++){ auto r=mk(F[1],clk,jit); auto d=decode_frame(r.data(),r.size(),est,1000,40,true); if(d.error==DecodeError::NONE) est= est>0?0.8f*est+0.2f*d.bit_time_us:d.bit_time_us; }
      for(int rep=0;rep<5;rep++) for(auto &fr: F){ auto r=mk(fr,clk,jit); char e[32]; snprintf(e,32,"%02X %02X %02X %02X %02X",fr[0],fr[1],fr[2],fr[3],fr[4]);
        auto d=decode_frame(r.data(),r.size(),est,1000,40,true); s_ok+=hex(d)==e; if(d.error==DecodeError::NONE) est=0.8f*est+0.2f*d.bit_time_us;
        f_ok+=hex(decode_frame(r.data(),r.size(),0,1000,40,true))==e; n_ok+=hex(decode_frame(r.data(),r.size(),0,1000,40,false))==e; tot++; } }
    printf("jit+-%3dus clk=%.3f: stream(auto+memory) %3d/%d  single(auto) %3d/%d  nominal %3d/%d\n",jit,clk,s_ok,tot,f_ok,tot,n_ok,tot); }
  return rc;
}
