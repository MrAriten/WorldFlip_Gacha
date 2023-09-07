#ifndef LOCKER_H
#define LOCKER_H

#include <exception> //异常相关
#include <pthread.h> //线程相关
#include <semaphore.h> //信号量相关

//为什么这里需要把信号，信号量和锁封装到类呢？
//类中主要是Linux下三种锁进行封装，将锁的创建于销毁函数封装在类的构造与析构函数中，实现RAII机制
/*
RAII全称是“Resource Acquisition is Initialization”，直译过来是“资源获取即初始化”.
在构造函数中申请分配资源，在析构函数中释放资源。因为C++的语言机制保证了，
当一个对象创建的时候，自动调用构造函数，当对象超出作用域的时候会自动调用析构函数。
所以，在RAII的指导下，我们应该使用类来管理资源，将资源和对象的生命周期绑定
RAII的核心思想是将资源或者状态与对象的生命周期绑定，通过C++的语言机制，实现资源和状态的安全管理,智能指针是RAII最好的例子
 */
class sem //信号相关
{
public:
    sem()
    {
        if (sem_init(&m_sem, 0, 0) != 0)
        {
            throw std::exception();
        }
    }
    sem(int num)
    {
        if (sem_init(&m_sem, 0, num) != 0)
        {
            throw std::exception();
        }
    }
    ~sem()
    {
        sem_destroy(&m_sem);
    }
    bool wait()//阻塞等待信号
    {
        return sem_wait(&m_sem) == 0;
    }
    bool post()//向外广播信号？不能说是广播，只是将信号+1，和条件变量的在线程间广播大有不同
    {
        return sem_post(&m_sem) == 0;//sem_post会自动给信号量+1
    }

private:
    sem_t m_sem;
};
class locker//锁的封装，互斥锁的作用是保证任何时刻只有一个线程访问锁上的代码
{
public:
    locker()
    {
        if (pthread_mutex_init(&m_mutex, NULL) != 0)
        {
            throw std::exception();
        }
    }
    ~locker()
    {
        pthread_mutex_destroy(&m_mutex);
    }
    bool lock()
    {
        return pthread_mutex_lock(&m_mutex) == 0;
    }
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }
    pthread_mutex_t *get()
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
};
class cond//条件变量封装
{
public:
    cond()
    {
        if (pthread_cond_init(&m_cond, NULL) != 0)
        {
            //pthread_mutex_destroy(&m_mutex);//这里打注释是因为将private的锁给删除了，而直接传入参数锁
            throw std::exception();
        }
    }
    ~cond()
    {
        pthread_cond_destroy(&m_cond);
    }
    bool wait(pthread_mutex_t *m_mutex)//这里直接传入锁而无需设置一个私有变量
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_wait(&m_cond, m_mutex);
        /*
         * pthread_cond_timedwait用于阻塞当前线程，等待别的线程使用pthread_cond_signal()或pthread_cond_broadcast来唤醒它
         * pthread_cond_wait() 必须与pthread_mutex配套使用。pthread_cond_wait()函数一进入wait状态就会自动release mutex。
         * 当其他线程通过pthread_cond_signal()或pthread_cond_broadcast，把该线程唤醒，使pthread_cond_wait()通过（返回）时，
         * 该线程又自动获得该mutex。
         */
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t)//加入了超时检测的wait
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool signal()//发送一个信号给另外一个正在处于阻塞等待状态的线程,使其脱离阻塞状态,继续执行.
    {
        return pthread_cond_signal(&m_cond) == 0;
    }
    bool broadcast()//向外广播信号量
    {
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    //static pthread_mutex_t m_mutex;
    pthread_cond_t m_cond;
};
#endif
/*信号量和条件变量的区别
 *条件变量

　　条件变量使在多线程程序中用来实现“等待--->唤醒”逻辑常用的方法，是进程间同步的一种机制。条件变量用来阻塞一个线程，直到条件满足被触发为止，通常情况下条件变量和互斥量同时使用。

　　一般条件变量有两个状态：

　　①一个/多个线程为等待"条件变量的条件成立"而挂起;

　　②另一个线程在"条件变量条件成立时"通知其他线程。

 *信号量

　　信号量是一种特殊的变量，访问具有原子性。

　　只允许对它进行两个操作：

　　①等待信号量：当信号量值为0时，程序等待;当信号量值大于0时，信号量减1，程序继续运行。

　　②发送信号量：将信号量值加1。

　　说明：Linux提供了一组信号量API，声明在头文件sys/sem.h中。

 Linux条件变量和信号量的区别：

　　①使用条件变量可以一次唤醒所有等待者，而这个信号量没有的功能，感觉是最大区别。

　　②信号量始终有一个值，而条件变量是没有的，没有地方记录唤醒过多少次，也没有地方记录唤醒线程过多少次。
 从实现上来说一个信号量可以欧尼顾mutex+counter+condition variable实现的。因为信号量有一个状态，如果想精准的同步，那么信号量可能会有特殊的地方。
 信号量可以解决条件变量中存在的唤醒丢失问题。

　　③信号量的意图在于进程间同步，互斥锁和条件变量的意图在于线程间同步，但是信号量也可用于线程间，
 互斥锁和条件变量也可用于进程间。应当根据实际的情况进行决定。信号量最有用的场景是用以指明可用资源的数量。*/