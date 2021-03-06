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

#ifndef __EMBREE_ACCEL3_H__
#define __EMBREE_ACCEL3_H__

#include "accel.h"

namespace embree
{
  class Accel3 : public Accel
  {
  public:
    Accel3 (Accel* accel0 = NULL, Accel* accel1 = NULL, Accel* accel2 = NULL);
    ~Accel3();

  public:
    static void intersect (void* ptr, RTCRay& ray);
    static void intersect4 (const void* valid, void* ptr, RTCRay4& ray);
    static void intersect8 (const void* valid, void* ptr, RTCRay8& ray);
    static void intersect16 (const void* valid, void* ptr, RTCRay16& ray);

  public:
    static void occluded (void* ptr, RTCRay& ray);
    static void occluded4 (const void* valid, void* ptr, RTCRay4& ray);
    static void occluded8 (const void* valid, void* ptr, RTCRay8& ray);
    static void occluded16 (const void* valid, void* ptr, RTCRay16& ray);

  public:
    void print(size_t ident);
    void immutable();
    void build (size_t threadIndex, size_t threadCount);

  public:
    Accel* accel0;
    Accel* accel1;
    Accel* accel2;
  };
}

#endif
