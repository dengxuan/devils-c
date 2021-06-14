/** 
 @file list.c
 @brief ENet linked list functions
*/
#define DEVILS_BUILDING_LIB 1
#include "include/devils.h"

/** 
    @defgroup list ENet linked list utility functions
    @ingroup private
    @{
*/
void devils_list_clear(devils_list *list)
{
   list->sentinel.next = &list->sentinel;
   list->sentinel.previous = &list->sentinel;
}

devils_list_iterator
devils_list_insert(devils_list_iterator position, void *data)
{
   devils_list_iterator result = (devils_list_iterator)data;

   result->previous = position->previous;
   result->next = position;

   result->previous->next = result;
   position->previous = result;

   return result;
}

void *
devils_list_remove(devils_list_iterator position)
{
   position->previous->next = position->next;
   position->next->previous = position->previous;

   return position;
}

devils_list_iterator
devils_list_move(devils_list_iterator position, void *dataFirst, void *dataLast)
{
   devils_list_iterator first = (devils_list_iterator)dataFirst,
                        last = (devils_list_iterator)dataLast;

   first->previous->next = last->next;
   last->next->previous = first->previous;

   first->previous = position->previous;
   last->next = position;

   first->previous->next = first;
   position->previous = last;

   return first;
}

size_t
devils_list_size(devils_list *list)
{
   size_t size = 0;
   devils_list_iterator position;

   for (position = devils_list_begin(list);
        position != devils_list_end(list);
        position = devils_list_next(position))
      ++size;

   return size;
}

/** @} */
