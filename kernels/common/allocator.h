// ======================================================================== //
//                 INTEL CORPORATION PROPRIETARY INFORMATION                //
//   This software is supplied under the terms of a license agreement or    //
//   nondisclosure agreement with Intel Corporation and may not be copied   //
//    or disclosed except in accordance with the terms of that agreement.   //
//        Copyright (C) 2012 Intel Corporation. All Rights Reserved.        //
// ======================================================================== //

#pragma once

#include "sys/platform.h"
#include "sys/sysinfo.h"
#include "sys/sync/mutex.h"
#include <vector>
#include "alloc.h"

namespace embree
{
  template<typename T>
  struct NodeAllocatorPerThread
  {
    /*! Allocator default construction. */
    NodeAllocatorPerThread () : cur(0) {
      thread = new ThreadAllocator[getNumberOfLogicalThreads()];
    }

    /*! Allocator destructor. */
    ~NodeAllocatorPerThread() 
    {
      clear();
      delete[] thread; thread = NULL;
    }

    void clear () 
    {
      for (size_t i=0; i<blocks.size(); i++) 
        Alloc::global.free(blocks[i]); 
      
      cur = 0;
      blocks.resize(0);
    }

    __forceinline T* malloc(size_t tid) {
      return thread[tid].malloc(this);
    }

    __forceinline void free(size_t tid, T* ptr) {
      thread[tid].free(ptr,this);
    }

    size_t bytes() const {
      return blocks.size()*Alloc::blockSize;
    }

    /*! Per thread structure holding the current memory block. */
    struct __align(64) ThreadAllocator 
    {
      ALIGNED_CLASS_(64);
    public:

      /*! Default constructor. */
      __forceinline ThreadAllocator () 
        : avail(128), cur(0) {}

      void clear () {
        cur = 0;
      }

      /* Allocate memory. */
      __forceinline T* malloc(NodeAllocatorPerThread* alloc) 
      {
        if (cur == 0) {
          cur = avail.size()/2;
          alloc->malloc(&avail[0],cur);
        }
        return avail[--cur];
      }

      /* Free memory. */
      __forceinline void free(T* ptr, NodeAllocatorPerThread* alloc) 
      {
        if (cur == avail.size()) {
          cur = avail.size()/2;
          alloc->free(&avail[cur],cur);
        }
        avail[cur++] = ptr;
      }
    
    public:
      std::vector<T*> avail;
      size_t cur;
    };

  private:
    
    void free(T** ptrs, size_t size) 
    {
      Lock<MutexSys> lock(mutex);
      size_t end = cur+size;
      if (end > avail.size()) 
        avail.resize(end);
      for (size_t i=0; i<size; i++)
        avail[cur++] = ptrs[i];
    }
    
    void malloc(T** ptrs, size_t size)
    {
      Lock<MutexSys> lock(mutex);
      while (size > cur) newblock();
      for (size_t i=0; i<size; i++)
        ptrs[i] = avail[--cur];
    }

    void newblock ()
    {
      char* block = (char*) Alloc::global.malloc();
      blocks.push_back(block);
      size_t size = cur+(Alloc::blockSize+sizeof(T)-1)/sizeof(T);
      if (size > avail.size()) avail.resize(size);
      for (size_t i=0; i+sizeof(T) < Alloc::blockSize; i+=sizeof(T)) {
	assert(cur < avail.size());
        avail[cur++] = (T*) (block+i);
      }
    }

  private:
    ThreadAllocator* thread;
    MutexSys mutex;
    size_t cur;
    std::vector<T*> avail;
    std::vector<void*> blocks;       //!< available memory blocks
  };
}
