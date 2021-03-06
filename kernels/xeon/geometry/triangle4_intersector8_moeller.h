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

#ifndef __EMBREE_ACCEL_TRIANGLE4_INTERSECTOR8_MOELLER_H__
#define __EMBREE_ACCEL_TRIANGLE4_INTERSECTOR8_MOELLER_H__

#include "triangle4.h"
#include "triangle4_intersector1_moeller.h"

#include "../common/ray8.h"

namespace embree
{
  /*! Intersector for 4 triangles with 8 rays. This intersector
   *  implements a modified version of the Moeller Trumbore
   *  intersector from the paper "Fast, Minimum Storage Ray-Triangle
   *  Intersection". In contrast to the paper we precalculate some
   *  factors and factor the calculations differently to allow
   *  precalculating the cross product e1 x e2. */
  struct Triangle4Intersector8MoellerTrumbore
  {
    typedef Triangle4 Primitive;

    /*! Intersects a 4 rays with 4 triangles. */
    static __forceinline void intersect(const avxb& valid_i, Ray8& ray, const Triangle4& tri, const void* geom)
    {
      for (size_t i=0; i<tri.size(); i++)
      {
        STAT3(normal.trav_prims,1,popcnt(valid_i),8);

        /* load edges and geometry normal */
        avxb valid = valid_i;
        const avx3f p0 = broadcast8f(tri.v0,i);
        const avx3f e1 = broadcast8f(tri.e1,i);
        const avx3f e2 = broadcast8f(tri.e2,i);
        const avx3f Ng = broadcast8f(tri.Ng,i);
        
        /* calculate denominator */
        const avx3f C = p0 - ray.org;
        const avx3f R = cross(ray.dir,C);
        const avxf den = dot(Ng,ray.dir);
        const avxf absDen = abs(den);
        const avxf sgnDen = signmsk(den);
        
        /* test against edge p2 p0 */
        const avxf U = dot(R,e2) ^ sgnDen;
        valid &= U >= 0.0f;
        if (likely(none(valid))) continue;
        
        /* test against edge p0 p1 */
        const avxf V = dot(R,e1) ^ sgnDen;
        valid &= V >= 0.0f;
        if (likely(none(valid))) continue;
        
        /* test against edge p1 p2 */
        const avxf W = absDen-U-V;
        valid &= W >= 0.0f;
        if (likely(none(valid))) continue;
        
        /* perform depth test */
        const avxf T = dot(Ng,C) ^ sgnDen;
        valid &= (T >= absDen*ray.tnear) & (absDen*ray.tfar >= T);
        if (unlikely(none(valid))) continue;

        /* perform backface culling */
#if defined(__BACKFACE_CULLING__)
        valid &= den > avxf(zero);
        if (unlikely(none(valid))) continue;
#else
        valid &= den != avxf(zero);
        if (unlikely(none(valid))) continue;
#endif
        
        /* ray masking test */
#if defined(__USE_RAY_MASK__)
        valid &= (tri.mask[i] & ray.mask) != 0;
        if (unlikely(none(valid))) continue;
#endif

        /* update hit information for all rays that hit the triangle */
        const avxf rcpAbsDen = rcp(absDen);
        store8f(valid,&ray.u,U * rcpAbsDen);
        store8f(valid,&ray.v,V * rcpAbsDen);
        store8f(valid,&ray.tfar,T * rcpAbsDen);
        store8i(valid,&ray.geomID,tri.geomID[i]);
        store8i(valid,&ray.primID,tri.primID[i]);
        store8f(valid,&ray.Ng.x,Ng.x);
        store8f(valid,&ray.Ng.y,Ng.y);
        store8f(valid,&ray.Ng.z,Ng.z);
      }
    }

    static __forceinline void intersect(const avxb& valid, Ray8& ray, const Triangle4* tri, size_t num, const void* geom)
    {
      for (size_t i=0; i<num; i++)
        intersect(valid,ray,tri[i],geom);
    }

    /*! Test for 4 rays if they are occluded by any of the 4 triangle. */
    static __forceinline avxb occluded(const avxb& valid_i, const Ray8& ray, const Triangle4& tri, const void* geom)
    {
      avxb valid0 = valid_i;

      for (size_t i=0; i<tri.size(); i++)
      {
        STAT3(shadow.trav_prims,1,popcnt(valid_i),8);

        /* load edges and geometry normal */
        avxb valid = valid0;
        const avx3f p0 = broadcast8f(tri.v0,i);
        const avx3f e1 = broadcast8f(tri.e1,i);
        const avx3f e2 = broadcast8f(tri.e2,i);
        const avx3f Ng = broadcast8f(tri.Ng,i);

        /* calculate denominator */
        const avx3f C = p0 - ray.org;
        const avx3f R = cross(ray.dir,C);
        const avxf den = dot(Ng,ray.dir);
        const avxf absDen = abs(den);
        const avxf sgnDen = signmsk(den);
        
        /* test against edge p2 p0 */
        const avxf U = dot(R,e2) ^ sgnDen;
        valid &= U >= 0.0f;
        if (likely(none(valid))) continue;
        
        /* test against edge p0 p1 */
        const avxf V = dot(R,e1) ^ sgnDen;
        valid &= V >= 0.0f;
        if (likely(none(valid))) continue;
        
        /* test against edge p1 p2 */
        const avxf W = absDen-U-V;
        valid &= W >= 0.0f;
        if (likely(none(valid))) continue;
        
        /* perform depth test */
        const avxf T = dot(Ng,C) ^ sgnDen;
        valid &= (T >= absDen*ray.tnear) & (absDen*ray.tfar >= T);
        if (unlikely(none(valid))) continue;

        /* perform backface culling */
#if defined(__BACKFACE_CULLING__)
        valid &= den > avxf(zero);
        if (unlikely(none(valid))) continue;
#else
        valid &= den != avxf(zero);
        if (unlikely(none(valid))) continue;
#endif

        /* ray masking test */
#if defined(__USE_RAY_MASK__)
        valid &= (tri.mask[i] & ray.mask) != 0;
        if (unlikely(none(valid))) continue;
#endif

        /* update occlusion */
        valid0 &= !valid;
        if (none(valid0)) break;
      }
      return !valid0;
    }

    static __forceinline avxb occluded(const avxb& valid, const Ray8& ray, const Triangle4* tri, size_t num, const void* geom)
    {
      avxb valid0 = valid;
      for (size_t i=0; i<num; i++) {
        valid0 &= !occluded(valid0,ray,tri[i],geom);
        if (none(valid0)) break;
      }
      return !valid0;
    }

    /*! Intersect a ray with the 4 triangles and updates the hit. */
    static __forceinline void intersect(Ray8& ray, size_t k, const Triangle4& tri, void* geom)
    {
      /* calculate denominator */
      STAT3(normal.trav_prims,1,1,1);
      const sse3f O = broadcast4f(ray.org,k);
      const sse3f D = broadcast4f(ray.dir,k);
      const sse3f C = sse3f(tri.v0) - O;
      const sse3f R = cross(D,C);
      const ssef den = dot(sse3f(tri.Ng),D);
      const ssef absDen = abs(den);
      const ssef sgnDen = signmsk(den);

      /* perform edge tests */
      const ssef U = dot(R,sse3f(tri.e2)) ^ sgnDen;
      const ssef V = dot(R,sse3f(tri.e1)) ^ sgnDen;
      sseb valid = (U >= 0.0f) & (V >= 0.0f) & (U+V<=absDen);
      if (likely(none(valid))) return;
      
      /* perform depth test */
      const ssef T = dot(sse3f(tri.Ng),C) ^ sgnDen;
      valid &= (T > absDen*ssef(ray.tnear[k])) & (T < absDen*ssef(ray.tfar[k]));
      if (likely(none(valid))) return;

        /* perform backface culling */
#if defined(__BACKFACE_CULLING__)
      valid &= den > ssef(zero);
      if (unlikely(none(valid))) return;
#else
      valid &= den != ssef(zero);
      if (unlikely(none(valid))) return;
#endif

      /* ray masking test */
#if defined(__USE_RAY_MASK__)
      valid &= (tri.mask & ray.mask[k]) != 0;
      if (unlikely(none(valid))) return;
#endif

      /* update hit information */
      const ssef rcpAbsDen = rcp(absDen);
      const ssef u = U * rcpAbsDen;
      const ssef v = V * rcpAbsDen;
      const ssef t = T * rcpAbsDen;
      const size_t i = select_min(valid,t);
      ray.u[k]   = u[i];
      ray.v[k]   = v[i];
      ray.tfar[k] = t[i];
      ray.Ng.x[k] = tri.Ng.x[i];
      ray.Ng.y[k] = tri.Ng.y[i];
      ray.Ng.z[k] = tri.Ng.z[i];
      ray.geomID[k] = tri.geomID[i];
      ray.primID[k] = tri.primID[i];
    }

    static __forceinline void intersect(Ray8& ray, size_t k, const Triangle4* tri, size_t num, void* geom)
    {
      for (size_t i=0; i<num; i++)
        intersect(ray,k,tri[i],geom);
    }

    /*! Test if the ray is occluded by one of the triangles. */
    static __forceinline bool occluded(Ray8& ray, size_t k, const Triangle4& tri, void* geom)
    {
      /* calculate denominator */
      STAT3(shadow.trav_prims,1,1,1);
      const sse3f O = broadcast4f(ray.org,k);
      const sse3f D = broadcast4f(ray.dir,k);
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
      valid &= (T >= absDen*ssef(ray.tnear[k])) & (absDen*ssef(ray.tfar[k]) >= T);
      if (unlikely(none(valid))) return false;

        /* perform backface culling */
#if defined(__BACKFACE_CULLING__)
        valid &= den > ssef(zero);
        if (unlikely(none(valid))) return false;
#else
        valid &= den != ssef(zero);
        if (unlikely(none(valid))) return false;
#endif

      /* ray masking test */
#if defined(__USE_RAY_MASK__)
      valid &= (tri.mask & ray.mask[k]) != 0;
      if (unlikely(none(valid))) return false;
#endif
      return true;
    }

    static __forceinline bool occluded(Ray8& ray, size_t k, const Triangle4* tri, size_t num, void* geom) 
    {
      for (size_t i=0; i<num; i++) 
        if (occluded(ray,k,tri[i],geom))
          return true;

      return false;
    }
  };
}

#endif


