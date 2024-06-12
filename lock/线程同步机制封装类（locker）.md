# 线程同步机制封装类（locker）

> * 线程同步机制的目的：实现多线程同步，通过锁机制，确保任一时刻只能有一个线程能进入关键代码段
>
> * 三类线程同步机制：
>
>   * **信号量**。类似二进制，当信号量值为1，多个线程都有机会进入关键代码段，若线程A进入（执行了sem_wait），则将信号量-1，信号量减小为0，其它调用sem_wait的线程被阻塞，直到线程A完成操作，并执行sem_post将信号量+1，此后信号量非0，其余线程可以进入关键代码段
>
>   * **互斥锁**。当进入关键代码段，加锁；离开关键代码段时解锁，并唤醒等待该互斥锁的线程
>
>   * **条件变量**。提供“线程间通知机制”：当某个共享数据达到某个值的时候，唤醒等待这个共享数据的线程
>
> * 资源获取即初始化（Resource Acquisition is Initialization，RAII）：用类管理资源，将资源与对象的生命周期绑定，实现资源与状态的安全管理。

* 模块依赖

  ```c++
  #include <exception>	//异常处理
  #include <pthread.h>	//线程函数库
  #include <semaphore.h>	//信号量库
  ```

* 模块功能：对三种锁进行封装，将锁的创建和销毁函数封装在类的构造与析构函数中，实现RAII机制。

* 模块组成

  * `sem`类（信号量）

    ```C++
    class sem{
    public:
        sem();
        sem(int num);	//默认/有参构造，封装sem_init，初始化信号量
        ~sem();		    //析构，封装sem_destroy，销毁信号量
        bool wait();	//封装sem_wait，以原子操作方式将信号量的值减1，若信号量值为0，则sem_wait阻塞直到信号量非零
        bool post();	//封装sem_post，以原子操作的方式将信号量的值加1
    private:
        sem_t m_sem;	//信号量对象
    }
    ```

  * `locker`类（互斥锁）

    ```C++
    class locker{
    public:
        locker();		//默认构造，封装pthread_mutex_init，初始化互斥锁
        ~locker();		//析构，封装pthread_mutex_destroy，销毁互斥锁
        bool lock();	//封装pthread_mutex_lock，以原子操作方式加锁，若已被锁，则阻塞
        bool unlock();	//封装pthread_mutex_unlock，以原子操作的方式解锁
        pthread_mutex_t* get();	//获取互斥锁对象指针
    private:
        pthread_mutex_t m_mutex;	//互斥锁对象
    }
    ```

  * `cond`类（条件变量）

    ```C++
    class cond{
    public:
        cond();			//默认构造，封装pthread_cond_init，初始化条件变量
        ~cond();		//析构，封装pthread_cond_destroy，销毁条件变量
        bool wait(pthread_mutex_t *m_mutex);	//封装pthread_cond_wait，等待目标条件变量
        bool timewait(pthread_mutex_t *m_mutex, struct timespec t);//封装pthread_cond_timedwait，设置取消点函数，当目标线程推迟行动时，若调用此函数，则立即执行
        bool signal();	//封装pthread_cond_signal，用于唤醒一个等待目标条件变量的线程
        bool broadcast();//封装pthread_cond_broadcast，以广播方式唤醒所有等待目标条件变量的线程
    private:
        pthread_cond_t m_cond;;	//条件变量对象
    }
    ```
