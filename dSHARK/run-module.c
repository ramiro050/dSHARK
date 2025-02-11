// Copyright 2021 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// A example of setting up the HAL module to run simple pointwise array
// multiplication with the device implemented by different backends via
//_sample_driver().
//
// NOTE: this file does not properly handle error cases and will leak on
// failure. Applications that are just going to exit()/abort() on failure can
// probably get away with the same thing but really should prefer not to.

#include <stdio.h>

#include "iree/base/api.h"
#include "iree/hal/api.h"
#include "iree/modules/hal/module.h"
#include "iree/vm/api.h"
#include "iree/vm/bytecode_module.h"
#include "iree/base/internal/flags.h"
#include "iree/hal/cuda/registration/driver_module.h"
#include "iree/base/tracing.h"
#include "iree/hal/cuda/api.h"
#include "simple_embedding_test_bytecode_module_cuda_c.h"

iree_status_t create_sample_device(iree_allocator_t host_allocator,
                                   iree_hal_device_t** out_device, int index) {
  // Only register the CUDA HAL driver.
  IREE_RETURN_IF_ERROR(
      iree_hal_cuda_driver_module_register(iree_hal_driver_registry_default(), index));

  // Create the HAL driver from the name.
  iree_hal_driver_t* driver = NULL;
  iree_string_view_t identifier = iree_make_cstring_view("cuda");
  iree_status_t status = iree_hal_driver_registry_try_create_by_name(
      iree_hal_driver_registry_default(), identifier, host_allocator, &driver);

  // Create the default device (primary GPU).
  if (iree_status_is_ok(status)) {
    status = iree_hal_driver_create_default_device(driver, host_allocator,
                                                   out_device);
  }

  iree_hal_driver_release(driver);
  return iree_ok_status();
}

const iree_const_byte_span_t load_bytecode_module_data() {
  const struct iree_file_toc_t* module_file_toc =
      simple_embedding_test_bytecode_module_cuda_c_create();
  return iree_make_const_byte_span(module_file_toc->data,
                                   module_file_toc->size);
}

iree_status_t Run(int index) {
  // TODO(benvanik): move to instance-based registration.
  IREE_RETURN_IF_ERROR(iree_hal_module_register_types());

  iree_vm_instance_t* instance = NULL;
  IREE_RETURN_IF_ERROR(
      iree_vm_instance_create(iree_allocator_system(), &instance));

  iree_hal_device_t* device = NULL;
  IREE_RETURN_IF_ERROR(create_sample_device(iree_allocator_system(), &device, index),
                       "create device");
  iree_vm_module_t* hal_module = NULL;
  IREE_RETURN_IF_ERROR(
      iree_hal_module_create(device, iree_allocator_system(), &hal_module));

  // Load bytecode module from the embedded data.
  const iree_const_byte_span_t module_data = load_bytecode_module_data();

  iree_vm_module_t* bytecode_module = NULL;
  IREE_RETURN_IF_ERROR(iree_vm_bytecode_module_create(
      module_data, iree_allocator_null(), iree_allocator_system(),
      &bytecode_module));

  // Allocate a context that will hold the module state across invocations.
  iree_vm_context_t* context = NULL;
  iree_vm_module_t* modules[] = {hal_module, bytecode_module};
  IREE_RETURN_IF_ERROR(iree_vm_context_create_with_modules(
      instance, IREE_VM_CONTEXT_FLAG_NONE, &modules[0], IREE_ARRAYSIZE(modules),
      iree_allocator_system(), &context));
  iree_vm_module_release(hal_module);
  iree_vm_module_release(bytecode_module);

  // Lookup the entry point function.
  // Note that we use the synchronous variant which operates on pure type/shape
  // erased buffers.
  const char kMainFunctionName[] = "module.simple_mul";
  iree_vm_function_t main_function;
  IREE_RETURN_IF_ERROR(iree_vm_context_resolve_function(
      context, iree_make_cstring_view(kMainFunctionName), &main_function));

  // Initial buffer contents for 4 * 2 = 8.
  const float kFloat4[] = {4.0f, 4.0f, 4.0f, 4.0f};
  const float kFloat2[] = {2.0f, 2.0f, 2.0f, 2.0f};

  // Allocate buffers in device-local memory so that if the device has an
  // independent address space they live on the fast side of the fence.
  iree_hal_dim_t shape[1] = {IREE_ARRAYSIZE(kFloat4)};
  iree_hal_buffer_view_t* arg0_buffer_view = NULL;
  iree_hal_buffer_view_t* arg1_buffer_view = NULL;
  IREE_RETURN_IF_ERROR(iree_hal_buffer_view_allocate_buffer(
      iree_hal_device_allocator(device), shape, IREE_ARRAYSIZE(shape),
      IREE_HAL_ELEMENT_TYPE_FLOAT_32, IREE_HAL_ENCODING_TYPE_DENSE_ROW_MAJOR,
      (iree_hal_buffer_params_t){
          .type = IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL |
                  IREE_HAL_MEMORY_TYPE_HOST_VISIBLE,
          .usage = IREE_HAL_BUFFER_USAGE_DISPATCH |
                   IREE_HAL_BUFFER_USAGE_TRANSFER |
                   IREE_HAL_BUFFER_USAGE_MAPPING,
      },
      iree_make_const_byte_span(kFloat4, sizeof(kFloat4)), &arg0_buffer_view));
  IREE_RETURN_IF_ERROR(iree_hal_buffer_view_allocate_buffer(
      iree_hal_device_allocator(device), shape, IREE_ARRAYSIZE(shape),
      IREE_HAL_ELEMENT_TYPE_FLOAT_32, IREE_HAL_ENCODING_TYPE_DENSE_ROW_MAJOR,
      (iree_hal_buffer_params_t){
          .type = IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL |
                  IREE_HAL_MEMORY_TYPE_HOST_VISIBLE,
          .usage = IREE_HAL_BUFFER_USAGE_DISPATCH |
                   IREE_HAL_BUFFER_USAGE_TRANSFER |
                   IREE_HAL_BUFFER_USAGE_MAPPING,
      },
      iree_make_const_byte_span(kFloat2, sizeof(kFloat2)), &arg1_buffer_view));

  // Setup call inputs with our buffers.
  iree_vm_list_t* inputs = NULL;
  IREE_RETURN_IF_ERROR(iree_vm_list_create(
                           /*element_type=*/NULL,
                           /*capacity=*/2, iree_allocator_system(), &inputs),
                       "can't allocate input vm list");

  iree_vm_ref_t arg0_buffer_view_ref =
      iree_hal_buffer_view_move_ref(arg0_buffer_view);
  iree_vm_ref_t arg1_buffer_view_ref =
      iree_hal_buffer_view_move_ref(arg1_buffer_view);
  IREE_RETURN_IF_ERROR(
      iree_vm_list_push_ref_move(inputs, &arg0_buffer_view_ref));
  IREE_RETURN_IF_ERROR(
      iree_vm_list_push_ref_move(inputs, &arg1_buffer_view_ref));

  // Prepare outputs list to accept the results from the invocation.
  // The output vm list is allocated statically.
  iree_vm_list_t* outputs = NULL;
  IREE_RETURN_IF_ERROR(iree_vm_list_create(
                           /*element_type=*/NULL,
                           /*capacity=*/1, iree_allocator_system(), &outputs),
                       "can't allocate output vm list");

  // Synchronously invoke the function.
  IREE_RETURN_IF_ERROR(iree_vm_invoke(
      context, main_function, IREE_VM_INVOCATION_FLAG_NONE,
      /*policy=*/NULL, inputs, outputs, iree_allocator_system()));

  // Get the result buffers from the invocation.
  iree_hal_buffer_view_t* ret_buffer_view =
      (iree_hal_buffer_view_t*)iree_vm_list_get_ref_deref(
          outputs, 0, iree_hal_buffer_view_get_descriptor());
  if (ret_buffer_view == NULL) {
    return iree_make_status(IREE_STATUS_NOT_FOUND,
                            "can't find return buffer view");
  }

  // Read back the results and ensure we got the right values.
  float results[] = {0.0f, 0.0f, 0.0f, 0.0f};
  IREE_RETURN_IF_ERROR(
      iree_hal_buffer_read_data(iree_hal_buffer_view_buffer(ret_buffer_view), 0,
                                results, sizeof(results)));
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(results); ++i) {
    if (results[i] != 8.0f) {
      return iree_make_status(IREE_STATUS_UNKNOWN, "result mismatches");
    }
  }

  iree_vm_list_release(inputs);
  iree_vm_list_release(outputs);
  iree_hal_device_release(device);
  iree_vm_context_release(context);
  iree_vm_instance_release(instance);
  return iree_ok_status();
}

int run_module(int index) {
  const iree_status_t result = Run(index);
  int ret = (int)iree_status_code(result);
  if (!iree_status_is_ok(result)) {
    iree_status_fprint(stderr, result);
    iree_status_free(result);
  }
  fprintf(stdout, "simple_embedding done\n");
  return ret;
}
