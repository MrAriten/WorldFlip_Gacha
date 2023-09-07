#include "lst_timer.h"
#include "../http/http_conn.h"

sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}
sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

void sort_timer_lst::add_timer(util_timer *timer)//添加定时器，内部调用私有成员add_timer
{
    if (!timer)
    {
        return;
    }
    if (!head)
    {
        head = tail = timer;
        return;
    }
    if (timer->expire < head->expire)//如果新的定时器超时时间小于当前头部结点,直接将当前定时器结点作为头部结点
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    add_timer(timer, head);//否则调用私有成员，调整内部结点
}
void sort_timer_lst::adjust_timer(util_timer *timer)//调整定时器，任务发生变化时，调整定时器在链表中的位置
{
    if (!timer)
    {
        return;
    }
    util_timer *tmp = timer->next;
    if (!tmp || (timer->expire < tmp->expire))//被调整的定时器在链表尾部，定时器超时值仍然小于下一个定时器超时值，不调整
    {
        return;
    }
    if (timer == head)//被调整定时器是链表头结点，将定时器取出，重新插入
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    }
    else//被调整定时器在内部，将定时器取出，重新插入
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}
void sort_timer_lst::del_timer(util_timer *timer)//删除定时器
{
    if (!timer)
    {
        return;
    }
    if ((timer == head) && (timer == tail))//链表中只有一个定时器，需要删除该定时器
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    if (timer == head)//被删除的定时器为头结点
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    if (timer == tail)//被删除的定时器为尾结点
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    timer->prev->next = timer->next;//被删除的定时器在链表内部，常规链表结点删除
    timer->next->prev = timer->prev;
    delete timer;
}
void sort_timer_lst::tick()//定时任务处理函数
{
    if (!head)
    {
        return;
    }
    //获取当前时间
    time_t cur = time(NULL);
    util_timer *tmp = head;
    while (tmp)//遍历定时器链表
    {
        if (cur < tmp->expire)//链表容器为升序排列，当前时间小于定时器的超时时间，后面的定时器也没有到期
        {
            break;
        }
        tmp->cb_func(tmp->user_data);//当前定时器到期，则调用回调函数，执行定时事件，回调函数定义在最下面！
        head = tmp->next;//将处理后的定时器从链表容器中删除，并重置头结点
        if (head)
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}

void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)//私有成员，被公有成员add_timer和adjust_time调用，主要用于调整链表内部结点
{
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;
    while (tmp)//遍历当前结点之后的链表，按照超时时间找到目标定时器对应的位置，常规双向链表插入操作
    {
        if (timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    if (!tmp)//遍历完发现，目标定时器需要放到尾结点处
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}

void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//信号处理函数
void Utils::sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    //可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char *)&msg, 1, 0);//将信号值从管道写端写入，传输字符类型，而非整型
    errno = save_errno;
}

//设置信号函数
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;//信号处理函数中仅仅发送信号值，不做对应逻辑处理
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);//用来将信号集初始化，然后把所有的信号加入到此信号集里。
    assert(sigaction(sig, &sa, NULL) != -1);
    //int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
    //signum表示操作的信号。
    //act表示对信号设置新的处理方式。
    //oldact表示信号原来的处理方式。
    //返回值，0 表示成功，-1 表示有错误发生。
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;

void cb_func(client_data *user_data)
{
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);//删除非活动连接在socket上的注册事件
    assert(user_data);
    close(user_data->sockfd);//关闭文件描述符
    http_conn::m_user_count--;//减少连接数
}
