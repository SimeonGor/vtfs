#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/stat.h>
#include <linux/string.h>

#define MODULE_NAME "vtfs"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("secs-dev");
MODULE_DESCRIPTION("A simple FS kernel module");

#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)

#define VTFS_ROOT_INODE_NUMBER 1000

// Битовая маска для отслеживания созданных файлов
static unsigned long file_mask = 0;

// Прототипы функций
static struct inode* vtfs_get_inode(struct super_block* sb,
                                     const struct inode* dir,
                                     umode_t mode,
                                     int i_ino);
static int vtfs_fill_super(struct super_block *sb, void *data, int silent);
static struct dentry* vtfs_mount(struct file_system_type* fs_type,
                                  int flags,
                                  const char* token,
                                  void* data);
static void vtfs_kill_sb(struct super_block* sb);
static struct dentry* vtfs_lookup(struct inode* parent_inode,
                                   struct dentry* child_dentry,
                                   unsigned int flag);
static int vtfs_iterate(struct file* filp, struct dir_context* ctx);
static int vtfs_create(struct mnt_idmap *idmap,
                        struct inode *parent_inode,
                        struct dentry *child_dentry,
                        umode_t mode,
                        bool excl);
static int vtfs_unlink(struct inode *parent_inode,
                        struct dentry *child_dentry);

// Структуры операций для inode и файлов
static struct inode_operations vtfs_inode_ops = {
  .lookup = vtfs_lookup,
  .create = vtfs_create,
  .unlink = vtfs_unlink,
};

static struct file_operations vtfs_dir_ops = {
  .iterate_shared = vtfs_iterate,
};

// Структура описания файловой системы
static struct file_system_type vtfs_fs_type = {
  .name = "vtfs",
  .mount = vtfs_mount,
  .kill_sb = vtfs_kill_sb,
};

// Создание нового inode
static struct inode* vtfs_get_inode(struct super_block* sb,
                                     const struct inode* dir,
                                     umode_t mode,
                                     int i_ino) {
  struct inode *inode = new_inode(sb);
  if (inode != NULL) {
    inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
    inode->i_ino = i_ino;
  }
  return inode;
}

// Функция lookup - определяет что за сущность описывается данной нодой
static struct dentry* vtfs_lookup(struct inode* parent_inode,
                                   struct dentry* child_dentry,
                                   unsigned int flag) {
  ino_t root = parent_inode->i_ino;
  const char *name = child_dentry->d_name.name;
  
  // Для корневой директории
  if (root == VTFS_ROOT_INODE_NUMBER) {
    if (strcmp(name, "test.txt") == 0 && (file_mask & 1)) {
      // Создаём inode для файла test.txt
      struct inode *inode = vtfs_get_inode(parent_inode->i_sb, NULL,
                                           S_IFREG | S_IRWXUGO, 101);
      inode->i_op = &vtfs_inode_ops;
      inode->i_fop = NULL;
      d_add(child_dentry, inode);
    } else if (strcmp(name, "new_file.txt") == 0 && (file_mask & 2)) {
      // Создаём inode для файла new_file.txt
      struct inode *inode = vtfs_get_inode(parent_inode->i_sb, NULL,
                                           S_IFREG | S_IRWXUGO, 102);
      inode->i_op = &vtfs_inode_ops;
      inode->i_fop = NULL;
      d_add(child_dentry, inode);
    } else if (strcmp(name, "dir") == 0) {
      // Создаём inode для директории dir
      struct inode *inode = vtfs_get_inode(parent_inode->i_sb, NULL,
                                           S_IFDIR | S_IRWXUGO, 200);
      inode->i_op = &vtfs_inode_ops;
      inode->i_fop = &vtfs_dir_ops;
      d_add(child_dentry, inode);
    }
  }
  
  return NULL;
}

// Функция iterate - выводит список объектов в директории
static int vtfs_iterate(struct file* filp, struct dir_context* ctx) {
  struct dentry* dentry = filp->f_path.dentry;
  struct inode* inode = dentry->d_inode;
  ino_t ino = inode->i_ino;
  int emitted = 0;

  // Для корневой директории
  if (ino == VTFS_ROOT_INODE_NUMBER) {
    // Всегда показываем . и ..
    if (ctx->pos == 0) {
      dir_emit(ctx, ".", 1, ino, DT_DIR);
      ctx->pos++;
    }
    if (ctx->pos == 1) {
      dir_emit(ctx, "..", 2, dentry->d_parent->d_inode->i_ino, DT_DIR);
      ctx->pos++;
    }
    
    // Показываем test.txt только если он создан (бит 0)
    if (ctx->pos == 2) {
      if (file_mask & 1) {
        dir_emit(ctx, "test.txt", 8, 101, DT_REG);
        emitted = 1;
      }
      ctx->pos++;
    }
    
    // Показываем new_file.txt только если он создан (бит 1)
    if (ctx->pos == 3) {
      if (file_mask & 2) {
        dir_emit(ctx, "new_file.txt", 12, 102, DT_REG);
        emitted = 1;
      }
      ctx->pos++;
    }
    
    // Показываем директорию dir
    if (ctx->pos == 4) {
      dir_emit(ctx, "dir", 3, 200, DT_DIR);
      ctx->pos++;
    }
    
    return 0;
  } else if (ino == 200) {
    // Для директории dir
    if (ctx->pos == 0) {
      dir_emit(ctx, ".", 1, ino, DT_DIR);
      ctx->pos++;
    }
    if (ctx->pos == 1) {
      dir_emit(ctx, "..", 2, dentry->d_parent->d_inode->i_ino, DT_DIR);
      ctx->pos++;
    }
    return 0;
  }

  return 0;
}

// Функция create - создание файла
static int vtfs_create(struct mnt_idmap *idmap,
                        struct inode *parent_inode,
                        struct dentry *child_dentry,
                        umode_t mode,
                        bool excl) {
  ino_t root = parent_inode->i_ino;
  const char *name = child_dentry->d_name.name;
  
  if (root == VTFS_ROOT_INODE_NUMBER) {
    if (strcmp(name, "test.txt") == 0) {
      struct inode *inode = vtfs_get_inode(parent_inode->i_sb, NULL,
                                           S_IFREG | S_IRWXUGO, 101);
      inode->i_op = &vtfs_inode_ops;
      inode->i_fop = NULL;
      d_add(child_dentry, inode);
      file_mask |= 1;  // Установить бит 0
    } else if (strcmp(name, "new_file.txt") == 0) {
      struct inode *inode = vtfs_get_inode(parent_inode->i_sb, NULL,
                                           S_IFREG | S_IRWXUGO, 102);
      inode->i_op = &vtfs_inode_ops;
      inode->i_fop = NULL;
      d_add(child_dentry, inode);
      file_mask |= 2;  // Установить бит 1
    }
  }
  
  return 0;
}

// Функция unlink - удаление файла
static int vtfs_unlink(struct inode *parent_inode,
                        struct dentry *child_dentry) {
  const char *name = child_dentry->d_name.name;
  ino_t root = parent_inode->i_ino;
  
  if (root == VTFS_ROOT_INODE_NUMBER) {
    if (strcmp(name, "test.txt") == 0) {
      file_mask &= ~1;  // Сбросить бит 0
    } else if (strcmp(name, "new_file.txt") == 0) {
      file_mask &= ~2;  // Сбросить бит 1
    }
  }
  
  return 0;
}

// Заполнение super_block
static int vtfs_fill_super(struct super_block *sb, void *data, int silent) {
  struct inode* inode = vtfs_get_inode(sb, NULL, S_IFDIR | S_IRWXUGO, VTFS_ROOT_INODE_NUMBER);
  
  if (inode == NULL) {
    return -ENOMEM;
  }

  // Устанавливаем операции для корневой директории
  inode->i_op = &vtfs_inode_ops;
  inode->i_fop = &vtfs_dir_ops;

  sb->s_root = d_make_root(inode);
  if (sb->s_root == NULL) {
    return -ENOMEM;
  }

  LOG("Super block filled successfully\n");
  return 0;
}

// Монтирование файловой системы
static struct dentry* vtfs_mount(struct file_system_type* fs_type,
                                  int flags,
                                  const char* token,
                                  void* data) {
  struct dentry* ret = mount_nodev(fs_type, flags, data, vtfs_fill_super);
  if (ret == NULL) {
    printk(KERN_ERR "[vtfs] Can't mount file system\n");
  } else {
    LOG("Mounted successfully\n");
  }
  return ret;
}

// Отмонтирование файловой системы
static void vtfs_kill_sb(struct super_block* sb) {
  LOG("vtfs super block is destroyed. Unmount successfully.\n");
  kill_anon_super(sb);
}

// Инициализация модуля
static int __init vtfs_init(void) {
  int ret;
  
  LOG("VTFS joined the kernel\n");
  
  ret = register_filesystem(&vtfs_fs_type);
  if (ret != 0) {
    printk(KERN_ERR "[vtfs] Failed to register filesystem\n");
    return ret;
  }
  
  LOG("Filesystem registered successfully\n");
  return 0;
}

// Выгрузка модуля
static void __exit vtfs_exit(void) {
  int ret = unregister_filesystem(&vtfs_fs_type);
  if (ret != 0) {
    printk(KERN_ERR "[vtfs] Failed to unregister filesystem\n");
  }
  
  LOG("VTFS left the kernel\n");
}

module_init(vtfs_init);
module_exit(vtfs_exit);
