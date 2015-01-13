#include <common/errno.h>
#include <common/fadvise.h>
#include <common/fcntl.h>
#include <fs/console.h>
#include <fs/devfs.h>
#include <fs/pipe.h>
#include <fs/socket.h>
#include <fs/winfs.h>
#include <syscall/mm.h>
#include <syscall/syscall.h>
#include <syscall/vfs.h>
#include <datetime.h>
#include <log.h>
#include <str.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <limits.h>
#include <malloc.h>

/* Notes on symlink solving:

   Sometimes we need to perform file operation and symlink checking at
   one place.

   For example, if we test symlink first and try opening file later,
   another process may replace the file with a symlink after the symlink
   check. This will result in opening a symlink file as a regular file.

   But for path components this is fine as if a symlink check fails for
   a component, the whole operation immediately fails.
*/

struct vfs_data
{
	struct file *fds[MAX_FD_COUNT];
	int fds_cloexec[MAX_FD_COUNT];
	struct file_system *fs_first;
	char cwd[PATH_MAX];
	int umask;
};

static struct vfs_data * const vfs = VFS_DATA_BASE;

static void vfs_add(struct file_system *fs)
{
	fs->next = vfs->fs_first;
	vfs->fs_first = fs;
}

/* Get file handle to a fd */
struct file *vfs_get(int fd)
{
	if (fd < 0 || fd >= MAX_FD_COUNT)
		return NULL;
	return vfs->fds[fd];
}

/* Reference a file, only used on raw file handles not created by sys_open() */
void vfs_ref(struct file *f)
{
	f->ref++;
}

/* Release a file, only used on raw file handles not created by sys_open() */
void vfs_release(struct file *f)
{
	if (--f->ref == 0)
		f->op_vtable->close(f);
}

/* Close a file descriptor fd */
void vfs_close(int fd)
{
	struct file *f = vfs->fds[fd];
	vfs_release(f);
	vfs->fds[fd] = NULL;
	vfs->fds_cloexec[fd] = 0;
}

void vfs_init()
{
	log_info("vfs subsystem initializating...\n");
	mm_mmap(VFS_DATA_BASE, sizeof(struct vfs_data), PROT_READ | PROT_WRITE, MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, NULL, 0);
	struct file *console_in, *console_out;
	console_alloc(&console_in, &console_out);
	console_out->ref++;
	vfs->fds[0] = console_in;
	vfs->fds[1] = console_out;
	vfs->fds[2] = console_out;
	vfs_add(winfs_alloc());
	vfs_add(devfs_alloc());
	/* Initialize CWD */
	vfs->cwd[0] = '/';
	vfs->cwd[1] = 0;
	vfs->umask = S_IWGRP | S_IWOTH;
	socket_init();
	log_info("vfs subsystem initialized.\n");
}

void vfs_reset()
{
	/* Handle O_CLOEXEC */
	for (int i = 0; i < MAX_FD_COUNT; i++)
	{
		struct file *f = vfs->fds[i];
		if (f && vfs->fds_cloexec[i])
			vfs_close(i);
	}
	vfs->umask = S_IWGRP | S_IWOTH;
}

void vfs_shutdown()
{
	for (int i = 0; i < MAX_FD_COUNT; i++)
	{
		struct file *f = vfs->fds[i];
		if (f)
			vfs_close(i);
	}
	socket_shutdown();
	mm_munmap(VFS_DATA_BASE, sizeof(struct vfs_data));
}

int vfs_store_file(struct file *f, int cloexec)
{
	for (int i = 0; i < MAX_FD_COUNT; i++)
		if (vfs->fds[i] == NULL)
		{
			vfs->fds[i] = f;
			vfs->fds_cloexec[i] = cloexec;
			return i;
		}
	return -EMFILE;
}

DEFINE_SYSCALL(read, int, fd, char *, buf, size_t, count)
{
	log_info("read(%d, %p, %p)\n", fd, buf, count);
	struct file *f = vfs->fds[fd];
	if (f && f->op_vtable->read)
	{
		if (!mm_check_write(buf, count))
			return -EFAULT;
		return f->op_vtable->read(f, buf, count);
	}
	else
		return -EBADF;
}

DEFINE_SYSCALL(write, int, fd, const char *, buf, size_t, count)
{
	log_info("write(%d, %p, %p)\n", fd, buf, count);
	struct file *f = vfs->fds[fd];
	if (f && f->op_vtable->write)
	{
		if (!mm_check_read(buf, count))
			return -EFAULT;
		return f->op_vtable->write(f, buf, count);
	}
	else
		return -EBADF;
}

DEFINE_SYSCALL(pread64, int, fd, char *, buf, size_t, count, loff_t, offset)
{
	log_info("pread64(%d, %p, %p, %lld)\n", fd, buf, count, offset);
	struct file *f = vfs->fds[fd];
	if (f && f->op_vtable->pread)
	{
		if (!mm_check_write(buf, count))
			return -EFAULT;
		return f->op_vtable->pread(f, buf, count, offset);
	}
	else
		return -EBADF;
}

DEFINE_SYSCALL(pwrite64, int, fd, const char *, buf, size_t, count, loff_t, offset)
{
	log_info("pwrite64(%d, %p, %p, %lld)\n", fd, buf, count, offset);
	struct file *f = vfs->fds[fd];
	if (f && f->op_vtable->pwrite)
	{
		if (!mm_check_read(buf, count))
			return -EFAULT;
		return f->op_vtable->pwrite(f, buf, count, offset);
	}
	else
		return -EBADF;
}

DEFINE_SYSCALL(readv, int, fd, const struct iovec *, iov, int, iovcnt)
{
	log_info("readv(%d, 0x%p, %d)\n", fd, iov, iovcnt);
	struct file *f = vfs->fds[fd];
	if (f && f->op_vtable->read)
	{
		for (int i = 0; i < iovcnt; i++)
			if (!mm_check_write(iov[i].iov_base, iov[i].iov_len))
				return -EFAULT;
		size_t count = 0;
		for (int i = 0; i < iovcnt; i++)
		{
			int r = f->op_vtable->read(f, iov[i].iov_base, iov[i].iov_len);
			if (r < 0)
				return r;
			count += r;
			if (r < iov[i].iov_len)
				return count;
		}
		return count;
	}
	else
		return -EBADF;
}

DEFINE_SYSCALL(writev, int, fd, const struct iovec *, iov, int, iovcnt)
{
	log_info("writev(%d, 0x%p, %d)\n", fd, iov, iovcnt);
	struct file *f = vfs->fds[fd];
	if (f && f->op_vtable->write)
	{
		for (int i = 0; i < iovcnt; i++)
			if (!mm_check_read(iov[i].iov_base, iov[i].iov_len))
				return -EFAULT;
		size_t count = 0;
		for (int i = 0; i < iovcnt; i++)
		{
			int r = f->op_vtable->write(f, iov[i].iov_base, iov[i].iov_len);
			if (r < 0)
				return r;
			count += r;
			if (r < iov[i].iov_len)
				return count;
		}
		return count;
	}
	else
		return -EBADF;
}

DEFINE_SYSCALL(preadv, int, fd, const struct iovec *, iov, int, iovcnt, off_t, offset)
{
	log_info("preadv(%d, 0x%p, %d, 0x%x)\n", fd, iov, iovcnt, offset);
	struct file *f = vfs->fds[fd];
	if (f && f->op_vtable->pread)
	{
		for (int i = 0; i < iovcnt; i++)
			if (!mm_check_write(iov[i].iov_base, iov[i].iov_len))
				return -EFAULT;
		size_t count = 0;
		for (int i = 0; i < iovcnt; i++)
		{
			int r = f->op_vtable->pread(f, iov[i].iov_base, iov[i].iov_len, offset);
			if (r < 0)
				return r;
			count += r;
			offset += r;
			if (r < iov[i].iov_len)
				return count;
		}
		return count;
	}
	else
		return -EBADF;
}

DEFINE_SYSCALL(pwritev, int, fd, const struct iovec *, iov, int, iovcnt, off_t, offset)
{
	log_info("pwritev(%d, 0x%p, %d, 0x%x)\n", fd, iov, iovcnt, offset);
	struct file *f = vfs->fds[fd];
	if (f && f->op_vtable->pwrite)
	{
		for (int i = 0; i < iovcnt; i++)
			if (!mm_check_read(iov[i].iov_base, iov[i].iov_len))
				return -EFAULT;
		size_t count = 0;
		for (int i = 0; i < iovcnt; i++)
		{
			int r = f->op_vtable->pwrite(f, iov[i].iov_base, iov[i].iov_len, offset);
			if (r < 0)
				return r;
			count += r;
			offset += r;
			if (r < iov[i].iov_len)
				return count;
		}
		return count;
	}
	else
		return -EBADF;
}

DEFINE_SYSCALL(lseek, int, fd, off_t, offset, int, whence)
{
	log_info("lseek(%d, %d, %d)\n", fd, offset, whence);
	struct file *f = vfs->fds[fd];
	if (f && f->op_vtable->llseek)
	{
		loff_t n;
		int r = f->op_vtable->llseek(f, offset, &n, whence);
		if (r < 0)
			return r;
		if (n >= INT_MAX)
			return -EOVERFLOW; /* TODO: Do we need to rollback? */
		return (off_t) n;
	}
	else
		return -EBADF;
}

DEFINE_SYSCALL(llseek, int, fd, unsigned long, offset_high, unsigned long, offset_low, loff_t *, result, int, whence)
{
	loff_t offset = ((uint64_t) offset_high << 32ULL) + offset_low;
	log_info("llseek(%d, %lld, %p, %d)\n", fd, offset, result, whence);
	struct file *f = vfs->fds[fd];
	if (f && f->op_vtable->llseek)
	{
		if (!mm_check_write(result, sizeof(loff_t)))
			return -EFAULT;
		return f->op_vtable->llseek(f, offset, result, whence);
	}
	else
		return -EBADF;
}

static int find_filesystem(const char *path, struct file_system **out_fs, char **out_subpath)
{
	struct file_system *fs;
	for (fs = vfs->fs_first; fs; fs = fs->next)
	{
		char *p = fs->mountpoint;
		char *subpath = path;
		while (*p && *p == *subpath)
		{
			p++;
			subpath++;
		}
		if (*p == 0)
		{
			*out_fs = fs;
			*out_subpath = subpath;
			if (**out_subpath == '/')
				(*out_subpath)++;
			return 1;
		}
	}
	return 0;
}

/* Normalize a unix path: remove redundant "/", "." and ".."
   We allow aliasing `current` and `out` */
static int normalize_path(const char *current, const char *pathname, char *out)
{
	/* TODO: Avoid overflow */
	char *p = out;
	if (*pathname == '/')
	{
		*p++ = '/';
		pathname++;
	}
	else
	{
		if (current == out)
			p += strlen(current);
		else
		{
			while (*current)
				*p++ = *current++;
		}
		if (p[-1] != '/')
			*p++ = '/';
	}
	while (pathname[0])
	{
		if (pathname[0] == '/')
			pathname++;
		else if (pathname[0] == '.' && pathname[1] == '/')
			pathname += 2;
		else if (pathname[0] == '.' && pathname[1] == 0)
			/* Do not omit the tailing dot, as e.g. "/blah/" is not equivalent for "/blah/." for O_NOFOLLOW
			 * when "/blah" is a symlink to some other directory.
			 */
			*p++ = *pathname++;
		else if (pathname[0] == '.' && pathname[1] == '.' && (pathname[2] == '/' || pathname[2] == 0))
		{
			if (pathname[2] == 0)
				pathname += 2;
			else
				pathname += 3;
			p--;
			while (p > out && p[-1] != '/')
				p--;
		}
		else
		{
			while (*pathname && *pathname != '/')
				*p++ = *pathname++;
			if (*pathname == '/')
				*p++ = *pathname++;
		}
	}
	/* Remove redundant "/" mark at tail, unless the whole path is just "/" */
	if (p - 1 > out && p[-1] == '/')
		p[-1] = 0;
	else
		*p = 0;
	return 1;
}

/*
Test if a component of the given path is a symlink
Return 0 for success, errno for error
*/
static int resolve_symlink(struct file_system *fs, char *path, char *subpath, char *target)
{
	if (!fs->readlink)
	{
		log_warning("The underlying filesystem does not support symlink.\n");
		return -ENOENT;
	}
	/* Test from right to left */
	/* Note: Currently we assume the symlink only appears in subpath */
	int found = 0;
	log_info("PATH: %s\n", path);
	for (char *p = subpath + strlen(subpath) - 1; p > subpath; p--)
	{
		if (*p == '/')
		{
			*p = 0;
			log_info("Testing %s\n", path);
			int r = fs->readlink(subpath, target, MAX_PATH);
			if (r >= 0)
			{
				log_info("It is a symlink, target: %s\n", target);
				found = 1;
				/* Combine symlink target with remaining path */
				char *q = p + 1;
				char *t = target + r;
				if (*t != '/')
					*t++ = '/';
				while (*q)
					*t++ = *q++;
				*t++ = 0;
				/* Remove symlink basename from path */
				while (p[-1] != '/')
					p--;
				p[0] = 0;
				/* Combine heading file path with remaining path */
				if (!normalize_path(path, target, path))
					return -ENOENT;
				break;
			}
			else if (r != -ENOENT)
				/* A component exists, or i/o failed, returning failure */
				return r;
			*p = '/';
		}
	}
	if (!found)
	{
		log_warning("No component is a symlink.\n");
		return -ENOENT;
	}
	return 0;
}

int vfs_open(const char *pathname, int flags, int mode, struct file **f)
{
	/*
	Supported flags:
	* O_APPEND
	o O_ASYNC
	* O_CLOEXEC
	o O_DIRECT
	* O_DIRECTORY
	o O_DSYNC
	* O_EXCL
	o O_LARGEFILE
	o O_NOATIME
	o O_NOCTTY
	* O_NOFOLLOW
	o O_NONBLOCK
	* O_PATH
	* O_RDONLY
	* O_RDWR
	o O_SYNC
	o O_TMPFILE
	* O_TRUNC
	* O_WRONLY
	All filesystem not supporting these flags should explicitly check "flags" parameter
	*/
	if ((flags & O_DIRECT)
		|| (flags & O_DSYNC)
		|| (flags & O_LARGEFILE) || (flags & O_NOATIME) || (flags & O_NOCTTY)
		|| (flags & O_NONBLOCK) || (flags & O_SYNC) || (flags & O_TMPFILE))
	{
		log_error("Unsupported flag combination found.\n");
		//return -EINVAL;
	}
	if (mode != 0)
	{
		log_error("mode != 0\n");
		//return -EINVAL;
	}
	/* Resolve path */
	char path[MAX_PATH], target[MAX_PATH];
	if (!normalize_path(vfs->cwd, pathname, path))
		return -ENOENT;
	for (int symlink_level = 0;; symlink_level++)
	{
		if (symlink_level == MAX_SYMLINK_LEVEL)
			return -ELOOP;
		/* Find filesystem */
		struct file_system *fs;
		char *subpath;
		if (!find_filesystem(path, &fs, &subpath))
			return -ENOENT;
		if (!fs->open)
			return -ENOENT;
		/* Try opening the file directly */
		log_info("Try opening %s\n", path);
		int ret = fs->open(*subpath ? subpath : ".", flags, mode, f, target, MAX_PATH);
		if (ret == 0)
		{
			/* We're done opening the file */
			log_info("Open file succeeded.\n");
			return 0;
		}
		else if (ret == 1)
		{
			/* The file is a symlink, continue symlink resolving */
			log_info("It is a symlink, target: %s\n", target);
			/* Remove basename */
			char *p = path + strlen(path) - 1;
			while (*p != '/')
				p--;
			p[1] = 0;
			/* Combine file path with symlink target */
			if (!normalize_path(path, target, path))
				return -ENOENT;
		}
		else if (ret == -ENOENT)
		{
			log_info("Open file failed, testing whether a component is a symlink...\n");
			if (resolve_symlink(fs, path, subpath, target) < 0)
				return ret;
		}
		else
		{
			log_warning("Open file error.\n");
			return ret;
		}
	}
}

DEFINE_SYSCALL(open, const char *, pathname, int, flags, int, mode)
{
	log_info("open(%p: \"%s\", %x, %x)\n", pathname, pathname, flags, mode);
	if (!mm_check_read_string(pathname))
		return -EFAULT;
	struct file *f;
	int r = vfs_open(pathname, flags, mode, &f);
	if (r < 0)
		return r;
	int fd = vfs_store_file(f, (flags & O_CLOEXEC) > 0);
	if (fd < 0)
		vfs_release(f);
	return fd;
}

DEFINE_SYSCALL(close, int, fd)
{
	log_info("close(%d)\n", fd);
	struct file *f = vfs->fds[fd];
	if (!f)
		return -EBADF;
	vfs_close(fd);
	return 0;
}

DEFINE_SYSCALL(mknod, const char *, pathname, int, mode, unsigned int, dev)
{
	log_info("mknod(\"%s\", %x, (%d:%d))", pathname, mode, major(dev), minor(dev));
	if (!mm_check_read_string(pathname))
		return -EFAULT;
	/* TODO: Touch that file */
	return 0;
}

DEFINE_SYSCALL(link, const char *, oldpath, const char *, newpath)
{
	log_info("link(\"%s\", \"%s\")\n", oldpath, newpath);
	if (!mm_check_read_string(oldpath) || !mm_check_read_string(newpath))
		return -EFAULT;
	struct file *f;
	char path[MAX_PATH], target[MAX_PATH];
	if (!normalize_path(vfs->cwd, newpath, path))
		return -ENOENT;
	int r = vfs_open(oldpath, O_PATH | O_NOFOLLOW, 0, &f);
	if (r < 0)
		return r;
	if (!winfs_is_winfile(f))
		return -EPERM;
	for (int symlink_level = 0;; symlink_level++)
	{
		if (symlink_level == MAX_SYMLINK_LEVEL)
			return -ELOOP;
		struct file_system *fs;
		char *subpath;
		if (!find_filesystem(path, &fs, &subpath))
		{
			vfs_release(f);
			return -ENOENT;
		}
		log_info("Try linking file...\n");
		int ret;
		if (!fs->link)
			ret = -ENOENT;
		else
			ret = fs->link(f, subpath);
		if (ret == 0)
		{
			log_info("Link succeeded.\n");
			vfs_release(f);
			return 0;
		}
		else if (ret == -ENOENT)
		{
			log_info("Link failed, testing whether a component is a symlink...\n");
			if (resolve_symlink(fs, path, subpath, target) < 0)
			{
				vfs_release(f);
				return -ENOENT;
			}
		}
		else
		{
			vfs_release(f);
			return ret;
		}
	}
}

DEFINE_SYSCALL(unlink, const char *, pathname)
{
	log_info("unlink(\"%s\")\n", pathname);
	if (!mm_check_read_string(pathname))
		return -EFAULT;
	char path[MAX_PATH], target[MAX_PATH];
	if (!normalize_path(vfs->cwd, pathname, path))
		return -ENOENT;
	for (int symlink_level = 0;; symlink_level++)
	{
		if (symlink_level == MAX_SYMLINK_LEVEL)
			return -ELOOP;
		struct file_system *fs;
		char *subpath;
		if (!find_filesystem(path, &fs, &subpath))
			return -ENOENT;
		log_info("Try unlinking file...\n");
		int ret = fs->unlink(subpath);
		if (ret == 0)
		{
			log_info("Unlink succeeded.\n");
			return 0;
		}
		else if (ret == -ENOENT)
		{
			log_info("Unlink failed, testing whether a component is a symlink...\n");
			if (resolve_symlink(fs, path, subpath, target) < 0)
				return -ENOENT;
		}
		else
			return ret;
	}
}

DEFINE_SYSCALL(symlink, const char *, symlink_target, const char *, linkpath)
{
	log_info("symlink(\"%s\", \"%s\")\n", symlink_target, linkpath);
	if (!mm_check_read_string(symlink_target) || !mm_check_read_string(linkpath))
		return -EFAULT;
	char path[MAX_PATH], target[MAX_PATH];
	if (!normalize_path(vfs->cwd, linkpath, path))
		return -ENOENT;
	for (int symlink_level = 0;; symlink_level++)
	{
		if (symlink_level == MAX_SYMLINK_LEVEL)
			return -ELOOP;
		struct file_system *fs;
		char *subpath;
		if (!find_filesystem(path, &fs, &subpath))
			return -ENOENT;
		log_info("Try creating symlink...\n");
		int ret = fs->symlink(symlink_target, subpath);
		if (ret == 0)
		{
			log_info("Symlink succeeded.\n");
			return 0;
		}
		else if (ret == -ENOENT)
		{
			log_info("Create symlink failed, testing whether a component is a symlink...\n");
			if (resolve_symlink(fs, path, subpath, target) < 0)
				return -ENOENT;
		}
		else
			return ret;
	}
}

DEFINE_SYSCALL(readlink, const char *, pathname, char *, buf, int, bufsize)
{
	log_info("readlink(\"%s\", %p, %d)\n", pathname, buf, bufsize);
	if (!mm_check_read_string(pathname) || !mm_check_write(buf, bufsize))
		return -EFAULT;
	char path[MAX_PATH], target[MAX_PATH];
	if (!normalize_path(vfs->cwd, pathname, path))
		return -ENOENT;
	for (int symlink_level = 0;; symlink_level++)
	{
		if (symlink_level == MAX_SYMLINK_LEVEL)
			return -ELOOP;
		struct file_system *fs;
		char *subpath;
		if (!find_filesystem(path, &fs, &subpath))
			return -ENOENT;
		log_info("Try reading symlink...\n");
		int ret = fs->readlink(subpath, buf, bufsize);
		if (ret == -ENOENT)
		{
			log_info("Symlink not found, testing whether a component is a symlink...\n");
			if (resolve_symlink(fs, path, subpath, target) < 0)
				return -ENOENT;
		}
		else
			return ret;
	}
}

DEFINE_SYSCALL(pipe2, int *, pipefd, int, flags)
{
	/*
	Supported flags:
	* O_CLOEXEC
	o O_DIRECT
	o O_NONBLOCK
	*/
	log_info("pipe2(%p, %d)\n", pipefd, flags);
	if ((flags & O_DIRECT) || (flags & O_NONBLOCK))
	{
		log_error("Unsupported flags combination: %x\n", flags);
		return -EINVAL;
	}
	if (!mm_check_write(pipefd, 2 * sizeof(int)))
		return -EFAULT;
	struct file *fread, *fwrite;
	int r = pipe_alloc(&fread, &fwrite, flags);
	if (r < 0)
		return r;
	/* TODO: Deal with EMFILE error */
	int rfd = vfs_store_file(fread, (flags & O_CLOEXEC) > 0);
	if (rfd < 0)
	{
		vfs_release(fread);
		vfs_release(fwrite);
		return rfd;
	}
	int wfd = vfs_store_file(fwrite, (flags & O_CLOEXEC) > 0);
	if (wfd < 0)
	{
		vfs_close(rfd);
		vfs_release(fwrite);
		return wfd;
	}
	pipefd[0] = rfd;
	pipefd[1] = wfd;
	log_info("read fd: %d\n", rfd);
	log_info("write fd: %d\n", wfd);
	return 0;
}

DEFINE_SYSCALL(pipe, int *, pipefd)
{
	return sys_pipe2(pipefd, 0);
}

static int vfs_dup(int fd, int newfd, int flags)
{
	struct file *f = vfs_get(fd);
	if (!f)
		return -EBADF;
	if (newfd == -1)
	{
		for (int i = 0; i < MAX_FD_COUNT; i++)
			if (vfs->fds[i] == NULL)
			{
				newfd = i;
				break;
			}
		if (newfd == -1)
			return -EMFILE;
	}
	else
	{
		if (newfd == fd || newfd < 0 || newfd >= MAX_FD_COUNT)
			return -EINVAL;
		if (vfs->fds[newfd])
			vfs_close(newfd);
	}
	vfs->fds[newfd] = f;
	vfs->fds_cloexec[newfd] = !!(flags & O_CLOEXEC);
	f->ref++;
	return newfd;
}

DEFINE_SYSCALL(dup, int, fd)
{
	log_info("dup(%d)\n", fd);
	return vfs_dup(fd, -1, 0);
}

DEFINE_SYSCALL(dup2, int, fd, int, newfd)
{
	log_info("dup2(%d, %d)\n", fd, newfd);
	return vfs_dup(fd, newfd, 0);
}

DEFINE_SYSCALL(dup3, int, fd, int, newfd, int, flags)
{
	log_info("dup3(%d, %d, 0x%x)\n", fd, newfd, flags);
	return vfs_dup(fd, newfd, flags);
}

DEFINE_SYSCALL(rename, const char *, oldpath, const char *, newpath)
{
	log_info("rename(\"%s\", \"%s\")\n", oldpath, newpath);
	if (!mm_check_read_string(oldpath) || !mm_check_read_string(newpath))
		return -EFAULT;
	struct file *f;
	char path[MAX_PATH], target[MAX_PATH];
	if (!normalize_path(vfs->cwd, newpath, path))
		return -ENOENT;
	int r = vfs_open(oldpath, O_PATH | __O_DELETE | O_NOFOLLOW, 0, &f);
	if (r < 0)
		return r;
	if (!winfs_is_winfile(f))
		return -EPERM;
	for (int symlink_level = 0;; symlink_level++)
	{
		if (symlink_level == MAX_SYMLINK_LEVEL)
			return -ELOOP;
		struct file_system *fs;
		char *subpath;
		if (!find_filesystem(path, &fs, &subpath))
		{
			vfs_release(f);
			return -ENOENT;
		}
		log_info("Try renaming file...\n");
		int ret;
		if (!fs->rename)
			ret = -ENOENT;
		else
			ret = fs->rename(f, subpath);
		if (ret == 0)
		{
			log_info("Rename succeeded.\n");
			vfs_release(f);
			return 0;
		}
		else if (ret == -ENOENT)
		{
			log_info("Rename failed, testing whether a component is a symlink...\n");
			if (resolve_symlink(fs, path, subpath, target) < 0)
			{
				vfs_release(f);
				return -ENOENT;
			}
		}
		else
		{
			vfs_release(f);
			return ret;
		}
	}
}

DEFINE_SYSCALL(mkdir, const char *, pathname, int, mode)
{
	log_info("mkdir(\"%s\", %x)\n", pathname, mode);
	if (mode != 0)
		log_error("mode != 0\n");
	if (!mm_check_read_string(pathname))
		return -EFAULT;
	char path[MAX_PATH], target[MAX_PATH];
	if (!normalize_path(vfs->cwd, pathname, path))
		return -ENOENT;
	for (int symlink_level = 0;; symlink_level++)
	{
		if (symlink_level == MAX_SYMLINK_LEVEL)
			return -ELOOP;
		struct file_system *fs;
		char *subpath;
		if (!find_filesystem(path, &fs, &subpath))
			return -ENOENT;
		log_info("Try creating directory...\n");
		int ret;
		if (!fs->mkdir)
			ret = -ENOENT;
		else
			ret = fs->mkdir(subpath, mode);
		if (ret == -ENOENT)
		{
			log_info("Creating directory failed, testing whether a component is a symlink...\n");
			if (resolve_symlink(fs, path, subpath, target) < 0)
				return -ENOENT;
		}
		else
			return ret;
	}
}

DEFINE_SYSCALL(rmdir, const char *, pathname)
{
	log_info("rmdir(\"%s\")\n", pathname);
	if (!mm_check_read_string(pathname))
		return -EFAULT;
	char path[MAX_PATH], target[MAX_PATH];
	if (!normalize_path(vfs->cwd, pathname, path))
		return -ENOENT;
	for (int symlink_level = 0;; symlink_level++)
	{
		if (symlink_level == MAX_SYMLINK_LEVEL)
			return -ELOOP;
		struct file_system *fs;
		char *subpath;
		if (!find_filesystem(path, &fs, &subpath))
			return -ENOENT;
		log_info("Try removing directory...\n");
		int ret;
		if (!fs->rmdir)
			ret = -ENOENT;
		else
			ret = fs->rmdir(subpath);
		if (ret == -ENOENT)
		{
			log_info("Removing directory failed, testing whether a component is a symlink...\n");
			if (resolve_symlink(fs, path, subpath, target) < 0)
				return -ENOENT;
		}
		else
			return ret;
	}
}

static intptr_t getdents_fill(void *buffer, uint64_t inode, const wchar_t *name, int namelen, char type, size_t size)
{
	struct linux_dirent *dirent = (struct linux_dirent *)buffer;
	dirent->d_ino = inode;
	if (dirent->d_ino != inode)
		return -EOVERFLOW;
	dirent->d_off = 0; /* TODO */
	intptr_t len = utf16_to_utf8_filename(name, namelen, dirent->d_name, size);
	/* Don't care much about the size, there is guaranteed to be enough room */
	dirent->d_name[len] = 0;
	dirent->d_name[len + 1] = type;
	log_info("Added %s, inode = %llx, type = %d\n", dirent->d_name, inode, type);
	dirent->d_reclen = (sizeof(struct linux_dirent64) + len + 1 + 8) & ~(uintptr_t)8;
	return dirent->d_reclen;
}

static intptr_t getdents64_fill(void *buffer, uint64_t inode, const wchar_t *name, int namelen, char type, size_t size)
{
	struct linux_dirent64 *dirent = (struct linux_dirent64 *)buffer;
	dirent->d_ino = inode;
	dirent->d_off = 0; /* TODO */
	dirent->d_type = type;
	intptr_t len = utf16_to_utf8_filename(name, namelen, dirent->d_name, size);
	/* Don't care much about the size, there is guaranteed to be enough room */
	dirent->d_name[len] = 0;
	log_info("Added %s, inode = %llx, type = %d\n", dirent->d_name, inode, type);
	dirent->d_reclen = (sizeof(struct linux_dirent64) + len + 1 + 8) & ~(uintptr_t)8;
	return dirent->d_reclen;
}

DEFINE_SYSCALL(getdents, int, fd, struct linux_dirent *, dirent, unsigned int, count)
{
	log_info("getdents(%d, %p, %d)\n", fd, dirent, count);
	if (!mm_check_write(dirent, count))
		return -EFAULT;
	struct file *f = vfs->fds[fd];
	if (f && f->op_vtable->getdents)
		return f->op_vtable->getdents(f, dirent, count, getdents_fill);
	else
		return -EBADF;
}

DEFINE_SYSCALL(getdents64, int, fd, struct linux_dirent64 *, dirent, unsigned int, count)
{
	log_info("getdents64(%d, %p, %d)\n", fd, dirent, count);
	if (!mm_check_write(dirent, count))
		return -EFAULT;
	struct file *f = vfs->fds[fd];
	if (f && f->op_vtable->getdents)
		return f->op_vtable->getdents(f, dirent, count, getdents64_fill);
	else
		return -EBADF;
}

static int stat_from_newstat(struct stat *stat, const struct newstat *newstat)
{
	INIT_STRUCT_STAT_PADDING(stat);
	stat->st_dev = newstat->st_dev;
	stat->st_ino = newstat->st_ino;
	if (stat->st_ino != newstat->st_ino)
		return -EOVERFLOW;
	stat->st_mode = newstat->st_mode;
	stat->st_nlink = newstat->st_nlink;
	if (stat->st_nlink != newstat->st_nlink)
		return -EOVERFLOW;
	stat->st_uid = newstat->st_uid;
	stat->st_gid = newstat->st_gid;
	stat->st_rdev = newstat->st_rdev;
	stat->st_size = newstat->st_size;
	stat->st_blksize = newstat->st_blksize;
	stat->st_blocks = newstat->st_blocks;
	stat->st_atime = newstat->st_atime;
	stat->st_atime_nsec = newstat->st_atime_nsec;
	stat->st_mtime = newstat->st_mtime;
	stat->st_mtime_nsec = newstat->st_mtime_nsec;
	stat->st_ctime = newstat->st_ctime;
	stat->st_ctime_nsec = newstat->st_ctime_nsec;
	return 0;
}

static int stat64_from_newstat(struct stat64 *stat, const struct newstat *newstat)
{
	INIT_STRUCT_STAT64_PADDING(stat);
	stat->st_dev = newstat->st_dev;
	stat->st_ino = newstat->st_ino;
	stat->st_mode = newstat->st_mode;
	stat->st_nlink = newstat->st_nlink;
	stat->st_uid = newstat->st_uid;
	stat->st_gid = newstat->st_gid;
	stat->st_rdev = newstat->st_rdev;
	stat->st_size = newstat->st_size;
	stat->st_blksize = newstat->st_blksize;
	stat->st_blocks = newstat->st_blocks;
	stat->st_atime = newstat->st_atime;
	stat->st_atime_nsec = newstat->st_atime_nsec;
	stat->st_mtime = newstat->st_mtime;
	stat->st_mtime_nsec = newstat->st_mtime_nsec;
	stat->st_ctime = newstat->st_ctime;
	stat->st_ctime_nsec = newstat->st_ctime_nsec;
	return 0;
}

static int vfs_fstat(int fd, struct newstat *stat)
{
	struct file *f = vfs->fds[fd];
	if (f && f->op_vtable->stat)
		return f->op_vtable->stat(f, stat);
	else
		return -EBADF;
}

static int vfs_stat(const char *pathname, struct newstat *stat)
{
	struct file *f;
	int r = vfs_open(pathname, O_PATH, 0, &f);
	if (r)
		return r;
	if (f->op_vtable->stat)
		r = f->op_vtable->stat(f, stat);
	else
		r = -EBADF;
	vfs_release(f);
	return r;
}

static int vfs_lstat(const char *pathname, struct newstat *stat)
{
	struct file *f;
	int r = vfs_open(pathname, O_PATH | O_NOFOLLOW, 0, &f);
	if (r)
		return r;
	if (f->op_vtable->stat)
		r = f->op_vtable->stat(f, stat);
	else
		r = -EBADF;
	vfs_release(f);
	return r;
}

DEFINE_SYSCALL(newfstat, int, fd, struct newstat *, buf)
{
	log_info("newfstat(%d, %p)\n", fd, buf);
	if (!mm_check_write(buf, sizeof(struct newstat)))
		return -EFAULT;
	return vfs_fstat(fd, buf);
}

DEFINE_SYSCALL(newstat, const char *, pathname, struct newstat *, buf)
{
	log_info("newstat(\"%s\", %p)\n", pathname, buf);
	if (!mm_check_read_string(pathname) || !mm_check_write(buf, sizeof(struct newstat)))
		return -EFAULT;
	return vfs_stat(pathname, buf);
}

DEFINE_SYSCALL(newlstat, const char *, pathname, struct newstat *, buf)
{
	log_info("newlstat(\"%s\", %p)\n", pathname, buf);
	if (!mm_check_read_string(pathname) || !mm_check_write(buf, sizeof(struct newstat)))
		return -EFAULT;
	return vfs_lstat(pathname, buf);
}

DEFINE_SYSCALL(fstat64, int, fd, struct stat64 *, buf)
{
	log_info("fstat64(%d, %p)\n", fd, buf);
	if (!mm_check_write(buf, sizeof(struct stat64)))
		return -EFAULT;
	struct newstat stat;
	int r = vfs_fstat(fd, &stat);
	if (r)
		return r;
	return stat64_from_newstat(buf, &stat);
}

DEFINE_SYSCALL(stat64, const char *, pathname, struct stat64 *, buf)
{
	log_info("stat64(\"%s\", %p)\n", pathname, buf);
	if (!mm_check_write(buf, sizeof(struct stat64)))
		return -EFAULT;
	struct newstat stat;
	int r = vfs_stat(pathname, &stat);
	if (r)
		return r;
	return stat64_from_newstat(buf, &stat);
}

DEFINE_SYSCALL(lstat64, const char *, pathname, struct stat64 *, buf)
{
	log_info("lstat64(\"%s\", %p)\n", pathname, buf);
	if (!mm_check_write(buf, sizeof(struct stat64)))
		return -EFAULT;
	struct newstat stat;
	int r = vfs_lstat(pathname, &stat);
	if (r)
		return r;
	return stat64_from_newstat(buf, &stat);
}

DEFINE_SYSCALL(fstat, int, fd, struct stat *, buf)
{
	log_info("fstat(%d, %p)\n", fd, buf);
	if (!mm_check_write(buf, sizeof(struct stat)))
		return -EFAULT;
	struct newstat stat;
	int r = vfs_fstat(fd, &stat);
	if (r)
		return r;
	return stat_from_newstat(buf, &stat);
}

DEFINE_SYSCALL(stat, const char *, pathname, struct stat *, buf)
{
	log_info("stat(\"%s\", %p)\n", pathname, buf);
	if (!mm_check_write(buf, sizeof(struct stat)))
		return -EFAULT;
	struct newstat stat;
	int r = vfs_stat(pathname, &stat);
	if (r)
		return r;
	return stat_from_newstat(buf, &stat);
}

DEFINE_SYSCALL(lstat, const char *, pathname, struct stat *, buf)
{
	log_info("lstat(\"%d\", %p)\n", pathname, buf);
	if (!mm_check_write(buf, sizeof(struct stat)))
		return -EFAULT;
	struct newstat stat;
	int r = vfs_lstat(pathname, &stat);
	if (r)
		return r;
	return stat_from_newstat(buf, &stat);
}

static int statfs_from_statfs64(struct statfs *statfs, struct statfs64 *statfs64)
{
	statfs->f_type = statfs64->f_type;
	statfs->f_bsize = statfs64->f_bsize;
	statfs->f_blocks = statfs64->f_blocks;
	if (statfs->f_blocks != statfs64->f_blocks)
		return -EOVERFLOW;
	statfs->f_bfree = statfs64->f_bfree;
	if (statfs->f_bfree != statfs64->f_bfree)
		return -EOVERFLOW;
	statfs->f_bavail = statfs64->f_bavail;
	if (statfs->f_bavail != statfs64->f_bavail)
		return -EOVERFLOW;
	statfs->f_files = statfs64->f_files;
	if (statfs->f_files != statfs64->f_files)
		return -EOVERFLOW;
	statfs->f_ffree = statfs64->f_ffree;
	if (statfs->f_ffree != statfs64->f_ffree)
		return -EOVERFLOW;
	statfs->f_fsid = statfs64->f_fsid;
	statfs->f_namelen = statfs64->f_namelen;
	statfs->f_frsize = statfs64->f_frsize;
	statfs->f_flags = statfs64->f_flags;
	statfs->f_spare[0] = 0;
	statfs->f_spare[1] = 0;
	statfs->f_spare[2] = 0;
	statfs->f_spare[3] = 0;
	return 0;
}

static int vfs_fstatfs(int fd, struct statfs64 *buf)
{
	struct file *f = vfs->fds[fd];
	if (f && f->op_vtable->statfs)
		return f->op_vtable->statfs(f, buf);
	else
		return -EBADF;
}

static int vfs_statfs(const char *pathname, struct statfs64 *buf)
{
	struct file *f;
	int r = vfs_open(pathname, O_PATH, 0, &f);
	if (r)
		return r;
	if (f->op_vtable->statfs)
		r = f->op_vtable->statfs(f, buf);
	else
		r = -EBADF;
	vfs_release(f);
	return r;
}

DEFINE_SYSCALL(fstatfs, int, fd, struct statfs *, buf)
{
	log_info("fstatfs(%d, %p)\n", fd, buf);
	if (!mm_check_write(buf, sizeof(struct statfs)))
		return -EFAULT;
	struct statfs64 statfs64;
	int r = vfs_fstatfs(fd, &statfs64);
	if (r)
		return r;
	return statfs_from_statfs64(buf, &statfs64);
}

DEFINE_SYSCALL(statfs, const char *, pathname, struct statfs *, buf)
{
	log_info("statfs(\"%s\", %p)\n", pathname, buf);
	if (!mm_check_write(buf, sizeof(struct statfs)))
		return -EFAULT;
	struct statfs64 statfs64;
	int r = vfs_statfs(pathname, &statfs64);
	if (r)
		return r;
	return statfs_from_statfs64(buf, &statfs64);
}

DEFINE_SYSCALL(fstatfs64, int, fd, size_t, sz, struct statfs64 *, buf)
{
	log_info("fstatfs64(%d, %d, %p)\n", fd, sz, buf);
	if (sz != sizeof(struct statfs64))
		return -EINVAL;
	if (!mm_check_write(buf, sizeof(struct statfs64)))
		return -EFAULT;
	return vfs_fstatfs(fd, buf);
}

DEFINE_SYSCALL(statfs64, const char *, pathname, size_t, sz, struct statfs64 *, buf)
{
	log_info("statfs64(\"%s\", %d, %p)\n", pathname, sz, buf);
	if (sz != sizeof(struct statfs64))
		return -EINVAL;
	if (!mm_check_write(buf, sizeof(struct statfs64)))
		return -EFAULT;
	return vfs_statfs(pathname, buf);
}

DEFINE_SYSCALL(fadvise64_64, int, fd, loff_t, offset, loff_t, len, int, advice)
{
	log_info("fadvise64_64(%d, %lld, %lld, %d)\n", fd, offset, len, advice);
	/* It seems windows does not support any of the fadvise semantics
	 * We simply check the validity of parameters and return
	 */
	if (!vfs->fds[fd])
		return -EBADF;
	switch (advice)
	{
	case POSIX_FADV_NORMAL:
	case POSIX_FADV_RANDOM:
	case POSIX_FADV_SEQUENTIAL:
	case POSIX_FADV_WILLNEED:
	case POSIX_FADV_DONTNEED:
	case POSIX_FADV_NOREUSE:
		return 0;
	}
	return -EINVAL;
}

DEFINE_SYSCALL(fadvise64, int, fd, loff_t, offset, size_t, len, int, advice)
{
	return sys_fadvise64_64(fd, offset, len, advice);
}

DEFINE_SYSCALL(ioctl, int, fd, unsigned int, cmd, unsigned long, arg)
{
	log_info("ioctl(%d, %x, %x)\n", fd, cmd, arg);
	struct file *f = vfs->fds[fd];
	if (f && f->op_vtable->ioctl)
		return f->op_vtable->ioctl(f, cmd, arg);
	else
		return -EBADF;
}

DEFINE_SYSCALL(utime, const char *, filename, const struct utimbuf *, times)
{
	log_info("utime(\"%s\", %p)\n", filename, times);
	if (!mm_check_read_string(filename) || (times && !mm_check_read(times, sizeof(struct utimbuf))))
		return -EFAULT;
	struct file *f;
	int r = vfs_open(filename, O_WRONLY, 0, &f);
	if (r < 0)
		return r;
	if (!times)
		r = f->op_vtable->utimens(f, NULL);
	else
	{
		struct timespec t[2];
		t[0].tv_sec = times->actime;
		t[0].tv_nsec = 0;
		t[1].tv_sec = times->modtime;
		t[1].tv_nsec = 0;
		r = f->op_vtable->utimens(f, t);
	}
	vfs_release(f);
	return r;
}

DEFINE_SYSCALL(utimes, const char *, filename, const struct timeval *, times)
{
	log_info("utimes(\"%s\", %p)\n", filename, times);
	if (!mm_check_read_string(filename) || (times && !mm_check_read(times, 2 * sizeof(struct timeval))))
		return -EFAULT;
	struct file *f;
	int r = vfs_open(filename, O_WRONLY, 0, &f);
	if (r < 0)
		return r;
	if (!times)
		r = f->op_vtable->utimens(f, NULL);
	else
	{
		struct timespec t[2];
		unix_timeval_to_unix_timespec(&times[0], &t[0]);
		unix_timeval_to_unix_timespec(&times[1], &t[1]);
		r = f->op_vtable->utimens(f, t);
	}
	vfs_release(f);
	return r;
}

DEFINE_SYSCALL(utimensat, int, dirfd, const char *, pathname, const struct timespec *, times, int, flags)
{
	log_info("utimensat(%d, \"%s\", %p, 0x%x)\n", dirfd, pathname, times, flags);
	if ((pathname && !mm_check_read_string(pathname)) || (times && !mm_check_read(times, 2 * sizeof(struct timespec))))
		return -EFAULT;
	if (!pathname)
	{
		/* Special case: use dirfd as file fd */
		struct file *f = vfs_get(dirfd);
		if (!f)
			return -EBADF;
		return f->op_vtable->utimens(f, times);
	}
	if (dirfd != AT_FDCWD)
	{
		/* TODO */
		log_error("Returning -ENOENT\n");
		return -ENOENT;
	}
	if (flags)
		log_error("flags (%x) not supported.\n", flags);
	struct file *f;
	int r = vfs_open(pathname, O_WRONLY, 0, &f);
	if (r < 0)
		return r;
	r = f->op_vtable->utimens(f, times);
	vfs_release(f);
	return r;
}

DEFINE_SYSCALL(chdir, const char *, pathname)
{
	log_info("chdir(%s)\n", pathname);
	if (!mm_check_read_string(pathname))
		return -EFAULT;
	/* TODO: Check whether pathname is a directory */
	int fd = sys_open(pathname, O_PATH, 0);
	if (fd < 0)
		return fd;
	sys_close(fd);
	normalize_path(vfs->cwd, pathname, vfs->cwd);
	/* Remove tailing "/." */
	int l = strlen(vfs->cwd);
	if (l >= 2 && vfs->cwd[l - 2] == '/' && vfs->cwd[l - 1] == '.')
	{
		if (l == 2)
			vfs->cwd[l - 1] = 0;
		else
			vfs->cwd[l - 2] = 0;
	}
	return 0;
}

DEFINE_SYSCALL(getcwd, char *, buf, size_t, size)
{
	log_info("getcwd(%p, %p): %s\n", buf, size, vfs->cwd);
	if (!mm_check_write(buf, size))
		return -EFAULT;
	if (size < strlen(vfs->cwd) + 1)
		return -ERANGE;
	strcpy(buf, vfs->cwd);
	return buf;
}

DEFINE_SYSCALL(fcntl, int, fd, int, cmd, int, arg)
{
	log_info("fcntl(%d, %d)\n", fd, cmd);
	struct file *f = vfs->fds[fd];
	if (!f)
		return -EBADF;
	switch (cmd)
	{
	case F_DUPFD:
		return sys_dup(fd);
	case F_GETFD:
	{
		int cloexec = vfs->fds_cloexec[fd];
		log_info("F_GETFD: CLOEXEC: %d\n", cloexec);
		return cloexec? FD_CLOEXEC: 0;
	}
	case F_SETFD:
	{
		int cloexec = (arg & FD_CLOEXEC)? 1: 0;
		log_info("F_SETFD: CLOEXEC: %d\n", cloexec);
		vfs->fds_cloexec[fd] = cloexec;
		return 0;
	}
	case F_GETFL:
	{
		log_info("F_GETFL: %x\n", f->flags);
		return f->flags;
	}

	default:
		log_error("Unsupported command: %d\n", cmd);
		return -EINVAL;
	}
}

DEFINE_SYSCALL(fcntl64, int, fd, int, cmd)
{
	return sys_fcntl(fd, cmd, 0);
}

DEFINE_SYSCALL(access, const char *, pathname, int, mode)
{
	log_info("access(\"%s\", %d)\n", pathname, mode);
	if (!mm_check_read_string(pathname))
		return -EFAULT;
	/* Currently emulate access behaviour by testing whether the file exists */
	struct file *f;
	int r = vfs_open(pathname, O_PATH, mode, &f);
	if (r < 0)
		return r;
	vfs_release(f);
	return 0;
}

DEFINE_SYSCALL(chmod, const char *, pathname, int, mode)
{
	log_info("chmod(\"%s\", %d)\n", pathname, mode);
	if (!mm_check_read_string(pathname))
		return -EFAULT;
	return 0;
}

DEFINE_SYSCALL(umask, int, mask)
{
	int old = vfs->umask;
	vfs->umask = mask;
	return old;
}

DEFINE_SYSCALL(chown, const char *, pathname, uid_t, owner, gid_t, group)
{
	log_info("chown(\"%s\", %d, %d)\n", pathname, owner, group);
	if (!mm_check_read_string(pathname))
		return -EFAULT;
	log_error("chown() not implemented.\n");
	return 0;
}

DEFINE_SYSCALL(fchown, int, fd, uid_t, owner, gid_t, group)
{
	log_info("fchown(%d, %d, %d)\n", fd, owner, group);
	log_error("fchown() not implemented.\n");
	return 0;
}

DEFINE_SYSCALL(openat, int, dirfd, const char *, pathname, int, flags, int, mode)
{
	log_info("openat(%d, %s, 0x%x, 0x%x)\n", dirfd, pathname, flags, mode);
	if (dirfd == AT_FDCWD)
		return sys_open(pathname, flags, mode);
	if (!mm_check_read_string(pathname))
		return -EFAULT;
	/* TODO */
	log_error("Returning -ENOENT\n");
	return -ENOENT;
}

DEFINE_SYSCALL(fstatat64, int, dirfd, const char *, pathname, struct stat64 *, buf, int, flags)
{
	log_info("fstatat64(%d, \"%s\", %p, %x)\n", dirfd, pathname, buf, flags);
	if (dirfd == AT_FDCWD)
		return sys_stat64(pathname, buf);
	if (!mm_check_read_string(pathname))
		return -EFAULT;
	/* TODO */
	log_error("fstatat64() not implemented.\n");
	return -ENOENT;
}

DEFINE_SYSCALL(unlinkat, int, dirfd, const char *, pathname, int, flags)
{
	log_info("unlinkat(%d, \"%s\", %x)\n", dirfd, pathname, flags);
	if (dirfd == AT_FDCWD)
		return sys_unlink(pathname);
	if (!mm_check_read_string(pathname))
		return -EFAULT;
	/* TODO */
	log_error("unlinkat() not implemented.\n");
	return -ENOENT;
}

DEFINE_SYSCALL(fchmodat, int, dirfd, const char *, pathname, int, mode, int, flags)
{
	log_info("fchmodat(%d, \"%s\", %d, %x)\n", dirfd, pathname, mode, flags);
	if (!mm_check_read_string(pathname))
		return -EFAULT;
	/* TODO */
	log_error("fchmodat() not implemented.\n");
	return 0;
}

DEFINE_SYSCALL(faccessat, int, dirfd, const char *, pathname, int, mode, int, flags)
{
	log_info("faccessat(%d, %s, 0x%x, 0x%x)\n", dirfd, pathname, mode, flags);
	if (dirfd == AT_FDCWD)
		return sys_access(pathname, mode); /* TODO */
	if (!mm_check_read_string(pathname))
		return -EFAULT;
	/* TODO */
	log_error("Returning -ENOENT\n");
	return -ENOENT;
}

DEFINE_SYSCALL(poll, struct linux_pollfd *, fds, int, nfds, int, timeout)
{
	log_info("poll(0x%p, %d, %d)\n", fds, nfds, timeout);
	if (!mm_check_write(fds, nfds * sizeof(struct linux_pollfd)))
		return -EFAULT;

	/* Count of handles to be waited on */
	int cnt = 0;
	/* Handles to be waited on */
	HANDLE *handles = (HANDLE *)alloca(nfds * sizeof(HANDLE));
	/* Indices of handles in the original fds[] array */
	int *indices = (int *)alloca(nfds * sizeof(int));

	if (timeout < 0)
		timeout = INFINITE;
	for (int i = 0; i < nfds; i++)
		fds[i].revents = 0;
	int num_result = 0;
	int done = 0;
	for (int i = 0; i < nfds; i++)
	{
		if (fds[i].fd < 0)
			continue;
		struct file *f = vfs->fds[fds[i].fd];
		/* TODO: Support for regular files */
		if (!f)
		{
			fds[i].revents = LINUX_POLLNVAL;
			num_result++;
			continue;
		}
		if (!f->op_vtable->get_poll_handle)
		{
			log_error("get_poll_handled() not implemented for file %d\n", fds[i].fd);
			continue;
		}
		if (f->op_vtable->get_poll_status)
		{
			int e = f->op_vtable->get_poll_status(f);
			if ((fds[i].events & e) > 0)
			{
				/* It is ready at this moment */
				fds[i].revents = fds[i].events & e;
				num_result++;
				done = 1;
				continue;
			}
		}
		int e;
		HANDLE handle = f->op_vtable->get_poll_handle(f, &e);
		if ((fds[i].events & e) > 0)
		{
			handles[cnt] = handle;
			indices[cnt] = i;
			cnt++;
		}
	}
	if (cnt && !done)
	{
		LARGE_INTEGER frequency, start;
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&start);
		int remain = timeout;
		for (;;)
		{
			DWORD result = WaitForMultipleObjects(cnt, handles, FALSE, remain);
			if (result == WAIT_TIMEOUT)
				return 0;
			else if (result < WAIT_OBJECT_0 || result >= WAIT_OBJECT_0 + cnt)
				return -ENOMEM; /* TODO: Find correct values */
			else
			{
				/* Wait successfully, fill in the revents field of that handle */
				int id = indices[result - WAIT_OBJECT_0];
				struct file *f = vfs->fds[fds[id].fd];
				/* Retrieve current event flags */
				int e;
				if (f->op_vtable->get_poll_status)
				{
					/* The file descriptor provides get_poll_status() function, use this to query precise event flags */
					e = f->op_vtable->get_poll_status(f);
				}
				else
				{
					/* Otherwise, the event flags associated with the poll object is used */
					f->op_vtable->get_poll_handle(f, &e);
				}
				if ((e & fds[id].events) == 0)
				{
					/*
					 * Some file descriptors (console, socket) may be not readable even if it is signaled
					 * Query the state again to make sure
					 */
					LARGE_INTEGER current;
					QueryPerformanceCounter(&current);
					if (timeout != INFINITE)
					{
						remain = timeout - (current.QuadPart - start.QuadPart) / (frequency.QuadPart * 1000LL);
						if (remain < 0)
							break;
					}
					continue;
				}
				fds[id].revents = fds[id].events & e;
				num_result++;
				break;
			}
		}
	}
	return num_result;
}

DEFINE_SYSCALL(select, int, nfds, struct fdset *, readfds, struct fdset *, writefds, struct fdset *, exceptfds, struct timeval *, timeout)
{
	log_info("select(%d, 0x%p, 0x%p, 0x%p, 0x%p)\n", nfds, readfds, writefds, exceptfds, timeout);
	if ((readfds && !mm_check_write(readfds, sizeof(struct fdset)))
		|| (writefds && !mm_check_write(writefds, sizeof(struct fdset)))
		|| (exceptfds && !mm_check_write(exceptfds, sizeof(struct fdset)))
		|| (timeout && !mm_check_read(timeout, sizeof(struct timeval))))
		return -EFAULT;
	int time;
	if (timeout)
		time = timeout->tv_sec * 1000 + timeout->tv_usec / 1000;
	else
		time = -1;
	int cnt = 0;
	struct linux_pollfd *fds = (struct linux_pollfd *)alloca(sizeof(struct linux_pollfd) * nfds);
	for (int i = 0; i < nfds; i++)
	{
		int events = 0;
		if (readfds && LINUX_FD_ISSET(i, readfds))
			events |= LINUX_POLLIN;
		if (writefds && LINUX_FD_ISSET(i, writefds))
			events |= LINUX_POLLOUT;
		if (exceptfds && LINUX_FD_ISSET(i, exceptfds))
			events |= LINUX_POLLERR;
		if (events)
		{
			fds[cnt].fd = i;
			fds[cnt].events = events;
			cnt++;
		}
	}
	int r = sys_poll(fds, cnt, time);
	if (r <= 0)
		return r;
	if (readfds)
		LINUX_FD_ZERO(nfds, readfds);
	if (writefds)
		LINUX_FD_ZERO(nfds, writefds);
	if (exceptfds)
		LINUX_FD_ZERO(nfds, exceptfds);
	for (int i = 0; i < nfds; i++)
	{
		if (readfds && (fds[i].revents & LINUX_POLLIN))
			LINUX_FD_SET(i, readfds);
		if (writefds && (fds[i].revents & LINUX_POLLOUT))
			LINUX_FD_SET(i, writefds);
		if (exceptfds && (fds[i].revents & LINUX_POLLERR))
			LINUX_FD_SET(i, exceptfds);
	}
	return r;
}
