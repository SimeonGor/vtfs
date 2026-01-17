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
  struct list_head list;  // Для списка файлов
  ino_t ino;
  char *data;             // Содержимое файла
  size_t size;            // Размер данных
  size_t capacity;        // Выделенная память
};

// Структура для представления записи в директории
struct vtfs_dir_entry {
  struct list_head list;  // Для связного списка
  char name[VTFS_MAX_FILENAME];
  ino_t ino;
  umode_t mode;           // Тип (файл/директория) и права
};

// Структура для представления директории
struct vtfs_dir {
  struct list_head list;     // Для списка директорий
  ino_t ino;
  ino_t parent_ino;
  struct list_head entries;  // Список записей в директории
};

// Структура для хранения состояния файловой системы (per-mount)
struct vtfs_fs_info {
  struct list_head directories;  // Список всех директорий
  struct list_head files;        // Список всех файлов
  ino_t next_ino;                // Следующий свободный inode номер
  struct mutex mutex;            // Мьютекс для синхронизации
};

// Получить fs_info из super_block
static inline struct vtfs_fs_info* VTFS_SB(struct super_block *sb) {
  return (struct vtfs_fs_info *)sb->s_fs_info;
}

// Прототипы функций
static struct inode* vtfs_get_inode(struct super_block* sb,
                                     const struct inode* dir,
                                     umode_t mode,
                                     ino_t i_ino);
static struct inode* vtfs_new_inode(struct super_block* sb,
                                     const struct inode* dir,
                                     umode_t mode);
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
static struct vtfs_file_data* vtfs_find_file_data(struct vtfs_fs_info *fsi, ino_t ino) {
  struct vtfs_file_data *file_data;
  
  list_for_each_entry(file_data, &fsi->files, list) {
    if (file_data->ino == ino) {
      return file_data;
    }
  }
  return NULL;
}

// Создать пустой файл
static struct vtfs_file_data* vtfs_create_file_data(struct vtfs_fs_info *fsi, ino_t ino) {
  struct vtfs_file_data *file_data;
  
  file_data = kmalloc(sizeof(*file_data), GFP_KERNEL);
  if (!file_data) {
    return NULL;
  }
  
  file_data->ino = ino;
  file_data->data = NULL;
  file_data->size = 0;
  file_data->capacity = 0;
  
  list_add_tail(&file_data->list, &fsi->files);
  return file_data;
}

// Удалить данные файла
static void vtfs_remove_file_data(struct vtfs_fs_info *fsi, ino_t ino) {
  struct vtfs_file_data *file_data;
  
  file_data = vtfs_find_file_data(fsi, ino);
  if (!file_data) {
    return;
  }
  
  if (file_data->data) {
    kfree(file_data->data);
  }
  
  list_del(&file_data->list);
  kfree(file_data);
}

// ========== Функции для работы с директориями ==========

// Найти директорию по inode
static struct vtfs_dir* vtfs_find_dir(struct vtfs_fs_info *fsi, ino_t ino) {
  struct vtfs_dir *dir;
  
  list_for_each_entry(dir, &fsi->directories, list) {
    if (dir->ino == ino) {
      return dir;
    }
  }
  return NULL;
}

// Создать новую директорию
static struct vtfs_dir* vtfs_create_dir(struct vtfs_fs_info *fsi, ino_t ino, ino_t parent_ino) {
  struct vtfs_dir *dir;
  
  dir = kmalloc(sizeof(*dir), GFP_KERNEL);
  if (!dir) {
    return NULL;
  }
  
  dir->ino = ino;
  dir->parent_ino = parent_ino;
  INIT_LIST_HEAD(&dir->entries);
  
  // Добавить в список директорий
  list_add_tail(&dir->list, &fsi->directories);
  
  return dir;
}

// Удалить директорию
static void vtfs_remove_dir(struct vtfs_fs_info *fsi, ino_t ino) {
  struct vtfs_dir *dir;
  struct vtfs_dir_entry *entry, *tmp;
  
  dir = vtfs_find_dir(fsi, ino);
  if (!dir) {
    return;
  }
  
  // Удалить все записи в директории
  list_for_each_entry_safe(entry, tmp, &dir->entries, list) {
    list_del(&entry->list);
    kfree(entry);
  }
  
  // Удалить директорию из списка
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

// Получить или создать inode (с кэшированием)
static struct inode* vtfs_get_inode(struct super_block* sb,
                                     const struct inode* dir,
                                     umode_t mode,
                                     ino_t i_ino) {
  struct inode *inode;
  struct vtfs_fs_info *fsi = VTFS_SB(sb);
  
  // Попытаться найти существующую inode в кэше или создать новую
  inode = iget_locked(sb, i_ino);
  if (!inode) {
    return NULL;
  }
  
  // Если inode новая (I_NEW флаг установлен), заполнить её
  if (inode->i_state & I_NEW) {
    inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
    inode->i_op = &vtfs_inode_ops;
    
    if (S_ISDIR(mode)) {
      inode->i_fop = &vtfs_dir_ops;
      set_nlink(inode, 2);
    } else {
      inode->i_fop = &vtfs_file_ops;
      set_nlink(inode, 1);
      
      // Установить размер файла
      struct vtfs_file_data *file_data = vtfs_find_file_data(fsi, i_ino);
      if (file_data) {
        inode->i_size = file_data->size;
      }
    }
    
    unlock_new_inode(inode);
  }
  
  return inode;
}

// Создание нового inode для новых файлов
static struct inode* vtfs_new_inode(struct super_block* sb,
                                     const struct inode* dir,
                                     umode_t mode) {
  struct inode *inode = new_inode(sb);
  struct vtfs_fs_info *fsi = VTFS_SB(sb);
  
  if (inode != NULL) {
    inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
    inode->i_ino = fsi->next_ino++;
    inode->i_op = &vtfs_inode_ops;
    
    // Установить i_nlink
    if (S_ISDIR(mode)) {
      inode->i_fop = &vtfs_dir_ops;
      set_nlink(inode, 2);
    } else {
      inode->i_fop = &vtfs_file_ops;
      set_nlink(inode, 1);
    }
  }
  return inode;
}

// Функция lookup - определяет что за сущность описывается данной нодой
static struct dentry* vtfs_lookup(struct inode* parent_inode,
                                   struct dentry* child_dentry,
                                   unsigned int flag) {
  struct vtfs_fs_info *fsi = VTFS_SB(parent_inode->i_sb);
  ino_t parent_ino = parent_inode->i_ino;
  const char *name = child_dentry->d_name.name;
  struct vtfs_dir *dir;
  struct vtfs_dir_entry *entry;
  struct inode *inode;
  
  mutex_lock(&fsi->mutex);
  
  dir = vtfs_find_dir(fsi, parent_ino);
  if (!dir) {
    mutex_unlock(&fsi->mutex);
    return NULL;
  }
  
  entry = vtfs_find_entry(dir, name);
  if (entry) {
    inode = vtfs_get_inode(parent_inode->i_sb, NULL, entry->mode, entry->ino);
    if (inode) {
      d_add(child_dentry, inode);
    }
  }
  
  mutex_unlock(&fsi->mutex);
  return NULL;
}

// Функция iterate - выводит список объектов в директории
static int vtfs_iterate(struct file* filp, struct dir_context* ctx) {
  struct vtfs_fs_info *fsi = VTFS_SB(file_inode(filp)->i_sb);
  struct dentry* dentry = filp->f_path.dentry;
  struct inode* inode = dentry->d_inode;
  ino_t ino = inode->i_ino;
  struct vtfs_dir *dir;
  struct vtfs_dir_entry *entry;
  int i = 0;
  
  mutex_lock(&fsi->mutex);
  
  dir = vtfs_find_dir(fsi, ino);
  if (!dir) {
    mutex_unlock(&fsi->mutex);
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
  
  mutex_unlock(&fsi->mutex);
  return 0;
}

// Функция create - создание файла
static int vtfs_create(struct mnt_idmap *idmap,
                        struct inode *parent_inode,
                        struct dentry *child_dentry,
                        umode_t mode,
                        bool excl) {
  struct vtfs_fs_info *fsi = VTFS_SB(parent_inode->i_sb);
  ino_t parent_ino = parent_inode->i_ino;
  const char *name = child_dentry->d_name.name;
  struct vtfs_dir *dir;
  struct inode *inode;
  struct vtfs_file_data *file_data;
  int ret;
  
  mutex_lock(&fsi->mutex);
  
  dir = vtfs_find_dir(fsi, parent_ino);
  if (!dir) {
    mutex_unlock(&fsi->mutex);
    return -ENOENT;
  }
  
  // Создать новый inode
  inode = vtfs_new_inode(parent_inode->i_sb, parent_inode, S_IFREG | S_IRWXUGO);
  if (!inode) {
    mutex_unlock(&fsi->mutex);
    return -ENOMEM;
  }
  
  // Создать данные файла
  file_data = vtfs_create_file_data(fsi, inode->i_ino);
  if (!file_data) {
    iput(inode);
    mutex_unlock(&fsi->mutex);
    return -ENOMEM;
  }
  
  ret = vtfs_add_entry(dir, name, inode->i_ino, S_IFREG | S_IRWXUGO);
  if (ret) {
    vtfs_remove_file_data(fsi, inode->i_ino);
    iput(inode);
    mutex_unlock(&fsi->mutex);
    return ret;
  }
  
  inode->i_size = 0;
  d_add(child_dentry, inode);
  
  mutex_unlock(&fsi->mutex);
  return 0;
}

// Функция unlink - удаление файла
static int vtfs_unlink(struct inode *parent_inode,
                        struct dentry *child_dentry) {
  struct vtfs_fs_info *fsi = VTFS_SB(parent_inode->i_sb);
  ino_t parent_ino = parent_inode->i_ino;
  const char *name = child_dentry->d_name.name;
  struct vtfs_dir *dir;
  struct vtfs_dir_entry *entry;
  struct inode *inode = d_inode(child_dentry);
  int ret;
  
  mutex_lock(&fsi->mutex);
  
  dir = vtfs_find_dir(fsi, parent_ino);
  if (!dir) {
    mutex_unlock(&fsi->mutex);
    return -ENOENT;
  }
  
  entry = vtfs_find_entry(dir, name);
  if (!entry) {
    mutex_unlock(&fsi->mutex);
    return -ENOENT;
  }
  
  // Удалить запись из директории
  ret = vtfs_remove_entry(dir, name);
  if (ret) {
    mutex_unlock(&fsi->mutex);
    return ret;
  }
  
  // Уменьшить счётчик ссылок в inode
  if (inode) {
    drop_nlink(inode);
    
    // Удалить данные файла только если это была последняя ссылка
    if (inode->i_nlink == 0) {
      vtfs_remove_file_data(fsi, entry->ino);
    }
  }
  
  mutex_unlock(&fsi->mutex);
  return 0;
}

// Функция mkdir - создание директории
static int vtfs_mkdir(struct mnt_idmap *idmap,
                       struct inode *parent_inode,
                       struct dentry *child_dentry,
                       umode_t mode) {
  struct vtfs_fs_info *fsi = VTFS_SB(parent_inode->i_sb);
  ino_t parent_ino = parent_inode->i_ino;
  const char *name = child_dentry->d_name.name;
  struct vtfs_dir *parent_dir, *new_dir;
  struct inode *inode;
  int ret;
  
  mutex_lock(&fsi->mutex);
  
  parent_dir = vtfs_find_dir(fsi, parent_ino);
  if (!parent_dir) {
    mutex_unlock(&fsi->mutex);
    return -ENOENT;
  }
  
  // Создать новый inode
  inode = vtfs_new_inode(parent_inode->i_sb, parent_inode, S_IFDIR | S_IRWXUGO);
  if (!inode) {
    mutex_unlock(&fsi->mutex);
    return -ENOMEM;
  }
  
  // Создать новую директорию
  new_dir = vtfs_create_dir(fsi, inode->i_ino, parent_ino);
  if (!new_dir) {
    iput(inode);
    mutex_unlock(&fsi->mutex);
    return -ENOMEM;
  }
  
  // Добавить запись в родительскую директорию
  ret = vtfs_add_entry(parent_dir, name, inode->i_ino, S_IFDIR | S_IRWXUGO);
  if (ret) {
    vtfs_remove_dir(fsi, inode->i_ino);
    iput(inode);
    mutex_unlock(&fsi->mutex);
    return ret;
  }
  
  d_add(child_dentry, inode);
  
  // Увеличить счётчик ссылок родительской директории (для ..)
  inc_nlink(parent_inode);
  
  mutex_unlock(&fsi->mutex);
  return 0;
}

// Функция rmdir - удаление директории
static int vtfs_rmdir(struct inode *parent_inode,
                       struct dentry *child_dentry) {
  struct vtfs_fs_info *fsi = VTFS_SB(parent_inode->i_sb);
  ino_t parent_ino = parent_inode->i_ino;
  const char *name = child_dentry->d_name.name;
  struct vtfs_dir *parent_dir, *target_dir;
  struct vtfs_dir_entry *entry;
  int ret;
  
  mutex_lock(&fsi->mutex);
  
  parent_dir = vtfs_find_dir(fsi, parent_ino);
  if (!parent_dir) {
    mutex_unlock(&fsi->mutex);
    return -ENOENT;
  }
  
  entry = vtfs_find_entry(parent_dir, name);
  if (!entry) {
    mutex_unlock(&fsi->mutex);
    return -ENOENT;
  }
  
  target_dir = vtfs_find_dir(fsi, entry->ino);
  if (!target_dir) {
    mutex_unlock(&fsi->mutex);
    return -ENOTDIR;
  }
  
  // Проверить, что директория пуста
  if (!vtfs_is_dir_empty(target_dir)) {
    mutex_unlock(&fsi->mutex);
    return -ENOTEMPTY;
  }
  
  // Удалить запись из родительской директории
  ret = vtfs_remove_entry(parent_dir, name);
  if (ret) {
    mutex_unlock(&fsi->mutex);
    return ret;
  }
  
  // Удалить саму директорию
  vtfs_remove_dir(fsi, entry->ino);
  
  // Уменьшить счётчик ссылок родительской директории (для ..)
  drop_nlink(parent_inode);
  
  mutex_unlock(&fsi->mutex);
  return 0;
}

// Функция link - создание жёсткой ссылки
static int vtfs_link(struct dentry *old_dentry,
                      struct inode *parent_dir,
                      struct dentry *new_dentry) {
  struct vtfs_fs_info *fsi = VTFS_SB(parent_dir->i_sb);
  struct inode *old_inode = d_inode(old_dentry);
  ino_t parent_ino = parent_dir->i_ino;
  const char *new_name = new_dentry->d_name.name;
  struct vtfs_dir *dir;
  int ret;
  
  // Жёсткие ссылки только для регулярных файлов
  if (!S_ISREG(old_inode->i_mode)) {
    return -EPERM;
  }
  
  mutex_lock(&fsi->mutex);
  
  dir = vtfs_find_dir(fsi, parent_ino);
  if (!dir) {
    mutex_unlock(&fsi->mutex);
    return -ENOENT;
  }
  
  // Добавить новую запись в директорию с тем же inode
  ret = vtfs_add_entry(dir, new_name, old_inode->i_ino, S_IFREG | S_IRWXUGO);
  if (ret) {
    mutex_unlock(&fsi->mutex);
    return ret;
  }
  
  // Увеличить счётчик ссылок в inode
  inc_nlink(old_inode);
  
  ihold(old_inode);
  d_add(new_dentry, old_inode);
  
  mutex_unlock(&fsi->mutex);
  return 0;
}

// Функция read - чтение из файла
static ssize_t vtfs_read(struct file *filp, char __user *buffer,
                          size_t len, loff_t *offset) {
  struct inode *inode = file_inode(filp);
  struct vtfs_fs_info *fsi = VTFS_SB(inode->i_sb);
  struct vtfs_file_data *file_data;
  size_t to_read;
  
  mutex_lock(&fsi->mutex);
  
  file_data = vtfs_find_file_data(fsi, inode->i_ino);
  if (!file_data) {
    mutex_unlock(&fsi->mutex);
    return -ENOENT;
  }
  
  // Проверить границы чтения
  if (*offset >= file_data->size) {
    mutex_unlock(&fsi->mutex);
    return 0;  // EOF
  }
  
  // Вычислить сколько байт читать
  to_read = min(len, file_data->size - (size_t)*offset);
  
  // Скопировать данные в user-space
  if (copy_to_user(buffer, file_data->data + *offset, to_read)) {
    mutex_unlock(&fsi->mutex);
    return -EFAULT;
  }
  
  *offset += to_read;
  mutex_unlock(&fsi->mutex);
  
  return to_read;
}

// Функция write - запись в файл
static ssize_t vtfs_write(struct file *filp, const char __user *buffer,
                           size_t len, loff_t *offset) {
  struct inode *inode = file_inode(filp);
  struct vtfs_fs_info *fsi = VTFS_SB(inode->i_sb);
  struct vtfs_file_data *file_data;
  size_t new_size;
  char *new_data;
  
  mutex_lock(&fsi->mutex);
  
  file_data = vtfs_find_file_data(fsi, inode->i_ino);
  if (!file_data) {
    mutex_unlock(&fsi->mutex);
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
      mutex_unlock(&fsi->mutex);
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
    mutex_unlock(&fsi->mutex);
    return -EFAULT;
  }
  
  // Обновить размер файла
  if (new_size > file_data->size) {
    file_data->size = new_size;
    inode->i_size = new_size;
  }
  
  *offset += len;
  mutex_unlock(&fsi->mutex);
  
  return len;
}

// Заполнение super_block
static int vtfs_fill_super(struct super_block *sb, void *data, int silent) {
  struct inode* inode;
  struct vtfs_fs_info *fsi;
  struct vtfs_dir *root;
  
  // Создать структуру fs_info
  fsi = kzalloc(sizeof(struct vtfs_fs_info), GFP_KERNEL);
  if (!fsi) {
    return -ENOMEM;
  }
  
  // Инициализировать fs_info
  INIT_LIST_HEAD(&fsi->directories);
  INIT_LIST_HEAD(&fsi->files);
  fsi->next_ino = VTFS_ROOT_INODE_NUMBER + 1;
  mutex_init(&fsi->mutex);
  
  // Сохранить fs_info в super_block
  sb->s_fs_info = fsi;
  
  // Создать корневую директорию
  mutex_lock(&fsi->mutex);
  root = vtfs_create_dir(fsi, VTFS_ROOT_INODE_NUMBER, VTFS_ROOT_INODE_NUMBER);
  if (!root) {
    mutex_unlock(&fsi->mutex);
    kfree(fsi);
    sb->s_fs_info = NULL;
    return -ENOMEM;
  }
  mutex_unlock(&fsi->mutex);

  // Получить корневую inode (с кэшированием)
  inode = vtfs_get_inode(sb, NULL, S_IFDIR | S_IRWXUGO, VTFS_ROOT_INODE_NUMBER);
  if (inode == NULL) {
    mutex_lock(&fsi->mutex);
    vtfs_remove_dir(fsi, VTFS_ROOT_INODE_NUMBER);
    mutex_unlock(&fsi->mutex);
    kfree(fsi);
    sb->s_fs_info = NULL;
    return -ENOMEM;
  }

  sb->s_root = d_make_root(inode);
  if (sb->s_root == NULL) {
    kfree(fsi);
    sb->s_fs_info = NULL;
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
  struct vtfs_fs_info *fsi = VTFS_SB(sb);
  struct vtfs_dir *dir, *dir_tmp;
  struct vtfs_dir_entry *entry, *entry_tmp;
  struct vtfs_file_data *file_data, *file_tmp;
  
  LOG("vtfs super block is destroyed. Unmount successfully.\n");
  
  if (fsi) {
    // Очистить все директории и их содержимое
    mutex_lock(&fsi->mutex);
    
    // Очистить все файлы
    list_for_each_entry_safe(file_data, file_tmp, &fsi->files, list) {
      if (file_data->data) {
        kfree(file_data->data);
      }
      list_del(&file_data->list);
      kfree(file_data);
    }
    
    // Очистить все директории
    list_for_each_entry_safe(dir, dir_tmp, &fsi->directories, list) {
      // Удалить все записи в директории
      list_for_each_entry_safe(entry, entry_tmp, &dir->entries, list) {
        list_del(&entry->list);
        kfree(entry);
      }
      // Удалить саму директорию
      list_del(&dir->list);
      kfree(dir);
    }
    
    mutex_unlock(&fsi->mutex);
    mutex_destroy(&fsi->mutex);
    
    kfree(fsi);
    sb->s_fs_info = NULL;
  }
  
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
