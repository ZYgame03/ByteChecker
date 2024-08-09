# ByteChecker

#### 项目简介

<hr>

在nuttx上的内存异常检测小程序，对Linux中Kfence的拙劣模仿...

#### 实现思路

<hr>

参考 linux 中 kfence 的实现方法

1. 申请一块用于检测的内存池，大小为：( 数据页数量 * 2 + 1 ) * 数据页大小，并将其初始化

2. 重写 malloc 和 free 函数

3. 当申请内存时，若本次申请合法，将返回 bc 内存池中的一块空闲内存，并根据其申请的大小在该内存前后填充特定的数据

4. 当释放内存时，会检测填充的数据是否被篡改已检测程序是否出现内存异常的情况

```shell
  ---+-----------+-----------+-----------+---
     | xxxxxxxx | O | xxxxxx | xxxxxxxx |
     | xxxxxxxx | B | xxxxxx | xxxxxxxx |
     | x BYTE x | J | x BY x | x BYTE x |
     | xxxxxxxx | E | x TE x | xxxxxxxx |
     | xxxxxxxx | C | xxxxxx | xxxxxxxx |
     | xxxxxxxx | T | xxxxxx | xxxxxxxx |
  ---+-----------+-----------+-----------+---
```

#### 项目文件说明

```shell
├── bc_main.c          # 程序核心代码
├── bytechecker.h      # bc头文件
├── CMakeLists.txt     # 编译相关
├── Kconfig            # 配置文件
├── Make.defs          # 编译相关
├── Makefile           # Makefile构建脚本
├── img                # 图片
|   ├── bc_test.png    # 测试截图
├── README.md          # README
```

#### 使用说明

<hr>

1. 将本项目复制到 nuttx-apps 中，并在 Kconfig 引用本项目的 Kconfig

2. 在配置中启用 ByteChecker

3. 在需要检测的程序中引用 bytechecker.h的头文件

4. 编译并运行 bytechecker，即可在程序运行时检测错误



