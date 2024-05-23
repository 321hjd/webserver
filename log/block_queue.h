#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include<iostream>
#include<stdlib.h>
#include<pthread.h>
#include<sys/time.h>
#include"../lock/locker.h"
using namespace std;

/*************************************************************
* 阻塞队列：循环数组实现，m_back = (m_back + 1) % m_max_size;  
* 线程安全：对队列的每个操作——都要先加互斥锁，操作完后，再解锁
* 类内实现：由于block_queue的成员函数基本都比较简短，因此在类内实现（会被编译器默认声明为内联函数，减小调用开销）
**************************************************************/

/*问题：
1.为什么阻塞队列不用单例模式？日志系统只能有一个，阻塞队列对应应该也只能有一个才对吧？

2.为什么返回队列大小也要加锁？而且一定要用一个临时变量赋值返回？
    1）因为如果不加锁，查询大小时的队列大小是不确定的，因为随时可能有别的线程对队列进行了修改
    2）如果不用临时变量tmp存储，解锁后直接返回m_size，情况和第一条类似，可能被修改

3.若当前队列为满，则往队列添加元素的线程会被挂起（阻塞），问题是：在何处阻塞？应该不是push()成员函数吧？还是在调用push()的函数内阻塞？

4.为什么pop()时判断队列是否为空用while而非if？
    1）本质：因为有多个消费者（多个写日志线程）竞争资源，wait()成功后，资源可能已经被别的线程使用了
    2）有可能多个线程都在等待这个资源可用的信号，信号发出后只有一个资源可用，但是有A，B两个线程都在等待，B比较速度快，获得互斥锁，然后加锁，消耗资源，然后解锁，之后A获得互斥锁，但A在wait()返回后发现资源已经被使用了，它便有两个选择，一个是去访问不存在的资源，另一个就是继续等待，那么继续等待下去的条件就是使用while，如果使用if，会顺序执行，并访问不存在的资源，导致错误


*/

template<class T>
class block_queue{
public:
    //有参构造函数，初始化阻塞队列，队列大小默认为1000
    block_queue(int max_size = 1000){
        if(max_size <= 0){
            exit(-1);
        }
        m_max_size = max_size;
        m_array = new T[max_size];
        m_size = 0;
        m_front = -1;
        m_back = -1;
    }
    //清空队列
    void clear(){
        m_mutex.lock();     //加锁
        m_size = 0;
        m_front = -1;
        m_back = -1;
        m_mutex.unlock();   //解锁
    }
    //析构函数
    ~block_queue(){
        m_mutex.lock();
        if(m_array != NULL){
            delete [] m_array;
        }
        m_mutex.unlock();
    }
    //判断队列是否已满
    bool full(){
        m_mutex.lock();
        //小技巧：使用 “>=” 而非 “==”
        if(m_size >= m_max_size){
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }
    //判断队列是否为空
    bool empty(){
        m_mutex.lock();
        //小技巧：将变量写在后面，防止写错为 "x=0" 导致debug困难
        if(0 == m_size){
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }
    //返回队首元素（将队首元素赋值给value）
    bool front(T &value){
        m_mutex.lock();
        if(0 == m_size){
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_front];
        m_mutex.unlock();
        return true;
    }
    //返回队尾元素
    bool back(T &value){
        m_mutex.lock();
        if(0 == m_size){
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_back];
        m_mutex.unlock();
        return true;
    }
    //返回队列大小
    int size(){
        int tmp = 0;
        m_mutex.lock();
        tmp = m_size;
        m_mutex.unlock();
        return tmp;
    }
    //返回队列最大大小
    int max_size(){
        int tmp = 0;
        m_mutex.lock();
        tmp = m_max_size;
        m_mutex.unlock();
        return tmp;
    }
    //往队列添加元素(item是待写入日志)
    //1.添加元素后，需要唤醒所有使用队列的线程（广播）；若当前没有线程等待条件变量,则唤醒无意义
    //2.当有元素push进队列,相当于生产者生产了一个元素
    //3.若当前队列为满，则往队列添加元素的线程会被挂起（阻塞）
    bool push(const T &item){
        m_mutex.lock();
        if(m_size >= m_max_size){
            m_cond.broadcast(); 
            m_mutex.unlock();
            return false;
        }

        m_back = (m_back + 1) % m_max_size; //循环数组
        m_array[m_back] = item;
        m_size++;
        m_cond.broadcast();
        m_mutex.unlock();
        return true;
    }
    //从队列弹出元素
    //若当前队列为空，则从队列获取元素的线程会挂起（等待条件变量，阻塞）
    bool pop(T &item){
        m_mutex.lock();
        //注意是while而非if判断
        while(m_size <= 0){
            if(!m_cond.wait(m_mutex.get())){
                m_mutex.unlock();
                return false;
            }
        }
        //注意基于循环队列的实现，即便是弹出，也不需要做删除处理，只需要移动m_front指针即可
        m_front = (m_front + 1) % m_max_size;   
        item = m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }
    //从队列弹出元素，并设置超时处理
    //1.不再使用while持续等待竞争资源，而是使用超时时间
    //2.若超时时间内获取timewait()成功返回，但m_size仍不大于0，说明资源已被其它线程使用，则立即返回并阻塞
    bool pop(T &item, int ms_timeout){
        struct timespec t = {0, 0};     //{s, ns}
        struct timeval now = {0, 0};
        gettimeofday(&now, NULL);
        m_mutex.lock();
        if(m_size <= 0){
            t.tv_sec = now.tv_sec + ms_timeout / 1000;
            t.tv_nsec = (ms_timeout % 1000) * 1000;
            if(!m_cond.timewait(m_mutex.get(), t)){
                m_mutex.unlock();
                return false;
            }
        }
        
        if(m_size <= 0){
            m_mutex.unlock();
            return false;
        }

        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }
private:
    locker m_mutex;	//互斥锁
    cond m_cond;	//条件变量，用于唤醒工作线程
    T *m_array;		//基于循环数组的队列，每个元素即为待写日志内容
    int m_size;     //队列元素数量
    int m_max_size; //队列最大容量
    int m_front;    //队头指针，指向队头的前一个元素，(m_front + 1) % m_max_size才指向真正的队头元素，m_front本身指向元素是已经取出的元素
    int m_back;     //队尾指针，指向队尾
};

#endif