#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/stat.h>

#define MODULE_NAME "vtfs"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("secs-dev");
MODULE_DESCRIPTION("A simple FS kernel module");

#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)

#define VTFS_ROOT_INODE_NUMBER 1000

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

// Заполнение super_block
static int vtfs_fill_super(struct super_block *sb, void *data, int silent) {
  struct inode* inode = vtfs_get_inode(sb, NULL, S_IFDIR | S_IRWXUGO, VTFS_ROOT_INODE_NUMBER);
  
  if (inode == NULL) {
    return -ENOMEM;
  }

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
  kill_litter_super(sb);
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
