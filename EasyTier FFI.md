> 本文档非Easytier官方文档，是为了解决Easytier ffi函数返回不明确的问题，让AI翻阅源代码编写。

# EasyTier FFI Network Info 解析技术文档

本文档详细解析 EasyTier FFI 层的网络信息收集机制，包括数据结构、核心函数、实现原理和使用方法。

# 一、概述

EasyTier FFI（Foreign Function Interface）提供了一套 C ABI 接口，允许其他编程语言通过 C 调用方式与 EasyTier 核心功能交互。其中 `collect_network_infos` 函数用于收集当前运行中所有网络实例的状态信息，是外部程序监控 EasyTier 运行状态的核心接口。

# 二、数据结构定义

## 2\.1 KeyValuePair 结构体

网络信息以键值对数组的形式返回，每个元素对应一个网络实例的信息。

```c
typedef struct {
  const char* key;    // 实例名称 (inst_name)
  const char* value;  // JSON 格式的实例运行信息
} KeyValuePair;
```

|字段|类型|说明|
|---|---|---|
|`key`|`const char*`|网络实例的名称，对应配置中的 `inst_name` 字段|
|`value`|`const char*`|JSON 格式的字符串，包含该实例的完整运行信息（NetworkInstanceRunningInfo）|

# 三、核心函数详解

## 3\.1 collect\_network\_infos

收集所有运行中网络实例的状态信息。

```c
/**
 * 收集运行中网络实例的信息
 * 
 * @param infos       指向 KeyValuePair 数组的指针，用于存储结果
 * @param max_length  数组的最大容量
 * @return            成功返回实际写入的条目数量，失败返回 -1
 * 
 * 注意：返回的 key 和 value 字符串由库分配，必须使用 free_string() 释放
 */
extern int collect_network_infos(KeyValuePair* infos, size_t max_length);
```

### 参数说明

- `infos`：调用者预先分配的 KeyValuePair 数组指针，用于接收返回的网络信息

- `max_length`：数组的最大容量，即最多返回多少个实例的信息

### 返回值

- `>= 0`：实际写入数组的条目数量，即运行中的实例个数

- `-1`：调用失败，可通过 `get_error_msg()` 获取详细错误信息

# 四、实现原理

## 4\.1 调用链路

FFI 层的 `collect_network_infos` 函数通过以下链路获取网络信息：

1. FFI 层接收 C 端调用，进行参数校验和安全检查

2. 调用全局单例 `INSTANCE_MANAGER.collect_network_infos_sync()`

3. 核心层同步方法内部创建 tokio runtime，调用异步版本

4. 遍历所有运行中的实例，调用 `get_running_info()` 获取状态

5. 将 `NetworkInstanceRunningInfo` 序列化为 JSON 字符串

6. 将实例名称和 JSON 字符串填充到 KeyValuePair 数组中返回

## 4\.2 核心实现代码

```rust
pub(crate) unsafe fn collect_network_infos(
  infos: *mut KeyValuePair,
  max_length: usize,
) -> std::ffi::c_int {
  // 1. 安全检查：不允许在 config server 回调中调用
  if in_config_server_callback() {
    set_error_msg("cannot collect network infos from config server callback");
    return -1;
  }

  if max_length == 0 {
    return 0;
  }

  let infos = unsafe {
    assert!(!infos.is_null());
    std::slice::from_raw_parts_mut(infos, max_length)
  };

  // 2. 调用核心层获取网络信息
  let collected_infos = match INSTANCE_MANAGER.collect_network_infos_sync() {
    Ok(infos) => infos,
    Err(e) => {
      set_error_msg(&format!("failed to collect network infos: {}", e));
      return -1;
    }
  };

  // 3. 遍历结果，序列化为 JSON 并填充数组
  let mut index = 0;
  for (instance_id, value) in collected_infos.iter() {
    if index >= max_length {
      break;
    }

    let Some(key) = INSTANCE_MANAGER.get_instance_name(instance_id) else {
      continue;
    };

    // 将运行信息序列化为 JSON 字符串
    let value = match serde_json::to_string(&value) {
      Ok(value) => value,
      Err(e) => {
        set_error_msg(&format!("failed to serialize instance info: {}", e));
        return -1;
      }
    };

    infos[index] = KeyValuePair {
      key: std::ffi::CString::new(key).unwrap().into_raw(),
      value: std::ffi::CString::new(value).unwrap().into_raw(),
    };
    index += 1;
  }

  index as std::ffi::c_int
}
```

## 4\.3 核心层数据结构

核心层返回的数据类型为 `BTreeMap<Uuid, NetworkInstanceRunningInfo>`，其中：

- `Uuid`：实例的唯一标识符

- `NetworkInstanceRunningInfo`：包含实例完整运行状态的结构体（序列化为 JSON 后返回）

除了正常运行的实例信息外，返回结果中还会包含已停止但有错误信息的实例状态。

# 五、返回数据格式详解

**重要**：以下是 `collect_network_infos` 返回的 value 字段（JSON 字符串）的详细结构说明，这是使用该接口最核心的内容。

## 5\.1 NetworkInstanceRunningInfo 结构

`value` 字段是 `NetworkInstanceRunningInfo` 结构体的 JSON 序列化结果，包含以下顶级字段：

|字段名|类型|说明|
|---|---|---|
|`running`|`boolean`|实例是否正在运行|
|`dev_name`|`string`|TUN 虚拟网卡设备名称|
|`error_msg`|`string | null`|错误信息，如果实例运行出错则包含错误描述，正常运行时为 null|
|`my_node_info`|`object | null`|本地节点的详细信息（MyNodeInfo 对象）|
|`peers`|`array`|对等节点信息列表（PeerInfo 数组）|
|`routes`|`array`|路由信息列表（Route 数组）|
|`peer_route_pairs`|`array`|对等节点与路由的配对信息|
|`events`|`array`|最近的事件列表（JSON 字符串数组，最多 20 条）|
|`foreign_network_summary`|`object`|外部网络摘要信息|

## 5\.2 MyNodeInfo（本地节点信息）

`my_node_info` 字段包含本地节点的详细信息：

|字段名|类型|说明|
|---|---|---|
|`peer_id`|`string`|节点的唯一标识符（Peer ID）|
|`virtual_ipv4`|`string | null`|虚拟 IPv4 地址（CIDR 格式，如 "10\.144\.144\.1/24"）|
|`hostname`|`string`|主机名|
|`version`|`string`|EasyTier 版本号|
|`ips`|`array`|节点的 IP 地址列表|
|`stun_info`|`object`|STUN 探测得到的 NAT 类型信息|
|`listeners`|`array`|监听器 URL 列表（如 tcp://0\.0\.0\.0:11010, udp://0\.0\.0\.0:11010 等）|
|`vpn_portal_cfg`|`object | null`|VPN 门户（WireGuard）配置信息，如果启用了 VPN 门户则不为 null|

## 5\.3 PeerInfo（对等节点信息）

`peers` 数组中的每个元素包含一个对等节点的信息：

|字段名|类型|说明|
|---|---|---|
|`peer_id`|`string`|对等节点的唯一标识符|
|`ipv4_addr`|`string`|对等节点的虚拟 IPv4 地址|
|`hostname`|`string`|对等节点的主机名|
|`version`|`string`|对等节点的 EasyTier 版本号|
|`cost`|`number`|路由开销（跳数）|
|`lat_ms`|`number`|延迟（毫秒）|
|`loss_rate`|`number`|丢包率|
|`rx_bytes`|`number`|接收字节数|
|`tx_bytes`|`number`|发送字节数|
|`tunnel_proto`|`string`|隧道协议（如 udp, tcp, ws, wss, wg 等）|
|`nat_type`|`string`|NAT 类型（如 FullCone, RestrictedCone, PortRestrictedCone, Symmetric, Unknown 等）|

## 5\.4 RouteInfo（路由信息）

`routes` 数组中的每个元素包含一条路由信息：

|字段名|类型|说明|
|---|---|---|
|`cidr`|`string`|目标网络 CIDR|
|`next_hop_peer_id`|`string`|下一跳对等节点 ID|
|`cost`|`number`|路由开销|
|`proxy_cidrs`|`array`|代理的子网 CIDR 列表|

## 5\.5 Event（事件信息）

`events` 数组中的每个元素是一个 JSON 格式的事件字符串，包含时间和事件类型。事件类型包括：

- `PeerAdded` \- 新对等节点加入

- `PeerConnAdded` \- 新连接建立

- `ListenerAdded` \- 新监听器添加

- `TunDeviceReady` \- TUN 设备就绪

- `VpnPortalClientConnected` \- VPN 门户客户端连接

- `DhcpIpv4Changed` \- DHCP 分配的 IPv4 地址变更

## 5\.6 JSON 示例

```json
{
  "running": true,
  "dev_name": "tun0",
  "error_msg": null,
  "my_node_info": {
    "peer_id": "439804259",
    "virtual_ipv4": "10.126.126.1/24",
    "hostname": "node-a",
    "version": "2.6.2",
    "ips": ["192.168.1.100"],
    "stun_info": {
      "udp_nat_type": "FullCone",
      "tcp_nat_type": "FullCone"
    },
    "listeners": [
      "tcp://0.0.0.0:11010",
      "udp://0.0.0.0:11010"
    ],
    "vpn_portal_cfg": null
  },
  "peers": [
    {
      "peer_id": "390879727",
      "ipv4_addr": "10.126.126.2",
      "hostname": "node-b",
      "version": "2.6.2",
      "cost": 1,
      "lat_ms": 3.452,
      "loss_rate": 0,
      "rx_bytes": 17746,
      "tx_bytes": 20910,
      "tunnel_proto": "udp",
      "nat_type": "FullCone"
    }
  ],
  "routes": [
    {
      "cidr": "10.126.126.0/24",
      "next_hop_peer_id": "390879727",
      "cost": 1,
      "proxy_cidrs": []
    }
  ],
  "peer_route_pairs": [],
  "events": [
    "{\"time\":\"2024-01-01T12:00:00+08:00\",\"event\":\"PeerAdded\"}"
  ],
  "foreign_network_summary": {}
}
```

# 六、使用示例

## 6\.1 完整 C 语言示例

```c
#include &lt;stdio.h&gt;
#include &lt;stdlib.h&gt;
#include &lt;string.h&gt;
#include &lt;stdbool.h&gt;
#include &lt;unistd.h&gt;

// FFI 结构体和函数声明
typedef struct {
  const char* key;
  const char* value;
} KeyValuePair;

extern int parse_config(const char* cfg_str);
extern int run_network_instance(const char* cfg_str);
extern void get_error_msg(const char** out);
extern void free_string(const char* s);
extern int collect_network_infos(KeyValuePair* infos, size_t max_length);

int main() {
  // 1. 配置并启动网络实例
  const char* config = "inst_name = \"test\"\nnetwork = \"test_network\"\n";
  int ret;

  ret = parse_config(config);
  if (ret != 0) {
    const char* err = NULL;
    get_error_msg(&amp;err);
    if (err) {
      printf("parse_config error: %s\n", err);
      free_string(err);
    }
    return 1;
  }

  ret = run_network_instance(config);
  if (ret != 0) {
    const char* err = NULL;
    get_error_msg(&amp;err);
    if (err) {
      printf("run_network_instance error: %s\n", err);
      free_string(err);
    }
    return 1;
  }
  printf("run_network_instance success\n");

  // 2. 分配 KeyValuePair 数组
  const size_t max_infos = 8;
  KeyValuePair* infos = (KeyValuePair*)malloc(sizeof(KeyValuePair) * max_infos);
  if (!infos) {
    fprintf(stderr, "malloc failed\n");
    return 1;
  }

  // 3. 周期性调用 collect_network_infos 并打印
  for (int i = 0; i &lt; 5; ++i) {
    memset(infos, 0, sizeof(KeyValuePair) * max_infos);

    int count = collect_network_infos(infos, max_infos);
    if (count &lt; 0) {
      const char* err = NULL;
      get_error_msg(&amp;err);
      if (err) {
        printf("collect_network_infos error: %s\n", err);
        free_string(err);
      }
      break;
    }

    printf("collect_network_infos: %d instance(s)\n", count);
    for (int j = 0; j &lt; count; ++j) {
      printf("  [%d] key: %s\n       value: %s\n", j, infos[j].key, infos[j].value);

      // 4. 释放每个字符串
      free_string(infos[j].key);
      free_string(infos[j].value);
    }

    sleep(1);
  }

  // 5. 释放数组
  free(infos);
  return 0;
}
```

# 七、内存管理

## 7\.1 内存分配与释放

FFI 接口遵循"谁分配谁释放"的原则，但需要特别注意字符串的管理：

|内存类型|分配方|释放方|释放方式|
|---|---|---|---|
|KeyValuePair 数组|调用方（C 端）|调用方（C 端）|`free()`|
|key 字符串|FFI 库（Rust 端）|调用方（C 端）|`free_string()`|
|value 字符串|FFI 库（Rust 端）|调用方（C 端）|`free_string()`|

## 7\.2 注意事项

- **必须使用 ****`free_string()`**** 释放字符串**，不能直接使用 C 标准库的 `free()`，因为字符串是由 Rust 的分配器分配的

- 每次调用 `collect_network_infos` 后，都需要释放返回的所有 key 和 value 字符串

- 建议在调用前将数组清零，以便在出错时能够安全地跳过未初始化的条目

- 错误信息字符串（通过 `get_error_msg()` 获取）同样需要使用 `free_string()` 释放

# 八、相关函数

## 8\.1 其他网络管理函数

|函数名|说明|
|---|---|
|`parse_config`|验证 TOML 配置字符串的语法正确性|
|`run_network_instance`|根据配置启动一个网络实例|
|`retain_network_instance`|保留指定的实例，停止其他所有实例|
|`delete_network_instance`|停止并删除指定的网络实例|
|`list_instance`|列出所有运行中实例的名称和 ID|
|`set_tun_fd`|为指定实例附加 TUN 设备文件描述符（移动端使用）|
|`call_json_rpc`|调用 EasyTier 的 JSON RPC 服务|

## 8\.2 辅助函数

|函数名|说明|
|---|---|
|`get_error_msg`|获取最后一次错误的详细信息|
|`free_string`|释放由 FFI 库分配的字符串内存|

# 九、限制与约束

- **线程安全**：`collect_network_infos` 可以在多线程环境中调用，但内部会获取全局锁

- **回调限制**：不能在 config server 事件回调中调用此函数，否则会失败

- **数组大小**：如果运行中的实例数量超过 `max_length`，超出部分会被截断

- **性能考虑**：该函数会创建临时的 tokio runtime 来执行异步操作，频繁调用可能影响性能

- **事件数量**：events 数组最多包含最近 20 条事件

> （注：部分内容可能由 AI 生成）
