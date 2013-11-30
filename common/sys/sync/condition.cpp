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

#include "condition.h"

#if defined(__WIN32__) && !defined(PTHREADS_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace embree
{
  struct ConditionImplementation
  {
    __forceinline ConditionImplementation ()
      : count(0)
    {
      mutex = CreateMutex(NULL,FALSE,NULL);
      event = CreateEvent(NULL,TRUE,FALSE,NULL);
      noThreadWaitingEvent = CreateEvent(NULL,FALSE,FALSE,NULL);
    }

    __forceinline ~ConditionImplementation ()
    {
      CloseHandle(mutex);
      CloseHandle(event);
      CloseHandle(noThreadWaitingEvent);
    }

    __forceinline void wait(MutexSys& mutex_in)
    {
      mutex_in.unlock();

      /* atomically increment thread count */
      WaitForSingleObject(mutex,INFINITE);
      ssize_t cnt0 = atomic_add(&count,+1);
      ReleaseMutex(mutex);

      /* all threads except the last one are wait in the barrier */
      if (WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0)
        throw std::runtime_error("WaitForSingleObject failed");

      /* atomically decrement thread count */
      ssize_t cnt1 = atomic_add(&count,-1);

      /* the last thread that left the barrier resets the event again */
      if (cnt1 == 1) {
        if (ResetEvent(event) == 0)
          throw std::runtime_error("ResetEvent failed");
        if (SetEvent(noThreadWaitingEvent) == 0)
          throw std::runtime_error("SetEvent failed");
      }

      mutex_in.lock();
    }

    __forceinline void broadcast() 
    {
      /* we support only one broadcast at a given time */
      WaitForSingleObject(mutex,INFINITE);

      /* if threads are waiting, let them continue */
      if (count > 0) 
      {
        if (SetEvent(event) == 0)
          throw std::runtime_error("SetEvent failed");

        if (WaitForSingleObject(noThreadWaitingEvent, INFINITE) != WAIT_OBJECT_0)
          throw std::runtime_error("WaitForSingleObject failed");
      }

      /* allow further broadcasts */
      ReleaseMutex(mutex);
    }

  public:
    HANDLE mutex;
    HANDLE event;
    HANDLE noThreadWaitingEvent;
    volatile atomic_t count;
  };
}
#endif

#if defined(__UNIX__) || defined(PTHREADS_WIN32)
#include <pthread.h>
namespace embree
{
  struct ConditionImplementation
  {
    __forceinline ConditionImplementation () { 
      pthread_cond_init(&cond,NULL); 
    }
    
    __forceinline ~ConditionImplementation() { 
      pthread_cond_destroy(&cond);
    } 
    
    __forceinline void wait(MutexSys& mutex) { 
      pthread_cond_wait(&cond, (pthread_mutex_t*)mutex.mutex); 
    }
    
    __forceinline void broadcast() { 
      pthread_cond_broadcast(&cond); 
    }

  public:
    pthread_cond_t cond;
  };
}
#endif

namespace embree 
{
  ConditionSys::ConditionSys () { 
    cond = new ConditionImplementation; 
  }

  ConditionSys::~ConditionSys() { 
    delete (ConditionImplementation*) cond;
  }

  void ConditionSys::wait(MutexSys& mutex) { 
    ((ConditionImplementation*) cond)->wait(mutex);
  }

  void ConditionSys::broadcast() { 
    ((ConditionImplementation*) cond)->broadcast();
  }
}