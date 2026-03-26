/*******************************************************************************
*                                                                              *
*  Define size independent data types and operations.                          *
*  Ported from Genesis-Plus-GX for frank-genesis                                 *
*                                                                              *
*******************************************************************************/

#ifndef OSD_CPU_H
#define OSD_CPU_H

#undef TRUE
#undef FALSE
#define TRUE  1
#define FALSE 0

typedef unsigned char   UINT8;
typedef unsigned short  UINT16;
typedef unsigned int    UINT32;
typedef signed char     INT8;
typedef signed short    INT16;
typedef signed int      INT32;

/* INLINE macro - use standard inline */
#ifndef INLINE
#define INLINE static inline __attribute__((always_inline))
#endif

/******************************************************************************
 * Union of UINT8, UINT16 and UINT32 in native endianess of the target
 * This is used to access bytes and words in a machine independent manner.
 ******************************************************************************/

typedef union {
#ifdef LSB_FIRST
  struct { UINT8 l,h,h2,h3; } b;
  struct { UINT16 l,h; } w;
#else
  struct { UINT8 h3,h2,h,l; } b;
  struct { UINT16 h,l; } w;
#endif
  UINT32 d;
}  PAIR;

#endif  /* defined OSD_CPU_H */
