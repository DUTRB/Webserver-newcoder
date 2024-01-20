#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include "locker.h"
#include <list>
#include <cstdio>
#include <exception>

//线程池类  定义为模板类是为了代码的复用  模板参数T是任务类
template<typename T>
class threadpool{
public:
    //初始化线程数量为8 请求数量10000个
    threadpool(int thread_number = 8, int m_max_requests = 10000);
    ~threadpool();
    //添加任务的方法
    bool append(T* request);

private:
    static void* worker(void * arg);
    void run();

private:
    
    int m_thread_number;        //线程数量
    
    pthread_t * m_threads;      //线程池数组，大小为m_thread_number
    
    int m_max_requests;         //请求队列中最多允许的，等待处理的请求数量
    
    std::list< T*> m_workqueue; //请求队列
    
    locker m_queuelocker;       //互斥锁
    
    sem m_queuestat;            //信号量 用来判断是否有任务需要处理
    
    bool m_stop;                //是否结束线程

};

//构造函数
template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests):
    m_thread_number(thread_number), m_max_requests(max_requests),
    m_stop(false), m_threads(NULL){ //初始化成员变量
    
    if((thread_number <= 0) || (max_requests <= 0)){
        throw std::exception();
    }
    //创建线程池数组
    m_threads = new pthread_t[m_thread_number];
    if(!m_threads){
        throw std::exception();
    }
    //创建thread_number个线程，并将他们设置为线程脱离
    for(int i = 0; i < thread_number; i++){
        printf("create the %dth thread\n", i);
        //创建线程
        if(pthread_create(m_threads + i, NULL, worker, this) != 0){
            delete [] m_threads;
            throw std::exception();
        }

        if(pthread_detach(m_threads[i])){
            delete [] m_threads;
            throw std::exception();
        }
    }
}

template<typename T>
threadpool<T>::~threadpool(){
    delete [] m_threads;
    m_stop = true;
}

//往队列中添加任务
template<typename T>
bool threadpool<T>::append(T * request){
    m_queuelocker.lock();
    if(m_workqueue.size() > m_max_requests){
        m_queuelocker.unlock();
        return false;
    }

    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
//创建线程时需要执行的代码
template<typename T>
void * threadpool<T>::worker(void * arg){
    threadpool * pool = (threadpool *)arg;
    pool->run();
    return pool;
}

//线程池运行起来的函数
template<typename T>
void threadpool<T>::run(){
    while(!m_stop){
        m_queuestat.wait();
        m_queuelocker.lock();
        if(m_workqueue.empty()){
            m_queuelocker.unlock();
            continue;
        }

        T * request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();

        if(!request){
            continue;
        }
        //线程执行任务的处理函数
        request->process();
    }
}

#endif