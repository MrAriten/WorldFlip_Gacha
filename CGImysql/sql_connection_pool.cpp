#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

connection_pool::connection_pool()
{
	m_CurConn = 0;//已连接数
	m_FreeConn = 0;//空闲连接数
}

connection_pool *connection_pool::GetInstance()
{
	static connection_pool connPool;
	return &connPool;
}

//构造初始化
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn, int close_log)
{
    //登陆数据库要用的字段
	m_url = url;
	m_Port = Port;
	m_User = User;
	m_PassWord = PassWord;
	m_DatabaseName = DBName;
	m_close_log = close_log;

	for (int i = 0; i < MaxConn; i++)//循环创建连接
	{
		MYSQL *con = NULL;
		con = mysql_init(con);

		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}
		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);

		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}
		connList.push_back(con);//将创建的连接放入List中
		++m_FreeConn;
	}

	reserve = sem(m_FreeConn);//将reserve信号量设置成和FreeConn一样的值

	m_MaxConn = m_FreeConn;
}


//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
	MYSQL *con = NULL;

	if (0 == connList.size())//如果没有连接则返回
		return NULL;

	reserve.wait();//等待信号值，reserve>0时往下执行，=0时则阻塞等待
	
	lock.lock();//上锁来保护list，从而避免了冲突

	con = connList.front();//提取出List的前端元素
	connList.pop_front();//从List中解放con

	--m_FreeConn;
	++m_CurConn;

	lock.unlock();
	return con;
}

//释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *con)
{
	if (NULL == con)
		return false;

	lock.lock();//上锁保护List

	connList.push_back(con); //将con放回到List中
	++m_FreeConn;
	--m_CurConn;

	lock.unlock();

	reserve.post(); //向外广播，当前有空闲连接，sem_post会自动给信号量+1
	return true;
}

//销毁数据库连接池
void connection_pool::DestroyPool()
{

	lock.lock();//锁保护List
	if (connList.size() > 0)
	{
		list<MYSQL *>::iterator it;//创建迭代器
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			MYSQL *con = *it;
			mysql_close(con);//不断释放连接
		}
		m_CurConn = 0;
		m_FreeConn = 0;
		connList.clear();
	}

	lock.unlock();
}

//当前空闲的连接数
int connection_pool::GetFreeConn()
{
	return this->m_FreeConn;
}

connection_pool::~connection_pool()
{
	DestroyPool();
}

connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool){//RAII机制，将sql和conpool自动连接并能自动释放
	*SQL = connPool->GetConnection();
	
	conRAII = *SQL;
	poolRAII = connPool;
}

connectionRAII::~connectionRAII(){
	poolRAII->ReleaseConnection(conRAII);
}