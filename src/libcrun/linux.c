/*
 * crun - OCI runtime written in C
 *
 * Copyright (C) 2017 Giuseppe Scrivano <giuseppe@scrivano.org>
 * libocispec is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * libocispec is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with crun.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE

#include <config.h>
#include "linux.h"
#include "utils.h"
#include <string.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sys/capability.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <grp.h>
#include <signal.h>
#include "terminal.h"

#define ALL_PROPAGATIONS (MS_REC | MS_SHARED | MS_PRIVATE | MS_SLAVE | MS_UNBINDABLE)

struct remount_s
{
  struct remount_s *next;
  char *target;
  unsigned long flags;
  char *data;
};

struct private_data_s
{
  struct remount_s *remounts;

  /* Filled by libcrun_set_namespaces().  Useful to query what
     namespaces are available.  */
  int unshare_flags;
};

struct linux_namespace_s
{
  const char *name;
  int value;
};

static struct private_data_s *
get_private_data (struct libcrun_container_s *container)
{
  if (container->private_data == NULL)
    {
      struct private_data_s *p = xmalloc (sizeof (*p));
      memset (p, 0, sizeof (*p));
      container->private_data = p;
    }
  return container->private_data;
}

static struct linux_namespace_s namespaces[] =
  {
    {"mount", CLONE_NEWNS},
    {"cgroup", CLONE_NEWCGROUP},
    {"network", CLONE_NEWNET},
    {"ipc", CLONE_NEWIPC},
    {"pid", CLONE_NEWPID},
    {"uts", CLONE_NEWUTS},
    {"user", CLONE_NEWUSER},
    {NULL, 0}
  };

static int
find_namespace (const char *name)
{
  struct linux_namespace_s *it;
  for (it = namespaces; it->name; it++)
    if (strcmp (it->name, name) == 0)
      return it->value;
  return -1;
}

static int
syscall_clone (unsigned long flags, void *child_stack)
{
  return (int) syscall (__NR_clone, flags, child_stack);
}

static void
get_uid_gid_from_def (oci_container *def, uid_t *uid, gid_t *gid)
{
  *uid = 0;
  *gid = 0;

  if (def->process->user)
    {
      if (def->process->user->uid)
        *uid = def->process->user->uid;
      if (def->process->user->gid)
        *gid = def->process->user->gid;
    }
}

pid_t
libcrun_run_container (libcrun_container *container, int detach, container_entrypoint entrypoint, void *args, libcrun_error_t *err)
{
  oci_container *def = container->container_def;
  size_t i;
  int ret;
  int flags = 0;
  pid_t pid;
  cleanup_close int userns_pipe_read = -1;
  cleanup_close int userns_pipe_write = -1;
  int userns_sync_pipe[2];

  for (i = 0; i < def->linux->namespaces_len; i++)
    {
      int value = find_namespace (def->linux->namespaces[i]->type);
      if (UNLIKELY (value < 0))
        return crun_make_error (err, 0, "invalid namespace type: %s", def->linux->namespaces[i]->type);
      flags |= value;
    }

  get_private_data (container)->unshare_flags = flags;

  if (flags & CLONE_NEWUSER)
    {
      ret = pipe (userns_sync_pipe);
      if (UNLIKELY (ret < 0))
        return crun_make_error (err, errno, "pipe");
      userns_pipe_read = userns_sync_pipe[0];
      userns_pipe_write = userns_sync_pipe[1];
    }

  pid = syscall_clone (flags | (detach ? 0 : SIGCHLD), NULL);
  if (UNLIKELY (pid < 0))
    return crun_make_error (err, errno, "clone");

  get_uid_gid_from_def (container->container_def,
                        &container->container_uid,
                        &container->container_gid);

  if (pid)
    {
      if (flags & CLONE_NEWUSER)
        {
          ret = libcrun_set_usernamespace (container, pid, err);
          if (UNLIKELY (ret < 0))
            return ret;

          do
            ret = write (userns_pipe_write, "1", 1);
          while (ret < 0 && errno == EINTR);
          if (UNLIKELY (ret < 0))
            return crun_make_error (err, 0, "write to sync pipe");
        }

      return pid;
    }

  if (detach && setsid () < 0)
    {
      crun_make_error (err, errno, "setsid");
      goto out;
    }

  /* In the container.  Join namespaces if asked and jump into the entrypoint function.  */
  if (container->host_uid == 0 && !(flags & CLONE_NEWUSER))
    {
      gid_t *additional_gids = NULL;
      size_t additional_gids_len = 0;
      if (def->process->user)
        {
          additional_gids = def->process->user->additional_gids;
          additional_gids_len = def->process->user->additional_gids_len;
        }
      if (UNLIKELY (setgroups (additional_gids_len, additional_gids) < 0))
        {
          crun_make_error (err, errno, "setgroups");
          goto out;
        }
    }

  for (i = 0; i < def->linux->namespaces_len; i++)
    {
      int value;
      cleanup_close int fd = -1;
      if (def->linux->namespaces[i]->path == NULL)
        continue;

      value = find_namespace (def->linux->namespaces[i]->type);
      fd = open (def->linux->namespaces[i]->path, O_RDONLY);
      if (UNLIKELY (fd < 0))
        {
          crun_make_error (err, errno, "open '%s'", def->linux->namespaces[i]->path);
          goto out;
        }

      if (UNLIKELY (setns (fd, value) < 0))
        {
          crun_make_error (err, errno, "setns '%s'", def->linux->namespaces[i]->path);
          goto out;
        }
    }

  if (flags & CLONE_NEWUSER)
    {
      char tmp;
      do
        ret = read (userns_pipe_read, &tmp, 1);
      while (ret < 0 && errno == EINTR);
      if (UNLIKELY (ret < 0))
        return crun_make_error (err, 0, "read from sync pipe");
    }

  entrypoint (args);
  _exit (1);
 out:
  error (EXIT_FAILURE, (*err)->status, "%s", (*err)->msg);
  return 1;
}

struct propagation_flags_s
  {
    const char *name;
    int flags;
  };

static struct propagation_flags_s propagation_flags[] =
  {
    {"rshared", MS_REC | MS_SHARED},
    {"rslave", MS_REC | MS_SLAVE},
    {"rprivate", MS_REC | MS_PRIVATE},
    {"shared", MS_SHARED},
    {"slave", MS_SLAVE},
    {"private", MS_PRIVATE},
    {"unbindable", MS_UNBINDABLE},
    {"nosuid", MS_NOSUID},
    {"noexec", MS_NOEXEC},
    {"nodev", MS_NODEV},
    {"dirsync", MS_DIRSYNC},
    {"lazytime", MS_LAZYTIME},
    {"nodiratime", MS_NODIRATIME},
    {"noatime", MS_NOATIME},
    {"ro", MS_RDONLY},
    {"rw", 0},
    {"relatime", MS_RELATIME},
    {"strictatime", MS_STRICTATIME},
    {"synchronous", MS_SYNCHRONOUS},
    {NULL, 0}
  };

static unsigned long
get_mount_flags (const char *name, int *found)
{
  struct propagation_flags_s *it;
  if (found)
    *found = 0;
  for (it = propagation_flags; it->name; it++)
    if (strcmp (it->name, name) == 0)
      {
        if (found)
          *found = 1;
        return it->flags;
      }
  return 0;
}

static unsigned long
get_mount_flags_or_option (const char *name, char **option)
{
  int found;
  unsigned long flags = get_mount_flags (name, &found);
  cleanup_free char *prev = NULL;
  if (found)
    return flags;

  prev = *option;
  if (*option && **option)
    xasprintf (option, "%s,%s", *option, name);
  else
    *option = xstrdup (name);

  return 0;
}

int
pivot_root (const char * new_root, const char * put_old)
{
  return syscall (__NR_pivot_root, new_root, put_old);
}

static int
do_mount (libcrun_container *container,
          const char *source,
          const char *target,
          const char *filesystemtype,
          unsigned long mountflags,
          const void *data,
          int skip_labelling,
          libcrun_error_t *err)
{
  int ret;
  cleanup_free char *data_with_label = NULL;
  const char *label = container->container_def->linux->mount_label;

  if (!skip_labelling)
    {
      ret = add_selinux_mount_label (&data_with_label, data, label, err);
      if  (ret < 0)
        return ret;
      data = data_with_label;
    }
   ret = mount (source, target, filesystemtype, mountflags, data);
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "mount '%s' to '%s'", source, target);

  if (mountflags & ALL_PROPAGATIONS)
    {
      unsigned long rec = mountflags & MS_REC;
      unsigned long propagations = mountflags & (MS_SHARED | MS_PRIVATE | MS_SLAVE | MS_UNBINDABLE);
      unsigned long all_propagations[] = {MS_SHARED, MS_PRIVATE, MS_SLAVE, MS_UNBINDABLE, 0};
      size_t s;
      for (s = 0; all_propagations[s]; s++)
        {
          if (!(propagations & all_propagations[s]))
            continue;
          ret = mount ("none", target, "", rec | all_propagations[s], "");
          if (UNLIKELY (ret < 0))
            return crun_make_error (err, errno, "set propagation for '%s'", target);
        }
    }

  return ret;
}

static int
do_mount_cgroup (libcrun_container *container,
                 const char *source,
                 const char *target,
                 const char *filesystemtype,
                 unsigned long mountflags,
                 const void *data,
                 libcrun_error_t *err)
{
  int ret;
  size_t i;
  const char *subsystems[] = {"devices", "cpuset", "pids", "memory", "net_cls,net_prio",
                              "freezer", "blkio", "hugetlb", "cpu,cpuacct", "perf_event", NULL};
  cleanup_free char *cgroup_unified = NULL;

  xasprintf (&cgroup_unified, "%s/unified", target);

  ret = do_mount (container, source, target, "tmpfs", mountflags, "size=1024k", 0, err);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = mkdir (cgroup_unified, 0755);
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "mkdir for '%s' failed", cgroup_unified);

  ret = do_mount (container, source, cgroup_unified, "cgroup2", mountflags, NULL, 1, err);
  if (UNLIKELY (ret < 0))
    return ret;

  for (i = 0; subsystems[i]; i++)
    {
      cleanup_free char *subsystem_path = NULL;
      xasprintf (&subsystem_path, "%s/%s", target, subsystems[i]);

      ret = mkdir (subsystem_path, 0755);
      if (UNLIKELY (ret < 0))
        return crun_make_error (err, errno, "mkdir for '%s' failed", cgroup_unified);

      ret = do_mount (container, source, subsystem_path, "cgroup", mountflags, subsystems[i], 1, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  return 0;
}

struct device_s
{
  const char *path;
  char *type;
  int major;
  int minor;
  int mode;
};

struct device_s needed_devs[] =
  {
    {"/dev/null", "c", 1, 3, 0666},
    {"/dev/zero", "c", 1, 5, 0666},
    {"/dev/full", "c", 1, 7, 0666},
    {"/dev/tty", "c", 5, 0, 0666},
    {"/dev/random", "c", 1, 8, 0666},
    {"/dev/urandom", "c", 1, 9, 0666},
    {NULL, '\0', 0, 0, 0}
  };

static int
create_dev (libcrun_container *container, int devfd, struct device_s *device, const char *rootfs, int binds, libcrun_error_t *err)
{
  int ret;
  dev_t dev;
  mode_t type = (device->type[0] == 'b') ? S_IFBLK : ((device->type[0] == 'p') ? S_IFIFO : S_IFCHR);
  const char *fullname = device->path;
  /* Skip the common prefix /dev.  */
  const char *basename = device->path + 5;
  if (binds)
    {
      cleanup_free char *path_to_container = NULL;
      xasprintf (&path_to_container, "%s/dev/%s", rootfs, basename);

      ret = create_file_if_missing_at (devfd, basename, err);
      if (UNLIKELY (ret < 0))
        return ret;

      ret = do_mount (container, fullname, path_to_container, "", MS_BIND | MS_PRIVATE, "", 0, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }
  else
    {
      dev = makedev (device->major, device->minor);
      ret = mknodat (devfd, basename, device->mode | type, dev);
      /* We don't fail when the file already exists.  */
      if (UNLIKELY (ret < 0 && errno == EEXIST))
        return 0;
      if (UNLIKELY (ret < 0))
        return crun_make_error (err, errno, "mknod '%s'", basename);
    }
  return 0;
}

static int
create_missing_devs (libcrun_container *container, const char *rootfs, int binds, libcrun_error_t *err)
{
  int ret;
  size_t i;
  struct device_s *it;
  cleanup_close int dirfd = open (rootfs, O_DIRECTORY | O_RDONLY);
  cleanup_close int devfd = -1;
  oci_container *def = container->container_def;

  if (UNLIKELY (dirfd < 0))
    return crun_make_error (err, errno, "open rootfs directory '%s'", rootfs);

  devfd = openat (dirfd, "dev", O_DIRECTORY | O_RDONLY);
  if (UNLIKELY (devfd < 0))
    return crun_make_error (err, errno, "open /dev directory in '%s'", rootfs);

  for (i = 0; i < def->linux->devices_len; i++)
    {
      struct device_s device = {def->linux->devices[i]->path,
                                     def->linux->devices[i]->type,
                                     def->linux->devices[i]->major,
                                     def->linux->devices[i]->minor,
                                     def->linux->devices[i]->file_mode};
      ret = create_dev (container, devfd, &device, rootfs, binds, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  for (it = needed_devs; it->path; it++)
    {
      ret = create_dev (container, devfd, it, rootfs, binds, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  ret = symlinkat ("/proc/self/fd", devfd, "fd");
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "creating symlink for /dev/fd");

  ret = symlinkat ("/proc/self/fd/0", devfd, "stdin");
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "creating symlink for /dev/stdin");

  ret = symlinkat ("/proc/self/fd/1", devfd, "stdout");
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "creating symlink for /dev/stdout");

  ret = symlinkat ("/proc/self/fd/2", devfd, "stderr");
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "creating symlink for /dev/stderr");

  ret = symlinkat ("/proc/kcore", devfd, "core");
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "creating symlink for /dev/core");

  ret = symlinkat ("/dev/pts/ptmx", devfd, "ptmx");
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "creating symlink for /dev/ptmx");

  return 0;
}

static int
do_masked_and_readonly_paths (libcrun_container *container, libcrun_error_t *err)
{
  size_t i;
  int ret;
  oci_container *def = container->container_def;
  return 0;
  for (i = 0; i < def->linux->masked_paths_len; i++)
    {
      char *path = def->linux->masked_paths[i];

      ret = do_mount (container, "/dev/null", path, "", MS_BIND | MS_UNBINDABLE | MS_PRIVATE | MS_REC, "", 0, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }
  for (i = 0; i < def->linux->readonly_paths_len; i++)
    {
      char *path = def->linux->readonly_paths[i];

      ret = crun_ensure_directory (path, 0755, err);
      if (UNLIKELY (ret < 0))
        return ret;

      ret = do_mount (container, path, path, "", MS_BIND | MS_UNBINDABLE | MS_PRIVATE | MS_RDONLY | MS_REC, "", 0, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }
  return 0;
}

static int
do_pivot (libcrun_container *container, const char *rootfs, libcrun_error_t *err)
{
  int ret;
  cleanup_close int oldrootfd = open ("/", O_DIRECTORY | O_RDONLY);
  cleanup_close int newrootfd = open (rootfs, O_DIRECTORY | O_RDONLY);

  if (UNLIKELY (oldrootfd < 0))
    return crun_make_error (err, errno, "open '/'");
  if (UNLIKELY (newrootfd < 0))
    return crun_make_error (err, errno, "open '%s'", rootfs);

  ret = fchdir (newrootfd);
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "fchdir '%s'", rootfs);

  ret = pivot_root (".", ".");
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "pivot_root");

  ret = fchdir (oldrootfd);
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "fchdir '%s'", rootfs);

  ret = do_mount (container, "", ".", "", MS_PRIVATE | MS_REC, "", 0, err);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = umount2 (".", MNT_DETACH);
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "umount oldroot");

  ret = chdir ("/");
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "chdir to newroot");

  return 0;
}

static int
get_default_flags (libcrun_container *container, const char *destination, char **data)
{
  if (strcmp (destination, "/proc") == 0)
      return 0;
  if (strcmp (destination, "/dev/cgroup") == 0
      || strcmp (destination, "/sys/fs/cgroup") == 0)
    {
      *data = xstrdup ("none,name=");
      return MS_NOEXEC | MS_NOSUID | MS_STRICTATIME;
    }
  if (strcmp (destination, "/dev") == 0)
    {
      *data = xstrdup ("mode=755");
      return MS_NOEXEC | MS_STRICTATIME;
    }
  if (strcmp (destination, "/dev/shm") == 0)
    {
      *data = xstrdup ("mode=1777,size=65536k");
      return MS_NOEXEC | MS_NOSUID | MS_NODEV;
    }
  if (strcmp (destination, "/dev/mqueue") == 0)
      return MS_NOEXEC | MS_NOSUID | MS_NODEV;
  if (strcmp (destination, "/dev/pts") == 0)
    {
      if (container->host_uid == 0)
        *data = xstrdup ("newinstance,ptmxmode=0666,mode=620,gid=5");
      else
        *data = xstrdup ("newinstance,ptmxmode=0666,mode=620");
      return MS_NOEXEC | MS_NOSUID;
    }
  if (strcmp (destination, "/sys") == 0)
      return MS_NOEXEC | MS_NOSUID | MS_NODEV;

  return 0;
}

static void
free_remount (struct remount_s *r)
{
  free (r->data);
  free (r->target);
  free (r);
}

static struct remount_s *
make_remount (char *target, unsigned long flags, char *data, struct remount_s *next)
{
  struct remount_s *ret = xmalloc (sizeof (*ret));
  ret->target = xstrdup (target);
  ret->flags = flags;
  ret->data = xstrdup (data);
  ret->next = next;
  return ret;
}

static int
finalize_mounts (libcrun_container *container, const char *rootfs, int is_user_ns, libcrun_error_t *err)
{
  int ret;
  struct remount_s *r;
  for (r = get_private_data (container)->remounts; r;)
    {
      struct remount_s *next = r->next;
      ret = do_mount (container, "none", r->target, "", r->flags, r->data, 1, err);
      if (UNLIKELY (ret < 0))
        return ret;

      free_remount (r);

      r = next;
    }
  get_private_data (container)->remounts = NULL;
  return 0;
}

static int
do_mounts (libcrun_container *container, const char *rootfs, libcrun_error_t *err)
{
  size_t i;
  int ret;
  oci_container *def = container->container_def;
  for (i = 0; i < def->mounts_len; i++)
    {
      cleanup_free char *target = NULL;
      cleanup_free char *data = NULL;
      char *type;
      char *source;
      unsigned long flags = 0;
      int skip_labelling;

      if (rootfs)
        xasprintf (&target, "%s/%s", rootfs, def->mounts[i]->destination + 1);
      else
        target = xstrdup (def->mounts[i]->destination);

      ret = crun_ensure_directory (target, 0755, err);
      if (UNLIKELY (ret < 0))
        return ret;

      if (def->mounts[i]->options == NULL)
        flags = get_default_flags (container, def->mounts[i]->destination, &data);
      else
        {
          size_t j;
          for (j = 0; j < def->mounts[i]->options_len; j++)
            flags |= get_mount_flags_or_option (def->mounts[i]->options[j], &data);
        }

      type = def->mounts[i]->type;

      if (strcmp (type, "bind") == 0)
        flags |= MS_BIND;

      source = def->mounts[i]->source ? def->mounts[i]->source : type;

      skip_labelling = strcmp (type, "sysfs") == 0
        || strcmp (type, "proc") == 0
        || strcmp (type, "mqueue") == 0;

      if (strcmp (type, "cgroup") == 0)
        {
          ret = do_mount_cgroup (container, source, target, type, flags & ~MS_RDONLY, data, err);
          if (UNLIKELY (ret < 0))
            return ret;
        }
      else
        {
          ret = do_mount (container, source, target, type, flags & ~MS_RDONLY, data, skip_labelling, err);
          if (UNLIKELY (ret < 0))
            return ret;
        }

      if (flags & MS_RDONLY)
        {
          unsigned long remount_flags = (flags & ~ALL_PROPAGATIONS) | MS_REMOUNT | MS_BIND | MS_RDONLY;
          struct remount_s *r = make_remount (target, remount_flags, data, get_private_data (container)->remounts);
          get_private_data (container)->remounts = r;
        }
    }
  return 0;
}

int
libcrun_set_mounts (libcrun_container *container, const char *rootfs, libcrun_error_t *err)
{
  oci_container *def = container->container_def;
  int ret;
  int is_user_ns;
  unsigned long rootfsPropagation = 0;

  if (def->linux->rootfs_propagation)
    rootfsPropagation = get_mount_flags (def->linux->rootfs_propagation, NULL);
  else
    rootfsPropagation = MS_REC | MS_SLAVE;

  ret = do_mount (container, "", "/", "", MS_REC | rootfsPropagation, "", 0, err);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = do_mount (container, def->root->path, rootfs, "", MS_BIND | MS_REC | rootfsPropagation, "", 0, err);
  if (UNLIKELY (ret < 0))
    return ret;
  if (def->root->readonly)
    {
      unsigned long remount_flags = (rootfsPropagation & ~ALL_PROPAGATIONS) | MS_REMOUNT | MS_BIND | MS_RDONLY;
      struct remount_s *r = make_remount (def->root->path, remount_flags, "", get_private_data (container)->remounts);
      get_private_data (container)->remounts = r;
    }

  ret = do_mounts (container, rootfs, err);
  if (UNLIKELY (ret < 0))
    return ret;

  is_user_ns = (get_private_data (container)->unshare_flags & CLONE_NEWUSER);
  if (!is_user_ns)
    {
      is_user_ns = check_running_in_user_namespace (err);
      if (UNLIKELY (is_user_ns < 0))
        return is_user_ns;
    }

  ret = create_missing_devs (container, rootfs, is_user_ns, err);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = finalize_mounts (container, rootfs, is_user_ns, err);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = do_pivot (container, rootfs, err);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = do_masked_and_readonly_paths (container, err);
  if (UNLIKELY (ret < 0))
    return ret;

  return 0;
}

static int
uidgidmap_helper (char *helper, pid_t pid, char *map_file, libcrun_error_t *err)
{
#define MAX_ARGS 20
  char pid_fmt[16];
  char *args[MAX_ARGS + 1];
  char *next;
  size_t nargs = 0;
  args[nargs++] = helper;
  sprintf (pid_fmt, "%d", pid);
  args[nargs++] = pid_fmt;
  next = map_file;
  while (nargs < MAX_ARGS)
    {
      char *p = strsep (&next, " \n");
      if (next == NULL)
        break;
      args[nargs++] = p;
    }
  args[nargs++] = NULL;

  return run_process (args, err);
}

static int
newgidmap (pid_t pid, char *map_file, libcrun_error_t *err)
{
  return uidgidmap_helper ("/usr/bin/newgidmap", pid, map_file, err);
}

static int
newuidmap (pid_t pid, char *map_file, libcrun_error_t *err)
{
  return uidgidmap_helper ("/usr/bin/newuidmap", pid, map_file, err);
}

int
libcrun_set_usernamespace (libcrun_container *container, pid_t pid, libcrun_error_t *err)
{
#define MAX_MAPPINGS 5
  cleanup_free char *groups_file = NULL;
  cleanup_free char *uid_map_file = NULL;
  cleanup_free char *gid_map_file = NULL;
  cleanup_free char *uid_map = NULL;
  cleanup_free char *gid_map = NULL;
  int uid_map_len, gid_map_len;
  int ret;
  oci_container *def = container->container_def;

  if ((get_private_data (container)->unshare_flags & CLONE_NEWUSER) == 0)
    return 0;

  if (!def->linux->uid_mappings_len)
    {
      uid_map_len = format_default_id_mapping (&uid_map, container->container_uid, container->host_uid, 1);
      if (uid_map == NULL)
        uid_map_len = xasprintf (&uid_map, "%d %d 1", container->container_uid, container->host_uid);
    }
  else
    {
      size_t written = 0, len, s;
      char buffer[128];
      uid_map = xmalloc (sizeof (buffer) * MAX_MAPPINGS + 1);
      for (s = 0; s < def->linux->uid_mappings_len && s < MAX_MAPPINGS; s++)
        {
          len = sprintf (buffer, "%d %d %d\n",
                         def->linux->uid_mappings[s]->container_id,
                         def->linux->uid_mappings[s]->host_id,
                         def->linux->uid_mappings[s]->size);
          memcpy (uid_map + written, buffer, len);
          written += len;
        }
      uid_map[written] = '\0';
      uid_map_len = written;
    }

  if (!def->linux->gid_mappings_len)
    {
      gid_map_len = format_default_id_mapping (&gid_map, container->container_gid, container->host_gid, 0);
      if (gid_map == NULL)
        gid_map_len = xasprintf (&gid_map, "%d %d 1", container->container_gid, container->host_gid);
    }
  else
    {
      size_t written = 0, len, s;
      char buffer[128];
      gid_map = xmalloc (sizeof (buffer) * MAX_MAPPINGS + 1);
      for (s = 0; s < def->linux->gid_mappings_len && s < MAX_MAPPINGS; s++)
        {
          len = sprintf (buffer, "%d %d %d\n",
                         def->linux->gid_mappings[s]->container_id,
                         def->linux->gid_mappings[s]->host_id,
                         def->linux->gid_mappings[s]->size);
          memcpy (gid_map + written, buffer, len);
          written += len;
        }
      gid_map[written] = '\0';
      gid_map_len = written;
    }

  xasprintf (&groups_file, "/proc/%d/setgroups", pid);
  ret = write_file (groups_file, "deny", 4, err);
  if (UNLIKELY (ret < 0))
    return ret;

  xasprintf (&gid_map_file, "/proc/%d/gid_map", pid);
  ret = write_file (gid_map_file, gid_map, gid_map_len, err);
  if (ret < 1 && errno == EPERM && container->host_uid)
    {
      crun_error_release (err);
      ret = newgidmap (pid, gid_map, err);
    }
  if (UNLIKELY (ret < 0))
    return ret;

  xasprintf (&uid_map_file, "/proc/%d/uid_map", pid);
  ret = write_file (uid_map_file, uid_map, uid_map_len, err);
  if (ret < 1 && errno == EPERM && container->host_uid)
    {
      crun_error_release (err);
      ret = newuidmap (pid, uid_map, err);
    }
  if (UNLIKELY (ret < 0))
    return ret;
  return 0;
}

#define CAP_TO_MASK_0(x) (1L << ((x) & 31))
#define CAP_TO_MASK_1(x) CAP_TO_MASK_0(x - 32)

struct all_caps_s
{
  unsigned long effective[2];
  unsigned long permitted[2];
  unsigned long inheritable[2];
  unsigned long ambient[2];
  unsigned long bounding[2];
};

static int
set_required_caps (struct all_caps_s *caps, int no_new_privs, libcrun_error_t *err)
{
  unsigned long cap;
  int ret;
  struct __user_cap_header_struct hdr = { _LINUX_CAPABILITY_VERSION_3, 0 };
  struct __user_cap_data_struct data[2] = { { 0 } };

  ret = prctl (PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0);
  if (UNLIKELY (ret < 0 && !(errno == EINVAL || errno == EPERM)))
    return crun_make_error (err, errno, "prctl reset ambient");

  for (cap = 0; cap <= CAP_LAST_CAP; cap++)
    if ((cap < 32 && CAP_TO_MASK_0 (cap) & caps->ambient[0])
        || (cap >= 32 && CAP_TO_MASK_1 (cap) & caps->ambient[1]))
      {
        ret = prctl (PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, cap, 0, 0);
        if (UNLIKELY (ret < 0 && !(errno == EINVAL || errno == EPERM)))
          return crun_make_error (err, errno, "prctl ambient raise");
      }

  for (cap = 0; cap <= CAP_LAST_CAP; cap++)
    if ((cap < 32 && ((CAP_TO_MASK_0 (cap) & caps->bounding[0]) == 0))
        || (cap >= 32 && ((CAP_TO_MASK_1 (cap) & caps->bounding[1]) == 0)))
      {
        ret = prctl (PR_CAPBSET_DROP, cap, 0, 0, 0);
        if (UNLIKELY (ret < 0 && !(errno == EINVAL || errno == EPERM)))
          return crun_make_error (err, errno, "prctl drop bounding");
      }

  data[0].effective = caps->effective[0];
  data[1].effective = caps->effective[1];
  data[0].inheritable = caps->inheritable[0];
  data[1].inheritable = caps->inheritable[1];
  data[0].permitted = caps->permitted[0];
  data[1].permitted = caps->permitted[1];

  ret = capset (&hdr, data) < 0;
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "capset");

  if (no_new_privs)
    if (UNLIKELY (prctl (PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0))
      return crun_make_error (err, errno, "no new privs");

  return 0;
}

static int
read_caps (unsigned long caps[2], char **values, size_t len, libcrun_error_t *err)
{
  size_t i;
  for (i = 0; i < len; i++)
    {
      cap_value_t cap;
      if (cap_from_name (values[i], &cap) < 0)
        return crun_make_error (err, 0, "unknown cap: %s", values[i]);
      if (cap < 32)
          caps[0] |= CAP_TO_MASK_0 (cap);
      else
          caps[1] |= CAP_TO_MASK_1 (cap);
    }
  return 0;
}

int
libcrun_set_selinux_exec_label (libcrun_container *container, libcrun_error_t *err)
{
  char *label = container->container_def->process->selinux_label;
  if (label == NULL)
    return 0;
  return set_selinux_exec_label (label, err);
}

int
libcrun_set_caps (libcrun_container *container, libcrun_error_t *err)
{
  int ret;
  struct all_caps_s caps;
  oci_container *def = container->container_def;
  memset (&caps, 0, sizeof (caps));
  if (def->process->capabilities)
    {
      ret = read_caps (caps.effective,
                       def->process->capabilities->effective,
                       def->process->capabilities->effective_len,
                       err);
      if (ret < 0)
        return ret;

      ret = read_caps (caps.inheritable,
                       def->process->capabilities->inheritable,
                       def->process->capabilities->inheritable_len,
                       err);
      if (ret < 0)
        return ret;

      ret = read_caps (caps.ambient,
                       def->process->capabilities->ambient,
                       def->process->capabilities->ambient_len,
                       err);
      if (ret < 0)
        return ret;

      ret = read_caps (caps.bounding,
                       def->process->capabilities->bounding,
                       def->process->capabilities->bounding_len,
                       err);
      if (ret < 0)
        return ret;

      ret = read_caps (caps.permitted,
                       def->process->capabilities->permitted,
                       def->process->capabilities->permitted_len,
                       err);
      if (ret < 0)
        return ret;
    }
  return set_required_caps (&caps, def->process->no_new_privileges, err);
}

struct rlimit_s
{
  const char *name;
  int value;
};

struct rlimit_s rlimits[] =
  {
    {"RLIMIT_AS", RLIMIT_AS},
    {"RLIMIT_CORE", RLIMIT_CORE},
    {"RLIMIT_CPU", RLIMIT_CPU},
    {"RLIMIT_DATA", RLIMIT_DATA},
    {"RLIMIT_FSIZE", RLIMIT_FSIZE},
    {"RLIMIT_LOCKS", RLIMIT_LOCKS},
    {"RLIMIT_MEMLOCK", RLIMIT_MEMLOCK},
    {"RLIMIT_MSGQUEUE", RLIMIT_MSGQUEUE},
    {"RLIMIT_NICE", RLIMIT_NICE},
    {"RLIMIT_NOFILE", RLIMIT_NOFILE},
    {"RLIMIT_NPROC", RLIMIT_NPROC},
    {"RLIMIT_RSS", RLIMIT_RSS},
    {"RLIMIT_RTPRIO", RLIMIT_RTPRIO},
    {"RLIMIT_RTTIME", RLIMIT_RTTIME},
    {"RLIMIT_SIGPENDING", RLIMIT_SIGPENDING},
    {"RLIMIT_STACK", RLIMIT_STACK},
    {NULL, 0}
  };

static int
get_rlimit_resource (const char *name)
{
  struct rlimit_s *it;
  for (it = rlimits; it->name; it++)
    if (strcmp (it->name, name) == 0)
      return it->value;
  return -1;
}

int
libcrun_set_rlimits (libcrun_container *container, libcrun_error_t *err)
{
  oci_container *def = container->container_def;
  size_t i;
  if (def->process->rlimits == NULL)
    return 0;
  for (i = 0; i < def->process->rlimits_len; i++)
    {
      struct rlimit limit;
      char *type = def->process->rlimits[i]->type;
      int resource = get_rlimit_resource (type);
      if (UNLIKELY (resource < 0))
        return crun_make_error (err, 0, "invalid rlimit '%s'", type);
      limit.rlim_cur = def->process->rlimits[i]->soft;
      limit.rlim_max = def->process->rlimits[i]->hard;
      if (UNLIKELY (setrlimit (resource, &limit) < 0))
        return crun_make_error (err, errno, "setrlimit '%s'", type);
    }
  return 0;
}

int
libcrun_set_hostname (libcrun_container *container, libcrun_error_t *err)
{
  oci_container *def = container->container_def;
  int has_uts = get_private_data (container)->unshare_flags & CLONE_NEWUTS;
  int ret;
  if (def->hostname == NULL || def->hostname[0] == '\0')
    return 0;
  if (!has_uts)
    return crun_make_error (err, 0, "hostname requires the UTS namespace");
  ret = sethostname (def->hostname, strlen (def->hostname));
  if (UNLIKELY (ret < 0))
    return crun_make_error (err, errno, "sethostname");
  return 0;
}

int
libcrun_set_oom (libcrun_container *container, libcrun_error_t *err)
{
  oci_container *def = container->container_def;
  cleanup_close int fd = -1;
  int ret;
  char oom_buffer[16];
  if (def->process->oom_score_adj == 0)
    return 0;
  sprintf (oom_buffer, "%i", def->process->oom_score_adj);
  fd = open ("/proc/self/oom_score_adj", O_WRONLY);
  if (fd < 0)
    return crun_make_error (err, errno, "read /proc/self/oom_score_adj");
  ret = write (fd, oom_buffer, strlen (oom_buffer));
  if (ret < 0)
    return crun_make_error (err, errno, "write to /proc/self/oom_score_adj");
  return 0;
}

int
libcrun_set_sysctl (libcrun_container *container, libcrun_error_t *err)
{
  oci_container *def = container->container_def;
  size_t i;
  cleanup_close int dirfd = -1;

  if (!def->linux || !def->linux->sysctl)
    return 0;

  dirfd = open ("/sys/fs", O_DIRECTORY | O_RDONLY);
  if (UNLIKELY (dirfd < 0))
    return crun_make_error (err, errno, "open /sys/fs");

  for (i = 0; i < def->linux->sysctl->len; i++)
    {
      cleanup_free char *name = xstrdup (def->linux->sysctl->keys[i]);
      cleanup_close int fd = -1;
      int ret;
      char *it;
      for (it = name; *it; it++)
        if (*it == '.')
          *it = '/';

      fd = openat (dirfd, name, O_WRONLY);
      if (UNLIKELY (fd < 0))
        return crun_make_error (err, errno, "open /sys/fs/%s", name);

      ret = write (fd, def->linux->sysctl->values[i], strlen (def->linux->sysctl->values[i]));
      if (UNLIKELY (ret < 0))
        return crun_make_error (err, errno, "write to /sys/fs/%s", name);
    }
  return 0;
}

int
libcrun_set_terminal (libcrun_container *container, libcrun_error_t *err)
{
  int ret;
  cleanup_close int fd = -1;
  cleanup_free char *slave = NULL;
  oci_container *def = container->container_def;
  if (!def->process->terminal)
    return 0;

  fd = libcrun_new_terminal (&slave, err);
  if (UNLIKELY (fd < 0))
    return fd;

  ret = libcrun_set_stdio (slave, err);
  if (UNLIKELY (ret < 0))
    return ret;

  if (def->process->console_size)
    {
      ret = libcrun_terminal_setup_size (0, def->process->console_size->height,
                                         def->process->console_size->width,
                                         err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  ret = write_file ("/dev/console", NULL, 0, err);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = do_mount (container, slave, "/dev/console", "devpts", MS_BIND, NULL, 0, err);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = fd;
  fd = -1;

  return ret;
}
