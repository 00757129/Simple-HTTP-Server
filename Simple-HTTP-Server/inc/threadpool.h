#ifndef __THREAD_POOL_H_
#define __THREAD_POOL_H_

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>

#include "locker.h"

template<typename T>
class threadpool
{
public:
     threadpool(int thread_number = 8, int max_requests = 1000);
     ~threadpool();
     bool append(T* request);
private:
     static void* worker(void* arg);
     void run();

private:
     int m_thread_number; // 執行緒池中執行緒數量
     int m_max_requests; // 請求佇列中允許的最大請求數
     pthread_t* m_threads; // 執行緒池數組
     std::list<T*> m_workqueu; // 請求佇列
     locker m_queuelocker; // 保護請求佇列互斥鎖
     sem m_queuestat; // 是否有任務需要處理
     bool m_stop; // 是否結束線程
};

/*
     執行緒池建構函式：傳入執行緒數量和最大請求數量
*/
template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests) : m_thread_number(thread_number),
     m_max_requests(max_requests), m_stop(false), m_threads(NULL)
{
     if((thread_number <= 0) || (max_requests <= 0)){
         throw std::exception();
     }
     // 建立執行緒數組
     m_threads = new pthread_t[m_thread_number];
     if(!m_threads)
     {
         throw std::exception();
     }
     // 建立執行緒 並設定位元分離狀態
     for(int i = 0; i < thread_number; ++i){
         printf("create the %d thread\n", i);
         if(pthread_create(m_threads+i, NULL, worker, this) != 0){
             delete [] m_threads;
             throw std::exception();
         }
         if(pthread_detach(m_threads[i])){
             delete [] m_threads;
             throw std::exception();
         }
     }
}

/*
     析構函數：釋放資源
*/
template<class T>
threadpool<T>::~threadpool()
{
     delete [] m_threads;
     m_stop = true;
}

/*
     向工作隊列追加新的請求，同時透過信號量通知對方
*/
template<class T>
bool threadpool<T>::append(T* requset){
     m_queuelocker.lock();
     if(m_workqueu.size() > m_max_requests)
     {
         m_queuelocker.unlock();
         return false;
     }
     m_workqueu.push_back(requset);
     m_queuelocker.unlock();
     m_queuestat.post();
     return true;
}

template<class T>
void* threadpool<T>::worker(void* arg)
{
     threadpool* pool = (threadpool*)arg;
     pool->run();
     return pool;
}

template<class T>
void threadpool<T>::run()
{
     while (!m_stop)
     {
         m_queuestat.wait();
         m_queuelocker.lock();
         if(m_workqueu.empty()){
             m_queuelocker.unlock();
             continue;
         }
         T* request = m_workqueu.front();
         m_workqueu.pop_front();
         m_queuelocker.unlock();
         if(!request){
             continue;
         }
         request->process();
     }
}

#endif
