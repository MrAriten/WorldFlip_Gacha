#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>
#include <string>
//这里建议先直接往下拉到process函数，那个是http的核心
//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;
MYSQL_RES *res;


int generateRandomNumber(int min, int max) {//随机数生成，用于抽卡
    static bool initialized = false;
    if (!initialized) {
        srand(time(NULL));
        initialized = true;
    }
    int range = max - min + 1;
    return rand() % range + min;
}

void http_conn::initmysql_result(connection_pool *connPool)//初始化和数据库的连接，将数据库的用户名和密码传到users中
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);//利用RAII机制建立mysql到conpool的连接，从而可以自动释放

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);//将fd的属性进行修改
    return old_option;//保留旧设置
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;//创建一个epoll事件
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;//设置event属性
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);//将event注册到事件集中
    setnonblocking(fd);//将fd设置成非阻塞
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
//注册了EPOLLONESHOT事件的socket一旦被某个线程处理完毕，该线程就应该立即重置这个socket上的EPOLLONESHOT事件，
//以确保这个socket下一次可读时，其EPOLLIN事件能被触发，进而让其他工作线程有机会继续处理这个socket。
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);//将fd从事件集中移除
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);//将当前http对象的fd注册到事件集中
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    //memset是将某一块内存中的内容全部设置为指定的值， 这个函数通常为新申请的内存做初始化工作
    //这里是将各个缓冲区进行初始化
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
//关于主从状态机的解析，还是得看推文
//https://mp.weixin.qq.com/s/wAQHU-QZiRt1VACMZZjNlw

http_conn::LINE_STATUS http_conn::parse_line()//从状态机读取一行，分析是请求报文的哪一部分
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];//temp为将要分析的字节
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)//如果当前是\r字符，则有可能会读取到完整行
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')//下一个字符是\n，将\r\n改为\0\0
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n') //如果当前字符是\n，也有可能读取到完整行，一般是上次读取到\r就到buffer末尾了，没有接收完整，再次接收时会出现这种情况
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;//并没有找到\r\n，需要继续接收
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET读数据
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;

    std::string  username = m_url;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0')
    {
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;//初始化状态
    char *text = 0;

    /*
    NO_REQUEST 请求不完整，需要继续读取请求报文数据

    GET_REQUEST 获得了完整的HTTP请求

    BAD_REQUEST HTTP请求报文有语法错误

    INTERNAL_ERROR 服务器内部错误，该结果在主状态机逻辑switch的default下，一般不会触发
     */
    //注意这里while的两个条件
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        switch (m_check_state)//根据从状态选择下一步做什么
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            //解析完消息体即完成报文解析，避免再次进入循环，更新line_status
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

//下面的函数都是和返回HTTP请求有关的
http_conn::HTTP_CODE http_conn::do_request()//返回html文件到浏览器中
{
    strcpy(m_real_file, doc_root);//将初始化的m_real_file赋值为网站根目录
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');//找到m_url中/的位置

    //处理cgi，实现登录和注册校验
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    //如果请求资源为/0，表示跳转注册界面
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        //将网站目录和/register.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //如果请求资源为/1，表示跳转登录界面
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        //将网站目录和/log.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '8')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/kucun.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (strncmp(m_url,"/getRandomImage",5) == 0)
        return RANDOM_REQUEST;
    else if (strncmp(m_url,"/getMultiRandomImage",5) == 0)
        return RANDOM_MULTI_REQUEST;
    else if((strncmp(m_url,"/getUserInfo",5) == 0))
        return GET_USER_INFO;
    else
        //如果以上均不符合，即不是登录和注册，直接将url与网站目录拼接
        //这里的情况是welcome界面，请求服务器上的一个图片
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    //以只读方式获取文件描述符，通过mmap将该文件映射到内存中
    int fd = open(m_real_file, O_RDONLY);
    //mmap就是把html资源映射到了浏览器上
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);//避免文件描述符的浪费和占用
    return FILE_REQUEST;//表示请求文件存在，且可以访问
}
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
//服务器子线程调用process_write完成响应报文，随后注册epollout事件。服务器主线程检测写事件，并调用http_conn::write函数将响应报文发送给浏览器端。
bool http_conn::write()
{
    //在生成响应报文时初始化byte_to_send，包括头部信息和文件数据大小。
    //通过writev函数循环发送响应报文数据，根据返回值更新byte_have_send和iovec结构体的指针和长度，并判断响应报文整体是否发送成功。
    int temp = 0;

    if (bytes_to_send == 0)//若要发送的数据长度为0表示响应报文为空，一般不会出现这种情况
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);//将响应报文的状态行、消息头、空行和响应正文发送给浏览器端(iov结构集中写）
        //若writev单次发送成功，更新byte_to_send和byte_have_send的大小，若响应报文整体发送成功,则取消mmap映射,并判断是否是长连接.
        //长连接重置http类实例，注册读事件，不关闭连接，
        //短连接直接关闭连接

        //若writev单次发送不成功，判断是否是写缓冲区满了。
        //若不是因为缓冲区满了而失败，取消mmap映射，关闭连接
        //若eagain则满了，更新iovec结构体的指针和长度，并注册写事件，等待下一次写事件触发（当写缓冲区从不可写变为可写，触发epollout），
        //因此在此期间无法立即接收到同一用户的下一请求，但可以保证连接的完整性。

        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();//如果发送失败，但不是缓冲区问题，取消映射
            return false;
        }

        bytes_have_send += temp;//更新已发送字节
        bytes_to_send -= temp;//偏移文件iovec的指针
        if (bytes_have_send >= m_iv[0].iov_len)//第一个iovec头部信息的数据已发送完，发送第二个iovec数据
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else//继续发送第一个iovec头部信息的数据
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)//判断条件，数据已全部发送完
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);//在epoll树上重置EPOLLONESHOT事件

            if (m_linger)//浏览器的请求为长连接
            {
                init();//重新初始化HTTP对象
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE) //如果写入内容超出m_write_buf大小则报错
        return false;
    va_list arg_list;//定义可变参数列表
    va_start(arg_list, format);//将变量arg_list初始化为传入参数
    //将数据format从可变参数列表写入缓冲区写，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))//如果写入的数据长度超过缓冲区剩余空间，则报错
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;//更新m_write_idx位置
    va_end(arg_list);//清空可变参列表

    LOG_INFO("request:%s", m_write_buf);

    return true;
}
//添加状态行
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
//添加消息报头，具体的添加文本长度、连接状态和空行
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
//添加Content-Length，表示响应报文的长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
//添加文本类型，这里是html
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
//添加连接状态，通知浏览器端是保持连接还是关闭
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
//添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
//添加文本content
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}
bool http_conn::process_write(HTTP_CODE ret)//根据do_request的状态，调用process_write向m_write_buf中写入响应报文
{
    switch (ret)
    {
        case RANDOM_REQUEST:
        {

            add_status_line(200, ok_200_title);
            add_response("Content-Type:%s\r\n", "image/png");
            add_response("Connection:%s\r\n","close");
            add_blank_line();
            int random_num = rand() % 1000 + 1;
            std::string gacha_character;
            if(random_num<=50)
                gacha_character = "./characters/5/five_stars_"+ to_string(generateRandomNumber(1, 113))+".png\n./characters/five_stars"+".png\n",
                add_response("%s", gacha_character.c_str());
            else if(random_num<=250)
                gacha_character = "./characters/4/four_stars_"+ to_string(generateRandomNumber(1, 122))+".png\n./characters/four_stars"+".png\n",
                add_response("%s", gacha_character.c_str());
            else
                gacha_character = "./characters/3/three_stars_"+ to_string(generateRandomNumber(1, 80))+".png\n./characters/three_stars"+".png\n",
                add_response("%s", gacha_character.c_str());

            std::string  username = m_url;
            username = username.substr(16);
            connection_pool * m_connPool = connection_pool::GetInstance();
            MYSQL *mysql = NULL;
            connectionRAII mysqlcon(&mysql, m_connPool);//利用RAII机制建立mysql到conpool的连接，从而可以自动释放
            //在user表中检索username，passwd数据，浏览器端输入
            if ( mysql_query(mysql, ("INSERT INTO gacha_logs (username, gacha_character) VALUES ( '" +username+ "' , '"  +gacha_character+ "')").c_str() ))
            {
                LOG_ERROR("INSERT error:%s\n", mysql_error(mysql));
            }

            break;
        }
        case RANDOM_MULTI_REQUEST:
        {
            add_status_line(200, ok_200_title);
            add_response("Content-Type:%s\r\n", "image/png");
            add_response("Connection:%s\r\n","close");
            add_blank_line();
            for(int i = 0 ; i < 10 ; i++)
            {
                int random_num = rand() % 1000 + 1;
                std::string gacha_character;
                if (random_num <= 50)
                    gacha_character = "./characters/5/five_stars_" + to_string(generateRandomNumber(1, 113)) +
                                       ".png\n./characters/five_stars" + ".png\n",
                    add_response("%s", gacha_character.c_str());
                else if (random_num <= 250)
                    gacha_character = "./characters/4/four_stars_" + to_string(generateRandomNumber(1, 122)) +
                                      ".png\n./characters/four_stars" + ".png\n",
                    add_response("%s", gacha_character.c_str());
                else
                    gacha_character = "./characters/3/three_stars_" + to_string(generateRandomNumber(1, 80)) +
                                      ".png\n./characters/three_stars" + ".png\n",
                    add_response("%s", gacha_character.c_str());

                std::string  username = m_url;
                username = username.substr(21);
                connection_pool * m_connPool = connection_pool::GetInstance();
                MYSQL *mysql = NULL;
                connectionRAII mysqlcon(&mysql, m_connPool);//利用RAII机制建立mysql到conpool的连接，从而可以自动释放
                //在user表中检索username，passwd数据，浏览器端输入
                if ( mysql_query(mysql, ("INSERT INTO gacha_logs (username, gacha_character) VALUES ( '" +username+ "' , '"  +gacha_character+ "')").c_str() ))
                {
                    LOG_ERROR("INSERT error:%s\n", mysql_error(mysql));
                }
            }


            break;
        }
        case GET_USER_INFO:
        {
            add_status_line(200, ok_200_title);
            add_response("Content-Type:%s\r\n", "image/png");
            add_response("Connection:%s\r\n","close");
            add_blank_line();
            // 将查询结果拼接到字符串中
            std::string result;
            MYSQL_ROW row;
            int count = 0;
            if(res == NULL)
            {
                std::string  username = m_url;
                username = username.substr(13);
                if ( mysql_query(mysql, ("SELECT gacha_character FROM gacha_logs WHERE username='" + username + "' ORDER BY gacha_character DESC").c_str() ))
                {
                    LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
                }
                res = mysql_store_result(mysql);
            }
            while ((row = mysql_fetch_row(res)) != NULL && count < 10) {
                result += row[0]; // 将每一行的结果拼接到字符串中
                count++;
            }
            add_response("%s", result.c_str());
            if(row == NULL)
            {
                res = NULL;
                add_response("%s", "NULL\n");
                add_response("%s", "NULL\n");
            }

            break;
        }
        case INTERNAL_ERROR://内部错误，500
        {
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form))
                return false;
            break;
        }
        case BAD_REQUEST://报文语法有误，404
        {
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form))
                return false;
            break;
        }
        case FORBIDDEN_REQUEST://资源没有访问权限，403
        {
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form))
                return false;
            break;
        }
        //前三种都是报错机制，下面这个是文件请求，200
        case FILE_REQUEST:
        {
            //通过io向量机制iovec，声明两个iovec，第一个指向m_write_buf，第二个指向mmap的地址m_file_address
            //iovec是一个结构体，里面有两个元素，指针成员iov_base指向一个缓冲区，这个缓冲区是存放的是writev将要发送的数据
            //成员iov_len表示实际写入的长度
            add_status_line(200, ok_200_title);
            if (m_file_stat.st_size != 0)//如果请求的资源存在
            {
                add_headers(m_file_stat.st_size);
                m_iv[0].iov_base = m_write_buf;//第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = m_file_address;//第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            }
            else
            {
                const char *ok_string = "<html><body></body></html>";//如果请求的资源大小为0，则返回空白html文件
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                    return false;
            }
        }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;//除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();

    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
