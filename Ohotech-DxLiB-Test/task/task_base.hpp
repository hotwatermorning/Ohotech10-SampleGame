﻿//          Copyright hotwatermorning 2013 - 2013.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#ifndef HWM_TASK_GCC_TASKBASE_HPP
#define HWM_TASK_GCC_TASKBASE_HPP

namespace hwm {

namespace detail { namespace ns_task {

//! タスクキューで扱うタスクを表すベースクラス
struct task_base
{
    virtual ~task_base() {}
    virtual void run() = 0;
};

}}  //namespace detail::ns_task

}   //namespace hwm

#endif  //HWM_TASK_GCC_TASKBASE_HPP

