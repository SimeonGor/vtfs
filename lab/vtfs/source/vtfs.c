#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/uaccess.h>

#define MODULE_NAME "vtfs"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("secs-dev");
MODULE_DESCRIPTION("A simple FS kernel module");

#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)

#define VTFS_ROOT_INODE_NUMBER 1000
#define VTFS_MAX_FILENAME 255

// Структура для хранения содержимого файла
struct vtfs_file_data {
  struct list_head list;  // Для глобального списка файлов
  ino_t ino;
  char *data;             // Содержимое файла
  size_t size;            // Размер данных
  size_t capacity;        // Выделенная память
  unsigned int link_count; // Счётчик жёстких ссылок
};

// Структура для представления записи в директории
struct vtfs_dir_entry {
  struct list_head list;  // Для связного списка элементов директории
  char name[VTFS_MAX_FILENAME];
  ino_t ino;
  umode_t mode;           // Тип (файл/директория) и права
};

// Структура для представления директории
struct vtfs_dir {
  struct list_head list;     // Для глобального списка директорий
  ino_t ino;
  ino_t parent_ino;
  struct list_head entries;  // Список записей в директории
};

// Глобальные переменные для хранилища
static LIST_HEAD(all_directories);  // Глобальный список всех директорий
static LIST_HEAD(all_files);        // Глобальный список всех файлов
static ino_t next_ino = VTFS_ROOT_INODE_NUMBER + 1;
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

// ========== Функции для работы с содержимым файлов ==========

// Найти данные файла по inode
static struct vtfs_file_data* vtfs_find_file_data(ino_t ino) {
  struct vtfs_file_data *file_data;
  
  list_for_each_entry(file_data, &all_files, list) {
    if (file_data->ino == ino) {
      return file_data;
    }
  }
  return NULL;
}

// Создать пустой файл
static struct vtfs_file_data* vtfs_create_file_data(ino_t ino) {
  struct vtfs_file_data *file_data;
  
  file_data = kmalloc(sizeof(*file_data), GFP_KERNEL);
  if (!file_data) {
    return NULL;
  }
  
  file_data->ino = ino;
  file_data->data = NULL;
  file_data->size = 0;
  file_data->capacity = 0;
  file_data->link_count = 1;  // Первая ссылка
  
  list_add_tail(&file_data->list, &all_files);
  return file_data;
}

// Удалить данные файла
static void vtfs_remove_file_data(ino_t ino) {
  struct vtfs_file_data *file_data;
  
  file_data = vtfs_find_file_data(ino);
  if (!file_data) {
    return;
  }
  
  if (file_data->data) {
    kfree(file_data->data);
  }
  
  list_del(&file_data->list);
  kfree(file_data);
}

// ========== Функции для работы с RAM-хранилищем директорий ==========

// Найти директорию по inode
static struct vtfs_dir* vtfs_find_dir(ino_t ino) {
  struct vtfs_dir *dir;
  
  list_for_each_entry(dir, &all_directories, list) {
    if (dir->ino == ino) {
      return dir;
    }
  }
  return NULL;
}

// Создать новую директорию
static struct vtfs_dir* vtfs_create_dir(ino_t ino, ino_t parent_ino) {
  struct vtfs_dir *dir;
  
  dir = kmalloc(sizeof(*dir), GFP_KERNEL);
  if (!dir) {
    return NULL;
  }
  
  dir->ino = ino;
  dir->parent_ino = parent_ino;
  INIT_LIST_HEAD(&dir->entries);
  
  // Добавить в глобальный список директорий
  list_add_tail(&dir->list, &all_directories);
  
  return dir;
}

// Удалить директорию
static void vtfs_remove_dir(ino_t ino) {
  struct vtfs_dir *dir;
  struct vtfs_dir_entry *entry, *tmp;
  
  dir = vtfs_find_dir(ino);
  if (!dir) {
    return;
  }
  
  // Удалить все записи в директории
  list_for_each_entry_safe(entry, tmp, &dir->entries, list) {
    list_del(&entry->list);
    kfree(entry);
  }
  
  // Удалить директорию из глобального списка
  list_del(&dir->list);
  kfree(dir);
}

// Найти запись в директории по имени
static struct vtfs_dir_entry* vtfs_find_entry(struct vtfs_dir *dir, const char *name) {
  struct vtfs_dir_entry *entry;
  
  list_for_each_entry(entry, &dir->entries, list) {
    if (strcmp(entry->name, name) == 0) {
      return entry;
    }
  }
  return NULL;
}

// Добавить запись в директорию
static int vtfs_add_entry(struct vtfs_dir *dir, const char *name, ino_t ino, umode_t mode) {
  struct vtfs_dir_entry *entry;
  
  // Проверить, что имя не существует
  if (vtfs_find_entry(dir, name)) {
    return -EEXIST;
  }
  
  entry = kmalloc(sizeof(*entry), GFP_KERNEL);
  if (!entry) {
    return -ENOMEM;
  }
  
  strncpy(entry->name, name, VTFS_MAX_FILENAME - 1);
  entry->name[VTFS_MAX_FILENAME - 1] = '\0';
  entry->ino = ino;
  entry->mode = mode;
  
  list_add_tail(&entry->list, &dir->entries);
  return 0;
}

// Удалить запись из директории
static int vtfs_remove_entry(struct vtfs_dir *dir, const char *name) {
  struct vtfs_dir_entry *entry;
  
  entry = vtfs_find_entry(dir, name);
  if (!entry) {
    return -ENOENT;
  }
  
  list_del(&entry->list);
  kfree(entry);
  return 0;
}

// Проверить, пуста ли директория (кроме . и ..)
static bool vtfs_is_dir_empty(struct vtfs_dir *dir) {
  return list_empty(&dir->entries);
}

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
  struct vtfs_dir *dir;
  struct vtfs_dir_entry *entry;
  struct inode *inode;
  
  mutex_lock(&vtfs_mutex);
  
  dir = vtfs_find_dir(parent_ino);
  if (!dir) {
    mutex_unlock(&vtfs_mutex);
    return NULL;
  }
  
  entry = vtfs_find_entry(dir, name);
  if (entry) {
    inode = vtfs_get_inode(parent_inode->i_sb, NULL, entry->mode, entry->ino);
    if (inode) {
      inode->i_op = &vtfs_inode_ops;
      if (S_ISDIR(entry->mode)) {
        inode->i_fop = &vtfs_dir_ops;
      } else {
        inode->i_fop = &vtfs_file_ops;
      }
      d_add(child_dentry, inode);
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
  struct vtfs_dir *dir;
  struct vtfs_dir_entry *entry;
  int i = 0;
  
  mutex_lock(&vtfs_mutex);
  
  dir = vtfs_find_dir(ino);
  if (!dir) {
    mutex_unlock(&vtfs_mutex);
    return 0;
  }
  
  // Всегда показываем . и ..
  if (ctx->pos == 0) {
    dir_emit(ctx, ".", 1, ino, DT_DIR);
    ctx->pos++;
  }
  
  if (ctx->pos == 1) {
    dir_emit(ctx, "..", 2, dir->parent_ino, DT_DIR);
    ctx->pos++;
  }
  
  // Пропускаем уже обработанные записи
  i = 2;
  list_for_each_entry(entry, &dir->entries, list) {
    if (i >= ctx->pos) {
      unsigned char dtype = S_ISDIR(entry->mode) ? DT_DIR : DT_REG;
      dir_emit(ctx, entry->name, strlen(entry->name), entry->ino, dtype);
      ctx->pos++;
    }
    i++;
  }
  
  mutex_unlock(&vtfs_mutex);
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
  struct vtfs_dir *dir;
  struct inode *inode;
  struct vtfs_file_data *file_data;
  ino_t new_ino;
  int ret;
  
  mutex_lock(&vtfs_mutex);
  
  dir = vtfs_find_dir(parent_ino);
  if (!dir) {
    mutex_unlock(&vtfs_mutex);
    return -ENOENT;
  }
  
  new_ino = next_ino++;
  
  // Создать данные файла
  file_data = vtfs_create_file_data(new_ino);
  if (!file_data) {
    mutex_unlock(&vtfs_mutex);
    return -ENOMEM;
  }
  
  ret = vtfs_add_entry(dir, name, new_ino, S_IFREG | S_IRWXUGO);
  if (ret) {
    vtfs_remove_file_data(new_ino);
    mutex_unlock(&vtfs_mutex);
    return ret;
  }
  
  inode = vtfs_get_inode(parent_inode->i_sb, NULL, S_IFREG | S_IRWXUGO, new_ino);
  if (!inode) {
    vtfs_remove_entry(dir, name);
    vtfs_remove_file_data(new_ino);
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
  struct vtfs_dir *dir;
  struct vtfs_dir_entry *entry;
  struct vtfs_file_data *file_data;
  int ret;
  
  mutex_lock(&vtfs_mutex);
  
  dir = vtfs_find_dir(parent_ino);
  if (!dir) {
    mutex_unlock(&vtfs_mutex);
    return -ENOENT;
  }
  
  entry = vtfs_find_entry(dir, name);
  if (!entry) {
    mutex_unlock(&vtfs_mutex);
    return -ENOENT;
  }
  
  // Уменьшить счётчик ссылок
  file_data = vtfs_find_file_data(entry->ino);
  if (file_data) {
    file_data->link_count--;
    // Удалить данные файла только если это была последняя ссылка
    if (file_data->link_count == 0) {
      vtfs_remove_file_data(entry->ino);
    }
  }
  
  ret = vtfs_remove_entry(dir, name);
  
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
  struct vtfs_dir *parent_dir, *new_dir;
  struct inode *inode;
  ino_t new_ino;
  int ret;
  
  mutex_lock(&vtfs_mutex);
  
  parent_dir = vtfs_find_dir(parent_ino);
  if (!parent_dir) {
    mutex_unlock(&vtfs_mutex);
    return -ENOENT;
  }
  
  new_ino = next_ino++;
  
  // Создать новую директорию
  new_dir = vtfs_create_dir(new_ino, parent_ino);
  if (!new_dir) {
    mutex_unlock(&vtfs_mutex);
    return -ENOMEM;
  }
  
  // Добавить запись в родительскую директорию
  ret = vtfs_add_entry(parent_dir, name, new_ino, S_IFDIR | S_IRWXUGO);
  if (ret) {
    vtfs_remove_dir(new_ino);
    mutex_unlock(&vtfs_mutex);
    return ret;
  }
  
  inode = vtfs_get_inode(parent_inode->i_sb, NULL, S_IFDIR | S_IRWXUGO, new_ino);
  if (!inode) {
    vtfs_remove_entry(parent_dir, name);
    vtfs_remove_dir(new_ino);
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
  struct vtfs_dir *parent_dir, *target_dir;
  struct vtfs_dir_entry *entry;
  int ret;
  
  mutex_lock(&vtfs_mutex);
  
  parent_dir = vtfs_find_dir(parent_ino);
  if (!parent_dir) {
    mutex_unlock(&vtfs_mutex);
    return -ENOENT;
  }
  
  entry = vtfs_find_entry(parent_dir, name);
  if (!entry) {
    mutex_unlock(&vtfs_mutex);
    return -ENOENT;
  }
  
  target_dir = vtfs_find_dir(entry->ino);
  if (!target_dir) {
    mutex_unlock(&vtfs_mutex);
    return -ENOTDIR;
  }
  
  // Проверить, что директория пуста
  if (!vtfs_is_dir_empty(target_dir)) {
    mutex_unlock(&vtfs_mutex);
    return -ENOTEMPTY;
  }
  
  // Удалить запись из родительской директории
  ret = vtfs_remove_entry(parent_dir, name);
  if (ret) {
    mutex_unlock(&vtfs_mutex);
    return ret;
  }
  
  // Удалить саму директорию
  vtfs_remove_dir(entry->ino);
  
  mutex_unlock(&vtfs_mutex);
  return 0;
}

// Функция link - создание жёсткой ссылки
static int vtfs_link(struct dentry *old_dentry,
                      struct inode *parent_dir,
                      struct dentry *new_dentry) {
  struct inode *old_inode = d_inode(old_dentry);
  ino_t parent_ino = parent_dir->i_ino;
  const char *new_name = new_dentry->d_name.name;
  struct vtfs_dir *dir;
  struct vtfs_file_data *file_data;
  struct inode *new_inode;
  int ret;
  
  // Жёсткие ссылки только для регулярных файлов
  if (!S_ISREG(old_inode->i_mode)) {
    return -EPERM;
  }
  
  mutex_lock(&vtfs_mutex);
  
  dir = vtfs_find_dir(parent_ino);
  if (!dir) {
    mutex_unlock(&vtfs_mutex);
    return -ENOENT;
  }
  
  // Увеличить счётчик ссылок
  file_data = vtfs_find_file_data(old_inode->i_ino);
  if (!file_data) {
    mutex_unlock(&vtfs_mutex);
    return -ENOENT;
  }
  
  file_data->link_count++;
  
  // Добавить новую запись в директорию с тем же inode
  ret = vtfs_add_entry(dir, new_name, old_inode->i_ino, S_IFREG | S_IRWXUGO);
  if (ret) {
    file_data->link_count--;  // Откатить изменение
    mutex_unlock(&vtfs_mutex);
    return ret;
  }
  
  // Создать новую inode для новой ссылки
  new_inode = vtfs_get_inode(parent_dir->i_sb, NULL, S_IFREG | S_IRWXUGO, old_inode->i_ino);
  if (!new_inode) {
    vtfs_remove_entry(dir, new_name);
    file_data->link_count--;
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
  struct vtfs_file_data *file_data;
  size_t to_read;
  
  mutex_lock(&vtfs_mutex);
  
  file_data = vtfs_find_file_data(inode->i_ino);
  if (!file_data) {
    mutex_unlock(&vtfs_mutex);
    return -ENOENT;
  }
  
  // Проверить границы чтения
  if (*offset >= file_data->size) {
    mutex_unlock(&vtfs_mutex);
    return 0;  // EOF
  }
  
  // Вычислить сколько байт читать
  to_read = min(len, file_data->size - (size_t)*offset);
  
  // Скопировать данные в user-space
  if (copy_to_user(buffer, file_data->data + *offset, to_read)) {
    mutex_unlock(&vtfs_mutex);
    return -EFAULT;
  }
  
  *offset += to_read;
  mutex_unlock(&vtfs_mutex);
  
  return to_read;
}

// Функция write - запись в файл
static ssize_t vtfs_write(struct file *filp, const char __user *buffer,
                           size_t len, loff_t *offset) {
  struct inode *inode = file_inode(filp);
  struct vtfs_file_data *file_data;
  size_t new_size;
  char *new_data;
  
  mutex_lock(&vtfs_mutex);
  
  file_data = vtfs_find_file_data(inode->i_ino);
  if (!file_data) {
    mutex_unlock(&vtfs_mutex);
    return -ENOENT;
  }
  
  // Вычислить новый размер файла
  new_size = *offset + len;
  
  // Если нужно больше памяти, перевыделить
  if (new_size > file_data->capacity) {
    size_t new_capacity = max(new_size, file_data->capacity * 2);
    if (new_capacity == 0) {
      new_capacity = PAGE_SIZE;
    }
    
    new_data = krealloc(file_data->data, new_capacity, GFP_KERNEL);
    if (!new_data) {
      mutex_unlock(&vtfs_mutex);
      return -ENOMEM;
    }
    
    file_data->data = new_data;
    file_data->capacity = new_capacity;
  }
  
  // Если записываем за пределами текущего размера, заполнить нулями
  if (*offset > file_data->size) {
    memset(file_data->data + file_data->size, 0, *offset - file_data->size);
  }
  
  // Скопировать данные из user-space
  if (copy_from_user(file_data->data + *offset, buffer, len)) {
    mutex_unlock(&vtfs_mutex);
    return -EFAULT;
  }
  
  // Обновить размер файла
  if (new_size > file_data->size) {
    file_data->size = new_size;
    inode->i_size = new_size;
  }
  
  *offset += len;
  mutex_unlock(&vtfs_mutex);
  
  return len;
}

// Заполнение super_block
static int vtfs_fill_super(struct super_block *sb, void *data, int silent) {
  struct inode* inode;
  struct vtfs_dir *root;
  
  inode = vtfs_get_inode(sb, NULL, S_IFDIR | S_IRWXUGO, VTFS_ROOT_INODE_NUMBER);
  if (inode == NULL) {
    return -ENOMEM;
  }

  // Инициализировать корневую директорию в RAM
  mutex_lock(&vtfs_mutex);
  root = vtfs_create_dir(VTFS_ROOT_INODE_NUMBER, VTFS_ROOT_INODE_NUMBER);
  if (!root) {
    mutex_unlock(&vtfs_mutex);
    iput(inode);
    return -ENOMEM;
  }
  mutex_unlock(&vtfs_mutex);

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
  struct vtfs_dir *dir, *dir_tmp;
  struct vtfs_dir_entry *entry, *entry_tmp;
  struct vtfs_file_data *file_data, *file_tmp;
  
  LOG("vtfs super block is destroyed. Unmount successfully.\n");
  
  // Очистить все директории и их содержимое
  mutex_lock(&vtfs_mutex);
  
  // Очистить все файлы
  list_for_each_entry_safe(file_data, file_tmp, &all_files, list) {
    if (file_data->data) {
      kfree(file_data->data);
    }
    list_del(&file_data->list);
    kfree(file_data);
  }
  
  // Очистить все директории
  list_for_each_entry_safe(dir, dir_tmp, &all_directories, list) {
    // Удалить все записи в директории
    list_for_each_entry_safe(entry, entry_tmp, &dir->entries, list) {
      list_del(&entry->list);
      kfree(entry);
    }
    // Удалить саму директорию
    list_del(&dir->list);
    kfree(dir);
  }
  
  next_ino = VTFS_ROOT_INODE_NUMBER + 1;
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
