#ifndef __HTTPCONNECTION_H__
#define __HTTPCONNECTION_H__

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include "locker.h"
#include <sys/uio.h>
#include <sys/wait.h>

class http_conn
{
public:
     /* 檔案名稱的最大長度 */
     static const int FILENAME_LEN = 200;
     /* 讀取緩衝區大小 */
     static const int READ_BUFFER_SIZE = 2048;
     /* 寫入緩衝區大小 */
     static const int WRITE_BUFFER_SIZE = 1024;
     /* HTTP請求方式 */
     enum METHOD
     {
         GET = 0,
         POST,
         HEAD,
         PUT,
         DELETE,
         TRACE,
         OPTIONS,
         CONNECT,
         PATCH
     };

     /* 解析客戶請求時，主狀態機所處的狀態 */
     enum CHECK_STATE
     {
         CHECK_STATE_REQUESTLINE = 0,
         CHECK_STATE_HEADER,
         CHECK_STATE_CONTETE
     };

     /* 伺服器處理HTTP請求的可能結果 */
     enum HTTP_CODE
     {
         NO_REQUEST,
         GET_REQUEST,
         POST_REQUEST,
         BAD_REQUEST,
         NO_RESOURCE,
         FORBIDDEN_REQUEST,
         FILE_REQUEST,
         CGI_REQUEST,
         INTERNAL_ERROR, // 伺服器內部錯誤
         CLOSED_CONNECTION
     };

     /* 行的讀取狀態 */
     enum LINE_STATUS
     {
         LINE_OK = 0, // m_check_idx之前的資料為一行
         LINE_BAD, // 非\r\n結尾的一行
         LINE_OPEN // 尚未讀取到結尾
     };

public:
     http_conn(){};
     ~http_conn(){};

public:
     /* 初始化新接受的連線 */
     void init(int sockfd, const sockaddr_in &addr);
     /* 關閉連線 */
     void close_conn(bool real_close = true);
     /* 處理客戶請求 */
     void process();
     /* 非阻塞讀取操作 */
     bool read();
     /* 非阻塞寫入操作 */
     bool write();

private:
     /* 初始化連線 */
     void init();
     /* 解析HTTP請求 */
     HTTP_CODE process_read();
     /* 填充HTTP應答 */
     bool process_wirte(HTTP_CODE ret);

     /* 解析請求 */
     HTTP_CODE parse_request_line(char *text);
     HTTP_CODE parse_headers(char *text);
     HTTP_CODE parse_content(char *text);
     HTTP_CODE do_request();
     HTTP_CODE do_cgi_request();
     http_conn::HTTP_CODE execute_cgi();
     char *get_line(){
         return m_read_buf + m_start_line;
     };
     LINE_STATUS parse_line();

     /* HTTP應答 */
     void unmap();
     bool add_response(const char *format, ...);
     bool add_content(const char *content);
     bool add_status_line(int status, const char *title);
     bool add_headers(int content_length);
     bool add_content_length(int content_length);
     bool add_linger();
     bool add_blank_line();

public:
     /* 共用1個epollfd */
     static int m_epollfd;
     /* 使用者數量 */
     static int m_user_count;

private:
     /* 該HTTP連接的socket和對方的socket位址 */
     int m_sockfd;
     sockaddr_in m_address;

     /* 讀緩衝區 */
     char m_read_buf[READ_BUFFER_SIZE];
     /* 標記目前緩衝區中儲存的位元組數量 */
     int m_read_idx;
     /* 正在解析的字元在緩衝區的位置 */
     int m_check_idx;
     /* 正在解析的行的起始位置 */
     int m_start_line;

     /* 寫入緩衝區 */
     char m_write_buf[WRITE_BUFFER_SIZE];
     /* cgi緩衝區 */
     char m_cgi_buf[WRITE_BUFFER_SIZE];

     /* 寫緩衝區待發送的位元組數 */
     int m_write_idx;

     /* 目前所處狀態 */
     CHECK_STATE m_chek_state;
     /* 請求方法 */
     METHOD m_method;

     /* 客戶請求的目標檔案的完整路徑 */
     char m_real_file[FILENAME_LEN];
     /* 客戶請求的目標檔案名稱 */
     char *m_url;
     /* HTTP協定版本號 */
     char *m_version;
     /* 主機名稱 */
     char *m_host;
     /* HTTP請求的訊息體的長度 */
     int m_content_length;
     /* HTTP請求是否要保持連線 */
     bool m_linger;

     /* POST請求的Content資料 */
     char *m_content_data;

     /* 客戶請求的目標檔案被mmap到記憶體中的起始位置 */
     char *m_file_address;
     /* 目標檔案狀態 */
     struct stat m_file_stat;
     /* 使用writev執行寫入操作 */
     struct iovec m_iv[2];
     int m_iv_count;
};


void addFd(int epollfd, int fd, bool oneShot);
void removefd(int epollfd, int fd);
#endif
