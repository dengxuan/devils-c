/** 
 @file  callbacks.h
 @brief ENet callbacks
*/
#ifndef __DEVILS_CALLBACKS_H__
#define __DEVILS_CALLBACKS_H__

#include <stdlib.h>

typedef struct _devils_callbacks
{
    void *(DEVILS_CALLBACK *malloc)(size_t size);
    void(DEVILS_CALLBACK *free)(void *memory);
    void(DEVILS_CALLBACK *no_memory)(void);
} devils_callbacks;

/** @defgroup callbacks ENet internal callbacks
    @{
    @ingroup private
*/
extern void *devils_malloc(size_t);
extern void devils_free(void *);

/** @} */

#endif /* __DEVILS_CALLBACKS_H__ */
