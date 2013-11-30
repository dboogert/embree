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

#include "bvh4_intersector4_hybrid.h"
#include "common/registry_intersector.h"

#include "geometry/triangle4_intersector4_moeller.h"
#if defined (__AVX__)
#include "geometry/triangle8_intersector4_moeller.h"
#endif
#include "geometry/triangle4v_intersector4_pluecker.h"

#define SWITCH_THRESHOLD 3

namespace embree
{
  namespace isa
  {
    template<typename PrimitiveIntersector4>
    __forceinline void BVH4Intersector4Hybrid<PrimitiveIntersector4>::intersect1(const BVH4* bvh, NodeRef root, size_t k, Ray4& ray, 
                                                                                 const sse3f& ray_org, const sse3f& ray_dir, const sse3f& ray_rdir, 
                                                                                 const ssef& ray_tnear, const ssef& ray_tfar)
    {
      /*! stack state */
      StackItem stack[stackSizeSingle];  //!< stack of nodes 
      StackItem* stackPtr = stack+1;        //!< current stack pointer
      StackItem* stackEnd = stack+stackSizeSingle;
      stack[0].ptr = root;
      stack[0].dist = neg_inf;
      
      /*! offsets to select the side that becomes the lower or upper bound */
      const size_t nearX = ray_dir.x[k] >= 0.0f ? 0*sizeof(ssef) : 1*sizeof(ssef);
      const size_t nearY = ray_dir.y[k] >= 0.0f ? 2*sizeof(ssef) : 3*sizeof(ssef);
      const size_t nearZ = ray_dir.z[k] >= 0.0f ? 4*sizeof(ssef) : 5*sizeof(ssef);
      
      /*! load the ray into SIMD registers */
      const sse3f org (ray_org .x[k],ray_org .y[k],ray_org .z[k]);
      const sse3f rdir(ray_rdir.x[k],ray_rdir.y[k],ray_rdir.z[k]);
      const sse3f norg = -org, org_rdir(org*rdir);
      ssef rayNear(ray_tnear[k]), rayFar(ray_tfar[k]); 
      
      /* pop loop */
      while (true) pop:
      {
        /*! pop next node */
        if (unlikely(stackPtr == stack)) break;
        stackPtr--;
        NodeRef cur = NodeRef(stackPtr->ptr);
        
        /*! if popped node is too far, pop next one */
        if (unlikely(stackPtr->dist > ray.tfar[k]))
          continue;
        
        /* downtraversal loop */
        while (true)
        {
          /*! stop if we found a leaf */
          if (unlikely(cur.isLeaf())) break;
          STAT3(normal.trav_nodes,1,1,1);
          
          /*! single ray intersection with 4 boxes */
          const Node* node = cur.node();
          const size_t farX  = nearX ^ 16, farY  = nearY ^ 16, farZ  = nearZ ^ 16;
#if defined (__AVX2__)
          const ssef tNearX = msub(load4f((const char*)node+nearX), rdir.x, org_rdir.x);
          const ssef tNearY = msub(load4f((const char*)node+nearY), rdir.y, org_rdir.y);
          const ssef tNearZ = msub(load4f((const char*)node+nearZ), rdir.z, org_rdir.z);
          const ssef tFarX  = msub(load4f((const char*)node+farX ), rdir.x, org_rdir.x);
          const ssef tFarY  = msub(load4f((const char*)node+farY ), rdir.y, org_rdir.y);
          const ssef tFarZ  = msub(load4f((const char*)node+farZ ), rdir.z, org_rdir.z);
#else
          const ssef tNearX = (norg.x + load4f((const char*)node+nearX)) * rdir.x;
          const ssef tNearY = (norg.y + load4f((const char*)node+nearY)) * rdir.y;
          const ssef tNearZ = (norg.z + load4f((const char*)node+nearZ)) * rdir.z;
          const ssef tFarX  = (norg.x + load4f((const char*)node+farX )) * rdir.x;
          const ssef tFarY  = (norg.y + load4f((const char*)node+farY )) * rdir.y;
          const ssef tFarZ  = (norg.z + load4f((const char*)node+farZ )) * rdir.z;
#endif

#if defined(__SSE4_1__)
          const ssef tNear = maxi(maxi(tNearX,tNearY),maxi(tNearZ,rayNear));
          const ssef tFar  = mini(mini(tFarX ,tFarY ),mini(tFarZ ,rayFar ));
          const sseb vmask = cast(tNear) > cast(tFar);
          size_t mask = movemask(vmask)^0xf;
#else
          const ssef tNear = max(tNearX,tNearY,tNearZ,rayNear);
          const ssef tFar  = min(tFarX ,tFarY ,tFarZ ,rayFar);
          const sseb vmask = tNear <= tFar;
          size_t mask = movemask(vmask);
#endif
          
          /*! if no child is hit, pop next node */
          if (unlikely(mask == 0))
            goto pop;
          
          /*! one child is hit, continue with that child */
          size_t r = bitscan(mask); mask = __btc(mask,r);
          if (likely(mask == 0)) {
            cur = node->child(r);
            continue;
          }
          
          /*! two children are hit, push far child, and continue with closer child */
          NodeRef c0 = node->child(r); const float d0 = tNear[r];
          r = bitscan(mask); mask = __btc(mask,r);
          NodeRef c1 = node->child(r); const float d1 = tNear[r];
          if (likely(mask == 0)) {
            assert(stackPtr < stackEnd); 
            if (d0 < d1) { stackPtr->ptr = c1; stackPtr->dist = d1; stackPtr++; cur = c0; continue; }
            else         { stackPtr->ptr = c0; stackPtr->dist = d0; stackPtr++; cur = c1; continue; }
          }
          
          /*! Here starts the slow path for 3 or 4 hit children. We push
           *  all nodes onto the stack to sort them there. */
          assert(stackPtr < stackEnd); 
          stackPtr->ptr = c0; stackPtr->dist = d0; stackPtr++;
          assert(stackPtr < stackEnd); 
          stackPtr->ptr = c1; stackPtr->dist = d1; stackPtr++;
          
          /*! three children are hit, push all onto stack and sort 3 stack items, continue with closest child */
          assert(stackPtr < stackEnd); 
          r = bitscan(mask); mask = __btc(mask,r);
          NodeRef c = node->child(r); float d = tNear[r]; stackPtr->ptr = c; stackPtr->dist = d; stackPtr++;
          if (likely(mask == 0)) {
            sort(stackPtr[-1],stackPtr[-2],stackPtr[-3]);
            cur = (NodeRef) stackPtr[-1].ptr; stackPtr--;
            continue;
          }
          
          /*! four children are hit, push all onto stack and sort 4 stack items, continue with closest child */
          assert(stackPtr < stackEnd); 
          r = bitscan(mask); mask = __btc(mask,r);
          c = node->child(r); d = tNear[r]; stackPtr->ptr = c; stackPtr->dist = d; stackPtr++;
          sort(stackPtr[-1],stackPtr[-2],stackPtr[-3],stackPtr[-4]);
          cur = (NodeRef) stackPtr[-1].ptr; stackPtr--;
        }
        
        /*! this is a leaf node */
        STAT3(normal.trav_leaves,1,1,1);
        size_t num; Primitive* prim = (Primitive*) cur.leaf(num);
        PrimitiveIntersector4::intersect(ray,k,prim,num,bvh->geometry);
        rayFar = ray.tfar[k];
      }
    }
    
    template<typename PrimitiveIntersector4>
    void BVH4Intersector4Hybrid<PrimitiveIntersector4>::intersect(sseb* valid_i, BVH4* bvh, Ray4& ray)
    {
      /* load ray */
      const sseb valid0 = *valid_i;
      sse3f ray_org = ray.org, ray_dir = ray.dir;
      ssef ray_tnear = ray.tnear, ray_tfar  = ray.tfar;
#if defined(__FIX_RAYS__)
      const ssef float_range = 0.1f*FLT_MAX;
      ray_org = clamp(ray_org,sse3f(-float_range),sse3f(+float_range));
      ray_dir = clamp(ray_dir,sse3f(-float_range),sse3f(+float_range));
      ray_tnear = max(ray_tnear,FLT_MIN); 
      ray_tfar  = min(ray_tfar,float(inf)); 
#endif
      const sse3f rdir = rcp_safe(ray_dir);
      const sse3f org(ray_org), org_rdir = org * rdir;
      ray_tnear = select(valid0,ray_tnear,ssef(pos_inf));
      ray_tfar  = select(valid0,ray_tfar ,ssef(neg_inf));
      const ssef inf = ssef(pos_inf);
      
      /* allocate stack and push root node */
      ssef    stack_near[stackSizeChunk];
      NodeRef stack_node[stackSizeChunk];
      stack_node[0] = BVH4::invalidNode;
      stack_near[0] = inf;
      stack_node[1] = bvh->root;
      stack_near[1] = ray_tnear; 
      NodeRef* stackEnd = stack_node+stackSizeChunk;
      NodeRef* __restrict__ sptr_node = stack_node + 2;
      ssef*    __restrict__ sptr_near = stack_near + 2;
      
      while (1)
      {
        /* pop next node from stack */
        assert(sptr_node > stack_node);
        sptr_node--;
        sptr_near--;
        NodeRef curNode = *sptr_node;
        if (unlikely(curNode == BVH4::invalidNode)) {
          assert(sptr_node == stack_node);
          break;
        }
        
        /* cull node if behind closest hit point */
        ssef curDist = *sptr_near;
        const sseb active = curDist < ray_tfar;
        if (unlikely(none(active))) 
          continue;
        
        /* switch to single ray traversal */
        size_t bits = movemask(active);
        if (unlikely(__popcnt(bits) <= SWITCH_THRESHOLD)) {
          for (size_t i=__bsf(bits); bits!=0; bits=__btc(bits,i), i=__bsf(bits)) {
            intersect1(bvh,curNode,i,ray,ray_org,ray_dir,rdir,ray_tnear,ray_tfar);
          }
          ray_tfar = ray.tfar;
          continue;
        }
        
        while (1)
        {
          /* test if this is a leaf node */
          if (unlikely(curNode.isLeaf()))
            break;
          
          const sseb valid_node = ray_tfar > curDist;
          STAT3(normal.trav_nodes,1,popcnt(valid_node),4);
          const Node* __restrict__ const node = curNode.node();
          
          /* pop of next node */
          assert(sptr_node > stack_node);
          sptr_node--;
          sptr_near--;
          curNode = *sptr_node; 
          curDist = *sptr_near;
          
#pragma unroll(4)
          for (unsigned i=0; i<4; i++)
          {
            const NodeRef child = node->children[i];
            if (unlikely(child == BVH4::emptyNode)) break;
            
#if defined(__AVX2__)
            const ssef lclipMinX = msub(node->lower_x[i],rdir.x,org_rdir.x);
            const ssef lclipMinY = msub(node->lower_y[i],rdir.y,org_rdir.y);
            const ssef lclipMinZ = msub(node->lower_z[i],rdir.z,org_rdir.z);
            const ssef lclipMaxX = msub(node->upper_x[i],rdir.x,org_rdir.x);
            const ssef lclipMaxY = msub(node->upper_y[i],rdir.y,org_rdir.y);
            const ssef lclipMaxZ = msub(node->upper_z[i],rdir.z,org_rdir.z);
            const ssef lnearP = maxi(maxi(mini(lclipMinX, lclipMaxX), mini(lclipMinY, lclipMaxY)), mini(lclipMinZ, lclipMaxZ));
            const ssef lfarP  = mini(mini(maxi(lclipMinX, lclipMaxX), maxi(lclipMinY, lclipMaxY)), maxi(lclipMinZ, lclipMaxZ));
            const sseb lhit   = maxi(lnearP,ray_tnear) <= mini(lfarP,ray_tfar);      
#else
            const ssef lclipMinX = (node->lower_x[i] - org.x) * rdir.x;
            const ssef lclipMinY = (node->lower_y[i] - org.y) * rdir.y;
            const ssef lclipMinZ = (node->lower_z[i] - org.z) * rdir.z;
            const ssef lclipMaxX = (node->upper_x[i] - org.x) * rdir.x;
            const ssef lclipMaxY = (node->upper_y[i] - org.y) * rdir.y;
            const ssef lclipMaxZ = (node->upper_z[i] - org.z) * rdir.z;
            const ssef lnearP = max(max(min(lclipMinX, lclipMaxX), min(lclipMinY, lclipMaxY)), min(lclipMinZ, lclipMaxZ));
            const ssef lfarP  = min(min(max(lclipMinX, lclipMaxX), max(lclipMinY, lclipMaxY)), max(lclipMinZ, lclipMaxZ));
            const sseb lhit   = max(lnearP,ray_tnear) <= min(lfarP,ray_tfar);      
#endif
            
            /* if we hit the child we choose to continue with that child if it 
               is closer than the current next child, or we push it onto the stack */
            if (likely(any(lhit)))
            {
              assert(sptr_node < stackEnd);
              const ssef childDist = select(lhit,lnearP,inf);
              const NodeRef child = node->children[i];
              sptr_node++;
              sptr_near++;

              /* push hit child onto stack*/
              if (any(curDist < childDist))
              {
                *(sptr_node-1) = child;
                *(sptr_near-1) = childDist; 
              }
              
              /* push cur node onto stack and continue with hit child */
              else {
                *(sptr_node-1) = curNode;
                *(sptr_near-1) = curDist; 
                curDist = childDist;
                curNode = child;
              }
            }	      
          }
        }
        
        /* return if stack is empty */
        if (unlikely(curNode == BVH4::invalidNode)) {
          assert(sptr_node == stack_node);
          break;
        }
        
        /* intersect leaf */
        const sseb valid_leaf = ray_tfar > curDist;
        STAT3(normal.trav_leaves,1,popcnt(valid_leaf),4);
        size_t items; const Primitive* prim = (Primitive*) curNode.leaf(items);
        PrimitiveIntersector4::intersect(valid_leaf,ray,prim,items,bvh->geometry);
        ray_tfar = select(valid_leaf,ray.tfar,ray_tfar);
      }
      AVX_ZERO_UPPER();
    }

    template<typename PrimitiveIntersector4>
    __forceinline bool BVH4Intersector4Hybrid<PrimitiveIntersector4>::occluded1(const BVH4* bvh, NodeRef root, size_t k, Ray4& ray, 
                                                                                const sse3f& ray_org, const sse3f& ray_dir, const sse3f& ray_rdir, 
                                                                                const ssef& ray_tnear, const ssef& ray_tfar)
    {
      /*! stack state */
      NodeRef stack[stackSizeSingle];  //!< stack of nodes that still need to get traversed
      NodeRef* stackPtr = stack+1;        //!< current stack pointer
      NodeRef* stackEnd = stack+stackSizeSingle;
      stack[0]  = root;
      
      /*! offsets to select the side that becomes the lower or upper bound */
      const size_t nearX = ray_dir.x[k] >= 0.0f ? 0*sizeof(ssef) : 1*sizeof(ssef);
      const size_t nearY = ray_dir.y[k] >= 0.0f ? 2*sizeof(ssef) : 3*sizeof(ssef);
      const size_t nearZ = ray_dir.z[k] >= 0.0f ? 4*sizeof(ssef) : 5*sizeof(ssef);
      
      /*! load the ray into SIMD registers */
      const sse3f org (ray_org .x[k],ray_org .y[k],ray_org .z[k]);
      const sse3f rdir(ray_rdir.x[k],ray_rdir.y[k],ray_rdir.z[k]);
      const sse3f norg = -org, org_rdir(org*rdir);
      const ssef rayNear(ray_tnear[k]), rayFar(ray_tfar[k]); 
      
      /* pop loop */
      while (true) pop:
      {
        /*! pop next node */
        if (unlikely(stackPtr == stack)) break;
        stackPtr--;
        NodeRef cur = (NodeRef) *stackPtr;
        
        /* downtraversal loop */
        while (true)
        {
          /*! stop if we found a leaf */
          if (unlikely(cur.isLeaf())) break;
          STAT3(shadow.trav_nodes,1,1,1);
          
          /*! single ray intersection with 4 boxes */
          const Node* node = cur.node();
          const size_t farX  = nearX ^ 16, farY  = nearY ^ 16, farZ  = nearZ ^ 16;
#if defined (__AVX2__)
          const ssef tNearX = msub(load4f((const char*)node+nearX), rdir.x, org_rdir.x);
          const ssef tNearY = msub(load4f((const char*)node+nearY), rdir.y, org_rdir.y);
          const ssef tNearZ = msub(load4f((const char*)node+nearZ), rdir.z, org_rdir.z);
          const ssef tFarX  = msub(load4f((const char*)node+farX ), rdir.x, org_rdir.x);
          const ssef tFarY  = msub(load4f((const char*)node+farY ), rdir.y, org_rdir.y);
          const ssef tFarZ  = msub(load4f((const char*)node+farZ ), rdir.z, org_rdir.z);
#else
          const ssef tNearX = (norg.x + load4f((const char*)node+nearX)) * rdir.x;
          const ssef tNearY = (norg.y + load4f((const char*)node+nearY)) * rdir.y;
          const ssef tNearZ = (norg.z + load4f((const char*)node+nearZ)) * rdir.z;
          const ssef tFarX  = (norg.x + load4f((const char*)node+farX )) * rdir.x;
          const ssef tFarY  = (norg.y + load4f((const char*)node+farY )) * rdir.y;
          const ssef tFarZ  = (norg.z + load4f((const char*)node+farZ )) * rdir.z;
#endif
          
#if defined(__SSE4_1__)
          const ssef tNear = maxi(maxi(tNearX,tNearY),maxi(tNearZ,rayNear));
          const ssef tFar  = mini(mini(tFarX ,tFarY ),mini(tFarZ ,rayFar ));
          const sseb vmask = cast(tNear) > cast(tFar);
          size_t mask = movemask(vmask)^0xf;
#else
          const ssef tNear = max(tNearX,tNearY,tNearZ,rayNear);
          const ssef tFar  = min(tFarX ,tFarY ,tFarZ ,rayFar);
          const sseb vmask = tNear <= tFar;
          size_t mask = movemask(vmask);
#endif
          
          /*! if no child is hit, pop next node */
          if (unlikely(mask == 0))
            goto pop;
          
          /*! one child is hit, continue with that child */
          size_t r = bitscan(mask); mask = __btc(mask,r);
          if (likely(mask == 0)) {
            cur = node->child(r);
            continue;
          }
          
          /*! two children are hit, push far child, and continue with closer child */
          NodeRef c0 = node->child(r); const float d0 = tNear[r];
          r = bitscan(mask); mask = __btc(mask,r);
          NodeRef c1 = node->child(r); const float d1 = tNear[r];
          if (likely(mask == 0)) {
            assert(stackPtr < stackEnd);
            if (d0 < d1) { *stackPtr = c1; stackPtr++; cur = c0; continue; }
            else         { *stackPtr = c0; stackPtr++; cur = c1; continue; }
          }
          assert(stackPtr < stackEnd);
          *stackPtr = c0; stackPtr++;
          assert(stackPtr < stackEnd);
          *stackPtr = c1; stackPtr++;
          
          /*! three children are hit */
          r = bitscan(mask); mask = __btc(mask,r); cur = node->child(r); 
          if (likely(mask == 0)) continue;
          assert(stackPtr < stackEnd);
          *stackPtr = cur; stackPtr++;
          
          /*! four children are hit */
          cur = node->child(3);
        }
        
        /*! this is a leaf node */
        STAT3(shadow.trav_leaves,1,1,1);
        size_t num; Primitive* prim = (Primitive*) cur.leaf(num);
        if (PrimitiveIntersector4::occluded(ray,k,prim,num,bvh->geometry)) {
          ray.geomID[k] = 0;
          return true;
        }
      }
      return false;
    }
    
    template<typename PrimitiveIntersector4>
    void BVH4Intersector4Hybrid<PrimitiveIntersector4>::occluded(sseb* valid_i, BVH4* bvh, Ray4& ray)
    {
      /* load ray */
      const sseb valid = *valid_i;
      sseb terminated = !valid;
      sse3f ray_org = ray.org, ray_dir = ray.dir;
      ssef ray_tnear = ray.tnear, ray_tfar  = ray.tfar;
#if defined(__FIX_RAYS__)
      const ssef float_range = 0.1f*FLT_MAX;
      ray_org = clamp(ray_org,sse3f(-float_range),sse3f(+float_range));
      ray_dir = clamp(ray_dir,sse3f(-float_range),sse3f(+float_range));
      ray_tnear = max(ray_tnear,FLT_MIN); 
      ray_tfar  = min(ray_tfar,float(inf)); 
#endif
      const sse3f rdir = rcp_safe(ray_dir);
      const sse3f org(ray_org), org_rdir = org * rdir;
      ray_tnear = select(valid,ray_tnear,ssef(pos_inf));
      ray_tfar  = select(valid,ray_tfar ,ssef(neg_inf));
      const ssef inf = ssef(pos_inf);
      
      /* allocate stack and push root node */
      ssef    stack_near[stackSizeChunk];
      NodeRef stack_node[stackSizeChunk];
      stack_node[0] = BVH4::invalidNode;
      stack_near[0] = inf;
      stack_node[1] = bvh->root;
      stack_near[1] = ray_tnear; 
      NodeRef* stackEnd = stack_node+stackSizeChunk;
      NodeRef* __restrict__ sptr_node = stack_node + 2;
      ssef*    __restrict__ sptr_near = stack_near + 2;
      
      while (1)
      {
        /* pop next node from stack */
        assert(sptr_node > stack_node);
        sptr_node--;
        sptr_near--;
        NodeRef curNode = *sptr_node;
        if (unlikely(curNode == BVH4::invalidNode)) {
          assert(sptr_node == stack_node);
          break;
        }

        /* cull node if behind closest hit point */
        ssef curDist = *sptr_near;
        const sseb active = curDist < ray_tfar;
        if (unlikely(none(active))) 
          continue;
        
        /* switch to single ray traversal */
        size_t bits = movemask(active);
        if (unlikely(__popcnt(bits) <= SWITCH_THRESHOLD)) {
          for (size_t i=__bsf(bits); bits!=0; bits=__btc(bits,i), i=__bsf(bits)) {
            if (occluded1(bvh,curNode,i,ray,ray_org,ray_dir,rdir,ray_tnear,ray_tfar))
              terminated[i] = -1;
          }
          if (all(terminated)) break;
          ray_tfar = select(terminated,ssef(neg_inf),ray_tfar);
          continue;
        }
                
        while (1)
        {
          /* test if this is a leaf node */
          if (unlikely(curNode.isLeaf()))
            break;
          
          const sseb valid_node = ray_tfar > curDist;
          STAT3(shadow.trav_nodes,1,popcnt(valid_node),4);
          const Node* __restrict__ const node = curNode.node();
          
          /* pop of next node */
          assert(sptr_node > stack_node);
          sptr_node--;
          sptr_near--;
          curNode = *sptr_node;
          curDist = *sptr_near;
          
#pragma unroll(4)
          for (unsigned i=0; i<4; i++)
          {
            const NodeRef child = node->children[i];
            if (unlikely(child == BVH4::emptyNode)) break;
            
#if defined(__AVX2__)
            const ssef lclipMinX = msub(node->lower_x[i],rdir.x,org_rdir.x);
            const ssef lclipMinY = msub(node->lower_y[i],rdir.y,org_rdir.y);
            const ssef lclipMinZ = msub(node->lower_z[i],rdir.z,org_rdir.z);
            const ssef lclipMaxX = msub(node->upper_x[i],rdir.x,org_rdir.x);
            const ssef lclipMaxY = msub(node->upper_y[i],rdir.y,org_rdir.y);
            const ssef lclipMaxZ = msub(node->upper_z[i],rdir.z,org_rdir.z);
            const ssef lnearP = maxi(maxi(mini(lclipMinX, lclipMaxX), mini(lclipMinY, lclipMaxY)), mini(lclipMinZ, lclipMaxZ));
            const ssef lfarP  = mini(mini(maxi(lclipMinX, lclipMaxX), maxi(lclipMinY, lclipMaxY)), maxi(lclipMinZ, lclipMaxZ));
            const sseb lhit   = maxi(lnearP,ray_tnear) <= mini(lfarP,ray_tfar);      
#else
            const ssef lclipMinX = (node->lower_x[i] - org.x) * rdir.x;
            const ssef lclipMinY = (node->lower_y[i] - org.y) * rdir.y;
            const ssef lclipMinZ = (node->lower_z[i] - org.z) * rdir.z;
            const ssef lclipMaxX = (node->upper_x[i] - org.x) * rdir.x;
            const ssef lclipMaxY = (node->upper_y[i] - org.y) * rdir.y;
            const ssef lclipMaxZ = (node->upper_z[i] - org.z) * rdir.z;
            const ssef lnearP = max(max(min(lclipMinX, lclipMaxX), min(lclipMinY, lclipMaxY)), min(lclipMinZ, lclipMaxZ));
            const ssef lfarP  = min(min(max(lclipMinX, lclipMaxX), max(lclipMinY, lclipMaxY)), max(lclipMinZ, lclipMaxZ));
            const sseb lhit   = max(lnearP,ray_tnear) <= min(lfarP,ray_tfar);      
#endif
            
            /* if we hit the child we choose to continue with that child if it 
               is closer than the current next child, or we push it onto the stack */
            if (likely(any(lhit)))
            {
              assert(sptr_node < stackEnd);
              const ssef childDist = select(lhit,lnearP,inf);
              sptr_node++;
              sptr_near++;
              
              /* push hit child onto stack*/
              if (any(curDist < childDist))
              {
                *(sptr_node-1) = child;
                *(sptr_near-1) = childDist; 
              }
              
              /* push cur node onto stack and continue with hit child */
              else {
                *(sptr_node-1) = curNode;
                *(sptr_near-1) = curDist; 
                curDist = childDist;
                curNode = child;
              }
            }	      
          }
        }
        
        /* return if stack is empty */
        if (unlikely(curNode == BVH4::invalidNode)) {
          assert(sptr_node == stack_node);
          break;
        }
        
        /* intersect leaf */
        const sseb valid_leaf = ray_tfar > curDist;
        STAT3(shadow.trav_leaves,1,popcnt(valid_leaf),4);
        size_t items; const Primitive* prim = (Primitive*) curNode.leaf(items);
        terminated |= PrimitiveIntersector4::occluded(!terminated,ray,prim,items,bvh->geometry);
        if (all(terminated)) break;
        ray_tfar = select(terminated,ssef(neg_inf),ray_tfar);
      }
      store4i(valid & terminated,&ray.geomID,0);
      AVX_ZERO_UPPER();
    }
    
    DEFINE_INTERSECTOR4(BVH4Triangle4Intersector4HybridMoeller, BVH4Intersector4Hybrid<Triangle4Intersector4MoellerTrumbore>);
#if defined (__AVX__)
    DEFINE_INTERSECTOR4(BVH4Triangle8Intersector4HybridMoeller, BVH4Intersector4Hybrid<Triangle8Intersector4MoellerTrumbore>);
#endif
    DEFINE_INTERSECTOR4(BVH4Triangle4vIntersector4HybridPluecker, BVH4Intersector4Hybrid<Triangle4vIntersector4Pluecker>);

#if !defined(__AVX__)
    DEFINE_INTERSECTOR4TO8(BVH4Triangle4Intersector8HybridMoeller, BVH4Intersector4Hybrid<Triangle4Intersector4MoellerTrumbore>,BVH4);
    DEFINE_INTERSECTOR4TO8(BVH4Triangle4vIntersector8HybridPluecker, BVH4Intersector4Hybrid<Triangle4vIntersector4Pluecker>,BVH4);

    DEFINE_INTERSECTOR4TO16(BVH4Triangle4Intersector16HybridMoeller, BVH4Intersector4Hybrid<Triangle4Intersector4MoellerTrumbore>,BVH4);
    DEFINE_INTERSECTOR4TO16(BVH4Triangle4vIntersector16HybridPluecker, BVH4Intersector4Hybrid<Triangle4vIntersector4Pluecker>,BVH4);
#endif
  }
}