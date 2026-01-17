#include "http.h"

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <linux/errno.h>
#include <net/sock.h>

const char *SERVER_IP = "127.0.0.1";
const int SERVER_PORT = 18080;

// callee should kfree(vec->iov_base) on success/failure
static int fill_request(struct kvec *vec, const char *token, const char *method,
                        size_t arg_size, va_list args) {
  // 2048 bytes for URL and 64 bytes for anything else
  char *request_buffer = kzalloc(2048 + 64, GFP_KERNEL);
  if (!request_buffer) {
    return -ENOMEM;
  }

  strcpy(request_buffer, "GET /fs/");
  strcat(request_buffer, method);

  strcat(request_buffer, "?token=");
  strcat(request_buffer, token);

  for (int i = 0; i < arg_size; i++) {
    strcat(request_buffer, "&");
    strcat(request_buffer, va_arg(args, char *));
    strcat(request_buffer, "=");
    strcat(request_buffer, va_arg(args, char *));
  }

  strcat(request_buffer, " HTTP/1.1\r\nHost:");
  strcat(request_buffer, SERVER_IP);
  strcat(request_buffer, "\r\nConnection: close\r\n\r\n");

  memset(vec, 0, sizeof(struct kvec));
  vec->iov_base = request_buffer;
  vec->iov_len = strlen(request_buffer);

  return 0;
}

static int receive_all(struct socket *sock, char *buffer, size_t buffer_size) {
  struct msghdr hdr;
  struct kvec vec;
  int read = 0;

  while (read < buffer_size) {
    memset(&hdr, 0, sizeof(struct msghdr));
    memset(&vec, 0, sizeof(struct kvec));
    vec.iov_base = buffer + read;
    vec.iov_len = buffer_size - read;

    int ret = kernel_recvmsg(sock, &hdr, &vec, 1, vec.iov_len, 0);
    if (ret == 0) {
      break;
    } else if (ret < 0) {
      return -4;
    }
    read += ret;
  }

  return read;
}

static int64_t parse_http_response(char *raw_response, size_t raw_response_size,
                                   char *response, size_t response_size) {
  char *buffer = raw_response;

  // --- Response line ---
  {
    char *status_line = strsep(&buffer, "\r");
    (void)strsep(&status_line, " "); // "HTTP/1.1"
    if (status_line == NULL) {
      return -6;
    }
    char *status_code = strsep(&status_line, " ");
    if (status_code == NULL) {
      return -6;
    }

    printk(KERN_INFO "Received response with status code %s\n", status_code);

    if (strcmp(status_code, "200") != 0) {
      return -5;
    }
  }

  int length = -1;

  // --- Headers ---
  while (true) {
    if (buffer == NULL) {
      return -6;
    }

    char *header = strsep(&buffer, "\r");
    if (header == NULL) {
      return -6;
    }

    if (buffer == NULL) {
      return -6;
    }
    if (*buffer == '\n') {
      buffer++;
    }

    if (strcmp(header, "") == 0) {
      // end of headers
      break;
    }

    if (strncmp(header, "Content-Length: ", 16) == 0) {
      int error = kstrtoint(header + 16, 0, &length);
      if (error != 0) {
        return -6;
      }
      printk(KERN_INFO "Received response with content length %d\n", length);
    }
  }

  if (length < 0) {
    return -6;
  }

  // buffer now points to body
  if ((size_t)(buffer - raw_response) > raw_response_size) {
    return -6;
  }

  if ((size_t)length > raw_response_size - (size_t)(buffer - raw_response)) {
    return -6;
  }

  if (length < (int)sizeof(int64_t)) {
    return -7;
  }

  // Body format: [int64 ret][payload bytes...]
  int payload_len = length - (int)sizeof(int64_t);

  // Need +1 to add '\0' for JSON parsing via strstr/...
  if ((size_t)payload_len + 1 > response_size) {
    return -ENOSPC;
  }

  int64_t return_value;
  memcpy(&return_value, buffer, sizeof(int64_t));
  buffer += sizeof(int64_t);

  memcpy(response, buffer, payload_len);
  response[payload_len] = '\0';

  return return_value;
}

int64_t vtfs_http_call(const char *token, const char *method,
                       char *response_buffer, size_t buffer_size,
                       size_t arg_size, ...) {
  struct socket *sock;
  int64_t error;

  error = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
  if (error < 0) {
    return -1;
  }

  struct sockaddr_in s_addr = {
      .sin_family = AF_INET,
      .sin_addr = {.s_addr = in_aton(SERVER_IP)},
      .sin_port = htons(SERVER_PORT)
  };

  error = kernel_connect(sock, (struct sockaddr *)&s_addr,
                         sizeof(struct sockaddr_in), 0);
  if (error != 0) {
    sock_release(sock);
    return -2;
  }

  struct kvec kvec;
  va_list args;
  va_start(args, arg_size);
  error = fill_request(&kvec, token, method, arg_size, args);
  va_end(args);

  if (error != 0) {
    kernel_sock_shutdown(sock, SHUT_RDWR);
    sock_release(sock);
    return error;
  }

  struct msghdr msg;
  memset(&msg, 0, sizeof(struct msghdr));

  error = kernel_sendmsg(sock, &msg, &kvec, 1, kvec.iov_len);
  kfree(kvec.iov_base);

  if (error < 0) {
    kernel_sock_shutdown(sock, SHUT_RDWR);
    sock_release(sock);
    return -3;
  }

  size_t raw_buffer_size = buffer_size + 1024; // add 1KB for HTTP headers
  char *raw_response_buffer = kmalloc(raw_buffer_size, GFP_KERNEL);
  if (!raw_response_buffer) {
    kernel_sock_shutdown(sock, SHUT_RDWR);
    sock_release(sock);
    return -ENOMEM;
  }

  int read_bytes = receive_all(sock, raw_response_buffer, raw_buffer_size);

  kernel_sock_shutdown(sock, SHUT_RDWR);
  sock_release(sock);

  if (read_bytes < 0) {
    kfree(raw_response_buffer);
    return -4;
  }

  error = parse_http_response(raw_response_buffer, read_bytes,
                              response_buffer, buffer_size);

  kfree(raw_response_buffer);
  return error;
}

void encode(const char *src, char *dst) {
  while (*src != '\0') {
    if ((*src >= '0' && *src <= '9') ||
        (*src >= 'a' && *src <= 'z') ||
        (*src >= 'A' && *src <= 'Z')) {
      *dst++ = *src;
    } else {
      sprintf(dst, "%%%02X", (unsigned char)*src);
      dst += 3;
    }
    src++;
  }
  *dst = '\0';
}

// ========== Base64 ==========

static const char base64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64_encode(const char *src, size_t src_len, char *dst, size_t dst_size) {
  size_t i, j;
  unsigned char a, b, c;

  size_t needed = ((src_len + 2) / 3) * 4 + 1;
  if (dst_size < needed) {
    return -ENOSPC;
  }

  j = 0;
  for (i = 0; i < src_len; i += 3) {
    a = src[i];
    b = (i + 1 < src_len) ? src[i + 1] : 0;
    c = (i + 2 < src_len) ? src[i + 2] : 0;

    dst[j++] = base64_table[a >> 2];
    dst[j++] = base64_table[((a & 0x03) << 4) | (b >> 4)];
    dst[j++] = (i + 1 < src_len)
                   ? base64_table[((b & 0x0F) << 2) | (c >> 6)]
                   : '=';
    dst[j++] = (i + 2 < src_len) ? base64_table[c & 0x3F] : '=';
  }
  dst[j] = '\0';

  return 0;
}

int base64_decode(const char *src, char *dst, size_t dst_size) {
  size_t i, j;
  unsigned char a, b, c, d;
  size_t src_len = strlen(src);
  size_t needed = (src_len / 4) * 3;

  if (dst_size < needed) {
    return -ENOSPC;
  }

  j = 0;
  for (i = 0; i < src_len; i += 4) {
    a = b = c = d = 0;

    for (int k = 0; k < 64; k++) {
      if (base64_table[k] == src[i]) a = k;
      if (base64_table[k] == src[i + 1]) b = k;
      if (src[i + 2] != '=' && base64_table[k] == src[i + 2]) c = k;
      if (src[i + 3] != '=' && base64_table[k] == src[i + 3]) d = k;
    }

    dst[j++] = (a << 2) | (b >> 4);
    if (src[i + 2] != '=') {
      dst[j++] = (b << 4) | (c >> 2);
    }
    if (src[i + 3] != '=') {
      dst[j++] = (c << 6) | d;
    }
  }

  return (int)j;
}

// ========== JSON helpers ==========

static int json_parse_int(const char *p, int *out) {
  long val = 0;
  int sign = 1;
  int have_digit = 0;

  if (!p || !out)
    return -1;

  while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
    p++;

  if (*p == '-') {
    sign = -1;
    p++;
  }

  while (*p >= '0' && *p <= '9') {
    have_digit = 1;
    val = val * 10 + (*p - '0');
    p++;
  }

  if (!have_digit)
    return -1;

  *out = (int)(val * sign);
  return 0;
}

int json_get_code(const char *json) {
  const char *code_str = strstr(json, "\"code\":");
  int code;

  if (!code_str)
    return -1;

  code_str += 7;
  if (json_parse_int(code_str, &code) != 0)
    return -1;

  return code;
}

int json_get_data_int(const char *json, const char *field) {
  char search[256];
  const char *field_str;
  int value;

  snprintf(search, sizeof(search), "\"%s\":", field);

  field_str = strstr(json, search);
  if (!field_str)
    return -1;

  field_str += strlen(search);

  if (json_parse_int(field_str, &value) != 0)
    return -1;

  return value;
}

int json_get_data_string(const char *json, const char *field,
                         char *out, size_t out_size) {
  char search[256];
  snprintf(search, sizeof(search), "\"%s\":\"", field);

  const char *field_str = strstr(json, search);
  if (!field_str) {
    return -1;
  }

  field_str += strlen(search);

  size_t i = 0;
  while (*field_str != '"' && *field_str != '\0' && i < out_size - 1) {
    out[i++] = *field_str++;
  }
  out[i] = '\0';

  return 0;
}

// ========== API wrappers ==========

static int server_code_to_errno(int code) {
  switch (code) {
    case 0:  return 0;
    case 1:  return -EPERM;
    case 2:  return -ENOENT;
    case 17: return -EEXIST;
    case 20: return -ENOTDIR;
    case 21: return -EISDIR;
    case 39: return -ENOTEMPTY;
    default: return -EIO;
  }
}

int vtfs_api_list(const char *token, ino_t dir,
                  struct vtfs_list_entry *entries,
                  int max_entries, int *count) {
  const size_t RESP_SZ = 65536;

  char *response;
  char dir_str[32];
  int64_t ret;
  int code;
  const char *data_start;
  const char *ptr;

  response = kmalloc(RESP_SZ, GFP_KERNEL);
  if (!response) {
    return -ENOMEM;
  }

  snprintf(dir_str, sizeof(dir_str), "%lu", (unsigned long)dir);

  ret = vtfs_http_call(token, "list", response, RESP_SZ, 1, "dir", dir_str);
  if (ret < 0) {
    kfree(response);
    return (int)ret;
  }

  code = json_get_code(response);
  if (code != 0) {
    kfree(response);
    return server_code_to_errno(code);
  }

  data_start = strstr(response, "\"data\":{");
  if (!data_start) {
    *count = 0;
    kfree(response);
    return 0;
  }

  *count = 0;
  ptr = data_start + 8; // after "data":{

  while (*ptr && *count < max_entries) {
    while (*ptr == ' ' || *ptr == ',' || *ptr == '\n' || *ptr == '\r' || *ptr == '\t')
      ptr++;

    if (*ptr == '}')
      break;

    if (*ptr == '"') {
      ptr++;

      int i = 0;
      while (*ptr && *ptr != '"' && i < 255) {
        entries[*count].name[i++] = *ptr++;
      }
      entries[*count].name[i] = '\0';

      if (*ptr == '"') ptr++;

      while (*ptr == ' ' || *ptr == ':')
        ptr++;

      {
        int ino_val = 0;
        if (json_parse_int(ptr, &ino_val) == 0) {
          entries[*count].ino = (ino_t)ino_val;
          (*count)++;
        }
      }

      while (*ptr && (*ptr >= '0' && *ptr <= '9'))
        ptr++;
    } else {
      ptr++;
    }
  }

  kfree(response);
  return 0;
}

int vtfs_api_is_dir(const char *token, ino_t ino, int *is_dir) {
  char response[256];
  char ino_str[32];

  snprintf(ino_str, sizeof(ino_str), "%lu", (unsigned long)ino);

  int64_t ret = vtfs_http_call(token, "list", response, sizeof(response), 1,
                               "dir", ino_str);
  if (ret < 0) {
    return (int)ret;
  }

  int code = json_get_code(response);
  if (code == 20) {
    *is_dir = 0;
    return 0;
  } else if (code == 0) {
    *is_dir = 1;
    return 0;
  }

  return server_code_to_errno(code);
}

int vtfs_api_root(const char *token, ino_t *root_ino) {
  char response[256];
  int64_t ret = vtfs_http_call(token, "root", response, sizeof(response), 0);
  if (ret < 0) {
    return (int)ret;
  }

  int code = json_get_code(response);
  if (code != 0) {
    return server_code_to_errno(code);
  }

  int ino = json_get_data_int(response, "ino");
  if (ino < 0) {
    return -EIO;
  }

  *root_ino = (ino_t)ino;
  return 0;
}

int vtfs_api_lookup(const char *token, ino_t parent, const char *name, ino_t *result_ino) {
  char response[256];
  char parent_str[32], name_encoded[512];

  snprintf(parent_str, sizeof(parent_str), "%lu", (unsigned long)parent);
  encode(name, name_encoded);

  int64_t ret = vtfs_http_call(token, "lookup", response, sizeof(response), 2,
                               "parent", parent_str, "name", name_encoded);
  if (ret < 0) {
    return (int)ret;
  }

  int code = json_get_code(response);
  if (code != 0) {
    return server_code_to_errno(code);
  }

  int ino = json_get_data_int(response, "ino");
  if (ino < 0) {
    return -EIO;
  }

  *result_ino = (ino_t)ino;
  return 0;
}

int vtfs_api_create(const char *token, ino_t parent, const char *name, ino_t *result_ino) {
  char response[256];
  char parent_str[32], name_encoded[512];

  snprintf(parent_str, sizeof(parent_str), "%lu", (unsigned long)parent);
  encode(name, name_encoded);

  int64_t ret = vtfs_http_call(token, "create", response, sizeof(response), 2,
                               "parent", parent_str, "name", name_encoded);
  if (ret < 0) {
    return (int)ret;
  }

  int code = json_get_code(response);
  if (code != 0) {
    return server_code_to_errno(code);
  }

  int ino = json_get_data_int(response, "ino");
  if (ino < 0) {
    return -EIO;
  }

  *result_ino = (ino_t)ino;
  return 0;
}

int vtfs_api_mkdir(const char *token, ino_t parent, const char *name, ino_t *result_ino) {
  char response[256];
  char parent_str[32], name_encoded[512];

  snprintf(parent_str, sizeof(parent_str), "%lu", (unsigned long)parent);
  encode(name, name_encoded);

  int64_t ret = vtfs_http_call(token, "mkdir", response, sizeof(response), 2,
                               "parent", parent_str, "name", name_encoded);
  if (ret < 0) {
    return (int)ret;
  }

  int code = json_get_code(response);
  if (code != 0) {
    return server_code_to_errno(code);
  }

  int ino = json_get_data_int(response, "ino");
  if (ino < 0) {
    return -EIO;
  }

  *result_ino = (ino_t)ino;
  return 0;
}

int vtfs_api_unlink(const char *token, ino_t parent, const char *name) {
  char response[256];
  char parent_str[32], name_encoded[512];

  snprintf(parent_str, sizeof(parent_str), "%lu", (unsigned long)parent);
  encode(name, name_encoded);

  int64_t ret = vtfs_http_call(token, "unlink", response, sizeof(response), 2,
                               "parent", parent_str, "name", name_encoded);
  if (ret < 0) {
    return (int)ret;
  }

  int code = json_get_code(response);
  return server_code_to_errno(code);
}

int vtfs_api_rmdir(const char *token, ino_t parent, const char *name) {
  char response[256];
  char parent_str[32], name_encoded[512];

  snprintf(parent_str, sizeof(parent_str), "%lu", (unsigned long)parent);
  encode(name, name_encoded);

  int64_t ret = vtfs_http_call(token, "rmdir", response, sizeof(response), 2,
                               "parent", parent_str, "name", name_encoded);
  if (ret < 0) {
    return (int)ret;
  }

  int code = json_get_code(response);
  return server_code_to_errno(code);
}

int vtfs_api_read(const char *token, ino_t ino, loff_t offset, size_t len,
                  char *data, size_t *actual_len) {
  const size_t RESP_SZ = 65536;

  char *response;
  char *data_b64;
  char ino_str[32], offset_str[32], len_str[32];
  int64_t ret;
  int code;
  int decoded_len;

  response = kmalloc(RESP_SZ, GFP_KERNEL);
  if (!response) {
    return -ENOMEM;
  }

  data_b64 = kmalloc(RESP_SZ, GFP_KERNEL);
  if (!data_b64) {
    kfree(response);
    return -ENOMEM;
  }

  snprintf(ino_str, sizeof(ino_str), "%lu", (unsigned long)ino);
  snprintf(offset_str, sizeof(offset_str), "%lld", (long long)offset);
  snprintf(len_str, sizeof(len_str), "%zu", len);

  ret = vtfs_http_call(token, "read", response, RESP_SZ, 3,
                       "ino", ino_str, "offset", offset_str, "len", len_str);
  if (ret < 0) {
    kfree(data_b64);
    kfree(response);
    return (int)ret;
  }

  code = json_get_code(response);
  if (code != 0) {
    kfree(data_b64);
    kfree(response);
    return server_code_to_errno(code);
  }

  if (json_get_data_string(response, "data", data_b64, RESP_SZ) < 0) {
    *actual_len = 0;
    kfree(data_b64);
    kfree(response);
    return 0;
  }

  decoded_len = base64_decode(data_b64, data, len);
  if (decoded_len < 0) {
    kfree(data_b64);
    kfree(response);
    return decoded_len;
  }

  *actual_len = (size_t)decoded_len;
  kfree(data_b64);
  kfree(response);
  return 0;
}

int vtfs_api_write(const char *token, ino_t ino, loff_t offset,
                   const char *data, size_t len) {
  char response[256];
  char ino_str[32], offset_str[32];
  char *data_b64;

  data_b64 = kmalloc(((len + 2) / 3) * 4 + 1, GFP_KERNEL);
  if (!data_b64) {
    return -ENOMEM;
  }

  snprintf(ino_str, sizeof(ino_str), "%lu", (unsigned long)ino);
  snprintf(offset_str, sizeof(offset_str), "%lld", (long long)offset);

  if (base64_encode(data, len, data_b64, ((len + 2) / 3) * 4 + 1) < 0) {
    kfree(data_b64);
    return -ENOMEM;
  }

  int64_t ret = vtfs_http_call(token, "write", response, sizeof(response), 3,
                               "ino", ino_str, "offset", offset_str, "data", data_b64);
  kfree(data_b64);

  if (ret < 0) {
    return (int)ret;
  }

  int code = json_get_code(response);
  return server_code_to_errno(code);
}

int vtfs_api_link(const char *token, ino_t old_ino, ino_t new_parent,
                  const char *new_name) {
  char response[256];
  char old_ino_str[32], new_parent_str[32], new_name_encoded[512];

  snprintf(old_ino_str, sizeof(old_ino_str), "%lu", (unsigned long)old_ino);
  snprintf(new_parent_str, sizeof(new_parent_str), "%lu", (unsigned long)new_parent);
  encode(new_name, new_name_encoded);

  int64_t ret = vtfs_http_call(token, "link", response, sizeof(response), 3,
                               "oldIno", old_ino_str,
                               "newParent", new_parent_str,
                               "newName", new_name_encoded);
  if (ret < 0) {
    return (int)ret;
  }

  int code = json_get_code(response);
  return server_code_to_errno(code);
}
