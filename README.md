# WorldFlip_Gacha

基于C++的Linux世界弹射物语抽卡模拟器

- 使用 **线程池 + 非阻塞socket + epoll(ET和LT均实现) + 事件处理(Reactor和模拟Proactor均实现)** 的并发模型
- 使用**状态机**解析HTTP请求报文，能够根据服务器的请求做出正确响应
- **访问服务器数据库**实现用户的注册以及登陆，能够**保存抽卡记录**并**查看用户拥有的角色**
- 实现**同步/异步日志系统**，记录服务器运行状态
- 经Webbench压力测试可以实现**上万的并发连接**数据交换

## 功能展示

![Alt text](./SHOW.gif)

## Webbench压力测试结果

![Alt text](./webbench.png)

## 项目部署

在项目目录下创建Server_log文件夹

```bash
mkdir Server_log
```

安装好mysql后，输入以下命令初始化数据库

```mysql
create database yourdb;

use yourdb;

CREATE TABLE user(
    username char(50) NULL,
    passwd char(50) NULL
)ENGINE=InnoDB;

ALTER TABLE user MODIFY username char(50) NOT NULL;
ALTER TABLE user ADD CONSTRAINT unique_username UNIQUE (username);
#设置NOT NULL和唯一列，否则gacha_logs不允许username作为外键

CREATE TABLE gacha_logs (
                            id INT(11) NOT NULL AUTO_INCREMENT,
                            username char(50) NOT NULL,
                            gacha_character TEXT NOT NULL,
                            gacha_time TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
                            PRIMARY KEY (id),
                            FOREIGN KEY (username) REFERENCES user(username)
);
```

编译

```bash
sh ./build.sh
```

运行

```bash
./server
```

