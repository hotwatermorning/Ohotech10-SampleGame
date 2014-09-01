﻿//          Copyright hotwatermorning 2013 - 2013.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <cassert>
#include <atomic>
#include <future>
#include <limits>
#include <thread>
#include <utility>

#include "./locked_queue.hpp"
#include "./task_impl.hpp"

namespace hwm {

namespace detail { namespace ns_task {

//! @class タスクキュークラス
template<template<class...> class Allocator = std::allocator>
struct task_queue_with_allocator
{
    typedef std::unique_ptr<task_base>			task_ptr_t;
	typedef Allocator<task_ptr_t>				allocator;
	typedef locked_queue<task_ptr_t, std::deque<task_ptr_t, allocator>>
												queue_type;

    //! デフォルトコンストラクタ
    //! std::thread::hardware_concurrency()分だけスレッドを起動する
    task_queue_with_allocator()
        :   task_queue_()
        ,   terminated_flag_(false)
        ,   task_count_(0)
        ,   wait_before_destructed_(true)
    {
        setup(
            (std::max)(std::thread::hardware_concurrency(), 1u)
            );
    }

    //! @brief コンストラクタ
    //! @detail 引数に指定された値だけスレッドを起動する
    //! @param thread_limit [in] 起動する引数の数
    //! @param queue_limit [in] キューに保持できるタスク数の限界
    explicit
    task_queue_with_allocator(size_t num_threads, size_t queue_limit = ((std::numeric_limits<size_t>::max)()))
        :   task_queue_(queue_limit)
        ,   terminated_flag_(false)
        ,   task_count_(0)
        ,   wait_before_destructed_(true)
    {
        assert(num_threads >= 1);
        assert(queue_limit >= 1);

        setup(num_threads);
    }

    //! @brief デストラクタ
    //! @detail デストラクタは、スレッドの終了を待機するが、
    //! その際にwait_before_destructed()がtrueの場合、
    //! キューに積まれたタスクが全て実行され、すべての実行が完了してからスレッドを終了する。
    //! falseの場合、キューに積まれたままのタスクは実行されず、
    //! 呼び出し時点で取り出されているタスクのみ実行してスレッドを終了する。
    //! @note wait_before_destructed()はデフォルトでtrue
    ~task_queue_with_allocator()
    {
        if(wait_before_destructed()) {
            wait();
        }

		set_terminate_flag(true);
		//! Add dummy task to wake all threads,
		//! so the threads check terminate flag and terminate itself.
		for(size_t i = 0; i < num_threads(); ++i) {
			enqueue([]{});
		}

        join_threads();
    }

	size_t num_threads() const { return threads_.size(); }

    //! @brief タスクに新たな処理を追加
    //! @detail 内部のタスクキューが一杯の時はキューが空くまで処理をブロックする
    //! @param [in] f 別スレッドで実行したい関数や関数オブジェクトなど
    //! @param [in] fに対して適用したい引数。Movableでなければならない。
    //! @return タスクとshared stateを共有するstd::futureクラスのオブジェクト
    template<class F, class... Args>
    auto enqueue(F&& f, Args&& ... args) -> std::future<decltype(f(std::forward<Args>(args)...))>
    {
        typedef decltype(f(std::forward<Args>(args)...)) result_t;
        typedef std::promise<result_t> promise_t;

        promise_t promise;
        auto future(promise.get_future());

        task_ptr_t ptask =
            make_task(
                std::move(promise), std::forward<F>(f), std::forward<Args>(args)...);

        {
            task_count_lock_t lock(task_count_mutex_);
            ++task_count_;
        }

        try {
            task_queue_.enqueue(std::move(ptask));
        } catch(...) {
            task_count_lock_t lock(task_count_mutex_);
            --task_count_;
            if(task_count_ == 0) {
                c_task_.notify_all();
            }
            throw;
        }

        return future;
    }

    //! @brief すべてのタスクが実行され終わるのを待機する。
    //! @note wait()は、タスクの実行を待機するだけで、enqueue()の呼び出しはブロックしない。
    //! そのため、wait()の呼び出しが完了する前にenqueue()が行われ続けると、wait()は処理を返さない。
    void    wait() const
    {
        task_count_lock_t lock(task_count_mutex_);
        scoped_add sa(waiting_count_);

        c_task_.wait(lock, [this]{ return task_count_ == 0; });
    }

    //! @brief 指定時刻まですべてのタスクが実行され終わるのを待機する
    //! @tparam TimePoint std::chrono::time_point型に変換可能な型
    //! @param [in] tp すべてのタスクの実行完了を、どの時刻まで待機するか。
    //! @return すべてのタスクが実行され終わった場合、trueが返る。
    //! @note wait_until()は、タスクの実行を待機するだけで、enqueue()の呼び出しはブロックしない。
    //! そのため、wait_until()の呼び出しが完了する前にenqueue()が行われ続けると、wait_until()は処理を返さない。
    template<class TimePoint>
    bool    wait_until(TimePoint tp) const
    {
        task_count_lock_t lock(task_count_mutex_);
        scoped_add sa(waiting_count_);

        return c_task_.wait_until(lock, tp, [this]{ return task_count_ == 0; });
    }

    //! @brief 指定時間内ですべてのタスクが実行され終わるのを待機する
    //! @tparam Duration std::chrono::duration型に変換可能な型
    //! @param [in] tp すべてのタスクの実行完了を、どのくらい時間だけ待機するか。
    //! @return すべてのタスクが実行され終わった場合、trueが返る。
    //! @note wait_for()は、タスクの実行を待機するだけで、enqueue()の呼び出しはブロックしない。
    //! そのため、wait_for()の呼び出しが完了する前にenqueue()が行われ続けると、wait_for()は処理を返さない。
    template<class Duration>
    bool    wait_for(Duration dur) const
    {
        task_count_lock_t lock(task_count_mutex_);
        scoped_add sa(waiting_count_);

        return c_task_.wait_for(lock, dur, [this]{ return task_count_ == 0; });
    }

    //! @brief デストラクタが呼び出された時に、積まれているタスクがすべて実行されるまで待機するかどうかを返す。
    //! @return デストラクタが呼び出された時にすべてのタスクの実行を終了を待機する場合はtrueが返る。
    bool        wait_before_destructed() const
    {
        return wait_before_destructed_.load();
    }

    //! @brief デストラクタが呼び出された時に、積まれているタスクがすべて実行されるまで待機するかどうかを設定する。
    //! @param [in] デストラクタが呼び出された時にすべてのタスクの実行を終了を待機するように設定する場合はtrueを渡す。
    void        set_wait_before_destructed(bool state)
    {
        wait_before_destructed_.store(state);
    }

private:
	typedef std::unique_lock<std::mutex> task_count_lock_t;

    queue_type				    task_queue_;
    std::vector<std::thread>    threads_;
    std::atomic<bool>           terminated_flag_;
    std::mutex mutable          task_count_mutex_;
    size_t                      task_count_;
    std::atomic<size_t> mutable waiting_count_;
    std::condition_variable mutable c_task_;
    std::atomic<bool>           wait_before_destructed_;

    struct scoped_add
    {
        scoped_add(std::atomic<size_t> &value)
            :   v_(value)
        {
            ++v_;
        }

        ~scoped_add()
        {
            --v_;
        }

		//! 明示的にコピーコンストラクタ／コピー代入演算子を削除
		//! これによって暗黙的なムーブコンストラクタ／ムーブ代入演算子も定義されなくなる
        scoped_add(scoped_add const &) = delete;
        scoped_add& operator=(scoped_add const &) = delete;

    private:
        std::atomic<size_t> &v_;
    };

private:

    void    set_terminate_flag(bool state)
    {
        terminated_flag_.store(state);
    }

    bool    is_terminated() const
    {
        return terminated_flag_.load();
    }

    bool    is_waiting() const
    {
        return waiting_count_.load() != 0;
    }

	void	process(int thread_index)
	{
		for( ; ; ) {
			if(is_terminated()) {
				break;
			}

			task_ptr_t task = task_queue_.dequeue();

			bool should_notify = false;

			task->run();

			{
				task_count_lock_t lock(task_count_mutex_);
				--task_count_;

				if(is_waiting() && task_count_ == 0) {
					should_notify = true;
				}
			}

			if(should_notify) {
				c_task_.notify_all();
			}
		}
	}

    void    setup(size_t num_threads)
    {
		threads_.resize(num_threads);
        for(size_t i = 0; i < num_threads; ++i) {
			threads_[i] = std::thread([this, i] { process(i); });
        }
    }

    void    join_threads()
    {
        assert(is_terminated());

        for(auto &th: threads_) {
            th.join();
        }
    }
};

}}  //detail::ns_task

//! hwm::detail::ns_task内のtask_queueクラスをhwm名前空間で使えるように
using detail::ns_task::task_queue_with_allocator;
using task_queue = task_queue_with_allocator<std::allocator>;

}   //namespace hwm
