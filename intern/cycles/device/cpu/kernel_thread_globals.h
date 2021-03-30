/*
 * Copyright 2011-2021 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "kernel/device/cpu/compat.h"
#include "kernel/device/cpu/globals.h"

CCL_NAMESPACE_BEGIN

/* A special class which extends memory ownership of the `KernelGlobals` decoupling any resource
 * which is not thread-safe for access. Every worker thread which needs to operate on
 * `KernelGlobals` needs to initialize its own copy of this object.
 *
 * NOTE: Only minimal subset of objects are copied: `KernelData` is never copied. This means that
 * there is no unnecessary data duplication happening when using this object. */
class CPUKernelThreadGlobals : public KernelGlobals {
 public:
  CPUKernelThreadGlobals();

  /* TODO(sergey): Would be nice to have properly typed OSLGlobals even in the case when building
   * without OSL support. Will avoid need to those unnamed pointers and casts. */
  CPUKernelThreadGlobals(const KernelGlobals &kernel_globals, void *osl_globals_memory);

  ~CPUKernelThreadGlobals();

  CPUKernelThreadGlobals(const CPUKernelThreadGlobals &other) = delete;
  CPUKernelThreadGlobals(CPUKernelThreadGlobals &&other) noexcept;

  CPUKernelThreadGlobals &operator=(const CPUKernelThreadGlobals &other) = delete;
  CPUKernelThreadGlobals &operator=(CPUKernelThreadGlobals &&other);

 protected:
  void reset_runtime_memory();
};

CCL_NAMESPACE_END
