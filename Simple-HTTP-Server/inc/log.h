#ifndef __MyLOG_H__
#define __MyLOG_H__

#include <string>
#include <queue>
#include <list>
#include "locker.h"
#include <iostream>
#include <memory>

using namespace std;

// 日誌類型
enum LOG_LEVEL
{
     LOG_LEVEL_DEBUG,
     LOG_LEVEL_INFO,
     LOG_LEVEL_WARNING,
     LOG_LEVEL_ERROR
};


class Log{
public:
     static bool test(int a, int b){
         return a > b;
     }

     // 初始化函數
     static bool init(const string& file_dir, const string& file_name, int close_log, int split_lines);

     // 寫日誌
     static void write_log(LOG_LEVEL level, const char* formt, ...);

     // 強制刷新緩衝區
     static void flush(void);

     // 非同步線程函數
     static void* flush_log_thread(void* args){
         Log::async_write_log();
         return NULL;
     }

private:
     // 單例模式 建構子隱藏
     Log();
     ~Log();

     // 非同步寫入操作 將資料寫入檔案中
     static void* async_write_log();

     // 建立Log文件
     static FILE* create_log_file(const char* filename);
     // 檢查是否需要更新文件
     static void check_and_create(tm* my_tm);

private:
     static string m_dir_name; //日誌路徑
     static string m_log_name; // 日誌名稱
     static int m_split_lines; // 日誌最大行數
     static long long m_count; // 日誌行數記錄
     static int m_today; // 記錄目前日期
     static FILE* m_fp; // log檔指針
     static list<shared_ptr<string>>* m_workque;
     static locker m_mutex; // 阻塞佇列同步鎖定
     static locker m_mutex_res; // 全域資源互斥鎖
     static sem m_sem; // 使用信號量進行日誌的非同步控制

public:
     static int m_close_log; // 關閉日誌
     static Log* instance;
};

#define LOG_DEBUG(formt, ...) Log::write_log(LOG_LEVEL_DEBUG, formt, ##__VA_ARGS__); Log::flush();
#define LOG_INFO(formt, ...) Log::write_log(LOG_LEVEL_INFO, formt, ##__VA_ARGS__); Log::flush();
#define LOG_WARNING(formt, ...) Log::write_log(LOG_LEVEL_WARNING, formt, ##__VA_ARGS__); Log::flush();
#define LOG_ERROR(formt, ...) Log::write_log(LOG_LEVEL_ERROR, formt, ##__VA_ARGS__); Log::flush();


#endif
