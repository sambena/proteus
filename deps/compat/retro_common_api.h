/* Minimal stand-in for libretro-common's retro_common_api.h, enough for
 * libretro_dspfilter.h. */
#ifndef RETRO_COMMON_API_H
#define RETRO_COMMON_API_H

#ifdef __cplusplus
#define RETRO_BEGIN_DECLS extern "C" {
#define RETRO_END_DECLS }
#else
#define RETRO_BEGIN_DECLS
#define RETRO_END_DECLS
#endif

#endif
