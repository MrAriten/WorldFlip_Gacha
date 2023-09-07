#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h> //包括bind,accept,listen等socket编程函数以及接口数据结构
#include <netinet/in.h> //网络大小端转换的API
#include <arpa/inet.h> //提供IP地址转换函数
#include <stdio.h> //c语言编译环境下的可以调用的标准输入输出函数
#include <unistd.h> //提供对 POSIX 操作系统 API 的访问功能
#include <errno.h> //定义了整数变量 errno，它是通过系统调用设置的，在错误事件中的某些库函数表明了什么发生了错误。
#include <fcntl.h> //提供对文件控制的函数
#include <stdlib.h> //某些结构体定义和宏定义，如EXIT_FAILURE、EXIT_SUCCESS等
#include <cassert> //cassert是对assert.h头文件的封装，里面定义了一个assert函数，可以用于异常判断
#include <sys/epoll.h> //和epoll相关的函数

#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"

const int MAX_FD = 65536;           //最大文件描述符
const int MAX_EVENT_NUMBER = 10000; //最大事件数
const int TIMESLOT = 5;             //最小超时单位

class WebServer
{
public:
    WebServer();
    ~WebServer();

    void init(int port , string user, string passWord, string databaseName,
              int log_write , int opt_linger, int trigmode, int sql_num,
              int thread_num, int close_log, int actor_model);

    void thread_pool();
    void sql_pool();
    void log_write();
    void trig_mode();
    void eventListen();
    void eventLoop();
    void timer(int connfd, struct sockaddr_in client_address);
    void adjust_timer(util_timer *timer);
    void deal_timer(util_timer *timer, int sockfd);
    bool dealclinetdata();
    bool dealwithsignal(bool& timeout, bool& stop_server);
    void dealwithread(int sockfd);
    void dealwithwrite(int sockfd);

public:
    //基础
    int m_port;
    char *m_root;
    int m_log_write;
    int m_close_log;
    int m_actormodel;

    int m_pipefd[2];
    int m_epollfd;
    http_conn *users;

    //数据库相关
    connection_pool *m_connPool;
    string m_user;         //登陆数据库用户名
    string m_passWord;     //登陆数据库密码
    string m_databaseName; //使用数据库名
    int m_sql_num;

    //线程池相关
    threadpool<http_conn> *m_pool;
    int m_thread_num;

    //epoll_event相关
    epoll_event events[MAX_EVENT_NUMBER];

    int m_listenfd;
    int m_OPT_LINGER;
    int m_TRIGMode;
    int m_LISTENTrigmode;
    int m_CONNTrigmode;

    //定时器相关
    client_data *users_timer;
    Utils utils;
};
#endif
