#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

// 线程池类，定义成模板类是为了代码的复用，模板参数T是任务类
template <typename T>
class threadpool
{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    // 构造函数
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    // 析构函数
    ~threadpool();
    // 往线程池中添加任务的方法
    bool append(T *request, int state);
    bool append_p(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;         // 线程池中的线程数
    int m_max_requests;          // 请求队列中允许的最大请求数：请求队列中最多允许的，等待处理的请求数量
    pthread_t *m_threads;        // 描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue;  // 请求队列：装的是任务类
    locker m_queuelocker;        // 保护请求队列的互斥锁
    sem m_queuestat;             // 是否有任务需要处理：用一个信号量来判断
    connection_pool *m_connPool; // 数据库
    int m_actor_model;           // 模型切换，包括reactor模式和proactor模式
};
template <typename T> // 函数定义中冒号后面的内容是对前面变量的一个初始化
threadpool<T>::threadpool(int actor_model, connection_pool *connPool, int thread_number, int max_requests) : m_actor_model(actor_model), m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL), m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    // 创建数组，使用new动态创建，将来不要忘了销毁
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads) // 如果没有创建成功就抛出异常
        throw std::exception();
    // 创建thread_number个线程，并将他们设置为线程脱离，为什么设置线程脱离？我们不可能让父线程释放资源，等子线程用完了自己释放资源
    for (int i = 0; i < thread_number; ++i)
    {
        // worker是线程执行的函数，对这里不明白可以看一下pthread_create的参数都是什么意思
        if (pthread_create(m_threads + i, NULL, worker, this) != 0) // 如果不等于0就说明出错了，就释放数组抛出异常，如果成功就只执行if后面括号中的函数
        {
            delete[] m_threads; // 释放线程池数组
            throw std::exception();
        }
        // 到这里说明创建成功了，就要开始设置线程分离
        if (pthread_detach(m_threads[i])) // 如果不等于0说明出错了，就释放数组抛出异常，如果成功就只执行if后面括号中的函数
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}
template <typename T>
threadpool<T>::~threadpool() // 析构函数
{
    delete[] m_threads;
}
template <typename T>
bool threadpool<T>::append(T *request, int state) // 返回值是bool类型的
{
    m_queuelocker.lock();                     // 往线程池加入线程需要同步机制，先上锁，这里用到locker.h中的内容
    if (m_workqueue.size() >= m_max_requests) // 判断请求队列有没有超出最大数(注意区分请求队列和用以保护请求队列的互斥锁)
    {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
template <typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}
template <typename T>
void threadpool<T>::run()
{
    while (true)
    {
        m_queuestat.wait();
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue; // 跳过后面的语句继续while循环
        }
        T *request = m_workqueue.front(); // 获取请求队列的第一个任务
        m_workqueue.pop_front();          // 把任务pop掉
        m_queuelocker.unlock();           // 解锁任务队列
        if (!request)                     // 如果没有获取到任务就重新执行
            continue;
        // 下面是线程池中工作线程(worker)处理请求(request)的核心逻辑，主要实现了两种不同的处理模型：Reactor模式（m_actor_model为1）和Proactor模式（默认情况）
        if (1 == m_actor_model)        // Reactor模式，采用事件驱动，分为两个阶段处理
        {                              // 使用异步I/O，通过状态机（m_state）区分读写阶段
            if (0 == request->m_state) // 读取阶段（m_state == 0）
            {
                if (request->read_once())                                 // 尝试读取数据
                {                                                         // 读取成功
                    request->improv = 1;                                  // 表示需要改进活着继续处理
                    connectionRAII mysqlcon(&request->mysql, m_connPool); // 通过RALL方式从连接池获取MYSQL连接
                    request->process();                                   // 调用process处理请求
                }
                else // 读取失败
                {
                    request->improv = 1;
                    request->timer_flag = 1; // 可能表示需要关闭连接或者超时处理
                }
            }
            else // 写入阶段（m_state != 0）
            {
                if (request->write())
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        else // Proactor模式，更简单直接，但可能在高并发时性能不如Reactor
        {
            connectionRAII mysqlcon(&request->mysql, m_connPool); // 直接通过RALL方式获取MYSQL连接
            request->process();                                   // 调用process处理请求
        }
    }
}
#endif
