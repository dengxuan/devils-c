/** 
 @file  time.h
 @brief ENet time constants and macros
*/
#ifndef __DEVILS_TIME_H__
#define __DEVILS_TIME_H__

#define DEVILS_TIME_OVERFLOW 86400000

#define DEVILS_TIME_LESS(a, b) ((a) - (b) >= DEVILS_TIME_OVERFLOW)
#define DEVILS_TIME_GREATER(a, b) ((b) - (a) >= DEVILS_TIME_OVERFLOW)
#define DEVILS_TIME_LESS_EQUAL(a, b) (!DEVILS_TIME_GREATER(a, b))
#define DEVILS_TIME_GREATER_EQUAL(a, b) (!DEVILS_TIME_LESS(a, b))

#define DEVILS_TIME_DIFFERENCE(a, b) ((a) - (b) >= DEVILS_TIME_OVERFLOW ? (b) - (a) : (a) - (b))

#endif /* __DEVILS_TIME_H__ */
