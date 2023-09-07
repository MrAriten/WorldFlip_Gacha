#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <unistd.h> //unistd.h是C和C++程序设计语言中提供对POSIX操作系统API的访问功能的头文件
#include <signal.h> //signal.h是C标准函数库中的信号处理部分
#include <sys/types.h> //是Unix/Linux系统的基本系统数据类型的头文件,含有size_t,time_t,pid_t等类型
#include <sys/epoll.h> //和epoll相关类
#include <fcntl.h> //文件控制
#include <sys/socket.h> //socket编程
#include <netinet/in.h> //定义了一些网络编程所需的数据结构和常量SO_REUSEADDR、SO_KEEPALIVE还有网络字节序的转换
#include <arpa/inet.h> //通常与 <netinet/in.h> 一起使用。用于处理网络地址转换、主机字节序和网络字节序之间的转换
#include <assert.h> //函数和宏包括 NDEBUG 宏和 static_assert 宏等，它们用于更高级的断言和条件检查
#include <sys/stat.h> //通常用于访问文件和目录的状态信息以及文件权限的相关操作。
#include <string.h> //关于char*的操作，而不是C++中的string
#include <pthread.h> //关于线程的操作
#include <stdio.h> //标准输入输出库
#include <stdlib.h> //用于进行各种与程序执行、内存分配、随机数生成和其他基本操作相关的任务
#include <sys/mman.h> //包含了一组与内存映射（memory mapping）相关的函数、常量和数据结构。
#include <stdarg.h> //供了一组宏和函数，用于处理可变数量的参数，通常用于实现可变参数的函数，如 printf 和 scanf
#include <errno.h> //错误处理
#include <sys/wait.h> //提供了一组与进程控制和等待相关的函数、宏和数据结构，通常用于处理子进程的状态信息以及等待子进程的终止。
#include <sys/uio.h> //提供了一组与矢量I/O（Vector I/O）相关的函数和数据结构,readv writev 等
#include <map> //定义了C++中的 std::map 类模板

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

class http_conn
{
public:
    //设置读取文件的名称m_real_file大小
    static const int FILENAME_LEN = 200;
    //设置读缓冲区m_read_buf大小
    static const int READ_BUFFER_SIZE = 2048;
    //设置写缓冲区m_write_buf大小
    static const int WRITE_BUFFER_SIZE = 1024;

    enum METHOD//报文的请求方法，本项目只用到GET和POST
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    enum CHECK_STATE//主状态机的状态
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    enum HTTP_CODE//报文解析的结果
    {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION,
        RANDOM_REQUEST,
        RANDOM_MULTI_REQUEST,
        GET_USER_INFO
    };
    enum LINE_STATUS//从状态机的状态
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    //初始化套接字地址，函数内部会调用私有方法init
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    void close_conn(bool real_close = true);//关闭http连接
    void process();//读取浏览器端发来的全部数据
    bool read_once(); //响应报文写入函数
    bool write();
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    void initmysql_result(connection_pool *connPool);//同步线程初始化数据库读取表
    int timer_flag;
    int improv;


private:
    void init();
    HTTP_CODE process_read();//从m_read_buf读取，并处理请求报文
    bool process_write(HTTP_CODE ret);//向m_write_buf写入响应报文数据
    HTTP_CODE parse_request_line(char *text);//主状态机解析报文中的请求行数据
    HTTP_CODE parse_headers(char *text); //主状态机解析报文中的请求头数据
    HTTP_CODE parse_content(char *text);//主状态机解析报文中的请求内容
    HTTP_CODE do_request(); //生成响应报文
    char *get_line() { return m_read_buf + m_start_line; };//m_start_line是已经解析的字符，get_line用于将指针向后偏移，指向未处理的字符
    LINE_STATUS parse_line();//从状态机读取一行，分析是请求报文的哪一部分
    void unmap();//关闭文件映射

    //根据响应报文格式，生成对应8个部分，以下函数均由do_request调用
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;//epoll事件是只有一个的static变量
    static int m_user_count;
    MYSQL *mysql;
    int m_state;  //读为0, 写为1

private:
    int m_sockfd;
    sockaddr_in m_address;
    char m_read_buf[READ_BUFFER_SIZE];//存储读取的请求报文数据
    int m_read_idx;//缓冲区中m_read_buf中数据的最后一个字节的下一个位置
    int m_checked_idx;//m_read_buf读取的位置m_checked_idx
    int m_start_line;//m_read_buf中已经解析的字符个数

    char m_write_buf[WRITE_BUFFER_SIZE];//存储发出的响应报文数据
    int m_write_idx;//指示buffer中的长度

    CHECK_STATE m_check_state;//主状态机的状态
    METHOD m_method;//请求方法

    //以下为解析请求报文中对应的6个变量
    char m_real_file[FILENAME_LEN];//存储读取文件的名称
    char *m_url;
    char *m_version;
    char *m_host;
    int m_content_length;
    bool m_linger;

    char *m_file_address;//读取服务器上的文件地址
    struct stat m_file_stat;
    struct iovec m_iv[2];//io向量机制iovec
    int m_iv_count;
    int cgi;        //是否启用的POST
    char *m_string; //存储请求头数据
    int bytes_to_send;//剩余发送字节数
    int bytes_have_send;//已发送字节数
    char *doc_root;

    map<string, string> m_users; //存储数据库的用户名和密码信息
    int m_TRIGMode;
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
