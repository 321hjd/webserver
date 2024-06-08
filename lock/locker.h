#ifndef LOCKER_H
#define LOCKER_H

#include<exception>
#include<pthread.h>
#include<semaphore.h>

/*
功能：对多线程编程中的三类线程同步机制（信号量、互斥锁、条件变量）进行封装，将锁的创建和销毁函数封装在类的构造与析构函数中，实现RAII机制。
*/


//信号量 ---- 工作线程等待业务请求时使用到
class sem{
public:
    //默认/有参构造，封装sem_init，初始化信号量
    sem(){
        /*int sem_init (sem_t *__sem, int __pshared, unsigned int __value)
            __sem：信号量指针（地址）
            __pshared：信号量类型。如果其值为0，就表示这个信号量是当前进程的局部信号量，否则该信号量就可以在多个进程之间共享
            __value：信号量初始值
            返回值：成功则返回0，失败返回-1并设置errno
        */
        if(sem_init(&m_sem, 0, 0) != 0){
            //构造函数没有返回值，可以通过抛出异常来报告错误
            throw std::exception();
        }
    }
    sem(int num){
        if(sem_init(&m_sem, 0, num) != 0){
            throw std::exception();
        }
    }
    //析构，封装sem_destroy，销毁信号量
    ~sem(){
        sem_destroy(&m_sem);
    }
    //封装sem_wait，以原子操作方式将信号量的值减1，若信号量值为0，则sem_wait阻塞直到信号量非零
    //返回true代表成功调用sem_wait	    
    bool wait(){
        return sem_wait(&m_sem) == 0;
    }
    //封装sem_post，以原子操作的方式将信号量的值加1
    bool post(){
        return sem_post(&m_sem) == 0;
    }

private:
    sem_t m_sem;	//信号量对象
};

//互斥锁
class locker{
public:
    //默认构造，封装pthread_mutex_init，初始化互斥锁
    locker(){
        /*int pthread_mutex_init (pthread_mutex_t *__mutex,
			       const pthread_mutexattr_t *__mutexattr)
            __mutex：目标互斥锁指针
            __mutexattr：互斥锁属性，NULL-默认属性，其它属性（如跨进程共享、同进程线程共享等）
        */
        if(pthread_mutex_init(&m_mutex, NULL) != 0){
            throw std::exception();
        }
    }	
    //析构，封装pthread_mutex_destroy，销毁互斥锁
    ~locker(){
        pthread_mutex_destroy(&m_mutex);
    }	
    //封装pthread_mutex_lock，以原子操作方式加锁，若已被锁，则阻塞	
    bool lock(){
        return pthread_mutex_lock(&m_mutex) == 0;
    }
    //封装pthread_mutex_unlock，以原子操作的方式解锁
    bool unlock(){
        return pthread_mutex_unlock(&m_mutex) == 0;
    }
    //获取互斥锁对象指针
    pthread_mutex_t* get(){
        return &m_mutex;
    }
private:
    pthread_mutex_t m_mutex;	//互斥锁对象
};

//条件变量 ---- 阻塞队列中使用到
class cond{
public:
    //默认构造，封装pthread_cond_init，初始化条件变量
    cond(){
        //arg1: 目标条件变量指针
        //arg2：条件变量属性，NULL-默认属性
        if(pthread_cond_init(&m_cond, NULL) != 0){
            throw std::exception();
        }
    }		
    //析构，封装pthread_cond_destroy，销毁条件变量
    ~cond(){
        pthread_cond_destroy(&m_cond);
    }
    //封装pthread_cond_wait，等待目标条件变量
    //条件变量的使用机制需要配合互斥锁使用：其加锁解锁封装在了外部，因此无需在此处加解锁
    bool wait(pthread_mutex_t *m_mutex){
        int ret = 0;
        // pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_wait(&m_cond, m_mutex);
        // pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    //封装pthread_cond_timedwait，在超时时间t内等待目标条件变量
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t){
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    //封装pthread_cond_signal，用于唤醒一个等待目标条件变量的线程
    bool signal(){
        return pthread_cond_signal(&m_cond) == 0;
    }
    //封装pthread_cond_broadcast，以广播方式唤醒所有等待目标条件变量的线程
    bool broadcast(){
        return pthread_cond_broadcast(&m_cond) == 0;
    }
private:
    //static pthread_mutex_t m_mutex;   //原来使用的是作为成员变量的互斥锁
    pthread_cond_t m_cond;;	            //条件变量对象
};

#endif
