/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifdef ENABLE_SHADER_CACHE

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "zlib.h"

#ifdef HAVE_ZSTD
#include "zstd.h"
#endif

/* 3 is the recomended level, with 22 as the absolute maximum */
#define ZSTD_COMPRESSION_LEVEL 3

/* From the zlib docs:
 *    "If the memory is available, buffers sizes on the order of 128K or 256K
 *    bytes should be used."
 */
#define BUFSIZE 256 * 1024

static ssize_t
write_all(int fd, const void *buf, size_t count);

/**
 * Compresses cache entry in memory and writes it to disk. Returns the size
 * of the data written to disk.
 */
static size_t
deflate_and_write_to_disk(const void *in_data, size_t in_data_size, int dest,
                          const char *filename)
{
#ifdef HAVE_ZSTD
   /* from the zstd docs (https://facebook.github.io/zstd/zstd_manual.html):
    * compression runs faster if `dstCapacity` >= `ZSTD_compressBound(srcSize)`.
    */
   size_t out_size = ZSTD_compressBound(in_data_size);
   void * out = malloc(out_size);

   size_t ret = ZSTD_compress(out, out_size, in_data, in_data_size,
                              ZSTD_COMPRESSION_LEVEL);
   if (ZSTD_isError(ret)) {
      free(out);
      return 0;
   }
   ssize_t written = write_all(dest, out, ret);
   if (written == -1) {
      free(out);
      return 0;
   }
   free(out);
   return ret;
#else
   unsigned char *out;

   /* allocate deflate state */
   z_stream strm;
   strm.zalloc = Z_NULL;
   strm.zfree = Z_NULL;
   strm.opaque = Z_NULL;
   strm.next_in = (uint8_t *) in_data;
   strm.avail_in = in_data_size;

   int ret = deflateInit(&strm, Z_BEST_COMPRESSION);
   if (ret != Z_OK)
       return 0;

   /* compress until end of in_data */
   size_t compressed_size = 0;
   int flush;

   out = malloc(BUFSIZE * sizeof(unsigned char));
   if (out == NULL)
      return 0;

   do {
      int remaining = in_data_size - BUFSIZE;
      flush = remaining > 0 ? Z_NO_FLUSH : Z_FINISH;
      in_data_size -= BUFSIZE;

      /* Run deflate() on input until the output buffer is not full (which
       * means there is no more data to deflate).
       */
      do {
         strm.avail_out = BUFSIZE;
         strm.next_out = out;

         ret = deflate(&strm, flush);    /* no bad return value */
         assert(ret != Z_STREAM_ERROR);  /* state not clobbered */

         size_t have = BUFSIZE - strm.avail_out;
         compressed_size += have;

         ssize_t written = write_all(dest, out, have);
         if (written == -1) {
            (void)deflateEnd(&strm);
            free(out);
            return 0;
         }
      } while (strm.avail_out == 0);

      /* all input should be used */
      assert(strm.avail_in == 0);

   } while (flush != Z_FINISH);

   /* stream should be complete */
   assert(ret == Z_STREAM_END);

   /* clean up and return */
   (void)deflateEnd(&strm);
   free(out);
   return compressed_size;
# endif
}

/**
 * Decompresses cache entry, returns true if successful.
 */
static bool
inflate_cache_data(uint8_t *in_data, size_t in_data_size,
                   uint8_t *out_data, size_t out_data_size)
{
#ifdef HAVE_ZSTD
   size_t ret = ZSTD_decompress(out_data, out_data_size, in_data, in_data_size);
   return !ZSTD_isError(ret);
#else
   z_stream strm;

   /* allocate inflate state */
   strm.zalloc = Z_NULL;
   strm.zfree = Z_NULL;
   strm.opaque = Z_NULL;
   strm.next_in = in_data;
   strm.avail_in = in_data_size;
   strm.next_out = out_data;
   strm.avail_out = out_data_size;

   int ret = inflateInit(&strm);
   if (ret != Z_OK)
      return false;

   ret = inflate(&strm, Z_NO_FLUSH);
   assert(ret != Z_STREAM_ERROR);  /* state not clobbered */

   /* Unless there was an error we should have decompressed everything in one
    * go as we know the uncompressed file size.
    */
   if (ret != Z_STREAM_END) {
      (void)inflateEnd(&strm);
      return false;
   }
   assert(strm.avail_out == 0);

   /* clean up and return */
   (void)inflateEnd(&strm);
   return true;
#endif
}

#if DETECT_OS_WINDOWS
/* TODO: implement disk cache support on windows */

#else

#include <dirent.h>
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util/crc32.h"
#include "util/debug.h"
#include "util/disk_cache.h"
#include "util/disk_cache_os.h"
#include "util/ralloc.h"
#include "util/rand_xor.h"

/* Create a directory named 'path' if it does not already exist.
 *
 * Returns: 0 if path already exists as a directory or if created.
 *         -1 in all other cases.
 */
static int
mkdir_if_needed(const char *path)
{
   struct stat sb;

   /* If the path exists already, then our work is done if it's a
    * directory, but it's an error if it is not.
    */
   if (stat(path, &sb) == 0) {
      if (S_ISDIR(sb.st_mode)) {
         return 0;
      } else {
         fprintf(stderr, "Cannot use %s for shader cache (not a directory)"
                         "---disabling.\n", path);
         return -1;
      }
   }

   int ret = mkdir(path, 0755);
   if (ret == 0 || (ret == -1 && errno == EEXIST))
     return 0;

   fprintf(stderr, "Failed to create %s for shader cache (%s)---disabling.\n",
           path, strerror(errno));

   return -1;
}

/* Concatenate an existing path and a new name to form a new path.  If the new
 * path does not exist as a directory, create it then return the resulting
 * name of the new path (ralloc'ed off of 'ctx').
 *
 * Returns NULL on any error, such as:
 *
 *      <path> does not exist or is not a directory
 *      <path>/<name> exists but is not a directory
 *      <path>/<name> cannot be created as a directory
 */
static char *
concatenate_and_mkdir(void *ctx, const char *path, const char *name)
{
   char *new_path;
   struct stat sb;

   if (stat(path, &sb) != 0 || ! S_ISDIR(sb.st_mode))
      return NULL;

   new_path = ralloc_asprintf(ctx, "%s/%s", path, name);

   if (mkdir_if_needed(new_path) == 0)
      return new_path;
   else
      return NULL;
}

/* Given a directory path and predicate function, find the entry with
 * the oldest access time in that directory for which the predicate
 * returns true.
 *
 * Returns: A malloc'ed string for the path to the chosen file, (or
 * NULL on any error). The caller should free the string when
 * finished.
 */
static char *
choose_lru_file_matching(const char *dir_path,
                         bool (*predicate)(const char *dir_path,
                                           const struct stat *,
                                           const char *, const size_t))
{
   DIR *dir;
   struct dirent *entry;
   char *filename;
   char *lru_name = NULL;
   time_t lru_atime = 0;

   dir = opendir(dir_path);
   if (dir == NULL)
      return NULL;

   while (1) {
      entry = readdir(dir);
      if (entry == NULL)
         break;

      struct stat sb;
      if (fstatat(dirfd(dir), entry->d_name, &sb, 0) == 0) {
         if (!lru_atime || (sb.st_atime < lru_atime)) {
            size_t len = strlen(entry->d_name);

            if (!predicate(dir_path, &sb, entry->d_name, len))
               continue;

            char *tmp = realloc(lru_name, len + 1);
            if (tmp) {
               lru_name = tmp;
               memcpy(lru_name, entry->d_name, len + 1);
               lru_atime = sb.st_atime;
            }
         }
      }
   }

   if (lru_name == NULL) {
      closedir(dir);
      return NULL;
   }

   if (asprintf(&filename, "%s/%s", dir_path, lru_name) < 0)
      filename = NULL;

   free(lru_name);
   closedir(dir);

   return filename;
}

/* Is entry a regular file, and not having a name with a trailing
 * ".tmp"
 */
static bool
is_regular_non_tmp_file(const char *path, const struct stat *sb,
                        const char *d_name, const size_t len)
{
   if (!S_ISREG(sb->st_mode))
      return false;

   if (len >= 4 && strcmp(&d_name[len-4], ".tmp") == 0)
      return false;

   return true;
}

/* Returns the size of the deleted file, (or 0 on any error). */
static size_t
unlink_lru_file_from_directory(const char *path)
{
   struct stat sb;
   char *filename;

   filename = choose_lru_file_matching(path, is_regular_non_tmp_file);
   if (filename == NULL)
      return 0;

   if (stat(filename, &sb) == -1) {
      free (filename);
      return 0;
   }

   unlink(filename);
   free (filename);

   return sb.st_blocks * 512;
}

/* Is entry a directory with a two-character name, (and not the
 * special name of ".."). We also return false if the dir is empty.
 */
static bool
is_two_character_sub_directory(const char *path, const struct stat *sb,
                               const char *d_name, const size_t len)
{
   if (!S_ISDIR(sb->st_mode))
      return false;

   if (len != 2)
      return false;

   if (strcmp(d_name, "..") == 0)
      return false;

   char *subdir;
   if (asprintf(&subdir, "%s/%s", path, d_name) == -1)
      return false;
   DIR *dir = opendir(subdir);
   free(subdir);

   if (dir == NULL)
     return false;

   unsigned subdir_entries = 0;
   struct dirent *d;
   while ((d = readdir(dir)) != NULL) {
      if(++subdir_entries > 2)
         break;
   }
   closedir(dir);

   /* If dir only contains '.' and '..' it must be empty */
   if (subdir_entries <= 2)
      return false;

   return true;
}

/* Create the directory that will be needed for the cache file for \key.
 *
 * Obviously, the implementation here must closely match
 * _get_cache_file above.
*/
static void
make_cache_file_directory(struct disk_cache *cache, const cache_key key)
{
   char *dir;
   char buf[41];

   _mesa_sha1_format(buf, key);
   if (asprintf(&dir, "%s/%c%c", cache->path, buf[0], buf[1]) == -1)
      return;

   mkdir_if_needed(dir);
   free(dir);
}

static ssize_t
read_all(int fd, void *buf, size_t count)
{
   char *in = buf;
   ssize_t read_ret;
   size_t done;

   for (done = 0; done < count; done += read_ret) {
      read_ret = read(fd, in + done, count - done);
      if (read_ret == -1 || read_ret == 0)
         return -1;
   }
   return done;
}

static ssize_t
write_all(int fd, const void *buf, size_t count)
{
   const char *out = buf;
   ssize_t written;
   size_t done;

   for (done = 0; done < count; done += written) {
      written = write(fd, out + done, count - done);
      if (written == -1)
         return -1;
   }
   return done;
}

/* Evict least recently used cache item */
void
disk_cache_evict_lru_item(struct disk_cache *cache)
{
   char *dir_path;

   /* With a reasonably-sized, full cache, (and with keys generated
    * from a cryptographic hash), we can choose two random hex digits
    * and reasonably expect the directory to exist with a file in it.
    * Provides pseudo-LRU eviction to reduce checking all cache files.
    */
   uint64_t rand64 = rand_xorshift128plus(cache->seed_xorshift128plus);
   if (asprintf(&dir_path, "%s/%02" PRIx64 , cache->path, rand64 & 0xff) < 0)
      return;

   size_t size = unlink_lru_file_from_directory(dir_path);

   free(dir_path);

   if (size) {
      p_atomic_add(cache->size, - (uint64_t)size);
      return;
   }

   /* In the case where the random choice of directory didn't find
    * something, we choose the least recently accessed from the
    * existing directories.
    *
    * Really, the only reason this code exists is to allow the unit
    * tests to work, (which use an artificially-small cache to be able
    * to force a single cached item to be evicted).
    */
   dir_path = choose_lru_file_matching(cache->path,
                                       is_two_character_sub_directory);
   if (dir_path == NULL)
      return;

   size = unlink_lru_file_from_directory(dir_path);

   free(dir_path);

   if (size)
      p_atomic_add(cache->size, - (uint64_t)size);
}

void
disk_cache_evict_item(struct disk_cache *cache, char *filename)
{
   struct stat sb;
   if (stat(filename, &sb) == -1) {
      free(filename);
      return;
   }

   unlink(filename);
   free(filename);

   if (sb.st_blocks)
      p_atomic_add(cache->size, - (uint64_t)sb.st_blocks * 512);
}

void *
disk_cache_load_item(struct disk_cache *cache, char *filename, size_t *size)
{
   int fd = -1, ret;
   struct stat sb;
   uint8_t *data = NULL;
   uint8_t *uncompressed_data = NULL;
   uint8_t *file_header = NULL;

   fd = open(filename, O_RDONLY | O_CLOEXEC);
   if (fd == -1)
      goto fail;

   if (fstat(fd, &sb) == -1)
      goto fail;

   data = malloc(sb.st_size);
   if (data == NULL)
      goto fail;

   size_t ck_size = cache->driver_keys_blob_size;
   file_header = malloc(ck_size);
   if (!file_header)
      goto fail;

   if (sb.st_size < ck_size)
      goto fail;

   ret = read_all(fd, file_header, ck_size);
   if (ret == -1)
      goto fail;

   /* Check for extremely unlikely hash collisions */
   if (memcmp(cache->driver_keys_blob, file_header, ck_size) != 0) {
      assert(!"Mesa cache keys mismatch!");
      goto fail;
   }

   size_t cache_item_md_size = sizeof(uint32_t);
   uint32_t md_type;
   ret = read_all(fd, &md_type, cache_item_md_size);
   if (ret == -1)
      goto fail;

   if (md_type == CACHE_ITEM_TYPE_GLSL) {
      uint32_t num_keys;
      cache_item_md_size += sizeof(uint32_t);
      ret = read_all(fd, &num_keys, sizeof(uint32_t));
      if (ret == -1)
         goto fail;

      /* The cache item metadata is currently just used for distributing
       * precompiled shaders, they are not used by Mesa so just skip them for
       * now.
       * TODO: pass the metadata back to the caller and do some basic
       * validation.
       */
      cache_item_md_size += num_keys * sizeof(cache_key);
      ret = lseek(fd, num_keys * sizeof(cache_key), SEEK_CUR);
      if (ret == -1)
         goto fail;
   }

   /* Load the CRC that was created when the file was written. */
   struct cache_entry_file_data cf_data;
   size_t cf_data_size = sizeof(cf_data);
   ret = read_all(fd, &cf_data, cf_data_size);
   if (ret == -1)
      goto fail;

   /* Load the actual cache data. */
   size_t cache_data_size =
      sb.st_size - cf_data_size - ck_size - cache_item_md_size;
   ret = read_all(fd, data, cache_data_size);
   if (ret == -1)
      goto fail;

   /* Uncompress the cache data */
   uncompressed_data = malloc(cf_data.uncompressed_size);
   if (!inflate_cache_data(data, cache_data_size, uncompressed_data,
                           cf_data.uncompressed_size))
      goto fail;

   /* Check the data for corruption */
   if (cf_data.crc32 != util_hash_crc32(uncompressed_data,
                                        cf_data.uncompressed_size))
      goto fail;

   free(data);
   free(filename);
   free(file_header);
   close(fd);

   if (size)
      *size = cf_data.uncompressed_size;

   return uncompressed_data;

 fail:
   if (data)
      free(data);
   if (filename)
      free(filename);
   if (uncompressed_data)
      free(uncompressed_data);
   if (file_header)
      free(file_header);
   if (fd != -1)
      close(fd);

   return NULL;
}

/* Return a filename within the cache's directory corresponding to 'key'.
 *
 * Returns NULL if out of memory.
 */
char *
disk_cache_get_cache_filename(struct disk_cache *cache, const cache_key key)
{
   char buf[41];
   char *filename;

   if (cache->path_init_failed)
      return NULL;

   _mesa_sha1_format(buf, key);
   if (asprintf(&filename, "%s/%c%c/%s", cache->path, buf[0],
                buf[1], buf + 2) == -1)
      return NULL;

   return filename;
}

void
disk_cache_write_item_to_disk(struct disk_cache_put_job *dc_job,
                              struct cache_entry_file_data *cf_data,
                              char *filename)
{
   int fd = -1, fd_final = -1;

   /* Write to a temporary file to allow for an atomic rename to the
    * final destination filename, (to prevent any readers from seeing
    * a partially written file).
    */
   char *filename_tmp = NULL;
   if (asprintf(&filename_tmp, "%s.tmp", filename) == -1)
      goto done;

   fd = open(filename_tmp, O_WRONLY | O_CLOEXEC | O_CREAT, 0644);

   /* Make the two-character subdirectory within the cache as needed. */
   if (fd == -1) {
      if (errno != ENOENT)
         goto done;

      make_cache_file_directory(dc_job->cache, dc_job->key);

      fd = open(filename_tmp, O_WRONLY | O_CLOEXEC | O_CREAT, 0644);
      if (fd == -1)
         goto done;
   }

   /* With the temporary file open, we take an exclusive flock on
    * it. If the flock fails, then another process still has the file
    * open with the flock held. So just let that file be responsible
    * for writing the file.
    */
#ifdef HAVE_FLOCK
   int err = flock(fd, LOCK_EX | LOCK_NB);
#else
   struct flock lock = {
      .l_start = 0,
      .l_len = 0, /* entire file */
      .l_type = F_WRLCK,
      .l_whence = SEEK_SET
   };
   int err = fcntl(fd, F_SETLK, &lock);
#endif
   if (err == -1)
      goto done;

   /* Now that we have the lock on the open temporary file, we can
    * check to see if the destination file already exists. If so,
    * another process won the race between when we saw that the file
    * didn't exist and now. In this case, we don't do anything more,
    * (to ensure the size accounting of the cache doesn't get off).
    */
   fd_final = open(filename, O_RDONLY | O_CLOEXEC);
   if (fd_final != -1) {
      unlink(filename_tmp);
      goto done;
   }

   /* OK, we're now on the hook to write out a file that we know is
    * not in the cache, and is also not being written out to the cache
    * by some other process.
    */

   /* Write the driver_keys_blob, this can be used find information about the
    * mesa version that produced the entry or deal with hash collisions,
    * should that ever become a real problem.
    */
   int ret = write_all(fd, dc_job->cache->driver_keys_blob,
                       dc_job->cache->driver_keys_blob_size);
   if (ret == -1) {
      unlink(filename_tmp);
      goto done;
   }

   /* Write the cache item metadata. This data can be used to deal with
    * hash collisions, as well as providing useful information to 3rd party
    * tools reading the cache files.
    */
   ret = write_all(fd, &dc_job->cache_item_metadata.type,
                   sizeof(uint32_t));
   if (ret == -1) {
      unlink(filename_tmp);
      goto done;
   }

   if (dc_job->cache_item_metadata.type == CACHE_ITEM_TYPE_GLSL) {
      ret = write_all(fd, &dc_job->cache_item_metadata.num_keys,
                      sizeof(uint32_t));
      if (ret == -1) {
         unlink(filename_tmp);
         goto done;
      }

      ret = write_all(fd, dc_job->cache_item_metadata.keys[0],
                      dc_job->cache_item_metadata.num_keys *
                      sizeof(cache_key));
      if (ret == -1) {
         unlink(filename_tmp);
         goto done;
      }
   }

   size_t cf_data_size = sizeof(*cf_data);
   ret = write_all(fd, cf_data, cf_data_size);
   if (ret == -1) {
      unlink(filename_tmp);
      goto done;
   }

   /* Now, finally, write out the contents to the temporary file, then
    * rename them atomically to the destination filename, and also
    * perform an atomic increment of the total cache size.
    */
   size_t file_size = deflate_and_write_to_disk(dc_job->data, dc_job->size,
                                                fd, filename_tmp);
   if (file_size == 0) {
      unlink(filename_tmp);
      goto done;
   }
   ret = rename(filename_tmp, filename);
   if (ret == -1) {
      unlink(filename_tmp);
      goto done;
   }

   struct stat sb;
   if (stat(filename, &sb) == -1) {
      /* Something went wrong remove the file */
      unlink(filename);
      goto done;
   }

   p_atomic_add(dc_job->cache->size, sb.st_blocks * 512);

 done:
   if (fd_final != -1)
      close(fd_final);
   /* This close finally releases the flock, (now that the final file
    * has been renamed into place and the size has been added).
    */
   if (fd != -1)
      close(fd);
   free(filename_tmp);
}

/* Determine path for cache based on the first defined name as follows:
 *
 *   $MESA_GLSL_CACHE_DIR
 *   $XDG_CACHE_HOME/mesa_shader_cache
 *   <pwd.pw_dir>/.cache/mesa_shader_cache
 */
char *
disk_cache_generate_cache_dir(void *mem_ctx)
{
   char *path = getenv("MESA_GLSL_CACHE_DIR");
   if (path) {
      if (mkdir_if_needed(path) == -1)
         return NULL;

      path = concatenate_and_mkdir(mem_ctx, path, CACHE_DIR_NAME);
      if (!path)
         return NULL;
   }

   if (path == NULL) {
      char *xdg_cache_home = getenv("XDG_CACHE_HOME");

      if (xdg_cache_home) {
         if (mkdir_if_needed(xdg_cache_home) == -1)
            return NULL;

         path = concatenate_and_mkdir(mem_ctx, xdg_cache_home, CACHE_DIR_NAME);
         if (!path)
            return NULL;
      }
   }

   if (!path) {
      char *buf;
      size_t buf_size;
      struct passwd pwd, *result;

      buf_size = sysconf(_SC_GETPW_R_SIZE_MAX);
      if (buf_size == -1)
         buf_size = 512;

      /* Loop until buf_size is large enough to query the directory */
      while (1) {
         buf = ralloc_size(mem_ctx, buf_size);

         getpwuid_r(getuid(), &pwd, buf, buf_size, &result);
         if (result)
            break;

         if (errno == ERANGE) {
            ralloc_free(buf);
            buf = NULL;
            buf_size *= 2;
         } else {
            return NULL;
         }
      }

      path = concatenate_and_mkdir(mem_ctx, pwd.pw_dir, ".cache");
      if (!path)
         return NULL;

      path = concatenate_and_mkdir(mem_ctx, path, CACHE_DIR_NAME);
      if (!path)
         return NULL;
   }

   return path;
}

bool
disk_cache_enabled()
{
   /* If running as a users other than the real user disable cache */
   if (geteuid() != getuid())
      return false;

   /* At user request, disable shader cache entirely. */
   if (env_var_as_boolean("MESA_GLSL_CACHE_DISABLE", false))
      return false;

   return true;
}

bool
disk_cache_mmap_cache_index(void *mem_ctx, struct disk_cache *cache,
                            char *path)
{
   int fd = -1;
   bool mapped = false;

   cache->path = ralloc_strdup(cache, path);
   if (cache->path == NULL)
      goto path_fail;

   path = ralloc_asprintf(mem_ctx, "%s/index", cache->path);
   if (path == NULL)
      goto path_fail;

   fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
   if (fd == -1)
      goto path_fail;

   struct stat sb;
   if (fstat(fd, &sb) == -1)
      goto path_fail;

   /* Force the index file to be the expected size. */
   size_t size = sizeof(*cache->size) + CACHE_INDEX_MAX_KEYS * CACHE_KEY_SIZE;
   if (sb.st_size != size) {
      if (ftruncate(fd, size) == -1)
         goto path_fail;
   }

   /* We map this shared so that other processes see updates that we
    * make.
    *
    * Note: We do use atomic addition to ensure that multiple
    * processes don't scramble the cache size recorded in the
    * index. But we don't use any locking to prevent multiple
    * processes from updating the same entry simultaneously. The idea
    * is that if either result lands entirely in the index, then
    * that's equivalent to a well-ordered write followed by an
    * eviction and a write. On the other hand, if the simultaneous
    * writes result in a corrupt entry, that's not really any
    * different than both entries being evicted, (since within the
    * guarantees of the cryptographic hash, a corrupt entry is
    * unlikely to ever match a real cache key).
    */
   cache->index_mmap = mmap(NULL, size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
   if (cache->index_mmap == MAP_FAILED)
      goto path_fail;
   cache->index_mmap_size = size;

   cache->size = (uint64_t *) cache->index_mmap;
   cache->stored_keys = cache->index_mmap + sizeof(uint64_t);
   mapped = true;

path_fail:
   if (fd != -1)
      close(fd);

   return mapped;
}

void
disk_cache_destroy_mmap(struct disk_cache *cache)
{
   munmap(cache->index_mmap, cache->index_mmap_size);
}
#endif

#endif /* ENABLE_SHADER_CACHE */
