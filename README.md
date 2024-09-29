# ByteChecker

#### 项目简介

<hr>

基于nuttx的内存异常检测程序，对Linux中Kfence的拙劣模仿...

#### 实现思路

<hr>

参考 linux 中的 kfence ：

1. 申请一块用于检测的内存池，大小为：( 数据页数量 * 2 + 1 ) * 数据页大小，并将其初始化

2. 重写 malloc 和 free 函数

3. 申请内存时，若本次申请合法，将返回 bc 内存池中的一块空闲内存，并根据其申请的大小在该内存前后填充特定的数据

4. 释放内存时，会检测填充的数据是否被篡改以检测程序是否出现内存异常的情况

```shell
  ---+----------+---+--------+----------+---
     | xxxxxxxx | O | xxxxxx | xxxxxxxx |
     | xxxxxxxx | B | xxxxxx | xxxxxxxx |
     | x BYTE x | J | x BY x | x BYTE x |
     | xxxxxxxx | E | x TE x | xxxxxxxx |
     | xxxxxxxx | C | xxxxxx | xxxxxxxx |
     | xxxxxxxx | T | xxxxxx | xxxxxxxx |
  ---+----------+---+--------+----------+---
```

#### 项目文件

<hr>

```shell
.
├── bytechecker
│   ├── bc_main.c         
│   ├── bytechecker.h    
│   ├── CMakeLists.txt
│   ├── Kconfig
│   └── Make.defs     
├── img
│   └── bc_test.png      
├── Makefile
└── README.md
```