#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h> //C++标准库输入输出
#include <list> //链表
#include <mysql/mysql.h> //mysql库的相关操作
#include <error.h> //errno.h里面定义了一个全局的错误码errno,每一个错误码对应一个错误,这样就可以查询到是因为什么出错了。
#include <string.h>
#include <iostream>
#include <string>
/*
#include<string.h>是C语言的标准库，主要是对字符串进行操作的库函数，是基于char*进行操作的，
 例如常见的字符串操作函数stpcpy、strcat都是在该头文件里面声明的。
#include<string>是C++语言的标准库，该库里面定义了string类，你可以包含这个头文件，
 然后定义一个字符串对象，对于字符串的操作就基于该对象进行，例如:string str;
 */
#include "../lock/locker.h"
#include "../log/log.h"

using namespace std;

class connection_pool
{
public:
	MYSQL *GetConnection();				 //获取数据库连接
	bool ReleaseConnection(MYSQL *conn); //释放连接
	int GetFreeConn();					 //获取连接
	void DestroyPool();					 //销毁所有连接

	//单例模式
	static connection_pool *GetInstance();//懒汉模式实例化

	void init(string url, string User, string PassWord, string DataBaseName, int Port, int MaxConn, int close_log); 

private:
	connection_pool();
	~connection_pool();

	int m_MaxConn;  //最大连接数
	int m_CurConn;  //当前已使用的连接数
	int m_FreeConn; //当前空闲的连接数
	locker lock;
	list<MYSQL *> connList; //连接池，用List实现
	sem reserve;

public:
	string m_url;			 //主机地址
	string m_Port;		 //数据库端口号
	string m_User;		 //登陆数据库用户名
	string m_PassWord;	 //登陆数据库密码
	string m_DatabaseName; //使用数据库名
	int m_close_log;	//日志开关
};

class connectionRAII{

public:
	connectionRAII(MYSQL **con, connection_pool *connPool);
	~connectionRAII();
	
private:
	MYSQL *conRAII;
	connection_pool *poolRAII;
};

#endif
