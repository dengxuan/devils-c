/** 
 @file  list.h
 @brief ENet list management 
*/
#ifndef __DEVILS_LIST_H__
#define __DEVILS_LIST_H__

#include <stdlib.h>

typedef struct _devils_list_node
{
   struct _devils_list_node *next;
   struct _devils_list_node *previous;
} devils_list_node;

typedef devils_list_node *devils_list_iterator;

typedef struct _devils_list
{
   devils_list_node sentinel;
} devils_list;

extern void devils_list_clear(devils_list *);

extern devils_list_iterator devils_list_insert(devils_list_iterator, void *);
extern void *devils_list_remove(devils_list_iterator);
extern devils_list_iterator devils_list_move(devils_list_iterator, void *, void *);

extern size_t devils_list_size(devils_list *);

#define devils_list_begin(list) ((list)->sentinel.next)
#define devils_list_end(list) (&(list)->sentinel)

#define devils_list_empty(list) (devils_list_begin(list) == devils_list_end(list))

#define devils_list_next(iterator) ((iterator)->next)
#define devils_list_previous(iterator) ((iterator)->previous)

#define devils_list_front(list) ((void *)(list)->sentinel.next)
#define devils_list_back(list) ((void *)(list)->sentinel.previous)

#endif /* __DEVILS_LIST_H__ */
