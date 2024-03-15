#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "threads/thread.h"
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/cache.h"

/** Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);
static void extract_path_name (const char *path, char *dir, char *name);

/** Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format)
{
  //lock_init (&filesys_lock);
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();

  if (format)
    do_format ();

  free_map_open ();
}

/** Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void)
{
  cache_flush ();
  free_map_close ();
}

/** Creates a file named NAME with the given INITIAL_SIZE.
 *
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists, or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size, bool is_dir)
{
  block_sector_t inode_sector = 0;
  size_t length = strlen (name);
  char dir_name[length + 1];
  char file_name[length + 1];

  extract_path_name (name, dir_name, file_name);
  if (strlen (file_name) == 0 && !is_dir)
    return false;
  struct dir *dir = dir_open_path (dir_name);

  /* Create a new inode and add it to dir. */
  bool success = (dir != NULL && free_map_allocate (1, &inode_sector));
  if (!is_dir)
    success = success && inode_create (inode_sector, initial_size, is_dir);
  else
    {
      struct inode *dir_inode = dir_get_inode (dir);
      success = success && dir_create (inode_sector, initial_size, inode_get_inumber (dir_inode));
    }
  success = success && dir_add (dir, file_name, inode_sector);

  /* Free inode_sector if fail to create. */
  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;
}

/** Opens the file or directory with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.

   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *path)
{
  struct dir *dir = NULL;
  struct inode *inode = NULL;

  size_t length = strlen (path);
  char dir_path[length + 1];
  char file_name[length + 1];

  /* Open empty. */
  if (length == 0)
    return NULL;

  extract_path_name (path, dir_path, file_name);

  /* Open the inode. */
  dir = dir_open_path (dir_path);
  if (dir != NULL)
    if (strlen (file_name) > 0)  // Open a file.
      {
        dir_lookup (dir, file_name, &inode);
        dir_close (dir);
      }
    else    // Open a directory.
      inode = dir_get_inode (dir);

  if (inode == NULL || inode_is_removed (inode))
    return NULL;

  return file_open (inode);
}

/** Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name)
{
  struct dir *dir;
  bool success = false;

  size_t length = strlen (name);
  char dir_path[length + 1];
  char file_name[length + 1];

  /* Remove empty. */
  if (length == 0)
    return false;

  extract_path_name (name, dir_path, file_name);

  dir = dir_open_path (dir_path);
  if (dir != NULL)
    success = dir_remove (dir, file_name);

  dir_close (dir);

  return success;
}

/** Change the thread's current working directory to "name". */
bool
filesys_chdir (const char *name)
{
  struct dir *dir = dir_open_path (name);
  if (dir == NULL)
    return false;

  dir_close (thread_current ()->cwd);
  thread_current ()->cwd = dir;

  return true;
}

/** Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16, ROOT_DIR_SECTOR))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}

/**
 * @brief Extracts the directory and file name components from a path.
 *
 * If the path is a relative path, the function assumes
 * that it is relative to the current working directory (".").
 *
 * @param[in] path The path to extract the components from.
 * @param[out] dir The directory component of the path.
 * @param[out] name The file name component of the path.
 */
static void
extract_path_name (const char *path, char *dir, char *name)
{
  char *last_slash = strrchr (path, '/');

  if (last_slash == NULL)
    {
      // In current directory.
      strlcpy (dir, ".", 2);
      strlcpy (name, path, strlen (path) + 1);
    }
  else
    {
      if (last_slash == path)  // Root directory files, such as "/a".
        strlcpy (dir, "/", 2);
      else
        strlcpy (dir, path, (int) (last_slash - path + 1));

      strlcpy (name, last_slash + 1, strlen (last_slash + 1) + 1);
    }
}
