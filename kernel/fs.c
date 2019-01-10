
#include "fs.h"
#include "fs_internal.h"
#include "kmalloc.h"
#include "string.h"
#include "memory.h"
#include "process.h"

static struct fs *fs_list = 0;

struct fs_dirent *fs_resolve(const char *path)
{
	if(path[0] == '/') {
		return fs_dirent_namei(current->root_dir, &path[1]);
	} else {
		return fs_dirent_namei(current->current_dir, path);
	}
}

void fs_register(struct fs *f)
{
	f->next = fs_list;
	fs_list = f;
}

struct fs *fs_lookup(const char *name)
{
	struct fs *f;

	for(f = fs_list; f; f = f->next) {
		if(!strcmp(name, f->name)) {
			return f;
		}
	}
	return 0;
}

int fs_volume_format(struct fs *f, struct device *d )
{
	const struct fs_ops *ops = f->ops;
	if(!ops->volume_format)
		return KERROR_NOT_IMPLEMENTED;
	return f->ops->volume_format(d);
}

struct fs_volume *fs_volume_open(struct fs *f, struct device *d )
{
	const struct fs_ops *ops = f->ops;

	if(!ops->volume_open)
		return 0;

	struct fs_volume *v = f->ops->volume_open(d);
	if(v) {
		v->fs = f;
		v->device = device_addref(d);
	}
	return v;
}

struct fs_volume *fs_volume_addref(struct fs_volume *v)
{
	v->refcount++;
	return v;
}

int fs_volume_close(struct fs_volume *v)
{
	const struct fs_ops *ops = v->fs->ops;
	if(!ops->volume_close)
		return KERROR_NOT_IMPLEMENTED;

	v->refcount--;
	if(v->refcount <= 0) {
		v->fs->ops->volume_close(v);
		device_close(v->device);
		kfree(v);
	}

	return -1;
}

struct fs_dirent *fs_volume_root(struct fs_volume *v)
{
	const struct fs_ops *ops = v->fs->ops;
	if(!ops->volume_root)
		return 0;

	struct fs_dirent *d = v->fs->ops->volume_root(v);
	d->v = fs_volume_addref(v);
	return d;
}

int fs_dirent_readdir(struct fs_dirent *d, char *buffer, int buffer_length)
{
	const struct fs_ops *ops = d->v->fs->ops;
	if(!ops->readdir)
		return KERROR_NOT_IMPLEMENTED;
	return ops->readdir(d, buffer, buffer_length);
}

static struct fs_dirent *fs_dirent_lookup(struct fs_dirent *d, const char *name)
{
	const struct fs_ops *ops = d->v->fs->ops;

	if(!ops->lookup)
		return 0;

	if(!strcmp(name,".")) {
		// Special case: . refers to the containing directory.
		return fs_dirent_addref(d);
	} else {
		struct fs_dirent *r = ops->lookup(d, name);
		if(r) r->v = fs_volume_addref(d->v);
		return r;
	}
}

struct fs_dirent *fs_dirent_namei(struct fs_dirent *parent, const char *path)
{
	if(!parent || !path)
		return 0;

	char *lpath = kmalloc(strlen(path) + 1);
	strcpy(lpath, path);

	struct fs_dirent *d = parent;

	char *part = strtok(lpath, "/");
	while(part) {
		struct fs_dirent *n = fs_dirent_lookup(d, part);

		if(d!=parent) fs_dirent_close(d);

		if(!n) {
			// KERROR_NOT_FOUND
			kfree(lpath);
			return 0;
		}
		d = n;
		part = strtok(0, "/");
	}
	kfree(lpath);
	return d;
}

struct fs_dirent *fs_dirent_addref(struct fs_dirent *d)
{
	d->refcount++;
	return d;
}

int fs_dirent_close(struct fs_dirent *d)
{
	const struct fs_ops *ops = d->v->fs->ops;
	if(!ops->close)
		return KERROR_NOT_IMPLEMENTED;

	d->refcount--;
	if(d->refcount <= 0) {
		ops->close(d);
		// This close is paired with the addref in fs_dirent_lookup
		fs_volume_close(d->v);
		kfree(d);
	}

	return 0;
}

int fs_dirent_read(struct fs_dirent *d, char *buffer, uint32_t length, uint32_t offset)
{
	int total = 0;
	int bs = d->v->block_size;

	const struct fs_ops *ops = d->v->fs->ops;
	if(!ops->read_block)
		return KERROR_INVALID_REQUEST;

	if(offset > d->size) {
		return 0;
	}

	if(offset + length > d->size) {
		length = d->size - offset;
	}

	char *temp = memory_alloc_page(0);
	if(!temp)
		return -1;

	while(length > 0) {

		int blocknum = offset / bs;
		int actual = 0;

		if(offset % bs) {
			actual = ops->read_block(d, temp, blocknum);
			if(actual != bs)
				goto failure;
			actual = MIN(bs - offset % bs, length);
			memcpy(buffer, &temp[offset % bs], actual);
		} else if(length >= bs) {
			actual = ops->read_block(d, buffer, blocknum);
			if(actual != bs)
				goto failure;
		} else {
			actual = ops->read_block(d, temp, blocknum);
			if(actual != bs)
				goto failure;
			actual = length;
			memcpy(buffer, temp, actual);
		}

		buffer += actual;
		length -= actual;
		offset += actual;
		total += actual;
	}

	memory_free_page(temp);
	return total;

      failure:
	memory_free_page(temp);
	if(total == 0)
		return -1;
	return total;
}

struct fs_dirent * fs_dirent_mkdir(struct fs_dirent *d, const char *name)
{
	const struct fs_ops *ops = d->v->fs->ops;
	if(!ops->mkdir) return 0;

	struct fs_dirent *n = ops->mkdir(d, name);
	if(n) {
		n->v = fs_volume_addref(d->v);
		return n;
	}

	return 0;
}

struct fs_dirent * fs_dirent_mkfile(struct fs_dirent *d, const char *name)
{
	const struct fs_ops *ops = d->v->fs->ops;
	if(!ops->mkfile) return 0;

	struct fs_dirent *n = ops->mkfile(d, name);
	if(n) {
		n->v = fs_volume_addref(d->v);
		return n;
	}

	return 0;
}

int fs_dirent_remove(struct fs_dirent *d, const char *name)
{
	const struct fs_ops *ops = d->v->fs->ops;
	if(!ops->remove)
		return 0;
	return ops->remove(d, name);
}

int fs_dirent_write(struct fs_dirent *d, const char *buffer, uint32_t length, uint32_t offset)
{
	int total = 0;
	int bs = d->v->block_size;

	const struct fs_ops *ops = d->v->fs->ops;
	if(!ops->write_block || !ops->read_block)
		return KERROR_INVALID_REQUEST;

	char *temp = memory_alloc_page(0);

	// if writing past the (current) end of the file, resize the file first
	if (offset + length > d->size) {
		ops->resize(d, offset+length);
	}

	while(length > 0) {

		int blocknum = offset / bs;
		int actual = 0;

		if(offset % bs) {
			actual = ops->read_block(d, temp, blocknum);
			if(actual != bs)
				goto failure;

			actual = MIN(bs - offset % bs, length);
			memcpy(&temp[offset % bs], buffer, actual);

			int wactual = ops->write_block(d, temp, blocknum);
			if(wactual != bs)
				goto failure;

		} else if(length >= bs) {
			actual = ops->write_block(d, buffer, blocknum);
			if(actual != bs)
				goto failure;
		} else {
			actual = ops->read_block(d, temp, blocknum);
			if(actual != bs)
				goto failure;

			actual = length;
			memcpy(temp, buffer, actual);

			int wactual = ops->write_block(d, temp, blocknum);
			if(wactual != bs)
				goto failure;
		}

		buffer += actual;
		length -= actual;
		offset += actual;
		total += actual;
	}

	memory_free_page(temp);
	return total;

      failure:
	memory_free_page(temp);
	if(total == 0)
		return -1;
	return total;
}

int fs_dirent_size(struct fs_dirent *d)
{
	return d->size;
}

int fs_dirent_isdir( struct fs_dirent *d )
{
	return d->isdir;
}

int fs_dirent_copy(struct fs_dirent *src, struct fs_dirent *dst, int depth )
{
	char *buffer = memory_alloc_page(1);

	int length = fs_dirent_readdir(src, buffer, PAGE_SIZE);
	if (length <= 0) goto failure;

	char *name = buffer;
	while (name && (name - buffer) < length) {

		// Skip relative directory entries.
		if (strcmp(name,".") == 0 || (strcmp(name, "..") == 0)) {
			goto next_entry;
		}

		struct fs_dirent *new_src = fs_dirent_lookup(src, name);
		if(!new_src) {
			printf("couldn't lookup %s in directory!\n",name);
			goto next_entry;
		}

		int i;
		for(i=0;i<depth;i++) printf(">");

		if(fs_dirent_isdir(new_src)) {
			printf("%s (dir)\n", name);
			struct fs_dirent *new_dst = fs_dirent_mkdir(dst,name);
			if(!new_dst) {
				printf("couldn't create %s!\n",name);
				fs_dirent_close(new_src);
				goto next_entry;
			}
			int res = fs_dirent_copy(new_src, new_dst,depth+1);
			fs_dirent_close(new_dst);
			if(res<0) goto failure;
		} else {
			printf("%s (%d bytes)\n", name,fs_dirent_size(new_src));
			struct fs_dirent *new_dst = fs_dirent_mkfile(dst, name);
			if(!new_dst) {
				printf("couldn't create %s!\n",name);
				fs_dirent_close(new_src);
				goto next_entry;
			}

			char * filebuf = memory_alloc_page(0);
			if (!filebuf) {
				fs_dirent_close(new_src);
				fs_dirent_close(new_dst);
				goto failure;
			}

			uint32_t file_size = fs_dirent_size(new_src);
			uint32_t offset = 0;

			while(offset<file_size) {
				uint32_t chunk = MIN(PAGE_SIZE,file_size-offset);
				fs_dirent_read(new_src, filebuf, chunk, offset );
				fs_dirent_write(new_dst, filebuf, chunk, offset );
				offset += chunk;
			}

			memory_free_page(filebuf);

			fs_dirent_close(new_dst);
		}

		fs_dirent_close(new_src);

		next_entry:
		name += strlen(name) + 1;
	}

	memory_free_page(buffer);
	return 0;

failure:
	memory_free_page(buffer);
	return KERROR_NOT_FOUND;
}
