/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Headless libretro frontend for the Atari 2600 work: runs a core (Stella, the capture build
 * of Stella, or Proteus around either) for a number of frames and reports hashes of what came
 * out, so that two runs can be compared, and checks the capture interface against the video.
 *
 *   harness2600 <core> <rom> <frames> [options]
 *     --opt key=value     force a core option
 *     --input             press joypad buttons following a fixed script
 *     --press <frame>:<button>[:<frames>]
 *                         hold x, up, down, left, right, b, a or start from a frame on
 *     --key <frame>:<key> press backslash, [, ], -, = or a digit at a frame
 *     --no-set-variable   refuse options the core wants to change
 *     --show-options      print Proteus's options as they are after the last frame
 *     --capture           call the capture exports directly and check every frame
 *     --hashes <file>     write one line per frame: the hash of its pixels
 *     --bmp <file>        write the last frame
 *     --layers <prefix>   with --capture: write the last frame's layers as <prefix>_*.bmp
 *     --wav <file>        write the audio
 *     --state             hash the save state after the last frame
 *     --state-out <file>  write the save state after the last frame
 *     --state-in <file>   start from a save state, which makes runs of a game the same
 *     --objects <file>    with --capture: write every frame's objects
 *     --sound <file>      with --capture: write the audio registers whenever they change
 *     --tone <hz>         print how much of a pitch is in the sound
 *     --ram <file>        write the 128 bytes of RAM after every frame
 *     --slow <ms>         wait that long before every frame
 *     --native            hash frames that are larger than the core's at the core's size,
 *                         taking the pixel in the middle of every block
 *     --sysdir <dir>      the system directory (profiles, proteus.log)
 *     --quiet             do not print the core's log
 */
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"
#include "../src/proteus_capture.h"
#include "../src/fx.h"

#ifdef _WIN32
#include <windows.h>
#define LOAD(p)    (void*)LoadLibraryA(p)
#define SYM(h, n)  (void*)GetProcAddress((HMODULE)h, n)
#else
#include <dlfcn.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#define LOAD(p)    dlopen(p, RTLD_NOW)
#define SYM(h, n)  dlsym(h, n)
#endif

#define MAX_OPTS 256

static struct
{
   void (*set_environment)(retro_environment_t);
   void (*set_video_refresh)(retro_video_refresh_t);
   void (*set_audio_sample)(retro_audio_sample_t);
   void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
   void (*set_input_poll)(retro_input_poll_t);
   void (*set_input_state)(retro_input_state_t);
   void (*init)(void);
   void (*deinit)(void);
   void (*get_system_info)(struct retro_system_info*);
   void (*get_system_av_info)(struct retro_system_av_info*);
   void (*run)(void);
   size_t (*serialize_size)(void);
   bool (*serialize)(void*, size_t);
   bool (*unserialize)(const void*, size_t);
   void *(*get_memory_data)(unsigned);
   size_t (*get_memory_size)(unsigned);
   bool (*load_game)(const struct retro_game_info*);
   void (*unload_game)(void);
   /* optional */
   void (*capture_enable)(unsigned);
   const struct pxc_frame *(*capture)(unsigned);
} core;

static retro_keyboard_event_t keyboard_cb;
static bool take_set_variable = true;

/* What the core asks of the controller of port 0. */
static struct
{
   unsigned now[2];        /* strong, weak */
   unsigned starts[2];     /* times it went from nothing to something, or up by a half */
   unsigned most[2];
   unsigned frames[2];     /* frames it was on */
   unsigned calls;
} rumble;

static bool RETRO_CALLCONV rumble_cb(unsigned port, enum retro_rumble_effect effect, uint16_t strength)
{
   unsigned k = effect == RETRO_RUMBLE_STRONG ? 0 : 1;
   if (port != 0)
      return false;
   rumble.calls++;
   if (strength > rumble.now[k] + rumble.now[k] / 2 && strength > 1000)
      rumble.starts[k]++;
   if (strength > rumble.most[k])
      rumble.most[k] = strength;
   if (strength)
      rumble.frames[k]++;
   rumble.now[k] = strength;
   return true;
}

static struct { char key[64]; char value[128]; bool forced; } opts[MAX_OPTS];
static unsigned opt_count;
static bool options_updated;

static const char *system_dir = ".";
static bool quiet;
static bool scripted_input;
static bool roaming;         /* --roam: the stick in all four directions, for mazes */
static unsigned roam_seed;   /* --seed: another way through them */
static unsigned frame_no;

static uint32_t *video;          /* the last frame, packed */
static unsigned video_w, video_h;
static size_t video_cap;
static bool video_new;
static unsigned video_frames, video_dupes;
static unsigned geometry_w, geometry_h;

static int16_t *audio;
static size_t audio_frames, audio_cap;

static double now_ms(void)
{
#ifdef _WIN32
   LARGE_INTEGER t, freq;
   QueryPerformanceFrequency(&freq);
   QueryPerformanceCounter(&t);
   return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
#endif
}

static uint64_t fnv(uint64_t h, const void *data, size_t n)
{
   const uint8_t *p = data;
   for (size_t i = 0; i < n; i++)
   {
      h ^= p[i];
      h *= 0x100000001B3ull;
   }
   return h;
}
#define FNV_START 0xCBF29CE484222325ull

static void RETRO_CALLCONV log_cb(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap;
   if (quiet && level < RETRO_LOG_WARN)
      return;
   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
}

static int find_opt(const char *key)
{
   for (unsigned i = 0; i < opt_count; i++)
      if (!strcmp(opts[i].key, key))
         return (int)i;
   return -1;
}

static void declare_opt(const char *key, const char *def, bool forced)
{
   int i = find_opt(key);
   if (i < 0)
   {
      if (opt_count >= MAX_OPTS)
         return;
      i = (int)opt_count++;
      snprintf(opts[i].key, sizeof(opts[i].key), "%s", key);
      opts[i].forced = false;
   }
   if (forced || !opts[i].forced)
      snprintf(opts[i].value, sizeof(opts[i].value), "%s", def ? def : "");
   opts[i].forced = opts[i].forced || forced;
}

/* ---------------------------------------------------------------------------
 * The frontend's file access, which Stella reads its ROM through: plain files.
 * ------------------------------------------------------------------------- */

struct retro_vfs_file_handle { FILE *f; char path[1024]; };

static const char *RETRO_CALLCONV vfs_get_path(struct retro_vfs_file_handle *h) { return h->path; }

static struct retro_vfs_file_handle *RETRO_CALLCONV vfs_open(const char *path, unsigned mode, unsigned hints)
{
   struct retro_vfs_file_handle *h = calloc(1, sizeof(*h));
   const char *how = "rb";
   (void)hints;
   if ((mode & RETRO_VFS_FILE_ACCESS_READ_WRITE) == RETRO_VFS_FILE_ACCESS_READ_WRITE)
      how = (mode & RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING) ? "r+b" : "w+b";
   else if (mode & RETRO_VFS_FILE_ACCESS_WRITE)
      how = (mode & RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING) ? "r+b" : "wb";
   if (!h || !(h->f = fopen(path, how)))
   {
      free(h);
      return NULL;
   }
   snprintf(h->path, sizeof(h->path), "%s", path);
   return h;
}

static int RETRO_CALLCONV vfs_close(struct retro_vfs_file_handle *h)
{
   int r = fclose(h->f);
   free(h);
   return r ? -1 : 0;
}

static int64_t RETRO_CALLCONV vfs_size(struct retro_vfs_file_handle *h)
{
   long at = ftell(h->f), size;
   fseek(h->f, 0, SEEK_END);
   size = ftell(h->f);
   fseek(h->f, at, SEEK_SET);
   return size;
}

static int64_t RETRO_CALLCONV vfs_tell(struct retro_vfs_file_handle *h) { return ftell(h->f); }

static int64_t RETRO_CALLCONV vfs_seek(struct retro_vfs_file_handle *h, int64_t offset, int whence)
{
   int w = whence == RETRO_VFS_SEEK_POSITION_CURRENT ? SEEK_CUR
         : whence == RETRO_VFS_SEEK_POSITION_END ? SEEK_END : SEEK_SET;
   return fseek(h->f, (long)offset, w) ? -1 : ftell(h->f);
}

static int64_t RETRO_CALLCONV vfs_read(struct retro_vfs_file_handle *h, void *s, uint64_t len)
{
   return (int64_t)fread(s, 1, (size_t)len, h->f);
}

static int64_t RETRO_CALLCONV vfs_write(struct retro_vfs_file_handle *h, const void *s, uint64_t len)
{
   return (int64_t)fwrite(s, 1, (size_t)len, h->f);
}

static int RETRO_CALLCONV vfs_flush(struct retro_vfs_file_handle *h) { return fflush(h->f) ? -1 : 0; }
static int RETRO_CALLCONV vfs_remove(const char *path) { return remove(path) ? -1 : 0; }
static int RETRO_CALLCONV vfs_rename(const char *from, const char *to) { return rename(from, to) ? -1 : 0; }
static int64_t RETRO_CALLCONV vfs_truncate(struct retro_vfs_file_handle *h, int64_t len) { (void)h; (void)len; return -1; }

static int RETRO_CALLCONV vfs_stat(const char *path, int32_t *size)
{
#ifdef _WIN32
   DWORD attr = GetFileAttributesA(path);
   if (attr == INVALID_FILE_ATTRIBUTES)
      return 0;
   if (attr & FILE_ATTRIBUTE_DIRECTORY)
      return RETRO_VFS_STAT_IS_VALID | RETRO_VFS_STAT_IS_DIRECTORY;
#else
   struct stat st;
   if (stat(path, &st))
      return 0;
   if (S_ISDIR(st.st_mode))
      return RETRO_VFS_STAT_IS_VALID | RETRO_VFS_STAT_IS_DIRECTORY;
#endif
   {
      FILE *f = fopen(path, "rb");
      if (!f)
         return 0;
      fseek(f, 0, SEEK_END);
      if (size)
         *size = (int32_t)ftell(f);
      fclose(f);
   }
   return RETRO_VFS_STAT_IS_VALID;
}

static int RETRO_CALLCONV vfs_mkdir(const char *dir)
{
#ifdef _WIN32
   if (CreateDirectoryA(dir, NULL))
      return 0;
   return GetLastError() == ERROR_ALREADY_EXISTS ? -2 : -1;
#else
   if (!mkdir(dir, 0755))
      return 0;
   return errno == EEXIST ? -2 : -1;
#endif
}

static struct retro_vfs_dir_handle *RETRO_CALLCONV vfs_opendir(const char *dir, bool hidden) { (void)dir; (void)hidden; return NULL; }
static bool RETRO_CALLCONV vfs_readdir(struct retro_vfs_dir_handle *d) { (void)d; return false; }
static const char *RETRO_CALLCONV vfs_dirent_name(struct retro_vfs_dir_handle *d) { (void)d; return NULL; }
static bool RETRO_CALLCONV vfs_dirent_is_dir(struct retro_vfs_dir_handle *d) { (void)d; return false; }
static int RETRO_CALLCONV vfs_closedir(struct retro_vfs_dir_handle *d) { (void)d; return 0; }

static struct retro_vfs_interface vfs = {
   .get_path = vfs_get_path, .open = vfs_open, .close = vfs_close, .size = vfs_size,
   .tell = vfs_tell, .seek = vfs_seek, .read = vfs_read, .write = vfs_write, .flush = vfs_flush,
   .remove = vfs_remove, .rename = vfs_rename, .truncate = vfs_truncate, .stat = vfs_stat,
   .mkdir = vfs_mkdir, .opendir = vfs_opendir, .readdir = vfs_readdir,
   .dirent_get_name = vfs_dirent_name, .dirent_is_dir = vfs_dirent_is_dir,
   .closedir = vfs_closedir
};

static bool RETRO_CALLCONV env_cb(unsigned cmd, void *data)
{
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
      {
         struct retro_vfs_interface_info *v = data;
         if (v->required_interface_version > 3)
            return false;
         v->required_interface_version = 3;
         v->iface = &vfs;
         return true;
      }
      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((struct retro_log_callback*)data)->log = log_cb;
         return true;
      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
         *(const char**)data = system_dir;
         return true;
      case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
         *(unsigned*)data = 2;
         return true;
      case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
      {
         const struct retro_core_options_v2 *o = data;
         for (const struct retro_core_option_v2_definition *d = o->definitions; d->key; d++)
            declare_opt(d->key, d->default_value, false);
         return true;
      }
      case RETRO_ENVIRONMENT_SET_VARIABLES:
         for (const struct retro_variable *v = data; v->key; v++)
         {
            const char *list = strstr(v->value, "; ");
            const char *bar;
            char def[128];
            list = list ? list + 2 : v->value;
            bar  = strchr(list, '|');
            snprintf(def, sizeof(def), "%.*s", (int)(bar ? (size_t)(bar - list) : strlen(list)), list);
            declare_opt(v->key, def, false);
         }
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE:
      {
         struct retro_variable *v = data;
         int i = find_opt(v->key);
         v->value = i >= 0 ? opts[i].value : NULL;
         return i >= 0;
      }
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool*)data = options_updated;
         options_updated = false;
         return true;
      case RETRO_ENVIRONMENT_SET_VARIABLE:
      {
         const struct retro_variable *v = data;
         int i;
         if (!take_set_variable)
            return false;
         if (!v)
            return true;
         if ((i = find_opt(v->key)) < 0)
            return false;
         snprintf(opts[i].value, sizeof(opts[i].value), "%s", v->value);
         options_updated = true;
         if (!quiet)
            printf("[option] %s = %s\n", v->key, v->value);
         return true;
      }
      case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
         keyboard_cb = ((const struct retro_keyboard_callback*)data)->callback;
         return true;
      case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
         ((struct retro_rumble_interface*)data)->set_rumble_state = rumble_cb;
         return true;
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
         return *(const enum retro_pixel_format*)data == RETRO_PIXEL_FORMAT_XRGB8888;
      case RETRO_ENVIRONMENT_SET_GEOMETRY:
      case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
      {
         const struct retro_system_av_info *av = data;
         geometry_w = av->geometry.base_width;
         geometry_h = av->geometry.base_height;
         return true;
      }
      case RETRO_ENVIRONMENT_SET_MESSAGE:
         if (!quiet)
            printf("[osd] %s\n", ((const struct retro_message*)data)->msg);
         return true;
      case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
      case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
      case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
      case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
      case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
      case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
         return true;
      default:
         return false;
   }
}

static void RETRO_CALLCONV video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
   video_frames++;
   if (!data)
   {
      video_dupes++;
      return;
   }
   if ((size_t)w * h > video_cap)
   {
      video_cap = (size_t)w * h;
      video     = realloc(video, video_cap * sizeof(uint32_t));
   }
   /* The X of XRGB is nothing: two frames are the same if their colours are. */
   for (unsigned y = 0; y < h; y++)
   {
      const uint32_t *src = (const uint32_t*)(const void*)((const uint8_t*)data + y * pitch);
      for (unsigned x = 0; x < w; x++)
         video[(size_t)y * w + x] = src[x] & 0xFFFFFFu;
   }
   video_w   = w;
   video_h   = h;
   video_new = true;
}

static size_t RETRO_CALLCONV batch_cb(const int16_t *data, size_t frames)
{
   if (audio_frames + frames > audio_cap)
   {
      audio_cap = (audio_frames + frames) * 2;
      audio     = realloc(audio, audio_cap * 2 * sizeof(int16_t));
   }
   memcpy(audio + audio_frames * 2, data, frames * 2 * sizeof(int16_t));
   audio_frames += frames;
   return frames;
}

static void RETRO_CALLCONV sample_cb(int16_t l, int16_t r)
{
   int16_t f[2] = { l, r };
   batch_cb(f, 1);
}

static void RETRO_CALLCONV poll_cb(void) {}

/* --press <frame>:<button>[:<frames>] and --key <frame>:<key> */
#define MAX_PRESSES 64
static struct { unsigned frame, frames, id; } presses[MAX_PRESSES];
static unsigned press_count;
static struct { unsigned frame, key; } key_presses[MAX_PRESSES];
static unsigned key_count;
static bool add_press(const char *spec)
{
   static const struct { const char *name; unsigned id; } names[] = {
      { "x", RETRO_DEVICE_ID_JOYPAD_X }, { "up", RETRO_DEVICE_ID_JOYPAD_UP },
      { "down", RETRO_DEVICE_ID_JOYPAD_DOWN }, { "left", RETRO_DEVICE_ID_JOYPAD_LEFT },
      { "right", RETRO_DEVICE_ID_JOYPAD_RIGHT }, { "b", RETRO_DEVICE_ID_JOYPAD_B },
      { "a", RETRO_DEVICE_ID_JOYPAD_A }, { "start", RETRO_DEVICE_ID_JOYPAD_START }
   };
   char name[16] = "";
   unsigned frame = 0, frames = 3;
   if (sscanf(spec, "%u:%15[a-z]:%u", &frame, name, &frames) < 2 || press_count >= MAX_PRESSES)
      return false;
   for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
      if (!strcmp(names[i].name, name))
      {
         presses[press_count].frame  = frame;
         presses[press_count].frames = frames;
         presses[press_count].id     = names[i].id;
         press_count++;
         return true;
      }
   return false;
}

static bool add_key(const char *spec)
{
   unsigned frame = 0;
   const char *colon = strchr(spec, ':');
   unsigned key = 0;
   if (!colon || sscanf(spec, "%u", &frame) != 1 || key_count >= MAX_PRESSES)
      return false;
   colon++;
   if (!strcmp(colon, "backslash"))  key = RETROK_BACKSLASH;
   else if (!strcmp(colon, "["))     key = RETROK_LEFTBRACKET;
   else if (!strcmp(colon, "]"))     key = RETROK_RIGHTBRACKET;
   else if (!strcmp(colon, "-"))     key = RETROK_MINUS;
   else if (!strcmp(colon, "="))     key = RETROK_EQUALS;
   else if (colon[0] >= '0' && colon[0] <= '9' && !colon[1]) key = RETROK_0 + (unsigned)(colon[0] - '0');
   else
      return false;
   key_presses[key_count].frame = frame;
   key_presses[key_count].key   = key;
   key_count++;
   return true;
}

/* --poke <frame>:<byte>:<value> and --poke <frame>:<byte>:@<byte>: before a frame, a byte of
 * the console's memory gets a value, or that of another byte. It stages what a game comes
 * to but rarely: Pac-Man where a ghost is. */
static struct { unsigned frame, byte, value; bool copy; } pokes[MAX_PRESSES];
static unsigned poke_count;
static bool add_poke(const char *spec)
{
   unsigned frame = 0, byte = 0, value = 0;
   bool copy = false;
   if (poke_count >= MAX_PRESSES)
      return false;
   if (sscanf(spec, "%u:%u:@%u", &frame, &byte, &value) == 3)
      copy = true;
   else if (sscanf(spec, "%u:%u:%u", &frame, &byte, &value) != 3)
      return false;
   pokes[poke_count].frame = frame;
   pokes[poke_count].byte  = byte;
   pokes[poke_count].value = value;
   pokes[poke_count].copy  = copy;
   poke_count++;
   return true;
}

static void poke_memory(void)
{
   uint8_t *ram = poke_count && core.get_memory_data
         ? (uint8_t*)core.get_memory_data(RETRO_MEMORY_SYSTEM_RAM) : NULL;
   size_t n = ram ? core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM) : 0;
   for (unsigned i = 0; i < poke_count; i++)
      if (pokes[i].frame == frame_no && pokes[i].byte < n)
         ram[pokes[i].byte] = pokes[i].copy
               ? (pokes[i].value < n ? ram[pokes[i].value] : 0) : (uint8_t)pokes[i].value;
}

static void press_keys(void)
{
   for (unsigned i = 0; keyboard_cb && i < key_count; i++)
      if (key_presses[i].frame == frame_no)
      {
         keyboard_cb(true, key_presses[i].key, 0, 0);
         keyboard_cb(false, key_presses[i].key, 0, 0);
      }
}

/* Left, right and fire, each held for a while, from a fixed sequence. */
static int16_t RETRO_CALLCONV input_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
   uint32_t r;
   (void)index;
   if (port == 0 && device == RETRO_DEVICE_JOYPAD)
      for (unsigned i = 0; i < press_count; i++)
         if (presses[i].id == id && frame_no >= presses[i].frame
               && frame_no < presses[i].frame + presses[i].frames)
            return 1;
   if (roaming && port == 0 && device == RETRO_DEVICE_JOYPAD)
   {
      /* --roam: one of the four directions, for as long as it takes to get somewhere. */
      r = (frame_no / 24 + roam_seed * 7919u) * 2654435761u;
      r ^= r >> 15;
      switch (id)
      {
         case RETRO_DEVICE_ID_JOYPAD_UP:    return (r & 3) == 0;
         case RETRO_DEVICE_ID_JOYPAD_DOWN:  return (r & 3) == 1;
         case RETRO_DEVICE_ID_JOYPAD_LEFT:  return (r & 3) == 2;
         case RETRO_DEVICE_ID_JOYPAD_RIGHT: return (r & 3) == 3;
         case RETRO_DEVICE_ID_JOYPAD_B:     return frame_no > 50 && frame_no < 60;
         case RETRO_DEVICE_ID_JOYPAD_START: return frame_no > 30 && frame_no < 40;
         default:                           return 0;
      }
   }
   if (!scripted_input || port != 0 || device != RETRO_DEVICE_JOYPAD)
      return 0;
   r = (frame_no / 12) * 2654435761u;
   r ^= r >> 15;
   switch (id)
   {
      case RETRO_DEVICE_ID_JOYPAD_LEFT:  return (r & 3) == 1;
      case RETRO_DEVICE_ID_JOYPAD_RIGHT: return (r & 3) == 2;
      case RETRO_DEVICE_ID_JOYPAD_B:     return (r >> 2) & 1;
      case RETRO_DEVICE_ID_JOYPAD_START: return frame_no > 30 && frame_no < 40;
      default:                           return 0;
   }
}

static bool open_core(const char *path)
{
   void *h = LOAD(path);
   if (!h)
   {
      printf("cannot load %s\n", path);
      return false;
   }
#define GET(f, n) core.f = SYM(h, n); if (!core.f) { printf("missing %s\n", n); return false; }
   GET(set_environment, "retro_set_environment");
   GET(set_video_refresh, "retro_set_video_refresh");
   GET(set_audio_sample, "retro_set_audio_sample");
   GET(set_audio_sample_batch, "retro_set_audio_sample_batch");
   GET(set_input_poll, "retro_set_input_poll");
   GET(set_input_state, "retro_set_input_state");
   GET(init, "retro_init");
   GET(deinit, "retro_deinit");
   GET(get_system_info, "retro_get_system_info");
   GET(get_system_av_info, "retro_get_system_av_info");
   GET(run, "retro_run");
   GET(serialize_size, "retro_serialize_size");
   GET(serialize, "retro_serialize");
   GET(unserialize, "retro_unserialize");
   GET(get_memory_data, "retro_get_memory_data");
   GET(get_memory_size, "retro_get_memory_size");
   GET(load_game, "retro_load_game");
   GET(unload_game, "retro_unload_game");
#undef GET
   core.capture_enable = SYM(h, "retro_proteus_capture_enable");
   core.capture        = SYM(h, "retro_proteus_capture");
   return true;
}

static void write_bmp(const char *path, const uint32_t *px, unsigned w, unsigned h, unsigned sx, unsigned sy)
{
   unsigned ow = w * sx, oh = h * sy;
   unsigned row = (ow * 3 + 3) & ~3u;
   uint8_t head[54] = { 'B', 'M' };
   uint8_t *line = calloc(1, row);
   FILE *f = fopen(path, "wb");
   uint32_t v;
   if (!f || !line)
   {
      printf("cannot write %s\n", path);
      free(line);
      return;
   }
   v = 54 + row * oh; memcpy(head + 2, &v, 4);
   v = 54;            memcpy(head + 10, &v, 4);
   v = 40;            memcpy(head + 14, &v, 4);
   v = ow;            memcpy(head + 18, &v, 4);
   v = oh;            memcpy(head + 22, &v, 4);
   head[26] = 1;
   head[28] = 24;
   v = row * oh;      memcpy(head + 34, &v, 4);
   fwrite(head, 1, 54, f);
   for (unsigned y = oh; y-- > 0;)
   {
      for (unsigned x = 0; x < ow; x++)
      {
         uint32_t c = px[(size_t)(y / sy) * w + x / sx];
         line[x * 3]     = (uint8_t)c;
         line[x * 3 + 1] = (uint8_t)(c >> 8);
         line[x * 3 + 2] = (uint8_t)(c >> 16);
      }
      fwrite(line, 1, row, f);
   }
   fclose(f);
   free(line);
}

static void write_wav(const char *path, const int16_t *data, size_t frames, unsigned rate)
{
   FILE *f = fopen(path, "wb");
   uint32_t u32, bytes = (uint32_t)(frames * 4);
   uint16_t u16;
   if (!f)
      return;
   fwrite("RIFF", 1, 4, f); u32 = 36 + bytes; fwrite(&u32, 4, 1, f);
   fwrite("WAVEfmt ", 1, 8, f); u32 = 16; fwrite(&u32, 4, 1, f);
   u16 = 1; fwrite(&u16, 2, 1, f); u16 = 2; fwrite(&u16, 2, 1, f);
   u32 = rate; fwrite(&u32, 4, 1, f); u32 = rate * 4; fwrite(&u32, 4, 1, f);
   u16 = 4; fwrite(&u16, 2, 1, f); u16 = 16; fwrite(&u16, 2, 1, f);
   fwrite("data", 1, 4, f); u32 = bytes; fwrite(&u32, 4, 1, f);
   fwrite(data, 4, frames, f);
   fclose(f);
}

/* ---------------------------------------------------------------------------
 * Capture checks
 * ------------------------------------------------------------------------- */

static struct
{
   unsigned frames;
   unsigned missing;          /* retro_run completed a frame, the capture had none */
   unsigned size_mismatch;
   uint64_t pixel_mismatch;   /* palette[winner] is not the video pixel */
   uint64_t color_mismatch;   /* the top object's colour plane is not the winner */
   uint64_t tagged[8];
   uint64_t writes;
   uint64_t dropped;
   uint64_t samples;
   uint32_t last_serial;
   unsigned serial_repeats;
} cap;

static int top_layer(uint8_t tags, unsigned priority)
{
   static const struct { uint8_t tag; uint8_t layer; } order[3][6] = {
      { { PXC_P0, PXC_L_P0 }, { PXC_M0, PXC_L_M0 }, { PXC_P1, PXC_L_P1 }, { PXC_M1, PXC_L_M1 },
        { PXC_PF, PXC_L_PF }, { PXC_BL, PXC_L_BL } },
      { { PXC_PF, PXC_L_PF }, { PXC_BL, PXC_L_BL }, { PXC_P0, PXC_L_P0 }, { PXC_M0, PXC_L_M0 },
        { PXC_P1, PXC_L_P1 }, { PXC_M1, PXC_L_M1 } },
      { { PXC_P0, PXC_L_P0 }, { PXC_M0, PXC_L_M0 }, { PXC_PF, PXC_L_PF }, { PXC_P1, PXC_L_P1 },
        { PXC_M1, PXC_L_M1 }, { PXC_BL, PXC_L_BL } },
   };
   if (priority > 2)
      priority = 0;
   for (unsigned i = 0; i < 6; i++)
      if (tags & order[priority][i].tag)
         return order[priority][i].layer;
   return PXC_L_BK;
}

/* The objects Proteus finds in the captured frames (fx_track.c). */
static px_objects objects;
static struct
{
   unsigned least, most;      /* instances in a frame, ghosts aside */
   unsigned ghost_frames;     /* frames with an object drawn from its track */
   unsigned ghosts;
   uint32_t first_id, last_id;
} obj = { ~0u, 0, 0, 0, 0, 0 };

static FILE *objects_file;   /* --objects: every frame's instances */
static FILE *sound_file;     /* --sound: the audio registers whenever they change */

/* The six audio registers as the game left them, and a line for every write to one. */
static void note_sound(const struct pxc_frame *f)
{
   static uint8_t reg[6];   /* AUDC0 AUDC1 AUDF0 AUDF1 AUDV0 AUDV1 */
   static const char *names[6] = { "C0", "C1", "F0", "F1", "V0", "V1" };
   bool changed = false;
   for (uint32_t i = 0; i < f->write_count; i++)
   {
      const struct pxc_regwrite *w = &f->writes[i];
      if (w->reg < 0x15 || w->reg > 0x1A)
         continue;
      if (reg[w->reg - 0x15] != (w->value & (w->reg < 0x17 ? 0x0F : w->reg < 0x19 ? 0x1F : 0x0F)))
      {
         reg[w->reg - 0x15] = w->value & (w->reg < 0x17 ? 0x0F : w->reg < 0x19 ? 0x1F : 0x0F);
         fprintf(sound_file, "%5u line %3u  %s = %2u\n", frame_no, w->scanline,
               names[w->reg - 0x15], reg[w->reg - 0x15]);
         changed = true;
      }
   }
   if (changed)
      fprintf(sound_file, "%5u        voice 0: C %2u F %2u V %2u   voice 1: C %2u F %2u V %2u\n",
            frame_no, reg[0], reg[2], reg[4], reg[1], reg[3], reg[5]);
}

static void check_objects(const struct pxc_frame *f)
{
   static const char *names[PXC_LAYERS] = { "bk", "pf", "bl", "p0", "m0", "p1", "m1" };
   unsigned real = 0, ghosts = 0;
   if (!objects.next_id)
      px_objects_init(&objects);
   if (!obj.first_id)
      obj.first_id = objects.next_id;
   px_objects_update(&objects, f, true);
   if (objects_file)
   {
      fprintf(objects_file, "frame %u: %u\n", frame_no, objects.count);
      for (unsigned i = 0; i < objects.count; i++)
      {
         const px_instance *in = &objects.inst[i];
         fprintf(objects_file, "  %s copy %u at %3d,%3d size %2ux%2u colour %02X shape %08X track %u%s\n",
               names[in->cls], in->copy, in->x, in->y, in->w, in->h, in->color, in->hash,
               in->track, in->ghost ? " (from its track)" : "");
      }
   }
   for (unsigned i = 0; i < objects.count; i++)
      if (objects.inst[i].ghost)
         ghosts++;
      else
         real++;
   if (real < obj.least) obj.least = real;
   if (real > obj.most)  obj.most = real;
   obj.ghosts += ghosts;
   if (ghosts)
      obj.ghost_frames++;
   obj.last_id = objects.next_id;
}

static void check_capture(const struct pxc_frame *f)
{
   if (!f)
   {
      cap.missing++;
      return;
   }
   check_objects(f);
   if (sound_file)
      note_sound(f);
   cap.frames++;
   if (f->frame_serial == cap.last_serial)
      cap.serial_repeats++;
   cap.last_serial = f->frame_serial;
   cap.writes  += f->write_count;
   cap.dropped += f->writes_dropped;
   cap.samples += f->audio.count;

   if (!f->tags)
      return;
   if (f->width != video_w || f->height != video_h)
   {
      cap.size_mismatch++;
      return;
   }
   for (size_t i = 0; i < (size_t)f->width * f->height; i++)
   {
      uint8_t tags = f->tags[i];
      if ((video[i] & 0xFFFFFF) != f->palette[f->winner[i]])
         cap.pixel_mismatch++;
      for (unsigned bit = 0; bit < 8; bit++)
         if (tags & (1u << bit))
            cap.tagged[bit]++;
      if (!(tags & PXC_BLANK)
            && f->color[top_layer(tags, PXC_AUX_PRIORITY(f->aux[i]))][i] != f->winner[i])
         cap.color_mismatch++;
   }
}

static const uint32_t layer_tint[PXC_LAYERS] = {
   0x101018, 0x2060C0, 0xF0F0F0, 0xF04040, 0xF0A020, 0x40D040, 0x20D0D0
};
static const char *layer_name[PXC_LAYERS] = { "bk", "pf", "bl", "p0", "m0", "p1", "m1" };
static const uint8_t layer_tag[PXC_LAYERS] = { 0, PXC_PF, PXC_BL, PXC_P0, PXC_M0, PXC_P1, PXC_M1 };

static void write_layers(const char *prefix, const struct pxc_frame *f)
{
   size_t n = (size_t)f->width * f->height;
   uint32_t *img = malloc(n * sizeof(uint32_t));
   char path[1024];
   if (!img || !f->tags)
   {
      free(img);
      return;
   }

   /* Every object in its own flat colour; where two overlap the colours add up. */
   for (size_t i = 0; i < n; i++)
   {
      unsigned r = 0x10, g = 0x10, b = 0x18;
      if (f->tags[i] & PXC_BLANK)
         r = g = b = 0;
      for (unsigned l = 1; l < PXC_LAYERS; l++)
         if (f->tags[i] & layer_tag[l])
         {
            r += (layer_tint[l] >> 16) & 0xFF;
            g += (layer_tint[l] >> 8) & 0xFF;
            b += layer_tint[l] & 0xFF;
         }
      img[i] = ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
   }
   snprintf(path, sizeof(path), "%s_tags.bmp", prefix);
   write_bmp(path, img, f->width, f->height, 4, 2);

   /* Each layer alone, in its own colours, on magenta where it has nothing. */
   for (unsigned l = 0; l < PXC_LAYERS; l++)
   {
      for (size_t i = 0; i < n; i++)
      {
         bool there = !(f->tags[i] & PXC_BLANK) && (l == PXC_L_BK || (f->tags[i] & layer_tag[l]));
         img[i] = there ? f->palette[f->color[l][i]] : 0x400040;
      }
      snprintf(path, sizeof(path), "%s_%s.bmp", prefix, layer_name[l]);
      write_bmp(path, img, f->width, f->height, 4, 2);
   }

   /* Where the players are scanning, by copy. */
   for (size_t i = 0; i < n; i++)
   {
      unsigned c0 = PXC_AUX_P0_COPY(f->aux[i]), c1 = PXC_AUX_P1_COPY(f->aux[i]);
      img[i] = (c0 ? (0x40 + c0 * 0x30) << 16 : 0) | (c1 ? (0x40 + c1 * 0x30) << 8 : 0);
   }
   snprintf(path, sizeof(path), "%s_copies.bmp", prefix);
   write_bmp(path, img, f->width, f->height, 4, 2);
   free(img);
}

int main(int argc, char **argv)
{
   struct retro_game_info game = { NULL, NULL, 0, NULL };
   struct retro_system_info info;
   struct retro_system_av_info av;
   const char *hashes_path = NULL, *bmp_path = NULL, *layers_path = NULL, *wav_path = NULL;
   const char *state_in = NULL, *state_out = NULL;
   FILE *ram_file = NULL;
   bool want_capture = false, want_state = false, native = false, show_options = false;
   double time_sum = 0.0, time_max = 0.0, slow_ms = 0.0;
   double tones[8];
   unsigned tone_count = 0;
   unsigned frames;
   uint64_t video_hash = FNV_START;
   FILE *hashes = NULL;
   void *rom = NULL;
   long rom_size = 0;
   FILE *f;
   int failures = 0;

   setvbuf(stdout, NULL, _IONBF, 0);
   if (argc < 4)
   {
      printf("usage: harness2600 <core> <rom> <frames> [options]\n");
      return 2;
   }
   frames = (unsigned)atoi(argv[3]);

   for (int i = 4; i < argc; i++)
   {
      if (!strcmp(argv[i], "--opt") && i + 1 < argc)
      {
         char key[64];
         const char *eq = strchr(argv[++i], '=');
         if (!eq)
            continue;
         snprintf(key, sizeof(key), "%.*s", (int)(eq - argv[i]), argv[i]);
         declare_opt(key, eq + 1, true);
      }
      else if (!strcmp(argv[i], "--input"))   scripted_input = true;
      else if (!strcmp(argv[i], "--roam"))    roaming = true;
      else if (!strcmp(argv[i], "--poke") && i + 1 < argc)
      {
         if (!add_poke(argv[++i]))
         {
            printf("--poke %s: not <frame>:<byte>:<value> or <frame>:<byte>:@<byte>\n", argv[i]);
            return 2;
         }
      }
      else if (!strcmp(argv[i], "--seed") && i + 1 < argc) roam_seed = (unsigned)atoi(argv[++i]);
      else if (!strcmp(argv[i], "--capture")) want_capture = true;
      else if (!strcmp(argv[i], "--state"))   want_state = true;
      else if (!strcmp(argv[i], "--native"))  native = true;
      else if (!strcmp(argv[i], "--slow") && i + 1 < argc) slow_ms = atof(argv[++i]);
      else if (!strcmp(argv[i], "--quiet"))   quiet = true;
      else if (!strcmp(argv[i], "--hashes") && i + 1 < argc) hashes_path = argv[++i];
      else if (!strcmp(argv[i], "--bmp") && i + 1 < argc)    bmp_path = argv[++i];
      else if (!strcmp(argv[i], "--layers") && i + 1 < argc) layers_path = argv[++i];
      else if (!strcmp(argv[i], "--wav") && i + 1 < argc)    wav_path = argv[++i];
      else if (!strcmp(argv[i], "--press") && i + 1 < argc)
      {
         if (!add_press(argv[++i]))
         {
            printf("bad --press %s\n", argv[i]);
            return 2;
         }
      }
      else if (!strcmp(argv[i], "--key") && i + 1 < argc)
      {
         if (!add_key(argv[++i]))
         {
            printf("bad --key %s\n", argv[i]);
            return 2;
         }
      }
      else if (!strcmp(argv[i], "--no-set-variable")) take_set_variable = false;
      else if (!strcmp(argv[i], "--show-options"))   show_options = true;
      else if (!strcmp(argv[i], "--objects") && i + 1 < argc)   objects_file = fopen(argv[++i], "w");
      else if (!strcmp(argv[i], "--sound") && i + 1 < argc)     sound_file = fopen(argv[++i], "w");
      else if (!strcmp(argv[i], "--tone") && i + 1 < argc && tone_count < 8)
         tones[tone_count++] = atof(argv[++i]);
      else if (!strcmp(argv[i], "--ram") && i + 1 < argc)       ram_file = fopen(argv[++i], "w");
      else if (!strcmp(argv[i], "--state-in") && i + 1 < argc)  state_in = argv[++i];
      else if (!strcmp(argv[i], "--state-out") && i + 1 < argc) state_out = argv[++i];
      else if (!strcmp(argv[i], "--sysdir") && i + 1 < argc) system_dir = argv[++i];
      else
      {
         printf("unknown option %s\n", argv[i]);
         return 2;
      }
   }

   if (!open_core(argv[1]))
      return 1;
   if (want_capture && (!core.capture_enable || !core.capture))
   {
      printf("%s does not export the capture interface\n", argv[1]);
      return 1;
   }

   core.set_environment(env_cb);
   core.set_video_refresh(video_cb);
   core.set_audio_sample(sample_cb);
   core.set_audio_sample_batch(batch_cb);
   core.set_input_poll(poll_cb);
   core.set_input_state(input_cb);
   core.init();
   core.get_system_info(&info);
   printf("core   %s %s\n", info.library_name, info.library_version);

   if (!(f = fopen(argv[2], "rb")) || fseek(f, 0, SEEK_END) || (rom_size = ftell(f)) <= 0
         || fseek(f, 0, SEEK_SET) || !(rom = malloc((size_t)rom_size))
         || fread(rom, 1, (size_t)rom_size, f) != (size_t)rom_size)
   {
      printf("cannot read %s\n", argv[2]);
      return 1;
   }
   fclose(f);
   game.path = argv[2];
   game.data = rom;
   game.size = (size_t)rom_size;

   if (want_capture)
      core.capture_enable(PXC_ENABLE_VIDEO | PXC_ENABLE_WRITES | PXC_ENABLE_AUDIO);
   if (!core.load_game(&game))
   {
      printf("load_game failed\n");
      return 1;
   }
   core.get_system_av_info(&av);
   printf("av     %ux%u (max %ux%u) aspect %.4f, %.3f fps, %.1f Hz\n",
         av.geometry.base_width, av.geometry.base_height, av.geometry.max_width,
         av.geometry.max_height, av.geometry.aspect_ratio, av.timing.fps, av.timing.sample_rate);

   /* Stella starts a game with memory and registers of chance. A state makes runs the same:
    * it is loaded once the core has run, as a frontend does. */
   if (state_in)
   {
      FILE *sf = fopen(state_in, "rb");
      long n = 0;
      void *state = NULL;
      core.run();
      core.run();
      if (!sf || fseek(sf, 0, SEEK_END) || (n = ftell(sf)) <= 0 || fseek(sf, 0, SEEK_SET)
            || !(state = malloc((size_t)n)) || fread(state, 1, (size_t)n, sf) != (size_t)n
            || !core.unserialize(state, (size_t)n))
      {
         printf("cannot load the state %s\n", state_in);
         return 1;
      }
      fclose(sf);
      free(state);
      video_frames = video_dupes = 0;
      audio_frames = 0;
   }

   if (hashes_path)
      hashes = fopen(hashes_path, "w");

   for (frame_no = 0; frame_no < frames; frame_no++)
   {
      double ms;
      video_new = false;
      press_keys();
      if (slow_ms > 0.0)
      {
         /* Busy for a while: shows whether a core's frames depend on the time they take. */
         double until = now_ms() + slow_ms;
         while (now_ms() < until)
            ;
      }
      poke_memory();
      ms = now_ms();
      core.run();
      ms = now_ms() - ms;
      time_sum += ms;
      if (ms > time_max && frame_no > 10)
         time_max = ms;
      if (ram_file && core.get_memory_data)
      {
         const uint8_t *ram = core.get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
         size_t n = core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
         fprintf(ram_file, "%5u", frame_no);
         for (size_t k = 0; ram && k < n; k++)
            fprintf(ram_file, " %02X", ram[k]);
         fprintf(ram_file, "\n");
      }
      if (video_new && native && video_w > PXC_W && video_w % PXC_W == 0)
      {
         /* The middle of every block of pixels that stands for one of the core's. */
         unsigned sx = video_w / PXC_W, sy = sx > 1 ? sx / 2 : 1;
         unsigned nh = video_h / sy;
         for (unsigned y = 0; y < nh; y++)
            for (unsigned x = 0; x < PXC_W; x++)
               video[(size_t)y * PXC_W + x] =
                     video[(size_t)(y * sy + sy / 2) * video_w + x * sx + sx / 2];
         video_w = PXC_W;
         video_h = nh;
      }
      if (video_new)
      {
         uint64_t h = fnv(FNV_START, video, (size_t)video_w * video_h * sizeof(uint32_t));
         video_hash = fnv(video_hash, &h, sizeof(h));
         video_hash = fnv(video_hash, &video_w, sizeof(video_w));
         video_hash = fnv(video_hash, &video_h, sizeof(video_h));
         if (hashes)
            fprintf(hashes, "%u %ux%u %016llx\n", frame_no, video_w, video_h, (unsigned long long)h);
         if (want_capture)
            check_capture(core.capture(PXC_ABI_VERSION));
      }
      else if (hashes)
         fprintf(hashes, "%u -\n", frame_no);
   }
   if (hashes)
      fclose(hashes);

   printf("video  %016llx  %u frames (%u repeated), last %ux%u, geometry %ux%u\n",
         (unsigned long long)video_hash, video_frames, video_dupes, video_w, video_h,
         geometry_w, geometry_h);
   printf("audio  %016llx  %u samples\n",
         (unsigned long long)fnv(FNV_START, audio, audio_frames * 2 * sizeof(int16_t)),
         (unsigned)audio_frames);
   {
      /* Past the first second, where filters settle. */
      double l = 0, r = 0, d = 0, mean_l = 0, mean_r = 0;
      size_t from = audio_frames > 40000 ? 32000 : 0, n = audio_frames - from;
      int peak = 0;
      for (size_t i = from; i < audio_frames; i++)
      {
         double a = audio[i * 2], b = audio[i * 2 + 1];
         l += a * a;
         r += b * b;
         d += (a - b) * (a - b);
         mean_l += a;
         mean_r += b;
         if (abs(audio[i * 2]) > peak) peak = abs(audio[i * 2]);
         if (abs(audio[i * 2 + 1]) > peak) peak = abs(audio[i * 2 + 1]);
      }
      if (n)
         printf("sound  rms left %.0f right %.0f, of their difference %.0f; mean left %.0f right %.0f; peak %d\n",
               sqrt(l / n), sqrt(r / n), sqrt(d / n), mean_l / n, mean_r / n, peak);
   }
   printf("time   %.3f ms a frame on average, %.3f ms at most\n",
         frames ? time_sum / frames : 0.0, time_max);
   printf("rumble strong %u times, at most %u, for %u frames; weak %u times, at most %u, for %u frames\n",
         rumble.starts[0], rumble.most[0], rumble.frames[0], rumble.starts[1], rumble.most[1],
         rumble.frames[1]);
   for (unsigned t = 0; t < tone_count; t++)
   {
      /* How much of a pitch is in the sound (Goertzel), over all of it and both sides. */
      double w = 2.0 * 3.14159265358979 * tones[t] / av.timing.sample_rate;
      double c = 2.0 * cos(w), s0, s1 = 0.0, s2 = 0.0;
      for (size_t i = 0; i < audio_frames; i++)
      {
         s0 = (audio[i * 2] + audio[i * 2 + 1]) * 0.5 + c * s1 - s2;
         s2 = s1;
         s1 = s0;
      }
      printf("tone   %.1f Hz: %.0f\n", tones[t],
            audio_frames ? sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / (double)audio_frames * 2.0 : 0.0);
   }
   if (show_options)
      for (unsigned i = 0; i < opt_count; i++)
         if (!strncmp(opts[i].key, "proteus_", 8))
            printf("option %s = %s\n", opts[i].key, opts[i].value);

   if (want_state || state_out)
   {
      size_t n = core.serialize_size();
      void *state = malloc(n ? n : 1);
      if (n && state && core.serialize(state, n))
      {
         FILE *sf = state_out ? fopen(state_out, "wb") : NULL;
         printf("state  %016llx  %u bytes\n", (unsigned long long)fnv(FNV_START, state, n), (unsigned)n);
         if (sf)
         {
            fwrite(state, 1, n, sf);
            fclose(sf);
         }
         else if (state_out)
         {
            printf("cannot write %s\n", state_out);
            failures++;
         }
      }
      else
      {
         printf("state  unavailable\n");
         if (state_out)
            failures++;
      }
      free(state);
   }

   if (want_capture)
   {
      static const char *names[8] = { "P0", "M0", "P1", "M1", "BL", "PF", "blank", "-" };
      const struct pxc_frame *last = core.capture(PXC_ABI_VERSION);
      printf("capture %u frames, %u without capture, %u repeated serials, %u of another size\n",
            cap.frames, cap.missing, cap.serial_repeats, cap.size_mismatch);
      printf("capture pixels that differ from the video: %llu\n", (unsigned long long)cap.pixel_mismatch);
      printf("capture pixels whose top layer differs:    %llu\n", (unsigned long long)cap.color_mismatch);
      printf("capture tagged pixels:");
      for (unsigned i = 0; i < 7; i++)
         printf(" %s %llu", names[i], (unsigned long long)cap.tagged[i]);
      printf("\n");
      printf("capture register writes %llu (%llu dropped), voice samples %llu\n",
            (unsigned long long)cap.writes, (unsigned long long)cap.dropped,
            (unsigned long long)cap.samples);
      if (last)
         printf("capture last frame: serial %u, %ux%u of %u lines, %s, %u writes, %u samples at %.1f Hz\n",
               last->frame_serial, last->width, last->height, last->scanlines_total,
               last->pal ? "50 Hz" : "60 Hz", last->write_count, last->audio.count,
               last->audio.rate_x1000 / 1000.0);
      if (cap.frames == 0 || cap.missing || cap.size_mismatch || cap.pixel_mismatch
            || cap.color_mismatch || cap.serial_repeats)
      {
         printf("capture FAIL\n");
         failures++;
      }
      else
         printf("capture ok\n");
      printf("objects %u to %u in a frame, %u tracks in all; %u drawn from their tracks in %u frames\n",
            obj.least, obj.most, obj.last_id - obj.first_id, obj.ghosts, obj.ghost_frames);
      if (layers_path && last)
         write_layers(layers_path, last);
   }

   if (bmp_path && video)
   {
      /* Atari pixels are about twice as wide as high; keep wide frames as they are. */
      unsigned sx = video_w <= 320 ? 640 / video_w : 1, sy = video_w <= 320 ? 2 : 1;
      write_bmp(bmp_path, video, video_w, video_h, sx ? sx : 1, sy);
   }
   if (wav_path)
      write_wav(wav_path, audio, audio_frames, (unsigned)(av.timing.sample_rate + 0.5));

   core.unload_game();
   core.deinit();
   free(rom);
   return failures ? 1 : 0;
}
