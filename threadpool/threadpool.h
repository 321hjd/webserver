#ifndef THREADPOOL_H
#define THREADPOOL_H

#include<list>
#include<cstdio>
#include<exception>
#include<pthread.h>
#include"../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"


/*问题
1.为什么线程池不是单例模式？理论上来说不应该是和日志系统一样，一个webserver对应一个线程池？
    回答：
	1）因为这里的线程池是设计为模板类，可能用于http、ftp等各种应用层服务，也就需要实例化多个对应的服务对象。而单例模式的设计初衷是只能实例化一个对象，目的矛盾，因此没有采用单例模式。
	2）如果有多个需要使用单例模式的类，可以先写一个单例模式模板类（当前情况），然后用其它类如MyObj继承此模板类并指定模板参数类型T为MyObj，并声明构造/析构为private，同时将模板类声明为友元类。

2.为什么线程处理函数worker是静态函数？
    因为pthread_create要求输入的函数指针的参数是void*，若类成员worker不是静态成员，则会隐含传入this指针，和void*不匹配，导致错误

3.为什么必须要用worker内部调用run，而不是直接将run作为线程调用函数？


4.为什么要在同一个头文件中完成类的声明和实现？
    模板类通常需要在编译时对每个使用的类型实例化，这意味着模板定义需要对编译器可见。因此，模板的实现通常放在头文件中，而不是像非模板类那样分离头文件和源文件。主要是为了简化构建过程并避免与编译器相关的问题。
*/

template<typename T>
class threadpool{
public:
    /*
    * actor_model：触发模式
    * connPool：数据库连接池对象
    * thread_number：线程池中线程的数量
    * max_requests：请求队列中最多允许的、等待处理的请求的数量
    * */
    threadpool(int actor_model, connection_pool *connPool, int thread_num = 8, int max_request = 10000);
    ~threadpool();
    /*
    * 功能：向请求队列中添加任务对象
    * request：任务对象（本项目中是http，但设计为模板类，所以也可以扩展为其它如ftp等
    * state：任务状态，本项目中为http请求的模式，0-读，1-写
    */
    bool append(T *request, int state);
    bool append_p(T *request);

private:
    //工作线程运行函数，内部访问私有函数run，完成线程处理要求
    static void *worker(void *arg);
    //执行业务逻辑处理
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列允许的最大请求数（最大并发数量）
    pthread_t *m_threads;       //线程池数组，大小为m_thread_number
    std::list<T *> m_workqueue; //请求队列
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //信号量，通知工作线程是否有任务需要处理
    connection_pool *m_connPool;//数据库连接池
    int m_actor_model;          //反应堆模式切换，0-Proactor，1-Reactor
};

//模板类实现

//构造
template<typename T>
threadpool<T>::threadpool(int actor_model, connection_pool *connPool, int thread_number, int max_requests) : m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool){
    //检验数据有效性
    if(thread_number <= 0 || max_requests <= 0){
        throw std::exception();
    }
    //线程池初始化
    m_threads = new pthread_t[m_thread_number];
    if(!m_threads){
        throw std::exception();
    }
    for(int i = 0; i < thread_number; ++i){
        //循环创建线程，并将工作线程按要求运行
        if(pthread_create(m_threads + i, NULL, worker, this) != 0){
            delete [] m_threads;
            throw std::exception();
        }
        //将线程分离，从而不用单独回收工作线程(其它线程对其进行join)，程序结束后会自动释放资源
        //返回非零值代表错误
        if(pthread_detach(m_threads[i])){
            delete [] m_threads;
            throw std::exception();
        }
    }
}

//析构
template<typename T>
threadpool<T>::~threadpool(){
    delete [] m_threads;
}

//向请求队列添加任务对象
template<typename T>
bool threadpool<T>::append(T *request, int state){
    m_queuelocker.lock();   //加锁保护
    if(m_workqueue.size() >= m_max_requests){
        m_queuelocker.unlock();
        return false;
    }
    //添加任务并设置状态
    request->m_state = state;
    m_workqueue.push_back(request);
    m_queuelocker.unlock(); //解锁
    //信号量提醒有任务需要处理
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

//线程工作函数
template<typename T>
void *threadpool<T>::worker(void *arg){
    //将参数强转为线程池类，调用成员方法
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}

//run执行任务：工作线程从请求队列中取出某个任务进行处理
template<typename T>
void threadpool<T>::run(){
    //各线程一直处于运行状态，只是可能因为没有任务处理而阻塞
    while(true){
        //等待信号量提示
        m_queuestat.wait();
        //被唤醒后先加锁抢占资源
        m_queuelocker.lock();
        //若没有抢到资源，则解锁并继续等待
        if(m_workqueue.empty()){
            m_queuelocker.unlock();
            continue;
        }

        //取出任务对象，并解锁释放队列资源
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if(!request){
            continue;
        }

        //根据不同的事件处理模式进行操作
        if(1 == m_actor_model){     //Reactor
            //0-读
            if(0 == request->m_state){
                if(request->read_once()){
                    request->improv = 1;
                    //从连接池中取出一个数据库连接
                    //这里做了RAII优化，对象销毁时自动调用析构函数并将sql连接放回连接池
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    //process(模板类中的方法,这里是http类)进行处理
                    request->process();
                }
                else{
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
            //1-写
            else{
                if(request->write()){
                    request->improv = 1;
                }
                else{
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        else{                       //Proactor
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }
    }
}

#endif
