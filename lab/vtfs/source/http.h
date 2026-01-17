#ifndef VTFS_HTTP_H
#define VTFS_HTTP_H

#include <linux/inet.h>
#include <linux/types.h>
#include <linux/fs.h>

// Основная функция для HTTP запросов
int64_t vtfs_http_call(const char *token, const char *method,
                            char *response_buffer, size_t buffer_size,
                            size_t arg_size, ...);

// URL-кодирование
void encode(const char *, char *);

// Base64 кодирование
int base64_encode(const char *src, size_t src_len, char *dst, size_t dst_size);

// Base64 декодирование
int base64_decode(const char *src, char *dst, size_t dst_size);

// Простой JSON парсер - извлечь значение поля "code"
int json_get_code(const char *json);

// Простой JSON парсер - извлечь значение числового поля из data
int json_get_data_int(const char *json, const char *field);

// Простой JSON парсер - извлечь строковое значение поля из data
int json_get_data_string(const char *json, const char *field, char *out, size_t out_size);

// Структура для хранения информации о файле из list
struct vtfs_list_entry {
  char name[256];
  ino_t ino;
};

// API обёртки для работы с сервером
int vtfs_api_root(const char *token, ino_t *root_ino);
int vtfs_api_list(const char *token, ino_t dir, struct vtfs_list_entry *entries, int max_entries, int *count);
int vtfs_api_lookup(const char *token, ino_t parent, const char *name, ino_t *result_ino);
int vtfs_api_create(const char *token, ino_t parent, const char *name, ino_t *result_ino);
int vtfs_api_mkdir(const char *token, ino_t parent, const char *name, ino_t *result_ino);
int vtfs_api_unlink(const char *token, ino_t parent, const char *name);
int vtfs_api_rmdir(const char *token, ino_t parent, const char *name);
int vtfs_api_read(const char *token, ino_t ino, loff_t offset, size_t len, char *data, size_t *actual_len);
int vtfs_api_write(const char *token, ino_t ino, loff_t offset, const char *data, size_t len);
int vtfs_api_link(const char *token, ino_t old_ino, ino_t new_parent, const char *new_name);
int vtfs_api_is_dir(const char *token, ino_t ino, int *is_dir);

#endif // VTFS_HTTP_H
