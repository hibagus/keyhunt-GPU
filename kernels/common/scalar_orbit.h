#pragma once
#include "glv.h"

namespace keyhunt::gpu {
// The selected variant is uniform across a batch. Work planning stops at each
// variant boundary, so each lane still uses the existing checked seed stride.
KEYHUNT_HD inline bool point_orbit(Point& out,const Point& point,unsigned variant) {
    if(variant>=6 || is_infinity(point)){out=Point{};return false;}
    Point result=point;
    for(unsigned power=0;power<variant/2;++power)point_endomorphism(result,result);
    if(variant%2)neg(result.y,result.y);
    out=result;return true;
}
} // namespace keyhunt::gpu
