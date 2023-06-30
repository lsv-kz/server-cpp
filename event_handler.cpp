#include "main.h"

#if defined(LINUX_)
    #include <sys/sendfile.h>
#elif defined(FREEBSD_)
    #include <sys/uio.h>
#endif

using namespace std;
//=============================================================================
static Connect *work_list_start = NULL;
static Connect *work_list_end = NULL;
//---------------------------------------
static Connect *wait_list_start = NULL;
static Connect *wait_list_end = NULL;

static Connect *cgi_wait_list_start = NULL;
static Connect *cgi_wait_list_end = NULL;
//---------------------------------------
struct pollfd *poll_fd;

static mutex mtx_, mtx_cgi;
static condition_variable cond_;

static int close_thr = 0;
static int num_poll, num_work;
static unsigned int cgi_wait, cgi_work;
static int size_buf;
static char *snd_buf;

int send_html(Connect *r);
int create_multipart_head(Connect *req);
void set_part(Connect *r);
int send_headers(Connect *r);
static void worker(Connect *r);
static void func_choose(Connect *r);

void cgi_set_poll_list(Connect *r, int *n, time_t t);

void cgi_worker(Connect* r);
void fcgi_worker(Connect* r);
void scgi_worker(Connect* r);

int cgi_set_size_chunk(Connect *r);
int cgi_create_pipes(Connect *req);

int fcgi_create_connect(Connect *req);
int scgi_create_connect(Connect *req);
void wait_pid(Connect *req);
//======================================================================
int send_part_file(Connect *req)
{
    int rd, wr, len;
    errno = 0;

    if (req->respContentLength == 0)
        return 0;
#if defined(SEND_FILE_) && (defined(LINUX_) || defined(FREEBSD_))
    if (conf->SendFile == 'y')
    {
        if (req->respContentLength >= size_buf)
            len = size_buf;
        else
            len = req->respContentLength;
    #if defined(LINUX_)
        wr = sendfile(req->clientSocket, req->fd, &req->offset, len);
        if (wr == -1)
        {
            if (errno == EAGAIN)
                return ERR_TRY_AGAIN;
            print_err(req, "<%s:%d> Error sendfile(): %s\n", __func__, __LINE__, strerror(errno));
            return wr;
        }
    #elif defined(FREEBSD_)
        off_t wr_bytes;
        int ret = sendfile(req->fd, req->clientSocket, req->offset, len, NULL, &wr_bytes, 0);
        if (ret == -1)
        {
            if (errno == EAGAIN)
            {
                if (wr_bytes == 0)
                    return ERR_TRY_AGAIN;
                req->offset += wr_bytes;
                wr = wr_bytes;
            }
            else
            {
                print_err("<%s:%d> Error sendfile(): %s\n", __func__, __LINE__, strerror(errno));
                return -1;
            }
        }
        else if (ret == 0)
        {
            req->offset += wr_bytes;
            wr = wr_bytes;
        }
        else
        {
            print_err("<%s:%d> Error sendfile()=%d, wr_bytes=%ld\n", __func__, __LINE__, ret, wr_bytes);
            return -1;
        }
    #endif
    }
    else
#endif
    {
        if (req->respContentLength >= size_buf)
            len = size_buf;
        else
            len = req->respContentLength;

        rd = read(req->fd, snd_buf, len);
        if (rd <= 0)
        {
            if (rd == -1)
                print_err(req, "<%s:%d> Error read(): %s\n", __func__, __LINE__, strerror(errno));
            return rd;
        }

        wr = write_to_client(req, snd_buf, rd);
        if (wr < 0)
        {
            if (wr == ERR_TRY_AGAIN)
            {
                lseek(req->fd, -rd, SEEK_CUR);
                return ERR_TRY_AGAIN;
            }

            return wr;
        }
        else if (rd != wr)
            lseek(req->fd, wr - rd, SEEK_CUR);
    }

    req->send_bytes += wr;
    req->respContentLength -= wr;
    if (req->respContentLength == 0)
        wr = 0;

    return wr;
}
//======================================================================
void del_from_list(Connect *r)
{
    if ((r->operation == DYN_PAGE) && r->cgi)
    {
        if ((r->cgi_type == CGI) || 
            (r->cgi_type == PHPCGI))
        {
            if (r->cgi->from_script > 0)
            {
                close(r->cgi->from_script);
                r->cgi->from_script = -1;
            }
    
            if (r->cgi->to_script > 0)
            {
                close(r->cgi->to_script);
                r->cgi->to_script = -1;
            }
            
            wait_pid(r);
        }
        else if ((r->cgi_type == PHPFPM) || 
                (r->cgi_type == FASTCGI) ||
                (r->cgi_type == SCGI))
        {
            if (r->fcgi.fd > 0)
            {
                shutdown(r->fcgi.fd, SHUT_RDWR);
                close(r->fcgi.fd);
            }
        }
        
        delete r->cgi;
        r->cgi = NULL;
        r->scriptName = "";
    mtx_.lock();
        --cgi_work;
    mtx_.unlock();
    }
    else
    {
        if ((r->source_entity == FROM_FILE) || (r->source_entity == MULTIPART_ENTITY))
            close(r->fd);
    }

    if (r->prev && r->next)
    {
        r->prev->next = r->next;
        r->next->prev = r->prev;
    }
    else if (r->prev && !r->next)
    {
        r->prev->next = r->next;
        work_list_end = r->prev;
    }
    else if (!r->prev && r->next)
    {
        r->next->prev = r->prev;
        work_list_start = r->next;
    }
    else if (!r->prev && !r->next)
        work_list_start = work_list_end = NULL;
}
//======================================================================
static void add_work_list()
{
mtx_.lock();
    if (wait_list_start)
    {
        if (work_list_end)
            work_list_end->next = wait_list_start;
        else
            work_list_start = wait_list_start;

        wait_list_start->prev = work_list_end;
        work_list_end = wait_list_end;
        wait_list_start = wait_list_end = NULL;
    }
mtx_.unlock();
}
//======================================================================
static void cgi_add_work_list()
{
mtx_cgi.lock();
    if ((cgi_work < conf->MaxCgiProc) && cgi_wait_list_end)
    {
        int n_max = conf->MaxCgiProc - cgi_work;
        Connect *r = cgi_wait_list_end;

        for ( ; (n_max > 0) && r; r = cgi_wait_list_end, --n_max)
        {
            cgi_wait_list_end = r->prev;
            if (cgi_wait_list_end == NULL)
                cgi_wait_list_start = NULL;
            --cgi_wait;
            //--------------------------
            r->cgi = new (nothrow) Cgi;
            if (!r->cgi)
            {
                r->err = -RS500;
                end_response(r);
                continue;
            }

            if ((r->cgi_type == CGI) || (r->cgi_type == PHPCGI))
            {
                int ret = cgi_create_pipes(r);
                if (ret < 0)
                {
                    print_err(r, "<%s:%d> Error cgi_create_pipes()=%d\n", __func__, __LINE__, ret);
                    r->err = ret;
                    delete r->cgi;
                    r->cgi = NULL;
                    end_response(r);
                    continue;
                }
            }
            else if ((r->cgi_type == PHPFPM) || (r->cgi_type == FASTCGI))
            {
                int ret = fcgi_create_connect(r);
                if (ret < 0)
                {
                    r->err = ret;
                    end_response(r);
                    continue;
                }

                r->fcgi.status = FCGI_READ_DATA;
                r->fcgi.len_buf = 0;
            }
            else if (r->cgi_type == SCGI)
            {
                int ret = scgi_create_connect(r);
                if (ret < 0)
                {
                    r->err = ret;
                    end_response(r);
                    continue;
                }
            }
            else
            {
                print_err(r, "<%s:%d> operation=%d, cgi_type=%s\n", __func__, __LINE__, r->operation, get_cgi_type(r->cgi_type));
                end_response(r);
                continue;
            }
            //--------------------------
            if (work_list_end)
                work_list_end->next = r;
            else
                work_list_start = r;

            r->prev = work_list_end;
            r->next = NULL;
            work_list_end = r;
            ++cgi_work;
        }
    }
mtx_cgi.unlock();
}
//======================================================================
static int set_poll()
{
    num_work = num_poll = 0;
    time_t t = time(NULL);
    Connect *r = work_list_start, *next = NULL;
    for ( ; r; r = next)
    {
        next = r->next;

        if (r->sock_timer == 0)
            r->sock_timer = t;

        if (r->io_status == WORK)
        {
            ++num_work;
            continue;
        }

        if ((t - r->sock_timer) >= r->timeout)
        {
            print_err(r, "<%s:%d> operation=%s, Timeout=%ld\n", __func__, __LINE__, get_str_operation(r->operation), t - r->sock_timer);
            del_from_list(r);
            if (r->operation > READ_REQUEST)
            {
                r->err = -1;
                r->req_hd.iReferer = MAX_HEADERS - 1;
                r->reqHdValue[r->req_hd.iReferer] = "Timeout";
                end_response(r);
            }
            else
            {
                r->err = -1;
                end_response(r);
            }

            continue;
        }

        if (r->operation == DYN_PAGE)
        {
            cgi_set_poll_list(r, &num_poll, t);
        }
        else
        {
            poll_fd[num_poll].fd = r->clientSocket;
            poll_fd[num_poll].events = r->event;
            ++num_poll;
        }
    }

    return num_poll;
}
//======================================================================
static int worker(int num_chld)
{
    int ret = 0;
    if (num_poll > 0)
    {
        int time_poll = conf->TimeoutPoll;
        if (num_work > 0)
            time_poll = 0;

        ret = poll(poll_fd, num_poll, time_poll);
        if (ret == -1)
        {
            print_err("[%d]<%s:%d> Error poll(): %s\n", num_chld, __func__, __LINE__, strerror(errno));
            return -1;
        }
        else if (ret == 0)
        {
            if (num_work == 0)
                return 0;
        }
    }
    else
    {
        if (num_work == 0)
            return 0;
    }

    int i = 0, all = ret + num_work;
    Connect *r = work_list_start, *next;
    for ( ; (all > 0) && r; r = next)
    {
        next = r->next;

        if (r->io_status == WORK)
        {
            --all;
            func_choose(r);
        }
        else
        {
            if ((poll_fd[i].revents == POLLOUT) || (poll_fd[i].revents & POLLIN))
            {
                --all;
                r->io_status = WORK;
                func_choose(r);
            }
            else if (poll_fd[i].revents)
            {
                --all;
                if (r->operation == DYN_PAGE)
                {
                    if (poll_fd[i].fd == r->clientSocket)
                    {
                        print_err(r, "<%s:%d> Error: fd=%d, events=0x%x(0x%x), send_bytes=%lld\n", 
                                __func__, __LINE__, r->clientSocket, poll_fd[i].events, poll_fd[i].revents, r->send_bytes);
                        r->req_hd.iReferer = MAX_HEADERS - 1;
                        r->reqHdValue[r->req_hd.iReferer] = "Connection reset by peer";
                        r->err = -1;
                        del_from_list(r);
                        end_response(r);
                    }
                    else
                    {
                        switch (r->cgi_type)
                        {
                            case CGI:
                            case PHPCGI:
                                if ((r->cgi->op.cgi == CGI_SEND_ENTITY) && (r->cgi->dir == FROM_CGI))
                                {
                                    if (r->mode_send == CHUNK)
                                    {
                                        r->cgi->len_buf = 0;
                                        r->cgi->p = r->cgi->buf + 8;
                                        cgi_set_size_chunk(r);
                                        r->cgi->dir = TO_CLIENT;
                                        r->mode_send = CHUNK_END;
                                        r->sock_timer = 0;
                                    }
                                    else
                                    {
                                        del_from_list(r);
                                        end_response(r);
                                    }
                                }
                                else
                                {
                                    print_err(r, "<%s:%d> Error: events=0x%x(0x%x), %s/%s\n", __func__, __LINE__, 
                                           poll_fd[i].events, poll_fd[i].revents, get_cgi_operation(r->cgi->op.cgi), get_cgi_dir(r->cgi->dir));
                                    if (r->cgi->op.cgi <= CGI_READ_HTTP_HEADERS)
                                        r->err = -RS502;
                                    else
                                        r->err = -1;
                                    del_from_list(r);
                                    end_response(r);
                                }
                                break;
                            case PHPFPM:
                            case FASTCGI:
                                print_err(r, "<%s:%d> Error: events=0x%x(0x%x)\n", 
                                            __func__, __LINE__, poll_fd[i].events, poll_fd[i].revents);
                                if (r->cgi->op.fcgi <= FASTCGI_READ_HTTP_HEADERS)
                                    r->err = -RS502;
                                else
                                    r->err = -1;
                                del_from_list(r);
                                end_response(r);
                                break;
                            case SCGI:
                                if ((r->cgi->op.scgi == SCGI_SEND_ENTITY) && (r->cgi->dir == FROM_CGI))
                                {
                                    if (r->mode_send == CHUNK)
                                    {
                                        r->cgi->len_buf = 0;
                                        r->cgi->p = r->cgi->buf + 8;
                                        cgi_set_size_chunk(r);
                                        r->cgi->dir = TO_CLIENT;
                                        r->mode_send = CHUNK_END;
                                        r->sock_timer = 0;
                                    }
                                    else
                                    {
                                        del_from_list(r);
                                        end_response(r);
                                    }
                                }
                                else
                                {
                                    print_err(r, "<%s:%d> Error: events=0x%x(0x%x)\n", 
                                            __func__, __LINE__, poll_fd[i].events, poll_fd[i].revents);
                                    if (r->cgi->op.scgi <= SCGI_READ_HTTP_HEADERS)
                                        r->err = -RS502;
                                    else
                                        r->err = -1;
                                    del_from_list(r);
                                    end_response(r);
                                }
                                break;
                            default:
                                print_err(r, "<%s:%d> ??? Error: CGI_TYPE=%s\n", __func__, __LINE__, get_cgi_type(r->cgi_type));
                                r->err = -1;
                                del_from_list(r);
                                end_response(r);
                                break;
                        }
                    }
                }
                else
                {
                    print_err(r, "<%s:%d> Error: events=0x%x(0x%x)\n", __func__, __LINE__, poll_fd[i].events, poll_fd[i].revents);
                    del_from_list(r);
                    if (r->operation > READ_REQUEST)
                    {
                        r->req_hd.iReferer = MAX_HEADERS - 1;
                        r->reqHdValue[r->req_hd.iReferer] = "Connection reset by peer";
                    }
                    r->err = -1;
                    end_response(r);
                }
            }
            /*else if (poll_fd[i].revents == 0)
            {
                // --all; NO!!!!!
            }*/
    
            ++i;
        }
    }

    return i;
}
//======================================================================
void event_handler(int num_chld)
{
    size_buf = conf->SndBufSize;
    snd_buf = NULL;

#if defined(SEND_FILE_) && (defined(LINUX_) || defined(FREEBSD_))
    if (conf->SendFile != 'y')
#endif
    {
        snd_buf = new (nothrow) char [size_buf];
        if (!snd_buf)
        {
            print_err("[%d]<%s:%d> Error malloc(): %s\n", num_chld, __func__, __LINE__, strerror(errno));
            exit(1);
        }
    }

    poll_fd = new(nothrow) struct pollfd [conf->MaxWorkConnections];
    if (!poll_fd)
    {
        print_err("[%d]<%s:%d> Error malloc(): %s\n", num_chld, __func__, __LINE__, strerror(errno));
        exit(1);
    }

    while (1)
    {
        {
    unique_lock<mutex> lk(mtx_);
            while ((!work_list_start) && (!wait_list_start) && (!cgi_wait_list_start) && (!close_thr))
            {
                cond_.wait(lk);
            }

            if (close_thr)
                break;
        }

        cgi_add_work_list();
        add_work_list();
        set_poll();
        if (worker(num_chld) < 0)
            break;
    }

    delete [] poll_fd;
#if defined(SEND_FILE_) && (defined(LINUX_) || defined(FREEBSD_))
    if (conf->SendFile != 'y')
#endif
        if (snd_buf)
            delete [] snd_buf;
}
//======================================================================
void push_cgi(Connect *r)
{
    r->operation = DYN_PAGE;
    r->io_status = WORK;
    r->respStatus = RS200;
    r->sock_timer = 0;
    r->prev = NULL;
mtx_cgi.lock();
    r->next = cgi_wait_list_start;
    if (cgi_wait_list_start)
        cgi_wait_list_start->prev = r;
    cgi_wait_list_start = r;
    if (!cgi_wait_list_end)
        cgi_wait_list_end = r;
    ++cgi_wait;
mtx_cgi.unlock();
    cond_.notify_one();
}
//======================================================================
void add_wait_list(Connect *r)
{
    r->io_status = WORK;
    r->sock_timer = 0;
    r->next = NULL;
mtx_.lock();
    r->prev = wait_list_end;
    if (wait_list_start)
    {
        wait_list_end->next = r;
        wait_list_end = r;
    }
    else
        wait_list_start = wait_list_end = r;
mtx_.unlock();
    cond_.notify_one();
}
//======================================================================
void push_send_file(Connect *r)
{
    lseek(r->fd, r->offset, SEEK_SET);
    r->resp_headers.p = r->resp_headers.s.c_str();
    r->resp_headers.len = r->resp_headers.s.size();

    r->event = POLLOUT;
    r->source_entity = FROM_FILE;
    r->operation = SEND_RESP_HEADERS;
    add_wait_list(r);
}
//======================================================================
void push_pollin_list(Connect *r)
{
    r->event = POLLIN;
    r->source_entity = NO_ENTITY;
    add_wait_list(r);
}
//======================================================================
void push_send_multipart(Connect *r)
{
    r->resp_headers.p = r->resp_headers.s.c_str();
    r->resp_headers.len = r->resp_headers.s.size();
    
    r->event = POLLOUT;
    r->source_entity = MULTIPART_ENTITY;
    r->operation = SEND_RESP_HEADERS;
    add_wait_list(r);
}
//======================================================================
void push_send_html(Connect *r)
{
    r->event = POLLOUT;
    r->source_entity = FROM_DATA_BUFFER;
    r->operation = SEND_RESP_HEADERS;
    add_wait_list(r);
}
//======================================================================
void close_event_handler(void)
{
    close_thr = 1;
    cond_.notify_one();
}
//======================================================================
int send_html(Connect *r)
{
    int ret = write_to_client(r, r->html.p, r->html.len);
    if (ret < 0)
    {
        if (ret == ERR_TRY_AGAIN)
            return ERR_TRY_AGAIN;
        return -1;
    }

    r->html.p += ret;
    r->html.len -= ret;
    r->send_bytes += ret;
    if (r->html.len == 0)
        ret = 0;

    return ret;
}
//======================================================================
void set_part(Connect *r)
{
    r->mp.status = SEND_HEADERS;
    
    r->resp_headers.len = create_multipart_head(r);
    r->resp_headers.p = r->mp.hdr.c_str();
    
    r->offset = r->mp.rg->start;
    r->respContentLength = r->mp.rg->len;
    lseek(r->fd, r->offset, SEEK_SET);
}
//======================================================================
int send_headers(Connect *r)
{
    int wr = write_to_client(r, r->resp_headers.p, r->resp_headers.len);
    if (wr < 0)
    {
        if (wr == ERR_TRY_AGAIN)
            return ERR_TRY_AGAIN;
        else
            return -1;
    }
    else if (wr > 0)
    {
        r->resp_headers.p += wr;
        r->resp_headers.len -= wr;
    }
    
    return wr;
}
//======================================================================
static void func_choose(Connect *r)
{
    if (r->operation == DYN_PAGE)
    {
        if ((r->cgi_type == CGI) || (r->cgi_type == PHPCGI))
        {
            cgi_worker(r);
        }
        else if ((r->cgi_type == PHPFPM) || (r->cgi_type == FASTCGI))
        {
            fcgi_worker(r);
        }
        else if (r->cgi_type == SCGI)
        {
            scgi_worker(r);
        }
    }
    else
    {
        worker(r);
    }
}
//======================================================================
static void worker(Connect *r)
{
    if (r->operation == SEND_ENTITY)
    {
        if (r->source_entity == FROM_FILE)
        {
            int wr = send_part_file(r);
            if (wr == 0)
            {
                del_from_list(r);
                end_response(r);
            }
            else if (wr == ERR_TRY_AGAIN)
            {
                r->io_status = POLL;
            }
            else if (wr < 0)
            {
                r->err = wr;
                r->req_hd.iReferer = MAX_HEADERS - 1;
                r->reqHdValue[r->req_hd.iReferer] = "Connection reset by peer";
                del_from_list(r);
                end_response(r);
            }
            else // (wr > 0)
                r->sock_timer = 0;
        }
        else if (r->source_entity == MULTIPART_ENTITY)
        {
            if (r->mp.status == SEND_HEADERS)
            {
                int wr = send_headers(r);
                if (wr < 0)
                {
                    if (wr == ERR_TRY_AGAIN)
                        r->io_status = POLL;
                    else
                    {
                        r->err = -1;
                        r->req_hd.iReferer = MAX_HEADERS - 1;
                        r->reqHdValue[r->req_hd.iReferer] = "Connection reset by peer";
                        del_from_list(r);
                        end_response(r);
                    }
                }
                else if (wr > 0)
                {
                    r->send_bytes += wr;
                    if (r->resp_headers.len == 0)
                    {
                        r->mp.status = SEND_PART;
                    }
                    r->sock_timer = 0;
                }
            }
            else if (r->mp.status == SEND_PART)
            {
                int wr = send_part_file(r);
                if (wr == 0)
                {
                    r->sock_timer = 0;
                    r->mp.rg = r->rg.get();
                    if (r->mp.rg)
                    {
                        set_part(r);
                    }
                    else
                    {
                        r->mp.status = SEND_END;
                        r->mp.hdr = "";
                        r->mp.hdr << "\r\n--" << boundary << "--\r\n";
                        r->resp_headers.len = r->mp.hdr.size();
                        r->resp_headers.p = r->mp.hdr.c_str();
                    }
                }
                else if (wr == ERR_TRY_AGAIN)
                {
                    r->io_status = POLL;
                }
                else if (wr < 0)
                {
                    r->err = wr;
                    r->req_hd.iReferer = MAX_HEADERS - 1;
                    r->reqHdValue[r->req_hd.iReferer] = "Connection reset by peer";
                    del_from_list(r);
                    end_response(r);
                }
            }
            else if (r->mp.status == SEND_END)
            {
                int wr = send_headers(r);
                if (wr < 0)
                {
                    if (wr == ERR_TRY_AGAIN)
                        r->io_status = POLL;
                    else
                    {
                        r->err = -1;
                        r->req_hd.iReferer = MAX_HEADERS - 1;
                        r->reqHdValue[r->req_hd.iReferer] = "Connection reset by peer";
                        del_from_list(r);
                        end_response(r);
                    }
                }
                else if (wr > 0)
                {
                    r->send_bytes += wr;
                    if (r->resp_headers.len == 0)
                    {
                        del_from_list(r);
                        end_response(r);
                    }
                    else
                        r->sock_timer = 0;
                }
            }
        }
        else if (r->source_entity == FROM_DATA_BUFFER)
        {
            int wr = send_html(r);
            if (wr == 0)
            {
                del_from_list(r);
                end_response(r);
            }
            else if (wr == ERR_TRY_AGAIN)
                r->io_status = POLL;
            else if (wr < 0)
            {
                r->err = -1;
                r->req_hd.iReferer = MAX_HEADERS - 1;
                r->reqHdValue[r->req_hd.iReferer] = "Connection reset by peer";
                del_from_list(r);
                end_response(r);
            }
            else
                r->sock_timer = 0;
        }
    }
    else if (r->operation == SEND_RESP_HEADERS)
    {
        int wr = send_headers(r);
        if (wr > 0)
        {
            if (r->resp_headers.len == 0)
            {
                if (r->reqMethod == M_HEAD)
                {
                    del_from_list(r);
                    end_response(r);
                }
                else
                {
                    r->sock_timer = 0;
                    if (r->source_entity == FROM_DATA_BUFFER)
                    {
                        if (r->html.len == 0)
                        {
                            del_from_list(r);
                            end_response(r);
                        }
                        else
                        {
                            r->operation = SEND_ENTITY;
                        }
                    }
                    else if (r->source_entity == FROM_FILE)
                    {
                        r->operation = SEND_ENTITY;
                    }
                    else if (r->source_entity == MULTIPART_ENTITY)
                    {
                        if ((r->mp.rg = r->rg.get()))
                        {
                            r->operation = SEND_ENTITY;
                            set_part(r);
                        }
                        else
                        {
                            r->err = -1;
                            del_from_list(r);
                            end_response(r);
                        }
                    }
                }
            }
            else
                r->sock_timer = 0;
        }
        else if (wr < 0)
        {
            if (wr == ERR_TRY_AGAIN)
                r->io_status = POLL;
            else
            {
                r->err = -1;
                r->req_hd.iReferer = MAX_HEADERS - 1;
                r->reqHdValue[r->req_hd.iReferer] = "Connection reset by peer";
                del_from_list(r);
                end_response(r);
            }
        }
    }
    else if (r->operation == READ_REQUEST)
    {
        int ret = read_request_headers(r);
        if (ret < 0)
        {
            if (ret == ERR_TRY_AGAIN)
                r->io_status = POLL;
            else
            {
                r->err = -1;
                del_from_list(r);
                end_response(r);
            }
        }
        else if (ret > 0)
        {
            del_from_list(r);
            push_resp_list(r);
        }
        else
            r->sock_timer = 0;
    }
    else
    {
        fprintf(stderr, "<%s:%d> ? operation=%s\n", __func__, __LINE__, get_str_operation(r->operation));
        del_from_list(r);
        end_response(r);
    }
}
