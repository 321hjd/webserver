#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"

using namespace std;

/*************************************************************
* 日志系统：记录服务器运行状态、错误信息和访问数据，可以实现按天分类，超行分类功能
* 写入模式：同步写入、异步写入（生产者-消费者模型，基于阻塞队列）
* 单例模式：一个server只需要一个日志系统，单例模式防止不必要的资源浪费
**************************************************************/

class Log
{
public:
    //唯一的公共静态方法作为实例获取入口
    //线程安全问题：C++11以后,使用局部变量懒汉不用加锁
    static Log *get_instance(){
        static Log instance;
        return &instance;
    }
    //子线程异步写日志的回调函数
    static void *flush_log_thread(void *args){
        Log::get_instance()->async_write_log();
    }
    //日志系统初始化。可选择的参数有日志文件、日志缓冲区大小、最大行数以及最长日志条队列
    bool init(const char *file_name, int close_log, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);
    //写日志
    //level表示不同的日志选项
    //format表示不同的日志格式
    //...表示可变参数列表
    void write_log(int level, const char *format, ...);
    //调试用的日志写入函数
    void flush(void);
private:
    //私有构造和析构，实现单例模式
    Log();
    virtual ~Log();
    //从阻塞队列中取出一个日志string，写入文件m_fp中
    void *async_write_log(){
        string single_log;
        while(m_log_queue->pop(single_log)){
            m_mutex.lock();
            //将一个字符串复制到一个文件流中
            fputs(single_log.c_str(), m_fp);
            m_mutex.unlock();
        }
    }
private:
    char dir_name[128]; //路径名
    char log_name[128]; //log文件名
    int m_split_lines;  //日志最大行数
    int m_log_buf_size; //日志缓冲区大小
    long long m_count;  //日志行数记录
    int m_today;        //因为按天分类,记录当前时间是那一天
    FILE *m_fp;         //log的文件指针
    char *m_buf;        //日志缓冲区，用于存放日志并加入阻塞队列
    block_queue<string> *m_log_queue; //阻塞队列
    bool m_is_async;    //是否同步标志位,false-同步，true-异步
    locker m_mutex;     //互斥锁
    int m_close_log;    //0-打开/1-关闭日志
};

#define LOG_DEBUG(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(0, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_INFO(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(1, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_WARN(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(2, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_ERROR(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(3, format, ##__VA_ARGS__); Log::get_instance()->flush();}

#endif