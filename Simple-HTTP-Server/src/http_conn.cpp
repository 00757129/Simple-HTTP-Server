#include "http_conn.h"
#include "log.h"

#define DEBUG 2

const char *ok_200_title = "OK";
const char *err_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file from this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the requested file.\n";
/* 網站根目錄 */
const char *doc_root = "../template/web";
const char *cgi_root = "../template/cgi";

/*
     設定檔案描述符fd為非阻塞狀態
     返回舊的文件狀態
*/
int setNonBlocking(int fd)
{
     int old_option = fcntl(fd, F_GETFL);
     int new_option = old_option | O_NONBLOCK;
     fcntl(fd, F_SETFL, new_option); // 小坑
     return old_option;
}

/*
     將fd註冊到epollfd中，選擇是否oneShot(觸發一次)
     預設：EPOLLIN + EPOLLET + EPOLLRDHUP
*/
void addFd(int epollfd, int fd, bool oneShot)
{
     epoll_event event;
     event.data.fd = fd;
     event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
     if (oneShot)
     {
         event.events |= EPOLLONESHOT;
     }
     epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
     setNonBlocking(fd);
}


int writePipe(int __fd, const void *__buf, size_t n){
     return write(__fd, __buf, n);
}

int readPipe(int __fd, void *__buf, size_t __nbytes){
     return read(__fd, __buf, __nbytes);
}

/*
     將fd從epollfd中移除
*/
void removefd(int epollfd, int fd)
{
     epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
     close(fd);
}

/*
     向epollfd中修改fd，新增屬性ev
     event.events = ev | EPOLLIN | EPOLLONESHOT | EPOLLRDHUP;
*/
void modfd(int epollfd, int fd, int ev)
{
     epoll_event event;
     event.data.fd = fd;
     event.events = ev | EPOLLIN | EPOLLONESHOT | EPOLLRDHUP;
     epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

/*
     是否關閉與客戶端的連接套接字
*/
void http_conn::close_conn(bool real_close)
{
     if (real_close && (m_sockfd != -1))
     {
         removefd(m_epollfd, m_sockfd);
         m_sockfd = -1;
         m_user_count--;
     }
}

/*
     初始化連線：
         sockfd：連接套接字檔案描述符
         addr：客戶端位址
*/
void http_conn::init(int sockfd, const sockaddr_in &addr)
{
     m_sockfd = sockfd;
     m_address = addr;
     int res = 1;
     setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &res, sizeof(res));
     addFd(m_epollfd, m_sockfd, true);
     m_user_count++;
}

/*
     私有函數，初始化內部變數參數
*/
void http_conn::init()
{
     m_chek_state = CHECK_STATE_REQUESTLINE;
     m_linger = false;

     m_method = GET;
     m_url = 0;
     m_version = 0;
     m_content_length = 0;
     m_host = 0;
     m_start_line = 0;
     m_check_idx = 0;
     m_read_idx = 0;
     m_write_idx = 0;

     memset(m_read_buf, '\0', READ_BUFFER_SIZE);
     memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
     memset(m_real_file, '\0', FILENAME_LEN);
     memset(m_cgi_buf, '\0', WRITE_BUFFER_SIZE);
}

/*
     檢查m_read_buf中合法的一行。
     傳回值：
         LINE_OK：合法一行，\r\n皆被置為\0，idx指向下一行開始
         LINE_BAD: 非法一行
         LINE_OPEN：目前緩衝區中尚未完全讀取一行數據
*/
http_conn::LINE_STATUS http_conn::parse_line()
{
     char temp;
     for (; m_check_idx < m_read_idx; ++m_check_idx)
     {
         temp = m_read_buf[m_check_idx];
         if (temp == '\r')
         {
             if ((m_check_idx + 1) == m_read_idx)
             {
                 return LINE_OPEN;
             }
             else if (m_read_buf[m_check_idx + 1] == '\n')
             {
                 m_read_buf[m_check_idx++] = '\0';
                 m_read_buf[m_check_idx++] = '\0';
                 return LINE_OK;
             }
             return LINE_BAD;
         }
         else if (temp == '\n')
         {
             if ((m_check_idx > 1) && (m_read_buf[m_check_idx - 1] == '\r'))
             {
                 m_read_buf[m_check_idx - 1] = '\0';
                 m_read_buf[m_check_idx++] = '\0';
                 return LINE_OK;
             }
             return LINE_BAD;
         }
     }
     return LINE_OPEN;
}

/*
     從m_sockfd讀取盡可能多的資料到m_read_buf中
     傳回值：
         讀取成功：true
         讀取錯誤：false
*/
bool http_conn::read()
{
     if (DEBUG==1)
     {
         printf("start read data form socket:\n");
     }
     if (m_read_idx >= READ_BUFFER_SIZE)
     {
         if (DEBUG==1)
         {
             printf("讀取緩衝區不足:\n");
         }
         return false;
     }

     int bytes_read = 0;
     while (true)
     {
         if (DEBUG==1)
         {
             printf("目前緩衝區大小：%d, 已使用：%d, 剩餘：%d:\n", READ_BUFFER_SIZE, m_read_idx, READ_BUFFER_SIZE - m_read_idx);
         }
         bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
         if (bytes_read == -1)
         {
             if (errno == EAGAIN || errno == EWOULDBLOCK)
             {
                 break;
             }
             if (DEBUG==1)
             {
                 printf("讀取錯誤:\n");
             }
             return false;
         }else if (bytes_read == 0)
         {
             if (DEBUG==1)
             {
                 printf("讀0,對方已經關閉連線:\n");
             }
             return false;
         }
         m_read_idx += bytes_read;
     }
     if (DEBUG==1)
     {
         printf("%s\n", m_read_buf);
     }
     return true;
}

/*
     解析HTTP請求行，取得請求方法、目標URL，以及HTTP版本號
     傳回值：
         成功：NO_REQUEST
         失敗：BAD_REQUEST
*/
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
     /*
         格式：
             Method Url HTTP_version
             GET /index HTTP/1.1
     */
     m_url = strpbrk(text, " \t");
     if (!m_url)
     {
         return BAD_REQUEST;
     }
     *m_url++ = '\0';

     char *method = text;
    
     if (strcasecmp(method, "GET") == 0)
     {
         m_method = GET;
     }
     else if (strcasecmp(method, "POST") == 0)
     {
         if(DEBUG==1){
             printf("method = POST\n");
         }
         m_method = POST;
     }
     else
     {
         return BAD_REQUEST;
     }

     m_url += strspn(m_url, " \t");
     m_version = strpbrk(m_url, " \t");
     if (!m_version)
     {
         return BAD_REQUEST;
     }
     *m_version++ = '\0';
     m_version += strspn(m_version, " \t");
     if (strcasecmp(m_version, "HTTP/1.1") != 0)
     {
         return BAD_REQUEST;
     }

     if (strncasecmp(m_url, "http://", 7) == 0)
     {
         m_url += 7;
         m_url = strchr(m_url, '/');
     }

     if (!m_url || m_url[0] != '/')
     {
         return BAD_REQUEST;
     }
     m_chek_state = CHECK_STATE_HEADER;
     return NO_REQUEST;
}

/*
     傳入text，解析text所表示的header代表的資訊並記錄在變數中
     傳回值：
         如果存在content_lenth > 0，則傳回NO_REQUEST
             否則返回 GET_REQUEST
         如果是其它請求頭，回傳NO_REQUES
*/
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
     if (text[0] == '\0')
     {
         if (m_content_length != 0 || m_method == http_conn::POST)
         {
             m_chek_state = CHECK_STATE_CONTETE;
             return NO_REQUEST;
         }
         return GET_REQUEST;
     }
     else if (strncasecmp(text, "Connection:", 11) == 0)
     {
         text += 11;
         text += strspn(text, " \t");
         if (strcasecmp(text, "keep-alive") == 0)
         {
             m_linger = true;
         }
     }
     else if (strncasecmp(text, "Content-Length:", 15) == 0)
     {
         text += 15;
         text += strspn(text, " \t");
         m_content_length = atol(text);
     }
     else if (strncasecmp(text, "Host:", 5) == 0)
     {
         text += 5;
         text += strspn(text, " \t");
         m_host = text;
     }

     return NO_REQUEST;
}

/*
     解析text傳入的post的body數據
*/
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
     if (m_read_idx >= (m_content_length + m_check_idx))
     {
         text[m_content_length] = '\0';
         // 如果是GET請求則直接忽略
         if (m_method == http_conn::GET){
             if(DEBUG==1){
                 printf("!!!!! return GET_REQUEST\n");
             }
             return GET_REQUEST;
         }
         else if (m_method == http_conn::POST)
         {
             if(DEBUG==1){
                 printf("!!!!! return POST_REQUEST\n");
             }
             // 記錄POST請求
             m_content_data = text;
             return POST_REQUEST;
         }
     }
     return NO_REQUEST;
}

/*
     根據狀態機依序進行參數的解析
     根據最終狀態指向請求
     傳回值：
         解析成功返回NO_REQUEST
         解析失敗回傳BAD_REQUEST
*/
http_conn::HTTP_CODE http_conn::process_read()
{
     if (DEBUG==1)
     {
         printf("開始解析請求:\n");
     }
     LINE_STATUS line_status = LINE_OK;
     HTTP_CODE ret = NO_REQUEST;
     char *text = 0;

     if (DEBUG==1)
     {
         printf("%d, %d\n", m_chek_state == CHECK_STATE_CONTETE, line_status == LINE_OK);
     }
     while (((m_chek_state == CHECK_STATE_CONTETE) && (line_status == LINE_OK)) ||
            ((line_status = parse_line()) == LINE_OK))
     {
         if (DEBUG==1)
         {
             printf("執行解析:\n");
         }
         text = get_line();
         m_start_line = m_check_idx;
         if (DEBUG==1)
         {
             printf("get 1 http line : %s\n", text);
         }
         switch (m_chek_state)
         {
         // 狀態1：檢查請求行
         case CHECK_STATE_REQUESTLINE:
         {
             ret = parse_request_line(text);
             if (ret == BAD_REQUEST)
             {
                 LOG_INFO("[%ld BAD_STATE_REQUESTLINE %s]", pthread_self(), m_url);
                 return BAD_REQUEST;
             }
             break;
         }
         // 狀態2：檢查請求頭
         case CHECK_STATE_HEADER:
         {
             ret = parse_headers(text);
             if (ret == BAD_REQUEST)
             {
                 LOG_INFO("[%ld BAD_STATE_HEADER %s]", pthread_self(), m_url);
                 return BAD_REQUEST;
             }
             else if (ret == GET_REQUEST)
             {
                 if(DEBUG == 2)
                     printf("GET %s\n", m_url);
                 LOG_INFO("[%ld GET %s]", pthread_self(), m_url);
                 return do_request();
             }
             break;
         }
         // 狀態3：檢查最終狀態 執行對應的請求
         case CHECK_STATE_CONTETE:{
             ret = parse_content(text);
             if(DEBUG==1){
                 printf("CHECK_STATE_CONTETE = %d!\n", ret);
             }
             if (ret == GET_REQUEST)
             {
                 if(DEBUG==1){
                     printf("STATE = GET!\n");
                 }
                 if(DEBUG == 2){
                     printf("GET : %s\n", m_url);
                 }
                 LOG_INFO("[%ld GET %s]", pthread_self(), m_url);
                 return do_request();
             }
             else if (ret == POST_REQUEST)
             {
                 if(DEBUG==1){
                     printf("STATE = POST!\n");
                 }
                 if(DEBUG == 2){
                     printf("POST : %s\n", m_url);
                 }
                 LOG_INFO("[%ld POST %s]", pthread_self(), m_url);
                 return do_cgi_request();
             }
             line_status = LINE_OPEN;
             break;
         }
         default:
         {
             return INTERNAL_ERROR;
         }
         }
     }
     return NO_REQUEST;
}

/*
     尋找cgi檔案是否存在，並執行
*/
http_conn::HTTP_CODE http_conn::do_cgi_request()
{
     if(DEBUG==1){
         printf("《==== POST請求處理 ====》\n");
     }
     // 確定cgi檔案路徑
     strcpy(m_real_file, cgi_root);
     int len = strlen(cgi_root);
     strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

     if (DEBUG==1)
     {
         printf("CGI路徑: %s\n", m_real_file);
     }

     // 資源不存在
     if (stat(m_real_file, &m_file_stat) < 0)
     {
         return NO_RESOURCE;
     }
     // 禁止讀
     if (!(m_file_stat.st_mode & S_IROTH))
     { // S_IROTH 其它讀
         return FORBIDDEN_REQUEST;
     }
     // 如果是路徑
     if (S_ISDIR(m_file_stat.st_mode))
     {
         return BAD_REQUEST;
     }
     // 讀取檔案 映射到記憶體空間
     if (DEBUG==1)
     {
         printf("find cgi successful!, post data = %s\n", m_content_data);
     }
     return execute_cgi();
}

/*
     已確定cgi檔案存在，現在執行cgi程式
*/
http_conn::HTTP_CODE http_conn::execute_cgi()
{
     /*
         本函數實作想法如下：
             1.在目前子執行緒中，fork一個子程序，並使用管道連接
             2、關閉子進程中不必要的連接，並將管道讀寫檔案描述子重新導向為stdin和stdout
             3.子程序執行execl，切換為cgi程式開始執行
             4.cgi程式從stdin讀取數據，從stdout回傳數據
             5、cgi程式執行完畢，應回收子程序並且關閉管道釋放資源
     */

     // 01 pipe與fork
     int cgi_out[2]; // cgi程式讀取管道
     int cgi_in[2]; // cgi程式輸入管道
     pid_t pid; // 進程號

     // 建立管道
     if(pipe(cgi_in) < 0){
         return INTERNAL_ERROR; // 伺服器內部錯誤
     }
     if(pipe(cgi_out) < 0){
         return INTERNAL_ERROR; // 伺服器內部錯誤
     }
     if(DEBUG==1){
         printf("thread: %ld, call: execute_cgi, msg: pipe create successful!\n", pthread_self());
     }
     if((pid = fork()) == 0){/* 該部分為子程序 */
         close(cgi_out[0]); // pipe為單向通訊
         close(cgi_in[1]);

         dup2(cgi_out[1], 1); // dup2 stdin
         dup2(cgi_in[0], 0); // dup2 stdout
        
         // 透過環境變數設定Content-Length傳遞
         char content_env[30];
         sprintf(content_env, "CONTENT_LENGTH=%d", m_content_length);
         putenv(content_env);
         execl(m_real_file, m_real_file, NULL);

         if(DEBUG==1){
             printf("thread: %ld, call: execute_cgi, msg: execute cgi failed!\n", pthread_self());
         }
         exit(1); // 執行成功時不應該到這

     }else{/* 該部分為父程序 */
         close(cgi_out[1]);
         close(cgi_in[0]);

         // 傳送content內容，因為pipe向cgi是帶有快取的，所以可以直接寫入
         if(DEBUG==1){
             printf("thread: %ld, call: execute_cgi, msg: main process close pipe!\n", pthread_self());
             printf("write data : %ld : %s!\n", strlen(m_content_data), m_content_data);
         }
         int ret = writePipe(cgi_in[1], m_content_data, strlen(m_content_data));
         if(DEBUG==1){
             if(ret < 0){
                 printf("error : %s\n", strerror(errno));
             }
             //printf("thread: %ld, call: execute_cgi, msg: write data %d bytes!\n", pthread_self(), ret);
         }
         while (readPipe(cgi_out[0], m_cgi_buf + strlen(m_cgi_buf), WRITE_BUFFER_SIZE - strlen(m_cgi_buf) - 1) > 0)
         {
             // 不斷讀取管道中資料並存入快取區中
             if(DEBUG==1){
                 printf("thread: %ld, call: execute_cgi, msg: recv cgi data : %s", pthread_self(), m_cgi_buf);
             }
         }
         close(cgi_in[1]);
         close(cgi_out[0]);
         int status = 0;
         waitpid(pid, &status, 0);
         if(status > 0){
             return INTERNAL_ERROR;
         }
     }
     if(DEBUG==1){
         printf("cgi exec successful!\n");
     }
     return CGI_REQUEST;
}

/*
     目前僅支援GET請求，解析URL，判斷其是否可獲取
     讀取靜態文件，將其透過記憶體映射從內核態到用戶態，減少IO操作次數
     傳回值：
         FILE_REQUEST: 可取得（資料已載入至記憶體)
         BAD_REQUEST： 不可取得
*/
http_conn::HTTP_CODE http_conn::do_request()
{
     if(DEBUG==1){
         printf("《==== GET請求處理 ====》\n");
     }
     strcpy(m_real_file, doc_root);
     int len = strlen(doc_root);
     strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

     if (DEBUG==1)
     {
         printf("路徑: %s\n", m_real_file);
     }

     // 資源不存在 m_real_file
     if (stat(m_real_file, &m_file_stat) < 0)
     {
         return NO_RESOURCE;
     }
     // 禁止讀
     if (!(m_file_stat.st_mode & S_IROTH))
     { // S_IROTH 其它讀
         return FORBIDDEN_REQUEST;
     }
     // 如果是路徑
     if (S_ISDIR(m_file_stat.st_mode))
     {
         return BAD_REQUEST;
     }
     // 讀取檔案 映射到記憶體空間
     int fd = open(m_real_file, O_RDONLY);
     m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ,
                                   MAP_PRIVATE, fd, 0);
     close(fd);
     return FILE_REQUEST;
}

/*
     釋放記憶體中的檔案映射
*/
void http_conn::unmap()
{
     if (m_file_address)
     {
         munmap(m_file_address, m_file_stat.st_size);
         m_file_address = 0;
     }
}

/*
     將記憶體中的資料寫入請求方
*/
bool http_conn::write()
{
     int temp = 0;
     int bytes_have_send = 0;
     int bytes_to_send = m_write_idx;
     if (bytes_to_send == 0)
     {
         modfd(m_epollfd, m_sockfd, EPOLLIN);
         init();
         return true;
     }
     while (1)
     {
         temp = writev(m_sockfd, m_iv, m_iv_count);
         if (temp <= -1)
         {
             if (errno == EAGAIN)
             {
                 modfd(m_epollfd, m_sockfd, EPOLLOUT);
                 return true;
             }
             unmap();
             return false;
         }
         bytes_to_send -= temp;
         bytes_have_send += temp;
         if (bytes_to_send <= bytes_have_send)
         {
             unmap();
             if (m_linger)
             {
                 init();
                 modfd(m_epollfd, m_sockfd, EPOLLIN);
                 return true;
             }
             else
             {
                 modfd(m_epollfd, m_sockfd, EPOLLIN);
                 return false;
             }
         }
     }
}

/*
     將formt格式資料寫入回應中
*/
bool http_conn::add_response(const char *formt, ...)
{
     if (m_write_idx >= WRITE_BUFFER_SIZE)
     {
         return false;
     }
     va_list arg_list;
     va_start(arg_list, formt);
     int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, formt, arg_list);
     if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
     {
         return false;
     }
     m_write_idx += len;
     va_end(arg_list);
     return true;
}

/*
     將狀態行寫入到回應的請求行中
*/
bool http_conn::add_status_line(int status, const char *title)
{
     return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

/*
     將請求頭寫入到回應中
*/
bool http_conn::add_headers(int content_len)
{
     add_content_length(content_len);
     add_linger();
     add_blank_line();
     return true;
}



/*
     寫入Content-Length大小
*/
bool http_conn::add_content_length(int content_len)
{
     return add_response("Content-Length: %d\r\n", content_len);
}

/*
     寫入Connection狀態
*/
bool http_conn::add_linger()
{
     return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

/*
     寫入空白行
*/
bool http_conn::add_blank_line()
{
     return add_response("%s", "\r\n");
}

/*
     追加內容 Content內容
*/
bool http_conn::add_content(const char *content)
{
     return add_response("%s", content);
}

/*
     採用狀態機進行資料的回复
*/
bool http_conn::process_wirte(HTTP_CODE ret)
{
     switch (ret)
     {
     case INTERNAL_ERROR:
     { // 伺服器內部錯誤，傳回500狀態碼 和 詳細資訊
         add_status_line(500, error_500_title);
         add_headers(strlen(error_500_form));
         if (!add_content(error_500_form))
         {
             return false;
         }
         break;
     }

     case BAD_REQUEST:
     { // 請求錯誤，回傳400狀態碼
         add_status_line(400, err_400_title);
         add_headers(strlen(error_400_form));
         if (!add_content(error_400_form))
         {
             return false;
         }
         break;
     }

     case NO_RESOURCE:
     { // 請求資源不存在 回傳404狀態碼
         add_status_line(404, error_404_title);
         add_headers(strlen(error_404_form));
         if (!add_content(error_404_form))
         {
             return false;
         }
         break;
     }

     case FORBIDDEN_REQUEST:
     { // 權限不允許 回傳403狀態碼
         add_status_line(403, error_403_title);
         add_headers(strlen(error_403_form));
         if (!add_content(error_403_form))
         {
             return false;
         }
         break;
     }

     case FILE_REQUEST:
     { // 取得了文件
         add_status_line(200, ok_200_title);
         if (m_file_stat.st_size != 0)
         {
             add_headers(m_file_stat.st_size);
             m_iv[0].iov_base = m_write_buf;
             m_iv[0].iov_len = m_write_idx;
             m_iv[1].iov_base = m_file_address;
             m_iv[1].iov_len = m_file_stat.st_size;
             m_iv_count = 2;
             return true;
         }
         else
         {
             const char *ok_string = "<html><body></body></html>";
             add_headers(strlen(ok_string));
             if (!add_content(ok_string))
             {
                 return false;
             }
         }
         break;
     }
    
     case CGI_REQUEST:
     {// 取得了cgi
         add_status_line(200, ok_200_title);
         add_headers(strlen(m_cgi_buf));
         add_response(m_cgi_buf);
         m_iv[0].iov_base = m_write_buf;
         m_iv[0].iov_len = m_write_idx;
         m_iv_count = 1;
         return true;
     }

     default:
         return false;
     }

     m_iv[0].iov_base = m_write_buf;
     m_iv[0].iov_len = m_write_idx;
     m_iv_count = 1;
     return false;
}

/*
     處理線程：讀 + 寫
*/
void http_conn::process()
{
     HTTP_CODE read_ret = process_read();
     if (read_ret == NO_REQUEST)
     {
         modfd(m_epollfd, m_sockfd, EPOLLIN);
         return;
     }

     bool write_ret = process_wirte(read_ret);
     if (!read_ret)
     {
         close_conn();
     }
     modfd(m_epollfd, m_sockfd, EPOLLOUT);
}

