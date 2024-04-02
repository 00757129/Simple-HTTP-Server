#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <sys/time.h>
#include <stdarg.h>

#include "log.h"

Log *Log::instance = NULL;

string Log::m_dir_name; //日誌路徑
string Log::m_log_name; // 日誌名稱
int Log::m_split_lines; // 日誌最大行數
long long Log::m_count; // 日誌行數記錄
int Log::m_today; // 記錄目前日期
FILE *Log::m_fp = NULL; // log檔指標
locker Log::m_mutex; // 阻塞佇列同步鎖
locker Log::m_mutex_res; // 全域資源互斥鎖
sem Log::m_sem; // 使用信號量進行日誌的非同步控制
int Log::m_close_log; // 關閉日誌
list<shared_ptr<string>>* Log::m_workque = nullptr;
/*
     init()：初始化Log參數
*/
bool Log::init(const string& file_dir, const string& file_name, int close_log, int split_lines)
{
    // 01 啟動寫入線程
     pthread_t tid;
     // 只建立一個寫執行緒 避免寫過程中的資源競爭
     if (pthread_create(&tid, NULL, flush_log_thread, NULL) != 0)
     {
         printf("ERROR! Log::init ptherad_cerate failed! \n");
         exit(1);
     }
    
     m_close_log = close_log; // 是否關閉
     m_split_lines = split_lines; // 單一檔案最大行數
     m_dir_name = file_dir; // 檔案路徑
     m_log_name = file_name; // 檔案標識

     m_workque = new list<shared_ptr<string>>();

     // 建立檔案 檔案路徑 = m_dir_name/m_log_name_time
    time_t t = time(NULL);
    struct tm *sys_tm = localtime(&t);
    m_today = sys_tm->tm_yday;

    char tail[16] = {0};
    snprintf(tail, 16, "%d_%02d_%02d", sys_tm->tm_year + 1900, sys_tm->tm_mon + 1, sys_tm->tm_mday);
    string log_full_name = "";
    log_full_name = m_dir_name + "/" + m_log_name + "_" + string(tail);
    m_fp = create_log_file(log_full_name.c_str());
    if (m_fp == NULL)
    {
        printf("ERROR! Create file failed! \n");
        exit(1);
        return false;
    }
    return true;
}

// 建立filename名字的log檔案並傳回指針
FILE *Log::create_log_file(const char *filename)
{
     return fopen(filename, "a+");
}

void Log::check_and_create(tm *my_tm)
{
     // 日誌檢查 是否拆分日誌（狀況1：依日期拆分，狀況2：依目前檔案大小拆分）
     if (my_tm->tm_yday != m_today || m_count % m_split_lines == 0) // everyday log
     {
         //char new_log_name[256] = {0}; // 新檔案路徑
         fflush(m_fp);
         fclose(m_fp);
         char tail[16] = {0};

         //格式化日誌名稱中的時間部分
         snprintf(tail, 16, "%d_%02d_%02d_", my_tm->tm_year + 1900, my_tm->tm_mon + 1, my_tm->tm_mday);

         string new_log_name = "";
         // 日誌過期：建立新文件，文件名稱 = 文件名稱_當前日期
        if (m_today != my_tm->tm_mday)
        {
            new_log_name = m_dir_name + "/" + m_log_name + "_" + string(tail);
            m_today = my_tm->tm_mday;
            m_count = 0;
        }
        else
        {
            new_log_name = m_dir_name + "/" + m_log_name + "_" + string(tail) + "_" + to_string(m_count / m_split_lines);
        }
        m_fp = create_log_file(new_log_name.c_str());
    }
}

/*
    生產者調用（負責寫入資料到佇列）
     參數：
         LOG_LEVEL：DEBUG、INFO、WARNING、ERROR
         format：等待寫入的資料格式
         ...：可選參數 類似printf
     輸出：
         寫入日誌格式：
         [日期：時間] [日誌等級][輸入內文]
*/
void Log::write_log(LOG_LEVEL level, const char *formt, ...)
{
     // 01 取得目前日期和時間
    struct timeval now = {0, 0};
    int ret = gettimeofday(&now, NULL);
    if(ret == -1){
        LOG_WARNING("[Log::write_log gettimeofday() failed!]");
        return;
    }

    time_t t = now.tv_sec;
    struct tm *sys_tm = NULL;
    struct tm my_tm;
    sys_tm = localtime_r(&t, &my_tm);
    if(sys_tm == NULL){
        LOG_WARNING("[Log::write_log localtime() failed!]");
        return;
    }
    // struct tm my_tm = *sys_tm;

    // // 02 更新全域資源
     m_mutex_res.lock();
     m_count++; // 更新行數
     check_and_create(sys_tm); // 檢查是否需要更新文件
     m_mutex_res.unlock();

     // 03 格式化輸出
     string info_buf = "";

     // 03-01 時間格式化 [年-月-日-時-分-秒-微秒]
    char tail[50] = {0};
    int n = snprintf(tail, 48, "[%d-%02d-%02d %02d:%02d:%02d.%06ld]",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec);
    if(n < 0){
        LOG_WARNING("[Log::write_log snprintf() time failed!]");
        return;
    }
    info_buf += tail;

    //03-02 補充日誌等級 [日誌等級]
    switch (level)
    {
    case 0:
        info_buf += "[debug]";
        break;
    case 1:
        info_buf += "[info]";
        break;
    case 2:
        info_buf += "[warn]";
        break;
    case 3:
        info_buf += "[erro]";
        break;
    default:
        info_buf += "[debug]";
        break;
    }

    // 03-03 写入数据
    va_list valst;
    va_start(valst, formt);
    char msg[250] = {0};
    int m = vsnprintf(msg, 248, formt, valst);
    if(m < 0){
        LOG_WARNING("[Log::write_log vsnprintf() time failed!]");
        return;
    }
    msg[m] = '\n';
    msg[m + 1] = '\0';
    info_buf += msg;

    // 04 將格式化後的字串加入佇列（臨界資源）
     m_mutex.lock();
     m_workque->push_back(shared_ptr<string>(new string(info_buf)));
     // m_log_queue.push_back(info_buf);
     m_mutex.unlock();

     // 5 釋放信號量 通知日誌寫執行緒工作
    m_sem.post();
    va_end(valst);
}

void *Log::async_write_log()
{
    // string single_log;
    shared_ptr<string> t;
    while (true)
    {
        // // 01 等待信號量
         m_sem.wait();

         // 02 從佇列中取下資料 臨界資源保護
         m_mutex.lock();
         if(!m_workque->empty()){
             //cout << *m_workque->front() << endl;
              t = m_workque->front();
             m_workque->pop_front();
         }

         m_mutex.unlock();
         // 03 寫入數據
        if(t != nullptr){
            fputs((*t).c_str(), m_fp);
        }
        t.reset();
    }
}

void Log::flush()
{
    fflush(m_fp);
}
