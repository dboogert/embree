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

#ifndef __EMBREE_ACCEL_TRIANGLE4_INTERSECTOR1_MOELLER_H__
#define __EMBREE_ACCEL_TRIANGLE4_INTERSECTOR1_MOELLER_H__

#include "../common/default.h"
#include "triangle4.h"
#include "../common/ray.h"

namespace embree
{
  /*! Intersector for a single ray with 4 triangles. This intersector
   *  implements a modified version of the Moeller Trumbore
   *  intersector from the paper "Fast, Minimum Storage Ray-Triangle
   *  Intersection". In contrast to the paper we precalculate some
   *  factors and factor the calculations differently to allow
   *  precalculating the cross product e1 x e2. The resulting
   *  algorithm is similar to the fastest one of the paper "Optimizing
   *  Ray-Triangle Intersection via Automated Search". */
  struct Triangle4Intersector1MoellerTrumbore
  {
    typedef Triangle4 Primitive;

    /*! Intersect a ray with the 4 triangles and updates the hit. */
    static __forceinline void intersect(Ray& ray, const Triangle4& tri, void* geom)
    {
      /* calculate denominator */
      STAT3(normal.trav_prims,1,1,1);
      const sse3f O = sse3f(ray.org);
      const sse3f D = sse3f(ray.dir);
      const sse3f C = sse3f(tri.v0) - O;
      const sse3f R = cross(D,C);
      const ssef den = dot(sse3f(tri.Ng),D);
      const ssef absDen = abs(den);
      const ssef sgnDen = signmsk(den);

      /* perform edge tests */
      const ssef U = dot(R,sse3f(tri.e2)) ^ sgnDen;
      const ssef V = dot(R,sse3f(tri.e1)) ^ sgnDen;

      /* perform backface culling */
#if defined(__BACKFACE_CULLING__)
      sseb valid = (den > ssef(zero)) & (U >= 0.0f) & (V >= 0.0f) & (U+V<=absDen);
#else
      sseb valid = (den != ssef(zero)) & (U >= 0.0f) & (V >= 0.0f) & (U+V<=absDen);
#endif
      if (likely(none(valid))) return;
      
      /* perform depth test */
      const ssef T = dot(sse3f(tri.Ng),C) ^ sgnDen;
      valid &= (T > absDen*ssef(ray.tnear)) & (T < absDen*ssef(ray.tfar));
      if (likely(none(valid))) return;

      /* ray masking test */
#if defined(__USE_RAY_MASK__)
      valid &= (tri.mask & ray.mask) != 0;
      if (unlikely(none(valid))) return;
#endif

      /* update hit information */
      const ssef rcpAbsDen = rcp(absDen);
      const ssef u = U * rcpAbsDen;
      const ssef v = V * rcpAbsDen;
      const ssef t = T * rcpAbsDen;
      const size_t i = select_min(valid,t);
      ray.u   = u[i];
      ray.v   = v[i];
      ray.tfar = t[i];
      ray.Ng.x = tri.Ng.x[i];
      ray.Ng.y = tri.Ng.y[i];
      ray.Ng.z = tri.Ng.z[i];
      ray.geomID = tri.geomID[i];
      ray.primID = tri.primID[i];
    }

    static __forceinline void intersect(Ray& ray, const Triangle4* tri, size_t num, void* geom)
    {
      for (size_t i=0; i<num; i++)
        intersect(ray,tri[i],geom);
    }

    /*! Test if the ray is occluded by one of the triangles. */
    static __forceinline bool occluded(Ray& ray, const Triangle4& tri, void* geom)
    {
      /* calculate denominator */
      STAT3(shadow.trav_prims,1,1,1);
      const sse3f O = sse3f(ray.org);
      const sse3f D = sse3f(ray.dir);
      const sse3f C = sse3f(tri.v0) - O;
      const sse3f R = cross(D,C);
      const ssef den = dot(sse3f(tri.Ng),D);
      const ssef absDen = abs(den);
      const ssef sgnDen = signmsk(den);

      /* perform edge tests */
      const ssef U = dot(R,sse3f(tri.e2)) ^ sgnDen;
      const ssef V = dot(R,sse3f(tri.e1)) ^ sgnDen;
      const ssef W = absDen-U-V;
      sseb valid = (U >= 0.0f) & (V >= 0.0f) & (W >= 0.0f);
      if (unlikely(none(valid))) return false;
      
      /* perform depth test */
      const ssef T = dot(sse3f(tri.Ng),C) ^ sgnDen;
      valid &= (den != ssef(zero)) & (T >= absDen*ssef(ray.tnear)) & (absDen*ssef(ray.tfar) >= T);
      if (unlikely(none(valid))) return false;

      /* perform backface culling */
#if defined(__BACKFACE_CULLING__)
      valid &= den > ssef(zero);
      if (unlikely(none(valid))) return false;
#endif

      /* ray masking test */
#if defined(__USE_RAY_MASK__)
      valid &= (tri.mask & ray.mask) != 0;
      if (unlikely(none(valid))) return false;
#endif
      return true;
    }

    static __forceinline bool occluded(Ray& ray, const Triangle4* tri, size_t num, void* geom) 
    {
      for (size_t i=0; i<num; i++) 
        if (occluded(ray,tri[i],geom))
          return true;

      return false;
    }
  };
}

#endif


