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

#ifndef __EMBREE_ACCEL_TRIANGLE4V_INTERSECTOR4_PLUECKER_H__
#define __EMBREE_ACCEL_TRIANGLE4V_INTERSECTOR4_PLUECKER_H__

#include "triangle4v.h"
#include "../common/ray4.h"

namespace embree
{
  struct Triangle4vIntersector4Pluecker
  {
    typedef Triangle4v Primitive;

    /*! Intersects a 4 rays with 4 triangles. */
    static __forceinline void intersect(const sseb& valid_i, Ray4& ray, const Triangle4v& tri, void* geom)
    {
      for (size_t i=0; i<tri.size(); i++)
      {
        STAT3(normal.trav_prims,1,popcnt(valid_i),4);

        /* calculate vertices relative to ray origin */
        sseb valid = valid_i;
        const sse3f O = ray.org;
        const sse3f D = ray.dir;
        const sse3f v0 = broadcast4f(tri.v0,i)-O;
        const sse3f v1 = broadcast4f(tri.v1,i)-O;
        const sse3f v2 = broadcast4f(tri.v2,i)-O;
        
        /* calculate triangle edges */
        const sse3f e0 = v2-v0;
        const sse3f e1 = v0-v1;
        const sse3f e2 = v1-v2;
        
        /* calculate geometry normal and denominator */
        const sse3f Ng = cross(e1,e0);
        const sse3f Ng2 = Ng+Ng;
        const ssef den = dot(sse3f(Ng2),D);
        const ssef absDen = abs(den);
        const ssef sgnDen = signmsk(den);
        
        /* perform edge tests */
        const ssef U = dot(sse3f(cross(v2+v0,e0)),D) ^ sgnDen;
        valid &= U >= 0.0f;
        if (likely(none(valid))) continue;
        const ssef V = dot(sse3f(cross(v0+v1,e1)),D) ^ sgnDen;
        valid &= V >= 0.0f;
        if (likely(none(valid))) continue;
        const ssef W = dot(sse3f(cross(v1+v2,e2)),D) ^ sgnDen;
        valid &= W >= 0.0f;
        if (likely(none(valid))) continue;
        
        /* perform depth test */
        const ssef T = dot(v0,sse3f(Ng2)) ^ sgnDen;
        valid &= (T >= absDen*ray.tnear) & (absDen*ray.tfar >= T);
        if (unlikely(none(valid))) continue;

        /* perform backface culling */
#if defined(__BACKFACE_CULLING__)
        valid &= den > ssef(zero);
        if (unlikely(none(valid))) continue;
#else
        valid &= den != ssef(zero);
        if (unlikely(none(valid))) continue;
#endif

        /* ray masking test */
#if defined(__USE_RAY_MASK__)
        valid &= (tri.mask[i] & ray.mask) != 0;
        if (unlikely(none(valid))) continue;
#endif
        
        /* update hit information for all rays that hit the triangle */
        ray.u   = select(valid,U / absDen,ray.u );
        ray.v   = select(valid,V / absDen,ray.v );
        ray.tfar = select(valid,T / absDen,ray.tfar );
        ray.geomID = select(valid,tri.geomID[i],ray.geomID);
        ray.primID = select(valid,tri.primID[i],ray.primID);
        ray.Ng.x = select(valid,Ng2.x,ray.Ng.x);
        ray.Ng.y = select(valid,Ng2.y,ray.Ng.y);
        ray.Ng.z = select(valid,Ng2.z,ray.Ng.z);
      }
    }

    static __forceinline void intersect(const sseb& valid, Ray4& ray, const Triangle4v* tri, size_t num, void* geom)
    {
      for (size_t i=0; i<num; i++) {
        intersect(valid,ray,tri[i],geom);
      }
    }

    /*! Test for 4 rays if they are occluded by any of the 4 triangle. */
    static __forceinline sseb occluded(const sseb& valid_i, const Ray4& ray, const Triangle4v& tri, void* geom)
    {
      sseb valid0 = valid_i;

      for (size_t i=0; i<tri.size(); i++)
      {
        STAT3(shadow.trav_prims,1,popcnt(valid_i),4);

        /* calculate vertices relative to ray origin */
        sseb valid = valid0;
        const sse3f O = ray.org;
        const sse3f D = ray.dir;
        const sse3f v0 = broadcast4f(tri.v0,i)-O;
        const sse3f v1 = broadcast4f(tri.v1,i)-O;
        const sse3f v2 = broadcast4f(tri.v2,i)-O;

        /* calculate triangle edges */
        const sse3f e0 = v2-v0;
        const sse3f e1 = v0-v1;
        const sse3f e2 = v1-v2;
        
        /* calculate geometry normal and denominator */
        const sse3f Ng = cross(e1,e0);
        const sse3f Ng2 = Ng+Ng;
        const ssef den = dot(sse3f(Ng2),D);
        const ssef absDen = abs(den);
        const ssef sgnDen = signmsk(den);
        
        /* perform edge tests */
        const ssef U = dot(sse3f(cross(v2+v0,e0)),D) ^ sgnDen;
        valid &= U >= 0.0f;
        if (likely(none(valid))) continue;
        const ssef V = dot(sse3f(cross(v0+v1,e1)),D) ^ sgnDen;
        valid &= V >= 0.0f;
        if (likely(none(valid))) continue;
        const ssef W = dot(sse3f(cross(v1+v2,e2)),D) ^ sgnDen;
        valid &= W >= 0.0f;
        if (likely(none(valid))) continue;
        
        /* perform depth test */
        const ssef T = dot(v0,sse3f(Ng2)) ^ sgnDen;
        valid &= (T >= absDen*ray.tnear) & (absDen*ray.tfar >= T);

        /* perform backface culling */
#if defined(__BACKFACE_CULLING__)
        valid &= den > ssef(zero);
        if (unlikely(none(valid))) continue;
#else
        valid &= den != ssef(zero);
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

    static __forceinline sseb occluded(const sseb& valid, const Ray4& ray, const Triangle4v* tri, size_t num, void* geom)
    {
      sseb valid0 = valid;
      for (size_t i=0; i<num; i++) {
        valid0 &= !occluded(valid0,ray,tri[i],geom);
        if (none(valid0)) break;
      }
      return !valid0;
    }

    /*! Intersect a ray with the 4 triangles and updates the hit. */
    static __forceinline void intersect(Ray4& ray, size_t k, const Triangle4v& tri, void* geom)
    {
      /* calculate vertices relative to ray origin */
      STAT3(normal.trav_prims,1,1,1);
      const sse3f O = broadcast4f(ray.org,k);
      const sse3f D = broadcast4f(ray.dir,k);
      const sse3f v0 = tri.v0-O;
      const sse3f v1 = tri.v1-O;
      const sse3f v2 = tri.v2-O;

      /* calculate triangle edges */
      const sse3f e0 = v2-v0;
      const sse3f e1 = v0-v1;
      const sse3f e2 = v1-v2;

      /* calculate geometry normal and denominator */
      const sse3f Ng = cross(e1,e0);
      const sse3f Ng2 = Ng+Ng;
      const ssef den = dot(Ng2,D);
      const ssef absDen = abs(den);
      const ssef sgnDen = signmsk(den);

      /* perform edge tests */
      const ssef U = dot(cross(v2+v0,e0),D) ^ sgnDen;
      const ssef V = dot(cross(v0+v1,e1),D) ^ sgnDen;
      const ssef W = dot(cross(v1+v2,e2),D) ^ sgnDen;
      sseb valid = (U >= 0.0f) & (V >= 0.0f) & (W >= 0.0f);
      if (unlikely(none(valid))) return;

      /* perform depth test */
      const ssef T = dot(v0,Ng2) ^ sgnDen;
      valid &= (T >= absDen*ssef(ray.tnear[k])) & (absDen*ssef(ray.tfar[k]) >= T);
      if (unlikely(none(valid))) return;

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
      const ssef u = U / absDen;
      const ssef v = V / absDen;
      const ssef t = T / absDen;
      const size_t i = select_min(valid,t);
      ray.tfar[k] = t[i];
      ray.u[k] = u[i];
      ray.v[k] = v[i];
      ray.Ng.x[k] = Ng2.x[i];
      ray.Ng.y[k] = Ng2.y[i];
      ray.Ng.z[k] = Ng2.z[i];
      ray.geomID[k] = tri.geomID[i];
      ray.primID[k] = tri.primID[i];
    }

    static __forceinline void intersect(Ray4& ray, size_t k, const Triangle4v* tri, size_t num, void* geom)
    {
      for (size_t i=0; i<num; i++)
        intersect(ray,k,tri[i],geom);
    }

    /*! Test if the ray is occluded by one of the triangles. */
    static __forceinline bool occluded(Ray4& ray, size_t k, const Triangle4v& tri, void* geom)
    {
      /* calculate vertices relative to ray origin */
      STAT3(shadow.trav_prims,1,1,1);
      const sse3f O = broadcast4f(ray.org,k);
      const sse3f D = broadcast4f(ray.dir,k);
      const sse3f v0 = tri.v0-O;
      const sse3f v1 = tri.v1-O;
      const sse3f v2 = tri.v2-O;

      /* calculate triangle edges */
      const sse3f e0 = v2-v0;
      const sse3f e1 = v0-v1;
      const sse3f e2 = v1-v2;

      /* calculate geometry normal and denominator */
      const sse3f Ng = cross(e1,e0);
      const sse3f Ng2 = Ng+Ng;
      const ssef den = dot(Ng2,D);
      const ssef absDen = abs(den);
      const ssef sgnDen = signmsk(den);

      /* perform edge tests */
      const ssef U = dot(cross(v2+v0,e0),D) ^ sgnDen;
      const ssef V = dot(cross(v0+v1,e1),D) ^ sgnDen;
      const ssef W = dot(cross(v1+v2,e2),D) ^ sgnDen;
      sseb valid = (U >= 0.0f) & (V >= 0.0f) & (W >= 0.0f);
      if (unlikely(none(valid))) return false;
      
      /* perform depth test */
      const ssef T = dot(v0,Ng2) ^ sgnDen;
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

    static __forceinline bool occluded(Ray4& ray, size_t k, const Triangle4v* tri, size_t num, void* geom) 
    {
      for (size_t i=0; i<num; i++) 
        if (occluded(ray,k,tri[i],geom))
          return true;

      return false;
    }
  };
}

#endif


