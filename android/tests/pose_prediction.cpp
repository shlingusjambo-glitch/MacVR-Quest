#include "../app/src/main/cpp/PosePrediction.h"
#include <cassert>
int main() {
    PosePrediction p;
    for(int64_t t=1000000000;t<3000000000;t+=14000000) p.observe(t+60000000,t);
    assert(p.lead>59000000 && p.lead<=60000000);
    auto before=p.lead;
    p.observe(5000000000,1000000000); assert(p.lead==before); // repeated frame
    p.observe(5000000000,4000000000); assert(p.lead==before); // stall
    p.reset();assert(p.lead==40000000);
    for(int64_t t=6000000000;t<8000000000;t+=14000000) p.observe(t+200000000,t);
    assert(p.lead<=100000000 && p.lead>99000000);
    for(int64_t t=9000000000;t<11000000000;t+=14000000) p.observe(t,t);
    assert(p.lead>=0 && p.lead<1000000);
}
