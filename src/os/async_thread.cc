/*
 +----------------------------------------------------------------------+
 | Swoole                                                               |
 +----------------------------------------------------------------------+
 | This source file is subject to version 2.0 of the Apache license,    |
 | that is bundled with this package in the file LICENSE, and is        |
 | available through the world-wide-web at the following url:           |
 | http://www.apache.org/licenses/LICENSE-2.0.html                      |
 | If you did not receive a copy of the Apache2.0 license and are unable|
 | to obtain it through the world-wide-web, please send a note to       |
 | license@swoole.com so we can mail you a copy immediately.            |
 +----------------------------------------------------------------------+
 | Author: Tianfeng Han  <mikan.tenny@gmail.com>                        |
 +----------------------------------------------------------------------+
 */

#include "swoole_api.h"
#include "async.h"

#include <thread>
#include <atomic>
#include <unordered_map>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>

using namespace std;

typedef swAio_event async_event;

struct async_thread_event
{
    /* head */
    enum swAioOpcode type;
    /* info */
    thread::id tid;
};

class async_event_queue
{
public:
    inline void push(async_event *event)
    {
        unique_lock<mutex> lock(_mutex);
        _queue.push(event);
    }

    inline async_event* pop()
    {
        unique_lock<mutex> lock(_mutex);
        if (_queue.empty())
        {
            return nullptr;
        }
        async_event* retval = _queue.front();
        _queue.pop();
        return retval;
    }

    inline double get_max_wait_time()
    {
        unique_lock<mutex> lock(_mutex);
        if (_queue.empty())
        {
            return 0;
        }
        else
        {
            async_event* event = _queue.front();
            return swoole_microtime() - event->timestamp;
        }
    }

    inline size_t count()
    {
        return _queue.size();
    }

private:
    queue<async_event *> _queue;
    mutex _mutex;
};

class async_thread_pool
{
public:
    async_thread_pool(size_t _min_threads, size_t _max_threads)
    {
        running = false;
        min_thread_num = SW_MAX(SW_AIO_THREAD_DEFAULT_NUM, _min_threads);
        max_thread_num = SW_MAX(min_thread_num, _max_threads);
        max_idle_time = SW_AIO_THREAD_MAX_IDLE_TIME;
        max_wait_time = SW_AIO_TASK_MAX_WAIT_TIME;

        current_pid = getpid();

        if (swPipeBase_create(&_aio_pipe, 0) < 0)
        {
            swoole_throw_error(SW_ERROR_SYSTEM_CALL_FAIL);
        }
        _pipe_read = _aio_pipe.getFd(&_aio_pipe, 0);
        _pipe_write = _aio_pipe.getFd(&_aio_pipe, 1);
        swoole_event_add(_pipe_read, SW_EVENT_READ, SW_FD_AIO);
    }

    ~async_thread_pool()
    {
        shutdown();
        if (SwooleTG.reactor)
        {
            swoole_event_del(_pipe_read);
        }
        _aio_pipe.close(&_aio_pipe);
    }

    bool start()
    {
        running = true;
        current_task_id = 0;
        n_waiting = 0;
        n_closing = 0;
        for (size_t i = 0; i < min_thread_num; i++)
        {
            create_thread(true);
        }
        return true;
    }

    bool shutdown()
    {
        if (!running)
        {
            return false;
        }
        running = false;

        event_mutex.lock();
        _cv.notify_all();
        event_mutex.unlock();

        for (auto &i : threads)
        {
            thread *_thread = i.second;
            if (_thread->joinable())
            {
                _thread->join();
            }
            delete _thread;
        }

        return true;
    }

    async_event* dispatch(const async_event *request)
    {
        if (n_waiting == 0 && threads.size() < max_thread_num) {
            double _max_wait_time = _queue.get_max_wait_time();
            if (_max_wait_time > max_wait_time)
            {
                size_t n = threads.size() * 2;
                if (threads.size() + n > max_thread_num)
                {
                    n = max_thread_num - threads.size();
                }
                swTraceLog(SW_TRACE_AIO, "Create %zu thread, we will have %zu threads", n, threads.size() + n);
                while (n--)
                {
                    create_thread();
                }
            }
        }

        auto _event_copy = new async_event(*request);
        _event_copy->task_id = current_task_id++;
        _event_copy->timestamp = swoole_microtime();
        _queue.push(_event_copy);
        _cv.notify_one();
        return _event_copy;
    }

    inline size_t thread_count()
    {
        return threads.size();
    }

    inline size_t queue_count()
    {
        return _queue.count();
    }

    pid_t current_pid;

    void release_thread(thread::id tid)
    {
        auto i = threads.find(tid);
        if (i == threads.end())
        {
            swWarn("AIO thread#%zu is missing", tid);
            return;
        }
        else
        {
            thread *_thread = i->second;
            swTraceLog(SW_TRACE_AIO, "release idle thread#%zu, we have %zu now", tid, threads.size() - 1);
            if (_thread->joinable()) {
                _thread->join();
            }
            threads.erase(i);
            delete _thread;
        }
    }

private:

    void create_thread(const bool is_core_worker = false)
    {
        try
        {
            thread *_thread = new thread([this, is_core_worker]()
            {
                bool exit_flag = false;

                SwooleTG.buffer_stack = swString_new(SW_STACK_BUFFER_SIZE);
                if (SwooleTG.buffer_stack == nullptr)
                {
                    return;
                }

                swSignal_none();

                while (running)
                {
                    async_event *event = _queue.pop();
                    if (event)
                    {
                        if (sw_unlikely(event->handler == nullptr))
                        {
                            event->error = SW_ERROR_AIO_BAD_REQUEST;
                            event->ret = -1;
                        }
                        else if (sw_unlikely(event->canceled))
                        {
                            event->error = SW_ERROR_AIO_CANCELED;
                            event->ret = -1;
                        }
                        else
                        {
                            event->handler(event);
                        }

                        // swTraceLog(SW_TRACE_AIO, "aio_thread %s. ret=%d, error=%d", event->ret > 0 ? "ok" : "failed", event->ret, event->error);

                        _send_event:
                        while (true)
                        {
                            SwooleAIO.lock.lock(&SwooleAIO.lock);
                            int ret = write(_pipe_write, &event, sizeof(event));
                            SwooleAIO.lock.unlock(&SwooleAIO.lock);
                            if (ret < 0)
                            {
                                if (errno == EAGAIN)
                                {
                                    swSocket_wait(_pipe_write, 1000, SW_EVENT_WRITE);
                                    continue;
                                }
                                else if (errno == EINTR)
                                {
                                    continue;
                                }
                                else
                                {
                                    swSysWarn("sendto swoole_aio_pipe_write failed");
                                }
                            }
                            break;
                        }

                        // exit
                        if (exit_flag)
                        {
                            n_closing--;
                            break;
                        }
                    }
                    else if (running)
                    {
                        unique_lock<mutex> lock(event_mutex);
                        ++n_waiting;
                        if (is_core_worker)
                        {
                            _cv.wait(lock);
                        }
                        else
                        {
                            if (_cv.wait_for(lock, chrono::seconds(max_idle_time)) == cv_status::timeout)
                            {
                                printf("timeout\n");
                                /* notifies the main thread to release this thread */
                                async_thread_event *_event = new async_thread_event;
                                _event->type = SW_AIO_THREAD;
                                _event->tid = this_thread::get_id();
                                event = (async_event *) _event;
                                --n_waiting;
                                ++n_closing;
                                exit_flag = true;
                                goto _send_event;
                            }
                        }
                        --n_waiting;
                    }
                }
            });
            threads[_thread->get_id()] = _thread;
        }
        catch (const std::system_error& e)
        {
            swSysNotice("create aio thread failed, please check your system configuration or adjust max_thread_count");
            return;
        }
    }

    size_t min_thread_num;
    size_t max_thread_num;
    double max_wait_time;
    size_t max_idle_time;

    swPipe _aio_pipe;
    int _pipe_read;
    int _pipe_write;

    bool running;

    atomic<size_t> n_waiting;
    atomic<size_t> n_closing;
    size_t current_task_id = 0;

    unordered_map<thread::id, thread *> threads;
    async_event_queue _queue;
    mutex event_mutex;
    condition_variable _cv;
};

static async_thread_pool *pool = nullptr;

swAsyncIO SwooleAIO;

static void swAio_free(void *private_data)
{
    if (!SwooleAIO.init)
    {
        return;
    }
    if (pool->current_pid == getpid())
    {
        delete pool;
    }
    pool = nullptr;
    SwooleAIO.init = 0;
}

static int swAio_init()
{
    if (SwooleAIO.init)
    {
        swWarn("AIO has already been initialized");
        return SW_ERR;
    }
    if (!SwooleTG.reactor)
    {
        swWarn("no event loop, cannot initialized");
        return SW_ERR;
    }

    if (swMutex_create(&SwooleAIO.lock, 0) < 0)
    {
        swWarn("create mutex lock error");
        return SW_ERR;
    }

    if (SwooleAIO.min_thread_num == 0)
    {
        SwooleAIO.min_thread_num = SW_AIO_THREAD_DEFAULT_NUM;
    }
    if (SwooleAIO.max_thread_num == 0)
    {
        SwooleAIO.max_thread_num = SW_CPU_NUM * SW_AIO_THREAD_NUM_MULTIPLE;
    }
    if (SwooleAIO.min_thread_num > SwooleAIO.max_thread_num)
    {
        SwooleAIO.max_thread_num = SwooleAIO.min_thread_num;
    }

    swReactor_add_destroy_callback(SwooleTG.reactor, swAio_free, nullptr);

    pool = new async_thread_pool(SwooleAIO.min_thread_num, SwooleAIO.max_thread_num);
    pool->start();
    SwooleAIO.init = 1;

    return SW_OK;
}

size_t swAio_thread_count()
{
    return pool ? pool->thread_count() : 0;
}

ssize_t swAio_dispatch(const swAio_event *request)
{
    if (sw_unlikely(!SwooleAIO.init))
    {
        swAio_init();
    }
    SwooleAIO.task_num++;
    async_event *event = pool->dispatch(request);
    return event->task_id;
}

swAio_event* swAio_dispatch2(const swAio_event *request)
{
    if (sw_unlikely(!SwooleAIO.init))
    {
        swAio_init();
    }
    SwooleAIO.task_num++;
    return pool->dispatch(request);
}

int swAio_callback(swReactor *reactor, swEvent *event)
{
    async_event *events[SW_AIO_EVENT_NUM];
    ssize_t n = read(event->fd, events, sizeof(async_event*) * SW_AIO_EVENT_NUM);
    if (n < 0)
    {
        swSysWarn("read() failed");
        return SW_ERR;
    }
    for (size_t i = 0; i < n / sizeof(async_event *); i++)
    {
        async_event *event = events[i];
        if (event->type == SW_AIO_THREAD)
        {
            async_thread_event *event = (async_thread_event *) events[i];
            pool->release_thread(event->tid);
            delete event;
        }
        else if (!event->canceled)
        {
            event->callback(events[i]);
            SwooleAIO.task_num--;
            delete event;
        }
    }
    return SW_OK;
}
