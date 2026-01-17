#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "http.h"

#define MODULE_NAME "vtfs"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("secs-dev");
MODULE_DESCRIPTION("A simple remote FS kernel module");

#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)

#define VTFS_MAX_FILENAME 255
#define VTFS_MAX_TOKEN_LEN 256

// Глобальный токен для доступа к серверу
static char vtfs_token[VTFS_MAX_TOKEN_LEN] = "";
static DEFINE_MUTEX(vtfs_mutex);

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

static int vtfs_mkdir(struct mnt_idmap *idmap,
                       struct inode *parent_inode,
                       struct dentry *child_dentry,
                       umode_t mode);
static int vtfs_rmdir(struct inode *parent_inode,
                       struct dentry *child_dentry);

static ssize_t vtfs_read(struct file *filp, char __user *buffer,
                          size_t len, loff_t *offset);
static ssize_t vtfs_write(struct file *filp, const char __user *buffer,
                           size_t len, loff_t *offset);

static int vtfs_link(struct dentry *old_dentry,
                      struct inode *parent_dir,
                      struct dentry *new_dentry);

// Структуры операций для inode и файлов
static struct inode_operations vtfs_inode_ops = {
  .lookup = vtfs_lookup,
  .create = vtfs_create,
  .unlink = vtfs_unlink,
  .mkdir = vtfs_mkdir,
  .rmdir = vtfs_rmdir,
  .link = vtfs_link,
};

static struct file_operations vtfs_dir_ops = {
  .iterate_shared = vtfs_iterate,
};

static struct file_operations vtfs_file_ops = {
  .read = vtfs_read,
  .write = vtfs_write,
};

// Структура описания файловой системы
static struct file_system_type vtfs_fs_type = {
  .name = "vtfs",
  .mount = vtfs_mount,
  .kill_sb = vtfs_kill_sb,
};

// ========== Функции файловой системы ==========

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
  ino_t parent_ino = parent_inode->i_ino;
  const char *name = child_dentry->d_name.name;
  struct inode *inode;
  ino_t result_ino;
  int ret;
  int is_dir;
  
  mutex_lock(&vtfs_mutex);
  
  // Вызвать API lookup для поиска файла/директории
  ret = vtfs_api_lookup(vtfs_token, parent_ino, name, &result_ino);
  
  if (ret == 0) {
    // Определить тип - файл или директория
    ret = vtfs_api_is_dir(vtfs_token, result_ino, &is_dir);
    
    if (ret == 0) {
      umode_t mode = is_dir ? (S_IFDIR | S_IRWXUGO) : (S_IFREG | S_IRWXUGO);
      
      inode = vtfs_get_inode(parent_inode->i_sb, NULL, mode, result_ino);
      if (inode) {
        inode->i_op = &vtfs_inode_ops;
        if (is_dir) {
          inode->i_fop = &vtfs_dir_ops;
        } else {
          inode->i_fop = &vtfs_file_ops;
        }
        d_add(child_dentry, inode);
      }
    }
  }
  
  mutex_unlock(&vtfs_mutex);
  return NULL;
}

// Функция iterate - выводит список объектов в директории
static int vtfs_iterate(struct file* filp, struct dir_context* ctx) {
  struct dentry* dentry = filp->f_path.dentry;
  struct inode* inode = dentry->d_inode;
  ino_t ino = inode->i_ino;
  struct vtfs_list_entry *entries;
  int count = 0;
  int ret;
  int i;
  
  // Выделить память для списка файлов
  entries = kmalloc(256 * sizeof(struct vtfs_list_entry), GFP_KERNEL);
  if (!entries) {
    return -ENOMEM;
  }
  
  mutex_lock(&vtfs_mutex);
  
  // Всегда показываем . и ..
  if (ctx->pos == 0) {
    if (!dir_emit(ctx, ".", 1, ino, DT_DIR))
      goto out_ok;
    ctx->pos = 1;
  }

  if (ctx->pos == 1) {
    ino_t parent_ino = ino;
    if (dentry->d_parent && d_inode(dentry->d_parent))
      parent_ino = d_inode(dentry->d_parent)->i_ino;

    if (!dir_emit(ctx, "..", 2, parent_ino, DT_DIR))
      goto out_ok;
    ctx->pos = 2;
  }
  
  // Получить список файлов с сервера
  ret = vtfs_api_list(vtfs_token, ino, entries, 256, &count);
  if (ret != 0) {
    mutex_unlock(&vtfs_mutex);
    kfree(entries);
    return ret;
  }
  
  if (ret == 0) {
    // Показать файлы начиная с текущей позиции
    for (i = ctx->pos - 2; i < count; i++) {
      unsigned char dtype = DT_UNKNOWN;
      int is_dir_flag = 0;
      
      // Определить тип
      if (vtfs_api_is_dir(vtfs_token, entries[i].ino, &is_dir_flag) == 0) {
        dtype = is_dir_flag ? DT_DIR : DT_REG;
      }
      
      dir_emit(ctx, entries[i].name, strlen(entries[i].name), entries[i].ino, dtype);
      ctx->pos++;
    }
  }
  
  mutex_unlock(&vtfs_mutex);
  kfree(entries);
  return 0;
}

// Функция create - создание файла
static int vtfs_create(struct mnt_idmap *idmap,
                        struct inode *parent_inode,
                        struct dentry *child_dentry,
                        umode_t mode,
                        bool excl) {
  ino_t parent_ino = parent_inode->i_ino;
  const char *name = child_dentry->d_name.name;
  struct inode *inode;
  ino_t new_ino;
  int ret;
  
  mutex_lock(&vtfs_mutex);
  
  // Вызвать API create для создания файла
  ret = vtfs_api_create(vtfs_token, parent_ino, name, &new_ino);
  
  if (ret != 0) {
    mutex_unlock(&vtfs_mutex);
    return ret;
  }
  
  inode = vtfs_get_inode(parent_inode->i_sb, NULL, S_IFREG | S_IRWXUGO, new_ino);
  if (!inode) {
    // Попытаться удалить созданный файл
    vtfs_api_unlink(vtfs_token, parent_ino, name);
    mutex_unlock(&vtfs_mutex);
    return -ENOMEM;
  }
  
  inode->i_op = &vtfs_inode_ops;
  inode->i_fop = &vtfs_file_ops;
  d_add(child_dentry, inode);
  
  mutex_unlock(&vtfs_mutex);
  return 0;
}

// Функция unlink - удаление файла
static int vtfs_unlink(struct inode *parent_inode,
                        struct dentry *child_dentry) {
  ino_t parent_ino = parent_inode->i_ino;
  const char *name = child_dentry->d_name.name;
  int ret;
  
  mutex_lock(&vtfs_mutex);
  
  // Вызвать API unlink для удаления файла
  ret = vtfs_api_unlink(vtfs_token, parent_ino, name);
  
  mutex_unlock(&vtfs_mutex);
  return ret;
}

// Функция mkdir - создание директории
static int vtfs_mkdir(struct mnt_idmap *idmap,
                       struct inode *parent_inode,
                       struct dentry *child_dentry,
                       umode_t mode) {
  ino_t parent_ino = parent_inode->i_ino;
  const char *name = child_dentry->d_name.name;
  struct inode *inode;
  ino_t new_ino;
  int ret;
  
  mutex_lock(&vtfs_mutex);
  
  // Вызвать API mkdir для создания директории
  ret = vtfs_api_mkdir(vtfs_token, parent_ino, name, &new_ino);
  
  if (ret != 0) {
    mutex_unlock(&vtfs_mutex);
    return ret;
  }
  
  inode = vtfs_get_inode(parent_inode->i_sb, NULL, S_IFDIR | S_IRWXUGO, new_ino);
  if (!inode) {
    // Попытаться удалить созданную директорию
    vtfs_api_rmdir(vtfs_token, parent_ino, name);
    mutex_unlock(&vtfs_mutex);
    return -ENOMEM;
  }
  
  inode->i_op = &vtfs_inode_ops;
  inode->i_fop = &vtfs_dir_ops;
  d_add(child_dentry, inode);
  
  mutex_unlock(&vtfs_mutex);
  return 0;
}

// Функция rmdir - удаление директории
static int vtfs_rmdir(struct inode *parent_inode,
                       struct dentry *child_dentry) {
  ino_t parent_ino = parent_inode->i_ino;
  const char *name = child_dentry->d_name.name;
  int ret;
  
  mutex_lock(&vtfs_mutex);
  
  // Вызвать API rmdir для удаления директории
  ret = vtfs_api_rmdir(vtfs_token, parent_ino, name);
  
  mutex_unlock(&vtfs_mutex);
  return ret;
}

// Функция link - создание жёсткой ссылки
static int vtfs_link(struct dentry *old_dentry,
                      struct inode *parent_dir,
                      struct dentry *new_dentry) {
  struct inode *old_inode = d_inode(old_dentry);
  ino_t parent_ino = parent_dir->i_ino;
  const char *new_name = new_dentry->d_name.name;
  struct inode *new_inode;
  int ret;
  
  // Жёсткие ссылки только для регулярных файлов
  if (!S_ISREG(old_inode->i_mode)) {
    return -EPERM;
  }
  
  mutex_lock(&vtfs_mutex);
  
  // Вызвать API link для создания жёсткой ссылки
  ret = vtfs_api_link(vtfs_token, old_inode->i_ino, parent_ino, new_name);
  
  if (ret != 0) {
    mutex_unlock(&vtfs_mutex);
    return ret;
  }
  
  // Создать новую inode для новой ссылки (с тем же номером inode)
  new_inode = vtfs_get_inode(parent_dir->i_sb, NULL, S_IFREG | S_IRWXUGO, old_inode->i_ino);
  if (!new_inode) {
    // Попытаться удалить созданную ссылку
    vtfs_api_unlink(vtfs_token, parent_ino, new_name);
    mutex_unlock(&vtfs_mutex);
    return -ENOMEM;
  }
  
  new_inode->i_op = &vtfs_inode_ops;
  new_inode->i_fop = &vtfs_file_ops;
  d_add(new_dentry, new_inode);
  
  mutex_unlock(&vtfs_mutex);
  return 0;
}

// Функция read - чтение из файла
static ssize_t vtfs_read(struct file *filp, char __user *buffer,
                          size_t len, loff_t *offset) {
  struct inode *inode = file_inode(filp);
  char *kernel_buffer;
  size_t actual_len = 0;
  int ret;
  
  // Выделить буфер в kernel space
  kernel_buffer = kmalloc(len, GFP_KERNEL);
  if (!kernel_buffer) {
    return -ENOMEM;
  }
  
  mutex_lock(&vtfs_mutex);
  
  // Вызвать API read для чтения данных
  ret = vtfs_api_read(vtfs_token, inode->i_ino, *offset, len, kernel_buffer, &actual_len);
  
  if (ret != 0) {
    mutex_unlock(&vtfs_mutex);
    kfree(kernel_buffer);
    return ret;
  }
  
  // Скопировать данные в user-space
  if (actual_len > 0) {
    if (copy_to_user(buffer, kernel_buffer, actual_len)) {
      mutex_unlock(&vtfs_mutex);
      kfree(kernel_buffer);
      return -EFAULT;
    }
  }
  
  *offset += actual_len;
  mutex_unlock(&vtfs_mutex);
  kfree(kernel_buffer);
  
  return actual_len;
}

// Функция write - запись в файл
static ssize_t vtfs_write(struct file *filp, const char __user *buffer,
                           size_t len, loff_t *offset) {
  struct inode *inode = file_inode(filp);
  char *kernel_buffer;
  int ret;
  
  // Выделить буфер в kernel space
  kernel_buffer = kmalloc(len, GFP_KERNEL);
  if (!kernel_buffer) {
    return -ENOMEM;
  }
  
  // Скопировать данные из user-space
  if (copy_from_user(kernel_buffer, buffer, len)) {
    kfree(kernel_buffer);
    return -EFAULT;
  }
  
  mutex_lock(&vtfs_mutex);
  
  // Вызвать API write для записи данных
  ret = vtfs_api_write(vtfs_token, inode->i_ino, *offset, kernel_buffer, len);
  
  mutex_unlock(&vtfs_mutex);
  kfree(kernel_buffer);
  
  if (ret != 0) {
    return ret;
  }
  
  *offset += len;
  return len;
}

// Заполнение super_block
static int vtfs_fill_super(struct super_block *sb, void *data, int silent) {
  struct inode* inode;
  ino_t root_ino;
  int ret;
  
  mutex_lock(&vtfs_mutex);
  
  // Получить корневой inode с сервера
  ret = vtfs_api_root(vtfs_token, &root_ino);
  
  if (ret != 0) {
    mutex_unlock(&vtfs_mutex);
    LOG("Failed to get root inode from server: %d\n", ret);
    return ret;
  }
  
  LOG("Got root inode: %lu\n", (unsigned long)root_ino);
  
  inode = vtfs_get_inode(sb, NULL, S_IFDIR | S_IRWXUGO, root_ino);
  if (inode == NULL) {
    mutex_unlock(&vtfs_mutex);
    return -ENOMEM;
  }

  // Устанавливаем операции для корневой директории
  inode->i_op = &vtfs_inode_ops;
  inode->i_fop = &vtfs_dir_ops;

  sb->s_root = d_make_root(inode);
  if (sb->s_root == NULL) {
    mutex_unlock(&vtfs_mutex);
    return -ENOMEM;
  }

  mutex_unlock(&vtfs_mutex);
  LOG("Super block filled successfully\n");
  return 0;
}

// Монтирование файловой системы
static struct dentry* vtfs_mount(struct file_system_type* fs_type,
                                  int flags,
                                  const char* token,
                                  void* data) {
  struct dentry* ret;
  
  // Сохранить токен
  mutex_lock(&vtfs_mutex);
  if (token) {
    strncpy(vtfs_token, token, VTFS_MAX_TOKEN_LEN - 1);
    vtfs_token[VTFS_MAX_TOKEN_LEN - 1] = '\0';
    LOG("Token saved: %s\n", vtfs_token);
  } else {
    vtfs_token[0] = '\0';
    LOG("No token provided\n");
  }
  mutex_unlock(&vtfs_mutex);
  
  ret = mount_nodev(fs_type, flags, data, vtfs_fill_super);
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
  
  // Очистить токен
  mutex_lock(&vtfs_mutex);
  vtfs_token[0] = '\0';
  mutex_unlock(&vtfs_mutex);
  
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
