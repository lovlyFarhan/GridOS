/**
	Filesystem framework
	Copyright(c) 2003-2013 Sihai(yaosihai@yeah.net).
	All rights reserved.
*/

#include <kernel/ke_memory.h>
#include <kernel/ke_srv.h>

#include <ddk/compiler.h>
#include <ddk/debug.h>
#include <errno.h>

#include <vfs.h>
#include <node.h>
#include <cache.h>

static struct fss_vfs_info fss;


/************************************************************************/
/* Volumn                                                               */
/************************************************************************/
struct fss_volumn *fss_volumn_create_simple()
{
	struct fss_volumn *v = NULL;
	
	v = km_valloc(sizeof(*v));
	if (v)
	{
		memset(v, 0, sizeof(*v));
		list_add_tail(&v->list, &fss.vol_list);
	}
	
	return v;
}

struct fss_volumn *fss_volumn_search(xstring id)
{
	struct fss_volumn *t;
	int found = 0;

	ke_rwlock_read_lock(&fss.vol_list_lock);
	list_for_each_entry(t, &fss.vol_list, list)
	{
		if (!FSS_STRCMP(id, t->volumn_id))
		{
			found = 1;
			break;
		}
	}
	ke_rwlock_read_unlock(&fss.vol_list_lock);

	if (found)
		return t;
	return NULL;
}

DLLEXPORT void fss_vfs_register(struct fss_vfs_driver *driver)
{
	void *private;
	struct fss_volumn *v = NULL;
	
	printk("注册文件系统 %s...\n", driver->name);
	list_add_tail(&driver->list, &fss.drv_list);
	
	/* Probe with volumn */
	v = fss_volumn_create_simple(/*TODO: set volumn information before probe*/);
	if (v == NULL)
		goto err;
	
	private = driver->ops->fMount(NULL);
	if (private == NULL)
		goto err;
	if (fss_tree_init(v, driver, private) == false)
		goto err;
	printk("注册ok。\n");
	return;
	
err:
	if (v)
	{
		//TODO:free volumn
	}
	if (private)
	{
		//TODO:Close the root file directory
	}
	
	return;
}

/************************************************************************/
/* File                                                                 */
/************************************************************************/
static ssize_t fss_dbd_make_valid(struct fss_file * who, struct dbd * which)
{
	unsigned long size = FSS_CACHE_DB_SIZE;
	int ret = -EBADF;
	
	/* Just an entry, not really opened by the file system driver */
	if (who->private == NULL)
		goto end;
	
	ret = who->volumn->drv->ops->fRead(who->private, which->block_id,
									   which->buffer, &size);
	which->valid_size = size;
	
end:
	return ret;
}

struct fss_file *fss_open(struct fss_file *current_dir, char *name)
{
	struct fss_file *f;

	/* 搜索文件将增加文件的引用计数 */
	f = fss_loop_file(current_dir, name, NULL, NULL);
	if (!f)
		goto err0;

	f->private = f->volumn->drv->ops->fOpen(f->parent->private, f->name);
	if (f->private == NULL)
	{
		//TODO: Close the vfs
		TODO("关闭僵尸文件");
	}

	return f;
err0:
	return NULL;
}

void fss_close(struct fss_file *who)
{
	//TODO
}

ssize_t fss_read(struct fss_file * who, unsigned long block, void *buffer)
{
	ssize_t ret = -ENOMEM;
	struct dbd * which = NULL;

	/* Acquire the dbd by offset */
	which = fss_dbd_get(who, block);
	if (unlikely(!which)) 
		goto end;

	/* Fill the dbd with valid data from disk */	
	ret = fss_dbd_make_valid(who, which);
	if (ret < 0)
		goto end;
	
	printk("%s %s %d.\n", __FILE__, __FUNCTION__, __LINE__);

	/* Read */
	memcpy(buffer, which->buffer, FSS_CACHE_DB_SIZE);
	ret = which->valid_size;
	
end:
	if (which)		
		fss_dbd_put(which);
	return ret;
}

ssize_t fss_get_size(struct fss_file *who)
{
	//TODO
	return 1024;
}

/************************************************************************/
/* User system call handler                                             */
/************************************************************************/
static void *req_open()
{
	
}

static void *req_read()
{
	
}

#include "../../libs/grid/include/sys/syscall.h"

/* 处理函数列表，注意必须和SYS_REQ_FILE_xxx的编号一致 */
static void * interfaces[_SYS_REQ_FILE_MAX] = {
	req_open,
	req_read,
};

static unsigned long kernel_srv(unsigned long req_id, void *req)
{
	unsigned long (*func)(void * req) = interfaces[req_id];
	return func(req);
}

const static struct ke_srv_info ke_srv_fss = {
	.name = "FSS服务",
	.service_id_base = SYS_REQ_FILE_BASE,
	.request_enqueue = kernel_srv,
};

void fss_main()
{
	INIT_LIST_HEAD(&fss.vol_list);
	INIT_LIST_HEAD(&fss.drv_list);
	ke_rwlock_init(&fss.vol_list_lock);

	fss_db_init();
	fss_map_init();

	ke_srv_register(&ke_srv_fss);
}


