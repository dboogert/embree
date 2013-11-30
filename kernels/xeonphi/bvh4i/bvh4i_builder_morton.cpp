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

#include "bvh4i.h"
#include "bvh4i_builder_morton.h"
#include "bvh4i_statistics.h"
#include "bvh4i/bvh4i_builder_util.h"

#include "common/registry_builder.h"

#define BVH_NODE_PREALLOC_FACTOR          1.2f
#define NUM_MORTON_IDS_PER_BLOCK            8
#define SINGLE_THREADED_BUILD_THRESHOLD  (MAX_MIC_THREADS*8)

//#define PROFILE

#if defined(__USE_STAT_COUNTERS__)
#define PROFILE
#endif
#define TIMER(x) 
#define DBG(x)  

#define L1_PREFETCH_ITEMS 8
#define L2_PREFETCH_ITEMS 44

namespace embree 
{
  AtomicMutex mtx;
  // =======================================================================================================
  // =======================================================================================================
  // =======================================================================================================

  __align(64) static double dt = 0.0f;

  BVH4iBuilderMorton::BVH4iBuilderMorton (BVH4i* bvh, BuildSource* source, void* geometry, const size_t minLeafSize, const size_t maxLeafSize)
  : bvh(bvh), source(source), scene((Scene*)geometry), topLevelItemThreshold(0), encodeShift(0), encodeMask(0), numBuildRecords(0), 
    morton(NULL), node(NULL), accel(NULL), numGroups(0), numPrimitives(0), numNodes(0), numAllocatedNodes(0), size_morton(0)
  {
  }

  BVH4iBuilderMorton::~BVH4iBuilderMorton()
  {
    if (morton) {
      assert(size_morton > 0);
      os_free(morton,size_morton);
    }
  }


  void BVH4iBuilderMorton::initEncodingAllocateData(size_t threadCount)
  {
    bvh->init();

    /* calculate total number of primrefs */
    size_t numPrimitivesOld = numPrimitives;
    numGroups     = source->groups();
    numPrimitives = source->size();


    size_t maxPrimsPerGroup = 0;
    for (size_t group=0; group<numGroups; group++) 
      {
	if (unlikely(scene->get(group) == NULL)) continue;
	if (scene->get(group)->type != TRIANGLE_MESH) continue;
	const TriangleMeshScene::TriangleMesh* __restrict__ const mesh = scene->getTriangleMesh(group);
	if (unlikely(!mesh->isEnabled())) continue;

	maxPrimsPerGroup = max(maxPrimsPerGroup,mesh->numTriangles);
      }

    /* calculate groupID, primID encoding */
    encodeShift = __bsr((unsigned int)maxPrimsPerGroup) + 1;
    assert( ((unsigned int)1 << encodeShift) > maxPrimsPerGroup);

    encodeMask = ((size_t)1 << encodeShift)-1;
    size_t maxGroups = ((size_t)1 << (31-encodeShift))-1;

    DBG(DBG_PRINT(numGroups));
    DBG(DBG_PRINT(maxPrimsPerGroup));
    DBG(DBG_PRINT(numPrimitives));
    DBG(DBG_PRINT(encodeMask));
    DBG(DBG_PRINT(encodeShift));
    DBG(DBG_PRINT(maxGroups));
    DBG(DBG_PRINT(size_morton));
    DBG(DBG_PRINT(bvh->size_node));
    DBG(DBG_PRINT(bvh->size_accel));

    if (maxPrimsPerGroup > encodeMask || numGroups > maxGroups)
    {
      DBG_PRINT(numGroups);
      DBG_PRINT(numPrimitives);
      DBG_PRINT(maxPrimsPerGroup);
      DBG_PRINT(encodeMask);
      DBG_PRINT(maxGroups);
      FATAL("ENCODING ERROR");      
    }

    /* preallocate arrays */
    const size_t additional_size = 16 * CACHELINE_SIZE;
    if (numPrimitivesOld != numPrimitives || numPrimitives == 0)
    {
      /* free previously allocated memory */
      if (morton) {
	assert(size_morton > 0);
	os_free(morton,size_morton);
      }
      if (node  ) { 
	assert(bvh->size_node > 0);
	os_free(node  ,bvh->size_node);
      }
      if (accel ) {
	assert(bvh->size_accel > 0);
	os_free(accel ,bvh->size_accel);
      }
      
      /* allocated memory for primrefs,nodes, and accel */
      const size_t minAllocNodes = numPrimitives ? threadCount * ALLOCATOR_NODE_BLOCK_SIZE * 4: 16;
      const size_t numPrims      = numPrimitives+4;
      const size_t numNodes      = max((size_t)(numPrimitives * BVH_NODE_PREALLOC_FACTOR),minAllocNodes);
      bvh->init(numNodes,numPrims);

      const size_t size_morton_tmp = numPrims * sizeof(MortonID32Bit) + additional_size;
      const size_t size_node       = numNodes * sizeof(BVHNode) + additional_size;
      const size_t size_accel      = numPrims * sizeof(Triangle1) + additional_size;
      numAllocatedNodes = size_node / sizeof(BVHNode);

      DBG(DBG_PRINT(size_morton_tmp));
      DBG(DBG_PRINT(size_node));
      DBG(DBG_PRINT(size_accel));

      morton = (MortonID32Bit* ) os_malloc(size_morton_tmp); 
      node   = (BVHNode*)        os_malloc(size_node  );     
      accel  = (Triangle1*)      os_malloc(size_accel );     

      assert(morton != 0);
      assert(node   != 0);
      assert(accel  != 0);

      memset(morton,0,size_morton_tmp);
      memset(node  ,0,size_node);
      memset(accel ,0,size_accel);	

      bvh->accel = accel;
      bvh->qbvh  = (BVH4i::Node*)node;
      bvh->size_node  = size_node;
      bvh->size_accel = size_accel;

      size_morton = size_morton_tmp;
    }

  }

  void BVH4iBuilderMorton::build(size_t threadIndex, size_t threadCount) 
  {
    if (g_verbose >= 2)
      std::cout << "building BVH4i with Morton builder (MIC)... " << std::flush;

    /* do some global inits first */
    initEncodingAllocateData(TaskScheduler::getNumThreads());

#if defined(PROFILE)

    double dt_min = pos_inf;
    double dt_avg = 0.0f;
    double dt_max = neg_inf;
    size_t iterations = 20;
    for (size_t i=0; i<iterations; i++) 
    {
      TaskScheduler::executeTask(threadIndex,threadCount,_build_parallel_morton,this,TaskScheduler::getNumThreads(),"build_parallel_morton");

      dt_min = min(dt_min,dt);
      dt_avg = dt_avg + dt;
      dt_max = max(dt_max,dt);
    }
    dt_avg /= double(iterations);

    std::cout << "[DONE]" << std::endl;
    std::cout << "  min = " << 1000.0f*dt_min << "ms (" << source->size()/dt_min*1E-6 << " Mtris/s)" << std::endl;
    std::cout << "  avg = " << 1000.0f*dt_avg << "ms (" << source->size()/dt_avg*1E-6 << " Mtris/s)" << std::endl;
    std::cout << "  max = " << 1000.0f*dt_max << "ms (" << source->size()/dt_max*1E-6 << " Mtris/s)" << std::endl;
    std::cout << BVH4iStatistics(bvh).str();

#else
    DBG(DBG_PRINT(numPrimitives));


    if (likely(numPrimitives > SINGLE_THREADED_BUILD_THRESHOLD && TaskScheduler::getNumThreads() > 1))
      {
	DBG(std::cout << "PARALLEL BUILD" << std::endl << std::flush);
	TaskScheduler::executeTask(threadIndex,threadCount,_build_parallel_morton,this,TaskScheduler::getNumThreads(),"build_parallel");
      }
    else
      {
	/* number of primitives is small, just use single threaded mode */
	if (likely(numPrimitives > 0))
	  {
	    DBG(std::cout << "SERIAL BUILD" << std::endl << std::flush);
	    build_parallel_morton(0,1,0,0,NULL);
	  }
	else
	  {
	    DBG(std::cout << "EMPTY SCENE BUILD" << std::endl << std::flush);
	    /* handle empty scene */
	    for (size_t i=0;i<4;i++)
	      bvh->qbvh[0].setInvalid(i);
	    for (size_t i=0;i<4;i++)
	      bvh->qbvh[1].setInvalid(i);
	    bvh->qbvh[0].lower[0].child = BVH4i::NodeRef(128);
	    bvh->root = bvh->qbvh[0].lower[0].child; 
	    bvh->bounds = BBox3f(*(Vec3fa*)&bvh->qbvh->lower[0],*(Vec3fa*)&bvh->qbvh->upper[0]);	    
	  }
      }

    if (g_verbose >= 2) {
      double perf = source->size()/dt*1E-6;
      std::cout << "[DONE] " << 1000.0f*dt << "ms (" << perf << " Mtris/s), primitives " << numPrimitives << std::endl;
      std::cout << BVH4iStatistics(bvh).str();
    }
#endif
  }

    
  // =======================================================================================================
  // =======================================================================================================
  // =======================================================================================================

  void BVH4iBuilderMorton::initThreadState(const size_t threadID, const size_t numThreads)
  {
    const size_t numBlocks = (numPrimitives+NUM_MORTON_IDS_PER_BLOCK-1) / NUM_MORTON_IDS_PER_BLOCK;
    const size_t startID   =      ((threadID+0)*numBlocks/numThreads) * NUM_MORTON_IDS_PER_BLOCK;
    const size_t endID     = min( ((threadID+1)*numBlocks/numThreads) * NUM_MORTON_IDS_PER_BLOCK ,numPrimitives) ;
    
    assert(startID % NUM_MORTON_IDS_PER_BLOCK == 0);

    /* find first group containing startID */
    size_t group = 0, skipped = 0;
    for (; group<numGroups; group++) 
    {       
      if (unlikely(scene->get(group) == NULL)) continue;
      if (scene->get(group)->type != TRIANGLE_MESH) continue;
      const TriangleMeshScene::TriangleMesh* __restrict__ const mesh = scene->getTriangleMesh(group);
      if (unlikely(!mesh->isEnabled())) continue;
      const size_t numTriangles = mesh->numTriangles;	
      if (skipped + numTriangles > startID) break;
      skipped += numTriangles;
    }

    /* store start group and offset */
    thread_startGroup[threadID] = group;
    thread_startGroupOffset[threadID] = startID - skipped;
  }

  void BVH4iBuilderMorton::barrierTest(const size_t threadID, const size_t numThreads) // FIXME: why is computePrimRefs faster...
  {
  }


  void BVH4iBuilderMorton::computeBounds(const size_t threadID, const size_t numThreads) // FIXME: why is computePrimRefs faster...
  {
    const size_t numBlocks = (numPrimitives+NUM_MORTON_IDS_PER_BLOCK-1) / NUM_MORTON_IDS_PER_BLOCK;
    const size_t startID   =      ((threadID+0)*numBlocks/numThreads) * NUM_MORTON_IDS_PER_BLOCK;
    const size_t endID     = min( ((threadID+1)*numBlocks/numThreads) * NUM_MORTON_IDS_PER_BLOCK ,numPrimitives) ;
    assert(startID % NUM_MORTON_IDS_PER_BLOCK == 0);

    __align(64) Centroid_Scene_AABB bounds;
    bounds.reset();

    size_t currentID = startID;

    size_t startGroup = thread_startGroup[threadID];
    size_t offset = thread_startGroupOffset[threadID];

    mic_f bounds_centroid_min((float)pos_inf);
    mic_f bounds_centroid_max((float)neg_inf);

    for (size_t group = startGroup; group<numGroups; group++) 
    {       
      if (unlikely(scene->get(group) == NULL)) continue;
      if (unlikely(scene->get(group)->type != TRIANGLE_MESH)) continue;
      const TriangleMeshScene::TriangleMesh* __restrict__ const mesh = scene->getTriangleMesh(group);
      if (unlikely(!mesh->isEnabled())) continue;

      const TriangleMeshScene::TriangleMesh::Triangle* tri = &mesh->triangle(offset);

      for (size_t i=offset; i<mesh->numTriangles && currentID < endID; i++, currentID++,tri++)	 
	{
	  prefetch<PFHINT_L2>(&tri + L2_PREFETCH_ITEMS);

	  const float *__restrict__ const vptr0 = (float*)&mesh->vertex(tri->v[0]);
	  const float *__restrict__ const vptr1 = (float*)&mesh->vertex(tri->v[1]);
	  const float *__restrict__ const vptr2 = (float*)&mesh->vertex(tri->v[2]);

	  prefetch<PFHINT_NT>(vptr1);
	  prefetch<PFHINT_NT>(vptr2);

	  const mic_f v0 = broadcast4to16f(vptr0);
	  const mic_f v1 = broadcast4to16f(vptr1);
	  const mic_f v2 = broadcast4to16f(vptr2);

	  prefetch<PFHINT_L1>(&tri + L1_PREFETCH_ITEMS);

	  const mic_f bmin = min(min(v0,v1),v2);
	  const mic_f bmax = max(max(v0,v1),v2);
	  const mic_f centroid = (bmin+bmax)*0.5f;
	  bounds_centroid_min = min(bounds_centroid_min,centroid);
	  bounds_centroid_max = max(bounds_centroid_max,centroid);
	}
      
      if (unlikely(currentID == endID)) break;
      offset = 0;
    }

    store4f(&bounds.centroid.lower,bounds_centroid_min);
    store4f(&bounds.centroid.upper,bounds_centroid_max);
    
    global_bounds.extend_centroid_bounds_atomic(bounds); // FIXME: check whether float atomics are fast
  }

  void BVH4iBuilderMorton::computeMortonCodes(const size_t threadID, const size_t numThreads)
  {
    const size_t numBlocks = (numPrimitives+NUM_MORTON_IDS_PER_BLOCK-1) / NUM_MORTON_IDS_PER_BLOCK;
    const size_t startID   =      ((threadID+0)*numBlocks/numThreads) * NUM_MORTON_IDS_PER_BLOCK;
    const size_t endID     = min( ((threadID+1)*numBlocks/numThreads) * NUM_MORTON_IDS_PER_BLOCK ,numPrimitives) ;
    assert(startID % NUM_MORTON_IDS_PER_BLOCK == 0);

    /* store the morton codes in 'morton' memory */
    MortonID32Bit* __restrict__ dest = ((MortonID32Bit*)morton) + startID; 

    /* compute mapping from world space into 3D grid */
    const mic_f base     = broadcast4to16f((float*)&global_bounds.centroid.lower);
    const mic_f diagonal = \
      broadcast4to16f((float*)&global_bounds.centroid.upper) - 
      broadcast4to16f((float*)&global_bounds.centroid.lower);
    const mic_f scale    = select(diagonal != 0, rcp(diagonal) * mic_f(LATTICE_SIZE_PER_DIM * 0.99f),mic_f(0.0f));

    size_t currentID = startID;
    size_t offset = thread_startGroupOffset[threadID];


    mic_i mID      = mic_i::zero();
    mic_i binID3_x = mic_i::zero();
    mic_i binID3_y = mic_i::zero();
    mic_i binID3_z = mic_i::zero();

    size_t slot = 0;

    for (size_t group = thread_startGroup[threadID]; group<numGroups; group++) 
    {       
      if (unlikely(scene->get(group) == NULL)) continue;
      if (unlikely(scene->get(group)->type != TRIANGLE_MESH)) continue;
      const TriangleMeshScene::TriangleMesh* const mesh = scene->getTriangleMesh(group);
      if (unlikely(!mesh->isEnabled())) continue;
      const size_t numTriangles = min(mesh->numTriangles-offset,endID-currentID);
       
      const unsigned int groupCode = (group << encodeShift);
      for (size_t i=0; i<numTriangles; i++)	  
      {
	const TriangleMeshScene::TriangleMesh::Triangle& tri = mesh->triangle(offset+i);
	prefetch<PFHINT_NT>(&tri + 16);
	prefetch<PFHINT_NT>(&tri + 4);

	const float *__restrict__ const vptr0 = (float*)&mesh->vertex(tri.v[0]);
	const float *__restrict__ const vptr1 = (float*)&mesh->vertex(tri.v[1]);
	const float *__restrict__ const vptr2 = (float*)&mesh->vertex(tri.v[2]);

	prefetch<PFHINT_L2>(vptr1);
	prefetch<PFHINT_L2>(vptr2);

	const mic_f v0 = broadcast4to16f(vptr0);
	const mic_f v1 = broadcast4to16f(vptr1);
	const mic_f v2 = broadcast4to16f(vptr2);

	const mic_f bmin  = min(min(v0,v1),v2);
	const mic_f bmax  = max(max(v0,v1),v2);
	const mic_f cent  = (bmin+bmax)*mic_f(0.5f);
	const mic_i binID = mic_i((cent-base)*scale);

	mID[2*slot+1] = groupCode | (offset+i);
	compactustore16i_low(0x1,&binID3_x[2*slot+0],binID); // extract
	compactustore16i_low(0x2,&binID3_y[2*slot+0],binID);
	compactustore16i_low(0x4,&binID3_z[2*slot+0],binID);
	slot++;
	if (unlikely(slot == NUM_MORTON_IDS_PER_BLOCK))
	  {
	    const mic_i code  = bitInterleave(binID3_x,binID3_y,binID3_z);
	    const mic_i final = select(0x5555,code,mID);      
	    assert((size_t)dest % 64 == 0);
	    store16i_ngo(dest,final);	    
	    slot = 0;
	    dest += 8;
	  }
        currentID++;
      }

      offset = 0;
      if (currentID == endID) break;
    }

    if (unlikely(slot != 0))
      {
	const mic_i code  = bitInterleave(binID3_x,binID3_y,binID3_z);
	const mic_i final = select(0x5555,code,mID);      
	assert((size_t)dest % 64 == 0);
	store16i_ngo(dest,final);	    
      }
  }
  
  void BVH4iBuilderMorton::recreateMortonCodes(SmallBuildRecord& current) const
  {
    const size_t items  = current.size();
    const size_t blocks = items / NUM_MORTON_IDS_PER_BLOCK;
    const size_t rest   = items % NUM_MORTON_IDS_PER_BLOCK;

    MortonID32Bit *__restrict__ m = &morton[current.begin];

    mic_f bounds_centroid_min((float)pos_inf);
    mic_f bounds_centroid_max((float)neg_inf);


    for (size_t i=0; i<blocks; i++,m+=NUM_MORTON_IDS_PER_BLOCK)
      {
	prefetch<PFHINT_L1EX>(&morton[i+  NUM_MORTON_IDS_PER_BLOCK]);
	prefetch<PFHINT_L2EX>(&morton[i+2*NUM_MORTON_IDS_PER_BLOCK]);

	for (size_t j=0;j<NUM_MORTON_IDS_PER_BLOCK;j++)
	  {
	    const unsigned int index  = m[j].index;
	    const unsigned int primID = index & encodeMask; 
	    const unsigned int geomID = index >> encodeShift; 
	    const TriangleMeshScene::TriangleMesh* __restrict__ const mesh = scene->getTriangleMesh(geomID);
	    const TriangleMeshScene::TriangleMesh::Triangle& tri = mesh->triangle(primID);

	    const float *__restrict__ const vptr0 = (float*)&mesh->vertex(tri.v[0]);
	    const float *__restrict__ const vptr1 = (float*)&mesh->vertex(tri.v[1]);
	    const float *__restrict__ const vptr2 = (float*)&mesh->vertex(tri.v[2]);

	    prefetch<PFHINT_L1>(vptr1);
	    prefetch<PFHINT_L1>(vptr2);

	    const mic_f v0 = broadcast4to16f(vptr0);
	    const mic_f v1 = broadcast4to16f(vptr1);
	    const mic_f v2 = broadcast4to16f(vptr2);
     
	    const mic_f bmin = min(min(v0,v1),v2);
	    const mic_f bmax = max(max(v0,v1),v2);
	    const mic_f centroid = (bmin+bmax)*0.5f;
	    bounds_centroid_min = min(bounds_centroid_min,centroid);
	    bounds_centroid_max = max(bounds_centroid_max,centroid);
	  }
      }   

    if (rest)
      {
	for (size_t j=0;j<rest;j++)
	  {
	    const unsigned int index  = m[j].index;
	    const unsigned int primID = index & encodeMask; 
	    const unsigned int geomID = index >> encodeShift; 

	    const TriangleMeshScene::TriangleMesh* __restrict__ const mesh = scene->getTriangleMesh(geomID);
	    const TriangleMeshScene::TriangleMesh::Triangle& tri = mesh->triangle(primID);

	    const mic_f v0 = broadcast4to16f((float*)&mesh->vertex(tri.v[0]));
	    const mic_f v1 = broadcast4to16f((float*)&mesh->vertex(tri.v[1]));
	    const mic_f v2 = broadcast4to16f((float*)&mesh->vertex(tri.v[2]));
     
	    const mic_f bmin = min(min(v0,v1),v2);
	    const mic_f bmax = max(max(v0,v1),v2);
	    const mic_f centroid = (bmin+bmax)*0.5f;
	    bounds_centroid_min = min(bounds_centroid_min,centroid);
	    bounds_centroid_max = max(bounds_centroid_max,centroid);
	  }
      }


    const mic_f base     = bounds_centroid_min;
    const mic_f diagonal = bounds_centroid_max - bounds_centroid_min;
    const mic_f scale    = select(diagonal != 0,rcp(diagonal) * mic_f(LATTICE_SIZE_PER_DIM * 0.99f),mic_f(0.0f));
    
    mic_i binID3_x = mic_i::zero();
    mic_i binID3_y = mic_i::zero();
    mic_i binID3_z = mic_i::zero();

    m = &morton[current.begin];

    for (size_t i=0; i<blocks; i++,m+=NUM_MORTON_IDS_PER_BLOCK)
      {
	for (size_t j=0;j<NUM_MORTON_IDS_PER_BLOCK;j++)
	  {
	    const unsigned int index  = m[j].index;
	    const unsigned int primID = index & encodeMask; 
	    const unsigned int geomID = index >> encodeShift; 

	    const TriangleMeshScene::TriangleMesh* __restrict__ const mesh = scene->getTriangleMesh(geomID);
	    const TriangleMeshScene::TriangleMesh::Triangle& tri = mesh->triangle(primID);

	    const mic_f v0 = broadcast4to16f((float*)&mesh->vertex(tri.v[0]));
	    const mic_f v1 = broadcast4to16f((float*)&mesh->vertex(tri.v[1]));
	    const mic_f v2 = broadcast4to16f((float*)&mesh->vertex(tri.v[2]));
     
	    const mic_f bmin = min(min(v0,v1),v2);
	    const mic_f bmax = max(max(v0,v1),v2);
	    const mic_f centroid = (bmin+bmax)*0.5f;
	    const mic_i binID = mic_i((centroid-base)*scale);

	    compactustore16i_low(0x1,&binID3_x[2*j+0],binID); 
	    compactustore16i_low(0x2,&binID3_y[2*j+0],binID);
	    compactustore16i_low(0x4,&binID3_z[2*j+0],binID);	    
	  }

	const mic_i mID = uload16i((int*)m);
	const mic_i code  = bitInterleave(binID3_x,binID3_y,binID3_z);
	const mic_i final = select(0x5555,code,mID);      
	ustore16i(m,final);	
      }
    if (rest)
      {
	for (size_t j=0;j<rest;j++)
	  {
	    const unsigned int index  = m[j].index;
	    const unsigned int primID = index & encodeMask; 
	    const unsigned int geomID = index >> encodeShift; 

	    const TriangleMeshScene::TriangleMesh* __restrict__ const mesh = scene->getTriangleMesh(geomID);
	    const TriangleMeshScene::TriangleMesh::Triangle& tri = mesh->triangle(primID);

	    const mic_f v0 = broadcast4to16f((float*)&mesh->vertex(tri.v[0]));
	    const mic_f v1 = broadcast4to16f((float*)&mesh->vertex(tri.v[1]));
	    const mic_f v2 = broadcast4to16f((float*)&mesh->vertex(tri.v[2]));
     
	    const mic_f bmin = min(min(v0,v1),v2);
	    const mic_f bmax = max(max(v0,v1),v2);
	    const mic_f centroid = (bmin+bmax)*0.5f;
	    const mic_i binID = mic_i((centroid-base)*scale);

	    compactustore16i_low(0x1,&binID3_x[2*j+0],binID); 
	    compactustore16i_low(0x2,&binID3_y[2*j+0],binID);
	    compactustore16i_low(0x4,&binID3_z[2*j+0],binID);	    
	  }
	const mic_m mask = ((unsigned int)1 << (2*rest))-1;
	const mic_i mID = uload16i((int*)m);
	const mic_i code  = bitInterleave(binID3_x,binID3_y,binID3_z);
	const mic_i final = select(0x5555,code,mID);      
	compactustore16i(mask,(int*)m,final);		
      }       

    quicksort_insertionsort_ascending<MortonID32Bit,32>(morton,current.begin,current.end-1); 

#if defined(DEBUG)
    for (size_t i=current.begin; i<current.end-1; i++)
      assert(morton[i].code <= morton[i+1].code);
#endif	    

  }


  void BVH4iBuilderMorton::radixsort(const size_t threadID, const size_t numThreads)
  {
    const size_t numBlocks = (numPrimitives+NUM_MORTON_IDS_PER_BLOCK-1) / NUM_MORTON_IDS_PER_BLOCK;
    const size_t startID   = ((threadID+0)*numBlocks/numThreads) * NUM_MORTON_IDS_PER_BLOCK;
    const size_t endID     = ((threadID+1)*numBlocks/numThreads) * NUM_MORTON_IDS_PER_BLOCK;
    assert(startID % NUM_MORTON_IDS_PER_BLOCK == 0);
    assert(endID % NUM_MORTON_IDS_PER_BLOCK == 0);

    assert(((numThreads)*numBlocks/numThreads) * NUM_MORTON_IDS_PER_BLOCK == ((numPrimitives+7)&(-8)));

    MortonID32Bit* __restrict__ mortonID[2];
    mortonID[0] = (MortonID32Bit*) morton; 
    mortonID[1] = (MortonID32Bit*) node;


    /* we need 4 iterations to process all 32 bits */
    for (size_t b=0; b<4; b++)
    {
      const MortonID32Bit* __restrict__ const src = (MortonID32Bit*)mortonID[((b+0)%2)];
      MortonID32Bit*       __restrict__ const dst = (MortonID32Bit*)mortonID[((b+1)%2)];

      __assume_aligned(&radixCount[threadID][0],64);
      
      /* count how many items go into the buckets */

#pragma unroll(16)
      for (size_t i=0; i<16; i++)
	store16i(&radixCount[threadID][i*16],mic_i::zero());


      for (size_t i=startID; i<endID; i+=NUM_MORTON_IDS_PER_BLOCK) {
	prefetch<PFHINT_NT>(&src[i+L1_PREFETCH_ITEMS]);
	prefetch<PFHINT_L2>(&src[i+L2_PREFETCH_ITEMS]);
	
#pragma unroll(NUM_MORTON_IDS_PER_BLOCK)
	for (unsigned long j=0;j<NUM_MORTON_IDS_PER_BLOCK;j++)
	  {
	    const unsigned int index = src[i+j].getByte(b);
	    radixCount[threadID][index]++;
	  }
      }

      LockStepTaskScheduler::syncThreads(threadID,numThreads);


      /* calculate total number of items for each bucket */


      mic_i count[16];
#pragma unroll(16)
      for (size_t i=0; i<16; i++)
	count[i] = mic_i::zero();


      for (size_t i=0; i<threadID; i++)
#pragma unroll(16)
	for (size_t j=0; j<16; j++)
	  count[j] += load16i((int*)&radixCount[i][j*16]);
      
      __align(64) unsigned int inner_offset[RADIX_BUCKETS];

#pragma unroll(16)
      for (size_t i=0; i<16; i++)
	store16i(&inner_offset[i*16],count[i]);

#pragma unroll(16)
      for (size_t i=0; i<16; i++)
	count[i] = load16i((int*)&inner_offset[i*16]);

      for (size_t i=threadID; i<numThreads; i++)
#pragma unroll(16)
	for (size_t j=0; j<16; j++)
	  count[j] += load16i((int*)&radixCount[i][j*16]);	  

     __align(64) unsigned int total[RADIX_BUCKETS];

#pragma unroll(16)
      for (size_t i=0; i<16; i++)
	store16i(&total[i*16],count[i]);

      __align(64) unsigned int offset[RADIX_BUCKETS];

      /* calculate start offset of each bucket */
      offset[0] = 0;
      for (size_t i=1; i<RADIX_BUCKETS; i++)    
        offset[i] = offset[i-1] + total[i-1];
      
      /* calculate start offset of each bucket for this thread */

#pragma unroll(RADIX_BUCKETS)
	for (size_t j=0; j<RADIX_BUCKETS; j++)
          offset[j] += inner_offset[j];

      /* copy items into their buckets */
      for (size_t i=startID; i<endID; i+=NUM_MORTON_IDS_PER_BLOCK) {
	prefetch<PFHINT_NT>(&src[i+L1_PREFETCH_ITEMS]);
	prefetch<PFHINT_L2>(&src[i+L2_PREFETCH_ITEMS]);

#pragma nounroll
	for (unsigned long j=0;j<NUM_MORTON_IDS_PER_BLOCK;j++)
	  {
	    const unsigned int index = src[i+j].getByte(b);
	    assert(index < RADIX_BUCKETS);
	    dst[offset[index]] = src[i+j];
	    prefetch<PFHINT_L2EX>(&dst[offset[index]+L1_PREFETCH_ITEMS]);
	    offset[index]++;
	  }
	evictL2(&src[i]);
      }

      if (b<3) LockStepTaskScheduler::syncThreads(threadID,numThreads);

    }
  }

  void BVH4iBuilderMorton::createTopLevelTree(const size_t threadID, const size_t numThreads)
  {
    size_t taskID = threadID;
    __align(64) SmallBuildRecord children[BVH4i::N];

    
    while(taskID < numBuildRecords)
      {
	SmallBuildRecord &sbr = buildRecords[taskID];
	if (sbr.size() > topLevelItemThreshold)
	  {
	    const size_t numChildren = createQBVHNode(sbr,children);
	    buildRecords[taskID] = children[0];
	    if (numChildren > 1)
	      {
		const unsigned int dest = numBuildRecordCounter.add(numChildren-1);
		for (size_t i=0;i<numChildren-1;i++)
		  buildRecords[dest + i] = children[i+1];
	      }
	  }
	taskID += numThreads;
      }   
  }

  void BVH4iBuilderMorton::recurseSubMortonTrees(const size_t threadID, const size_t numThreads)
  {
    NodeAllocator alloc(atomicID,numAllocatedNodes);
    
    double msec;
    TIMER(msec = getSeconds());
    TIMER(size_t items = 0);


    while (true)
    {
      const unsigned int taskID = LockStepTaskScheduler::taskCounter.inc();
      if (taskID >= numBuildRecords) break;
      
      SmallBuildRecord &br = buildRecords[taskID];

      recurse(br,alloc,RECURSE,threadID);
      
      /* mark toplevel of tree */
      node[br.parentID].upper.a = -1;

      TIMER(items += br.size());
    }    

#if 0
    TIMER(msec = getSeconds()-msec);    
    TIMER(mtx.lock());
    TIMER(std::cout << "threadID " << threadID << " items "<< items << " " << 1000. * msec << " ms => " << (double)items / (1000. * msec) << " items/ms " << std::endl << std::flush);
    TIMER(mtx.unlock());
#endif
  }

  __forceinline void convertToBVH4Layout(BVHNode *__restrict__ const bptr)
  {
#if 0
    BVH4i::Node tmp;
    for (int i=0;i<4;i++)
      {
	tmp.lower[i].x = bptr[i].lower.x;
	tmp.lower[i].y = bptr[i].lower.y;
	tmp.lower[i].z = bptr[i].lower.z;

	tmp.upper[i].x = bptr[i].upper.x;
	tmp.upper[i].y = bptr[i].upper.y;
	tmp.upper[i].z = bptr[i].upper.z;
	tmp.upper[i].child = bptr[i].upper.a;

	if (!bvhLeaf(bptr[i].lower.a))
	  {
	    tmp.lower[i].child = qbvhCreateNode(bvhChildID(bptr[i].lower.a)>>2,0); // bvhChildren(bptr[i].ext_min.t)
	  }
	else
	  {
	    tmp.lower[i].child = (bptr[i].lower.a ^ BVH_LEAF_MASK) | QBVH_LEAF_MASK;
	  }	  
      }

    BVH4i::Node * __restrict__  qptr = (BVH4i::Node*)bptr;
    *qptr = tmp;
#else
    const mic_i box01 = load16i((int*)(bptr + 0));
    const mic_i box23 = load16i((int*)(bptr + 2));

    const mic_i box_min01 = permute<2,0,2,0>(box01);
    const mic_i box_max01 = permute<3,1,3,1>(box01);

    const mic_i box_min23 = permute<2,0,2,0>(box23);
    const mic_i box_max23 = permute<3,1,3,1>(box23);
    const mic_i box_min0123 = select(0x00ff,box_min01,box_min23);
    const mic_i box_max0123 = select(0x00ff,box_max01,box_max23);

    const mic_m min_d_mask = bvhLeaf(box_min0123) != mic_i::zero();
    const mic_i childID    = bvhChildID(box_min0123)>>2;
    const mic_i min_d_node = qbvhCreateNode(childID,mic_i::zero());
    const mic_i min_d_leaf = (box_min0123 ^ BVH_LEAF_MASK) | QBVH_LEAF_MASK;
    const mic_i min_d      = select(min_d_mask,min_d_leaf,min_d_node);
    const mic_i bvh4_min   = select(0x7777,box_min0123,min_d);
    const mic_i bvh4_max   = box_max0123;
    store16i_nt((int*)(bptr + 0),bvh4_min);
    store16i_nt((int*)(bptr + 2),bvh4_max);
#endif
    
  }

  void BVH4iBuilderMorton::convertToSOALayout(const size_t threadID, const size_t numThreads)
  {
    const size_t startID = (threadID+0)*numNodes/numThreads;
    const size_t endID   = (threadID+1)*numNodes/numThreads;

    BVHNode  * __restrict__  bptr = ( BVHNode*)node + startID*4;

    BVH4i::Node * __restrict__  qptr = (BVH4i::Node*)node + startID;

    for (unsigned int n=startID;n<endID;n++,qptr++,bptr+=4)
      {
	prefetch<PFHINT_L1EX>(bptr+4);
	prefetch<PFHINT_L2EX>(bptr+4*4);
	convertToBVH4Layout(bptr);
	evictL1(bptr);
      }
  }
  
  // =======================================================================================================
  // =======================================================================================================
  // =======================================================================================================


  void BVH4iBuilderMorton::split_fallback(SmallBuildRecord& current, SmallBuildRecord& leftChild, SmallBuildRecord& rightChild) const
  {
    const unsigned int center = (current.begin + current.end)/2;
    leftChild.init(current.begin,center);
    rightChild.init(center,current.end);
  }
		

  __forceinline BBox3f BVH4iBuilderMorton::createSmallLeaf(SmallBuildRecord& current) const
  {
    mic_f bounds_min(pos_inf);
    mic_f bounds_max(neg_inf);

    Vec3fa lower(pos_inf);
    Vec3fa upper(neg_inf);
    size_t items = current.size();
    size_t start = current.begin;
    assert(items<=4);

    const mic_i morton_mask(encodeMask);
    const mic_i morton_shift(encodeShift);

    prefetch<PFHINT_L2EX>(&node[current.parentID]);
    prefetch<PFHINT_L2>(&morton[start+8]);

    for (size_t i=0; i<items; i++) 
      {	
	const unsigned int index = morton[start+i].index;
	const unsigned int primID = index & encodeMask; 
	const unsigned int geomID = index >> encodeShift; 

	const mic_i morton_index(morton[start+i].index);
	const mic_i morton_primID = morton_index & morton_mask;
	const mic_i morton_geomID = morton_index >> morton_shift;

	const TriangleMeshScene::TriangleMesh* __restrict__ const mesh = scene->getTriangleMesh(geomID);
	const TriangleMeshScene::TriangleMesh::Triangle& tri = mesh->triangle(primID);
      
	const float *__restrict__ const vptr0 = (float*)&mesh->vertex(tri.v[0]);
	const float *__restrict__ const vptr1 = (float*)&mesh->vertex(tri.v[1]);
	const float *__restrict__ const vptr2 = (float*)&mesh->vertex(tri.v[2]);

	const mic_f v0 = broadcast4to16f(vptr0); //FIXME: zero last component
	const mic_f v1 = broadcast4to16f(vptr1);
	const mic_f v2 = broadcast4to16f(vptr2);

	const mic_f tri_accel = initTriangle1(v0,v1,v2,morton_geomID,morton_primID,mic_i::zero());

	bounds_min = min(bounds_min,min(v0,min(v1,v2)));
	bounds_max = max(bounds_max,max(v0,max(v1,v2)));
	store16f_ngo(&accel[start+i],tri_accel);
      }

    store4f(&node[current.parentID].lower,bounds_min);
    store4f(&node[current.parentID].upper,bounds_max);
    node[current.parentID].createLeaf(start,items,items);
    __align(64) BBox3f bounds;
    store4f(&bounds.lower,bounds_min);
    store4f(&bounds.upper,bounds_max);
    return bounds;
  }


  BBox3f BVH4iBuilderMorton::createLeaf(SmallBuildRecord& current, NodeAllocator& alloc)
  {
#if defined(DEBUG)
    if (current.depth > BVH4i::maxBuildDepthLeaf) 
      throw std::runtime_error("ERROR: depth limit reached");
#endif
    
    /* create leaf for few primitives */
    if (current.size() <= MORTON_LEAF_THRESHOLD) {     
      return createSmallLeaf(current);
    }

    /* first split level */
    SmallBuildRecord record0, record1;
    split_fallback(current,record0,record1);

    /* second split level */
    SmallBuildRecord children[4];
    split_fallback(record0,children[0],children[1]);
    split_fallback(record1,children[2],children[3]);

    /* allocate next four nodes */
    size_t numChildren = 4;
    //const unsigned int currentIndex = allocNode(BVH4i::N);
    const size_t currentIndex = alloc.get(BVH4i::N);
   
    BBox3f bounds; 
    bounds = empty;
    /* recurse into each child */
    for (size_t i=0; i<numChildren; i++) {
      children[i].parentID = currentIndex+i;
      children[i].depth = current.depth+1;
      bounds.extend( createLeaf(children[i],alloc) );
    }

    node[current.parentID].lower = bounds.lower;
    node[current.parentID].upper = bounds.upper;
    node[current.parentID].createNode(currentIndex,numChildren);

    return bounds;
  }  

  __forceinline bool BVH4iBuilderMorton::split(SmallBuildRecord& current,
                                               SmallBuildRecord& left,
                                               SmallBuildRecord& right) const
  {
    /* mark as leaf if leaf threshold reached */
    if (unlikely(current.size() <= BVH4iBuilderMorton::MORTON_LEAF_THRESHOLD)) {
      //current.createLeaf();
      return false;
    }

    const unsigned int code_start = morton[current.begin].code;
    const unsigned int code_end   = morton[current.end-1].code;
    unsigned int bitpos = clz(code_start^code_end);

    /* if all items mapped to same morton code, then create new morton codes for the items */
    if (unlikely(bitpos == 32)) 
    {
      recreateMortonCodes(current);
      const unsigned int code_start = morton[current.begin].code;
      const unsigned int code_end   = morton[current.end-1].code;
      bitpos = clz(code_start^code_end);

      /* if the morton code is still the same, goto fall back split */
      if (unlikely(bitpos == 32)) 
      {
        size_t center = (current.begin + current.end)/2; 
        left.init(current.begin,center);
        right.init(center,current.end);
        return true;
      }
    }

    /* split the items at the topmost different morton code bit */
    const unsigned int bitpos_diff = 31-bitpos;
    const unsigned int bitmask = 1 << bitpos_diff;
    
    /* find location where bit differs using binary search */
    size_t begin = current.begin;
    size_t end   = current.end;
    while (begin + 1 != end) {
      const size_t mid = (begin+end)/2;
      const unsigned bit = morton[mid].code & bitmask;
      if (bit == 0) begin = mid; else end = mid;
    }
    size_t center = end;
#if defined(DEBUG)      
    for (unsigned int i=begin;  i<center; i++) assert((morton[i].code & bitmask) == 0);
    for (unsigned int i=center; i<end;    i++) assert((morton[i].code & bitmask) == bitmask);
#endif
    
    left.init(current.begin,center);
    right.init(center,current.end);
    return true;
  }

  size_t BVH4iBuilderMorton::createQBVHNode(SmallBuildRecord& current, SmallBuildRecord *__restrict__ const children)
  {

    /* create leaf node */
    if (unlikely(current.size() <= BVH4iBuilderMorton::MORTON_LEAF_THRESHOLD)) {
      children[0] = current;
      return 1;
    }

    /* fill all 4 children by always splitting the one with the largest number of primitives */
    __assume_aligned(children,sizeof(SmallBuildRecord));

    size_t numChildren = 1;
    children[0] = current;

    do {

      /* find best child with largest number of items*/
      int bestChild = -1;
      unsigned bestItems = 0;
      for (unsigned int i=0; i<numChildren; i++)
      {
        /* ignore leaves as they cannot get split */
        if (children[i].size() <= BVH4iBuilderMorton::MORTON_LEAF_THRESHOLD)
          continue;
        
        /* remember child with largest number of items */
        if (children[i].size() > bestItems) { 
          bestItems = children[i].size();
          bestChild = i;
        }
      }
      if (bestChild == -1) break;

      /*! split best child into left and right child */
      __align(64) SmallBuildRecord left, right;
      if (!split(children[bestChild],left,right))
        continue;
      
      /* add new children left and right */
      left.depth = right.depth = current.depth+1;
      children[bestChild] = children[numChildren-1];
      children[numChildren-1] = left;
      children[numChildren+0] = right;
      numChildren++;
      
    } while (numChildren < BVH4i::N);

    /* create leaf node if no split is possible */
    if (unlikely(numChildren == 1)) {
      children[0] = current;
      return 1;
    }

    /* allocate next four nodes and prefetch them */
    const size_t currentIndex = allocNode(BVH4i::N);    
    prefetch<PFHINT_L2EX>((float*)&node[currentIndex+0]);
    prefetch<PFHINT_L2EX>((float*)&node[currentIndex+2]);

    /* recurse into each child */
    for (size_t i=0; i<numChildren; i++) 
      {
	children[i].parentID = currentIndex+i;
      }

    /* init used/unused nodes */
    const mic_f init_node = load16f((float*)BVH4i::initQBVHNode);
    store16f((float*)&node[currentIndex+0],init_node);
    store16f((float*)&node[currentIndex+2],init_node);

    node[current.parentID].createNode(currentIndex,numChildren);
    return numChildren;
  }

  
  BBox3f BVH4iBuilderMorton::recurse(SmallBuildRecord& current, 
				     NodeAllocator& alloc,
				     const size_t mode, 
				     const size_t numThreads) 
  {
    /* stop toplevel recursion at some number of items */
    if (unlikely(mode == CREATE_TOP_LEVEL))
      {
	if (current.size()  <= topLevelItemThreshold &&
	    numBuildRecords >= numThreads) {
	  buildRecords[numBuildRecords++] = current; // FIXME: can overflow
	  return empty;
	}
      }

    __align(64) SmallBuildRecord children[BVH4i::N];

    /* create leaf node */
    if (unlikely(current.size() <= BVH4iBuilderMorton::MORTON_LEAF_THRESHOLD)) {
      return createSmallLeaf(current);
    }
    if (unlikely(current.depth >= BVH4i::maxBuildDepth)) {
      return createLeaf(current,alloc); 
    }

    /* fill all 4 children by always splitting the one with the largest number of primitives */
    size_t numChildren = 1;
    children[0] = current;

    do {

      /* find best child with largest number of items*/
      int bestChild = -1;
      unsigned bestItems = 0;
      for (unsigned int i=0; i<numChildren; i++)
      {
        /* ignore leaves as they cannot get split */
        if (children[i].size() <= BVH4iBuilderMorton::MORTON_LEAF_THRESHOLD)
          continue;
        
        /* remember child with largest number of items */
        if (children[i].size() > bestItems) { 
          bestItems = children[i].size();
          bestChild = i;
        }
      }
      if (bestChild == -1) break;

      /*! split best child into left and right child */
      __align(64) SmallBuildRecord left, right;
      if (!split(children[bestChild],left,right))
        continue;
      
      /* add new children left and right */
      left.depth = right.depth = current.depth+1;
      children[bestChild] = children[numChildren-1];
      children[numChildren-1] = left;
      children[numChildren+0] = right;
      numChildren++;
      
    } while (numChildren < BVH4i::N);

    /* create leaf node if no split is possible */
    if (unlikely(numChildren == 1)) {
      return createSmallLeaf(current);
    }

    /* allocate next four nodes and prefetch them */
    const size_t currentIndex = alloc.get(BVH4i::N);    
    prefetch<PFHINT_L2EX>((float*)&node[currentIndex+0]);
    prefetch<PFHINT_L2EX>((float*)&node[currentIndex+2]);


    /* recurse into each child */
    BBox3f bounds;
    bounds = empty;
    for (size_t i=0; i<numChildren; i++) 
    {
      children[i].parentID = currentIndex+i;

      if (children[i].size() <= BVH4iBuilderMorton::MORTON_LEAF_THRESHOLD)
	{
	  bounds.extend( createSmallLeaf(children[i]) );
	}
      else
	bounds.extend( recurse(children[i],alloc,mode,numThreads) );
    }

    /* init used/unused nodes */
    const mic_f init_node_lower = broadcast4to16f((float*)&BVH4i::initQBVHNode[0]);
    const mic_f init_node_upper = broadcast4to16f((float*)&BVH4i::initQBVHNode[1]);

    for (size_t i=numChildren; i<BVH4i::N; i++) 
      {
	store4f_nt((float*)&node[currentIndex+i].lower,init_node_lower);
	store4f_nt((float*)&node[currentIndex+i].upper,init_node_upper);
      }


    node[current.parentID].lower = bounds.lower;
    node[current.parentID].upper = bounds.upper;
    node[current.parentID].createNode(currentIndex,numChildren);

    return bounds;
  }


  void BVH4iBuilderMorton::refit(const size_t index) const
  {    
    BVHNode& entry = node[index];

    if (unlikely(entry.isLeaf()))
      return;

    const size_t children = entry.firstChildID();
    const size_t items    = entry.items();
    BVHNode* next = &node[children+0];
    
    Vec3fa lower(pos_inf);
    Vec3fa upper(neg_inf);
    const int e0 = entry.lower.a;
    const int e1 = entry.upper.a;
    
    for (size_t i=0; i<items; i++) 
    {
      const size_t childIndex = children + i;	    	    
      if (!next[i].isLeaf())
        refit(childIndex);
      
      lower = min(lower,next[i].lower);
      upper = max(upper,next[i].upper);
    }      
    
    entry.lower = Vec3fa(lower);
    entry.upper = Vec3fa(upper);
    entry.lower.a = e0;
    entry.upper.a = e1;
  }    

  void BVH4iBuilderMorton::refit_toplevel(const size_t index) const
  {    
    BVHNode& entry = node[index];

    if (entry.upper.a == -1 || entry.isLeaf())    
      return;

    const unsigned int children = entry.firstChildID();
    BVHNode* next = &node[children+0];
    
    const unsigned int items = entry.items();
    
    Vec3fa lower(pos_inf);
    Vec3fa upper(neg_inf);
    const int e0 = entry.lower.a;
    const int e1 = entry.upper.a;
    
    for (unsigned int i=0; i<items; i++) 
    {
      const unsigned int childIndex = children + i;	    	    
      if (!next[i].isLeaf())
        refit_toplevel(childIndex);
      
      lower = min(lower,next[i].lower);
      upper = max(upper,next[i].upper);
    }      
    
    entry.lower = Vec3fa(lower);
    entry.upper = Vec3fa(upper);
    entry.lower.a = e0;
    entry.upper.a = e1;
  }

  void BVH4iBuilderMorton::build_main (const size_t threadIndex, const size_t threadCount)
  { 
    TIMER(std::cout << std::endl);
    TIMER(double msec = 0.0);

    /* compute scene bounds */
    TIMER(msec = getSeconds());
    global_bounds.reset();
    LockStepTaskScheduler::dispatchTask( task_computeBounds, this, threadIndex, threadCount );
    TIMER(msec = getSeconds()-msec);    
    TIMER(std::cout << "task_computeBounds " << 1000. * msec << " ms" << std::endl << std::flush);
    TIMER(DBG_PRINT(global_bounds));



    /* compute morton codes */
    TIMER(msec = getSeconds());
    LockStepTaskScheduler::dispatchTask( task_computeMortonCodes, this, threadIndex, threadCount );   

    /* padding */
    MortonID32Bit* __restrict__ const dest = (MortonID32Bit*)morton;
    
    for (size_t i=numPrimitives; i<((numPrimitives+7)&(-8)); i++) {
      dest[i].code  = 0xffffffff; 
      dest[i].index = 0;
    }

    TIMER(msec = getSeconds()-msec);    
    TIMER(std::cout << "task_computeMortonCodes " << 1000. * msec << " ms" << std::endl << std::flush);

 

    /* sort morton codes */
    TIMER(msec = getSeconds());
    LockStepTaskScheduler::dispatchTask( task_radixsort, this, threadIndex, threadCount );

#if defined(DEBUG)
    for (size_t i=1; i<((numPrimitives+7)&(-8)); i++)
      assert(morton[i-1].code <= morton[i].code);

    for (size_t i=numPrimitives; i<((numPrimitives+7)&(-8)); i++) {
      assert(dest[i].code  == 0xffffffff); 
      assert(dest[i].index == 0);
    }
#endif	    

    TIMER(msec = getSeconds()-msec);    
    TIMER(std::cout << "task_radixsort " << 1000. * msec << " ms" << std::endl << std::flush);

    TIMER(msec = getSeconds());

    /* build and extract top-level tree */
    numBuildRecords = 0;
    atomicID.reset(BVH4i::N);
    topLevelItemThreshold = max((numPrimitives + threadCount-1)/((threadCount)),(size_t)64);

    SmallBuildRecord br;
    br.init(0,numPrimitives);
    br.parentID = 0;
    br.depth = 1;


#if 1

    buildRecords[0] = br;
    numBuildRecords = 1;
    size_t iterations = 0;
    while(numBuildRecords < threadCount*3)
      {
	numBuildRecordCounter.reset(numBuildRecords);
	LockStepTaskScheduler::dispatchTask( task_createTopLevelTree, this, threadIndex, threadCount );
	iterations++;

	if (unlikely(numBuildRecords == numBuildRecordCounter)) { break; }

	numBuildRecords = numBuildRecordCounter;
      }
#else

    /* perform first splits in single threaded mode */
    NodeAllocator alloc(atomicID,numAllocatedNodes);

    recurse(br,alloc,CREATE_TOP_LEVEL,threadIndex);	    


#endif

    /* sort all subtasks by size */
    quicksort_insertionsort_decending<SmallBuildRecord,16>(buildRecords,0,numBuildRecords-1);

    TIMER(msec = getSeconds()-msec);    
    TIMER(std::cout << "create top level " << 1000. * msec << " ms" << std::endl << std::flush);
    TIMER(DBG_PRINT(numBuildRecords));

    // for (size_t i = 0;i<numBuildRecords;i++)
    //   std::cout << i << " " << buildRecords[i] << std::endl;    
    // exit(0);


    TIMER(msec = getSeconds());
    
    /* build sub-trees */
    LockStepTaskScheduler::dispatchTask( task_recurseSubMortonTrees, this, threadIndex, threadCount );

    numNodes = atomicID >> 2;

    TIMER(msec = getSeconds()-msec);    
    TIMER(std::cout << "task_recurseSubMortonTrees " << 1000. * msec << " ms" << std::endl << std::flush);

    TIMER(msec = getSeconds());

    /* refit toplevel part of tree */
    refit_toplevel(0);

    TIMER(msec = getSeconds()-msec);    
    TIMER(std::cout << "refit top level " << 1000. * msec << " ms" << std::endl << std::flush);

    /* set global bounds */
    global_bounds.geometry = node[0];
  }

  void BVH4iBuilderMorton::build_parallel_morton(size_t threadIndex, size_t threadCount, size_t taskIndex, size_t taskCount, TaskScheduler::Event* event) 
  {
    TIMER(double msec = 0.0);

    /* start measurement */
    double t0 = 0.0f;
    if (g_verbose >= 2) t0 = getSeconds();

    /* initialize thread state */
    initThreadState(threadIndex,threadCount);
    
    /* let all thread except for control thread wait for work */
    if (threadIndex != 0) {
      LockStepTaskScheduler::dispatchTaskMainLoop(threadIndex,threadCount);
      return;
    }

    if (g_verbose >= 2) t0 = getSeconds();

    /* performs build of tree */
    build_main(threadIndex,threadCount);

    TIMER(msec = getSeconds());
    /* convert to optimized layout */
    LockStepTaskScheduler::dispatchTask( task_convertToSOALayout, this, threadIndex, threadCount );
    TIMER(msec = getSeconds()-msec);    
    TIMER(std::cout << "task_convertToSOALayout " << 1000. * msec << " ms" << std::endl << std::flush);

    /* set root and bounding box */
    bvh->root = bvh->qbvh[0].lower[0].child; 
    bvh->bounds = global_bounds.geometry;

    /* end task */
    LockStepTaskScheduler::releaseThreads(threadCount);
    
    /* stop measurement */
    if (g_verbose >= 2) dt = getSeconds()-t0;

  }

  void BVH4iBuilderMortonRegister () {
    ADD_BUILDER("bvh4i.morton",BVH4iBuilderMorton::create,1,inf);
  }
}

