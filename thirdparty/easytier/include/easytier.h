#ifndef EASYTIER_H
#define EASYTIER_H

#include <stdint.h>

/* 键值对结构体，对应 collect_network_infos 的返回 */
typedef struct {
    const char *key;
    const char *value;
} KeyValuePair;

/* 验证 TOML 配置，成功返回 0，失败返回 -1 */
int parse_config(const char *cfg_str);

/* 启动网络实例，传入 TOML 配置字符串，成功返回 0，失败返回 -1 */
int run_network_instance(const char *cfg_str);

/* 保留指定的网络实例（传入实例名数组），成功返回 0，失败返回 -1 */
int retain_network_instance(const char **inst_names, uintptr_t length);

/* 收集网络信息，返回填充的 KeyValuePair 数量，失败返回 -1 */
int collect_network_infos(KeyValuePair *infos, uintptr_t max_length);

/* 设置 TUN 文件描述符（高级用法），成功返回 0，失败返回 -1 */
int set_tun_fd(const char *inst_name, int fd);

/* 获取最近一次错误信息（如果无错误，输出 NULL） */
void get_error_msg(const char **out);

/* 释放字符串内存（与 get_error_msg 配对使用） */
void free_string(const char *s);

#endif /* EASYTIER_H */