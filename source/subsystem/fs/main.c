/**
	Filesystem framework
	Copyright(c) 2003-2013 Sihai(yaosihai@yeah.net).
	All rights reserved.
*/

#include <kernel/ke_memory.h>
#include <kernel/ke_srv.h>

#include <ddk/compiler.h>
#include <errno.h>

#include <fss.h>
#include <vfs.h>
#include <node.h>
#include <cache.h>
#include <fsnotify.h>

#include "sys/file_req.h"

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
		ke_rwlock_write_lock(&fss.vol_list_lock);
		list_add_tail(&v->list, &fss.vol_list);
		ke_rwlock_write_unlock(&fss.vol_list_lock);
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

	//TODO 支持更多
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
void fss_dbd_make_dirty(struct fss_file * who, struct dbd *which)
{
	/* 回写线程将通过dirty 来判断该不该回写 */
	FSS_DBD_LOCK(which);
	which->flags |= DB_DIRTY;
	FSS_DBD_UNLOCK(which);
}

ssize_t fss_dbd_make_valid(struct fss_file * who, struct dbd * which)
{
	unsigned long size = FSS_CACHE_DB_SIZE;
	int ret = -EBADF;
	
	FSS_DBD_LOCK(which);

	if (which->flags & DB_VALID_DATA)
	{
		ret = which->valid_size;
		goto end;
	}

	/* Just an entry, not really opened by the file system driver */
	if (who->private == NULL)
		goto end;	
	ret = who->volumn->drv->ops->fRead(who->private, which->block_id,
									   which->buffer, &size);
	which->valid_size = size;
	which->flags |= DB_VALID_DATA;

end:
	FSS_DBD_UNLOCK(which);
	return ret;
}

struct fss_file *fss_open(struct fss_file *current_dir, char *name)
{
	struct fss_file *f;

	/* 搜索文件将增加文件的引用计数 */
	f = fss_loop_file(current_dir, name, NULL, NULL);
	if (!f)
		goto err0;
	
	if (!f->private)
	{
		f->private = f->volumn->drv->ops->fOpen(f->parent->private, f->name);
		if (f->private == NULL)
			goto err1;
	}
	return f;
	
err1:
	fss_close(f);
err0:
	return NULL;
}

void fss_close(struct fss_file *who)
{
	//TODO
}

ssize_t fss_block_io(struct fss_file * who, unsigned long block, void *buffer, bool write)
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
	if (!write)
	{
		memcpy(buffer, which->buffer, FSS_CACHE_DB_SIZE);
		fnotify_msg_send(who, Y_FILE_EVENT_READ);
	}
	else
	{
		memcpy(which->buffer, buffer, FSS_CACHE_DB_SIZE);		
		fss_dbd_make_dirty(who, which);
		fnotify_msg_send(who, Y_FILE_EVENT_WRITE);
	}
	ret = which->valid_size;
	
end:
	if (which)		
		fss_dbd_put(which);
	return ret;
}

lsize_t fss_get_size(struct fss_file *who)
{
	//TODO
	//另外fss_get_size 的调用者可能不是用lsize_t存储的，有数据溢出问题
	return 1024*1024;
}

/************************************************************************/
/* User system call handler                                             */
/************************************************************************/
static bool check_user_buffer(void *buf, size_t size, int write)
{
	//TODO: Call kernel to validate the buffer 
	return true;
}

static ke_handle req_open(struct sysreq_file_open *req)
{
	ke_handle h;
	struct fss_file *filp, *current_dir = NULL;
	
	//TODO: to support current path
	filp = fss_open(current_dir, req->name);
	if (!filp)
		goto err;
	
	h = ke_handle_create(filp);
	if (h == KE_INVALID_HANDLE)
		goto err;

	req->file_size = fss_get_size(filp);
	
	return h;
	
err:
	if (filp)
		fss_close(filp);
	return KE_INVALID_HANDLE;
}

static void req_close(struct sysreq_file_close *req)
{
	ke_handle h = req->file;
	struct fss_file *filp = ke_handle_translate(h);
	
	if (filp)
	{
		fss_close(filp);
		ke_handle_put(h, filp);
	}
	
	ke_handle_delete(h);
}

static ssize_t req_read(struct sysreq_file_io *req)
{
	ssize_t ret = -1;
	struct fss_file *file = NULL;
	unsigned long block = req->pos / FSS_CACHE_DB_SIZE;
	void *buf = req->buffer;

	if (req->size != FSS_CACHE_DB_SIZE)
		goto end;

	/* The user buffer can be written? */
	if (check_user_buffer(buf, req->size, 1) == false)
		goto end;
	file = ke_handle_translate(req->file);
	if (!file)
		goto end;

	ret = fss_block_io(file, block, buf, false);
	if (ret < 0)
		goto end;

end:
	/* Update file size */
	req->current_size = fss_get_size(file);
	
	if (file)
		ke_handle_put(req->file, file);
	return ret;
}

static ssize_t req_write(struct sysreq_file_io *req)
{
	ssize_t ret = -1;
	struct fss_file *file = NULL;
	unsigned long block = req->pos / FSS_CACHE_DB_SIZE;
	void *buf = req->buffer;

	if (req->size != FSS_CACHE_DB_SIZE)
		goto end;
	
	/* The user buffer can be read? */
	if (check_user_buffer(buf, req->size, 0) == false)
		goto end;
	file = ke_handle_translate(req->file);
	if (!file)
		goto end;

	ret = fss_block_io(file, block, buf, true);
	if (ret < 0)
		goto end;

end:
	/* Update file size */
	req->current_size = fss_get_size(file);
	
	if (file)
		ke_handle_put(req->file, file);
	return ret;

}

/*
	readdir:目录读取的系统调用
	返回目录下文件名,索引号,文件块大小
	文件权限,创建时间
	@return:
		>= 0 写入请求缓冲区字节数  < 0 错误号
*/
static size_t req_readdir(struct sysreq_file_readdir *req)
{
	struct fss_file *file = NULL, *child_file;
	struct dirent_buffer *entry;
	int rd_count = 0, cpy_count = 0;
	int name_len, cpy_len = 0;
	int err;
	
	/* 请求 entry 是否为0 ? */
	if (!req->max_size)
	{
		err = -EINVAL;
		goto err;
	}

	/* 请求缓冲是否合法? */
	if (check_user_buffer(req->buffer, req->max_size, 1) == false)
	{
		err = -EINVAL;
		goto err;
	}
	
	file = ke_handle_translate(req->dir);	
	/* 文件不存在或者文件不为目录? */
	if ((!file) || (file->type != FSS_FILE_TYPE_DIR))
	{
		err = -ENOTDIR;
		goto err;
	}
	
	/* 该目录下文件读到内存目录树嘛? */
	if (!(file->t.dir.tree_flags & FSS_FILE_TREE_COMPLETION) &&
		fss_tree_make_full(file) < 0)
	{
		err = -EIO;
		goto err;
	}
	
	/* 
		读取目录下文件到req readdir_entry中.
		并通过file->brother链接到dir
		->child_head内.
	*/

	list_for_each_entry(child_file, &(file->t.dir.child_head), brother)
	{
		/* 寻找这次读取文件的起点 */
		if (rd_count < req->start_entry)
		{
			rd_count++;
			continue;
		}

		/* 是否还能往buffer中继续写? */
		if (cpy_len + sizeof(struct dirent_buffer) > req->max_size)
			break;
		
		/* 
			写入用户buffer内 
			能copy 文件名长度
		*/
		name_len = FSS_STRLEN(child_file->name);
		name_len = (name_len > (req->max_size - cpy_len - sizeof(struct dirent_buffer))) ?\
					(req->max_size - cpy_len - sizeof(struct dirent_buffer)) :\
					name_len;
		
		entry = (void *)req->buffer + cpy_len;
		entry->entry_type  = child_file->type;
		entry->name_length = name_len;
		FSS_STRNCPY(entry->name, child_file->name, name_len);

		cpy_len += sizeof(struct dirent_buffer) + entry->name_length;
		++cpy_count;
	}

	/* 
		记录下次读取目录下文件的起点 
	*/
	req->next_entry = req->start_entry + rd_count + cpy_count;
	
	err = cpy_len;
err:
	if (file)
		ke_handle_put(req->dir, file);
	return err;
}

/**
	@brief 文件通知操作

	req 中的ops 有表示具体是什么操作

	@return
		error code
*/
static int req_notify(struct sysreq_file_notify *req)
{
	int r;
	struct fss_file *file;

	if (NULL == (file = ke_handle_translate(req->file)))
	{
		r = -EINVAL;
		goto end;
	}
	
	switch(req->ops)
	{
	case SYSREQ_FILE_OPS_REG_FILE_NOTIFY:
		r = fnotify_event_register(file, req->ops_private.reg.mask,\
									req->ops_private.reg.func, req->ops_private.reg.para);
		break;
		
	case SYSREQ_FILE_OPS_UNREG_FILE_NOTIFY:
		r = fnotify_event_unregister(file, req->ops_private.reg.mask);		
		break;
		
	default:
		r = -EINVAL;
		break;
	}

	ke_handle_put(req->file, file);
end:
	return r;	
}

/**
	@brief 处理文件影射
	
	req 中的ops 有表示具体是什么操作
	
	@return
		1,error code
		2,对于影射来说，在req 中返回影射地址，NULL表示映射失败
*/
static int req_map(struct sysreq_file_map *req)
{
	int r;
	struct fss_file *file;

	if (NULL == (file = ke_handle_translate(req->file)))
	{
		r = -EINVAL;
		goto end;
	}

	r = 0;
	switch(req->ops)
	{
	case SYSREQ_FILE_OPS_MAP:
		{
			size_t map_size = fss_get_size(file);
			void *base;
			
			/* 请求缓冲是否合法? */
			if (check_user_buffer(req, sizeof(req), 1) == false)				
			{
				r = -EINVAL;
				goto err0;
			}

			if (NULL == (base = ke_map_file(file, map_size, req->prot)))
				r = -ENOMEM;
			else
			{			
				req->map_size = map_size;
				req->map_base =  base;
			}
		}
		break;
		
	case SYSREQ_FILE_OPS_UNMAP:
		TODO("SYSREQ_FILE_OPS_UNMAP");
		r = -ENOSYS;
		break;
		
	default:
		r = -EINVAL;
		break;
	}
	
err0:
	ke_handle_put(req->file, file);
end:
	return r;
}

/* 处理函数列表，注意必须和SYS_REQ_FILE_xxx的编号一致 */
static void * interfaces[_SYS_REQ_FILE_MAX];

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
	int fs_call_num;
	
	INIT_LIST_HEAD(&fss.vol_list);
	INIT_LIST_HEAD(&fss.drv_list);
	ke_rwlock_init(&fss.vol_list_lock);

	fss_db_init();
	fss_map_init();
	
	/* 初始化fss层系统调用函数 */
	for(fs_call_num = 0; fs_call_num < _SYS_REQ_FILE_MAX; ++fs_call_num)
		interfaces[fs_call_num] = ke_srv_null_sysxcal;
	interfaces[SYS_REQ_FILE_OPEN - SYS_REQ_FILE_BASE]    = req_open;
	interfaces[SYS_REQ_FILE_CLOSE - SYS_REQ_FILE_BASE]   = req_close;
	interfaces[SYS_REQ_FILE_READ - SYS_REQ_FILE_BASE]    = req_read;
	interfaces[SYS_REQ_FILE_WRITE - SYS_REQ_FILE_BASE]   = req_write;
	interfaces[SYS_REQ_FILE_READDIR - SYS_REQ_FILE_BASE] = req_readdir;
	interfaces[SYS_REQ_FILE_NOTIFY - SYS_REQ_FILE_BASE]  = req_notify;
	interfaces[SYS_REQ_FILE_MAP - SYS_REQ_FILE_BASE]     = req_map;
	
	
	ke_srv_register(&ke_srv_fss);
}


