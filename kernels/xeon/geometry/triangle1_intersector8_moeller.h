// ======================================================================== //
// Copyright 2009-2013 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#ifndef __EMBREE_ACCEL_TRIANGLE1_INTERSECTOR8_MOELLER_H__
#define __EMBREE_ACCEL_TRIANGLE1_INTERSECTOR8_MOELLER_H__

#include "triangle1.h"
#include "../common/ray8.h"

namespace embree
{
  /*! Intersector for individual precomputed triangles with 8
   *  rays. This intersector implements a modified version of the
   *  Moeller Trumbore intersector from the paper "Fast, Minimum
   *  Storage Ray-Triangle Intersection". In contrast to the paper we
   *  precalculate some factors and factor the calculations
   *  differently to allow precalculating the cross product e1 x
   *  e2. */
  struct Triangle1Intersector8MoellerTrumbore
  {
    typedef Triangle1 Primitive;

    static __forceinline void intersect(const avxb& valid_i, Ray8& ray, const Triangle1* __restrict__ tris, size_t num, const void* geom)
    {
      for (size_t i=0; i<num; i++) 
      {
        STAT3(normal.trav_prims,1,popcnt(valid_i),8);

        avxb valid = valid_i;
        const avx3f org = ray.org;
        const avx3f dir = ray.dir;
        const Triangle1& tri = tris[i];
        
        /* load vertices and calculate edges */
        const avxf v0 = broadcast4f(&tri.v0);
        const avxf v1 = broadcast4f(&tri.v1);
        const avxf v2 = broadcast4f(&tri.v2);
        const avxf e1 = v0-v1;
        const avxf e2 = v2-v0;

        /* calculate denominator */
        const avx3f _v0 = avx3f(shuffle<0>(v0),shuffle<1>(v0),shuffle<2>(v0));
        const avx3f C =  _v0 - org;
        const avx3f Ng = avx3f(tri.Ng);
        const avxf den = dot(dir,Ng);
        const avxf sgnDen = signmsk(den);
        const avxf absDen = abs(den);
#if defined(__BACKFACE_CULLING__)
        valid &= den > avxf(zero);
#else
        valid &= den != avxf(zero);
#endif
        
        /* perform edge tests */
        const avx3f R = cross(dir,C);
        const avx3f _e1(shuffle<0>(e1),shuffle<1>(e1),shuffle<2>(e1));
        const avxf V = dot(R,_e1)^sgnDen;
        const avx3f _e2(shuffle<0>(e2),shuffle<1>(e2),shuffle<2>(e2));
        const avxf U = dot(R,_e2)^sgnDen;
        valid &= V >= avxf(zero) & U >= avxf(zero) & U+V <= absDen;
        if (unlikely(none(valid))) continue;
      
        /* perform depth test */
        const avxf T = dot(C,Ng) ^ sgnDen;
        valid &= (T >= absDen*ray.tnear) & (absDen*ray.tfar >= T);
        if (unlikely(none(valid))) continue;
        
        /* ray masking test */
#if defined(__USE_RAY_MASK__)
        valid &= (tri.mask() & ray.mask) != 0;
        if (unlikely(none(valid))) continue;
#endif

        /* update hit information */
        const avxf rcpAbsDen = rcp(absDen);
        store8f(valid,(float*)&ray.u,U*rcpAbsDen);
        store8f(valid,(float*)&ray.v,V*rcpAbsDen);
        store8f(valid,(float*)&ray.tfar,T*rcpAbsDen);
        store8i(valid,(float*)&ray.geomID,tri.geomID());
        store8i(valid,(float*)&ray.primID,tri.primID());
        store8f(valid,(float*)&ray.Ng.x,Ng.x);
        store8f(valid,(float*)&ray.Ng.y,Ng.y);
        store8f(valid,(float*)&ray.Ng.z,Ng.z);
      }
    }

    static __forceinline avxb occluded(const avxb& valid_i, Ray8& ray, const Triangle1* __restrict__ tris, size_t num, const void* geom)
    {
      avxb valid0 = valid_i;
     
      for (size_t i=0; i<num; i++) 
      {
        STAT3(shadow.trav_prims,1,popcnt(valid0),8);

        avxb valid = valid0;
        const avx3f org = ray.org;
        const avx3f dir = ray.dir;
        const Triangle1& tri = tris[i];
        
        /* load vertices and calculate edges */
        const avxf v0 = broadcast4f(&tri.v0);
        const avxf v1 = broadcast4f(&tri.v1);
        const avxf v2 = broadcast4f(&tri.v2);
        const avxf e1 = v0-v1;
        const avxf e2 = v2-v0;
        
        /* calculate denominator */
        const avx3f _v0 = avx3f(shuffle<0>(v0),shuffle<1>(v0),shuffle<2>(v0));
        const avx3f C =  _v0 - org;
        const avxf Ng = broadcast4f(&tri.Ng);
        const avx3f _Ng = avx3f(shuffle<0>(Ng),shuffle<1>(Ng),shuffle<2>(Ng));
        const avxf den = dot(dir,_Ng);
        const avxf sgnDen = signmsk(den);
        const avxf absDen = abs(den);
#if defined(__BACKFACE_CULLING__)
        valid &= den > avxf(zero);
#else
        valid &= den != avxf(zero);
#endif

        /* perform edge tests */
        const avx3f R = cross(dir,C);
        const avx3f _e1(shuffle<0>(e1),shuffle<1>(e1),shuffle<2>(e1));
        const avxf V = dot(R,_e1)^sgnDen;
        const avx3f _e2(shuffle<0>(e2),shuffle<1>(e2),shuffle<2>(e2));
        const avxf U = dot(R,_e2)^sgnDen;
        valid &= V >= avxf(zero) & U >= avxf(zero) & U+V <= absDen;
        if (unlikely(none(valid))) continue;
      
        /* perform depth test */
        const avxf T = dot(C,_Ng) ^ sgnDen;
        valid &= (T >= absDen*ray.tnear) & (absDen*ray.tfar >= T);
        if (unlikely(none(valid))) continue;
        
        /* ray masking test */
#if defined(__USE_RAY_MASK__)
        valid &= (tri.mask() & ray.mask) != 0;
        if (unlikely(none(valid))) continue;
#endif

        /* update occlusion */
        valid0 &= !valid;
        if (none(valid0)) break;
      }
      return !valid0;
    }
  };
}

#endif


