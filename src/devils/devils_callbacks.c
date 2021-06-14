/** 
 @file callbacks.c
 @brief ENet callback functions
*/
#define DEVILS_BUILDING_LIB 1
#include "include/devils.h"

static devils_callbacks callbacks = {malloc, free, abort};

int devils_initialize_with_callbacks(devils_version version, const devils_callbacks *inits)
{
  if (version < DEVILS_VERSION_CREATE(1, 3, 0))
    return -1;

  if (inits->malloc != NULL || inits->free != NULL)
  {
    if (inits->malloc == NULL || inits->free == NULL)
      return -1;

    callbacks.malloc = inits->malloc;
    callbacks.free = inits->free;
  }

  if (inits->no_memory != NULL)
    callbacks.no_memory = inits->no_memory;

  return devils_initialize();
}

devils_version
devils_linked_version(void)
{
  return DEVILS_VERSION;
}

void *
devils_malloc(size_t size)
{
  void *memory = callbacks.malloc(size);

  if (memory == NULL)
    callbacks.no_memory();

  return memory;
}

void devils_free(void *memory)
{
  callbacks.free(memory);
}
