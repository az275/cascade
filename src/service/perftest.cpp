#include <cascade/config.h>
#include "perftest.hpp"
#include <derecho/conf/conf.hpp>
#include <derecho/core/detail/rpc_utils.hpp>
#include <type_traits>
#include <optional>
#include <queue>
#include <derecho/utils/time.h>
#include <unistd.h>
#include <fstream>

namespace derecho {
namespace cascade {


#ifdef ENABLE_EVALUATION
#define TLT_READY_TO_SEND       (11000)
#define TLT_EC_SENT             (12000)
#define TLT_EC_GET_FINISHED     (12042)
/////////////////////////////////////////////////////
// PerfTestClient/PerfTestServer implementation    //
/////////////////////////////////////////////////////

#define on_subgroup_type_index(tindex, func, ...) \
    if (std::type_index(typeid(VolatileCascadeStoreWithStringKey)) == tindex) { \
        func <VolatileCascadeStoreWithStringKey>(__VA_ARGS__); \
    } else if (std::type_index(typeid(PersistentCascadeStoreWithStringKey)) == tindex) { \
        func <PersistentCascadeStoreWithStringKey>(__VA_ARGS__); \
    } else if (std::type_index(typeid(TriggerCascadeNoStoreWithStringKey)) == tindex) { \
        func <TriggerCascadeNoStoreWithStringKey>(__VA_ARGS__); \
    } else { \
        throw derecho::derecho_exception(std::string("Unknown type_index:") + tindex.name()); \
    }

#define on_subgroup_type_index_with_return(tindex, result_handler, func, ...) \
    if (std::type_index(typeid(VolatileCascadeStoreWithStringKey)) == tindex) { \
        result_handler(func <VolatileCascadeStoreWithStringKey>(__VA_ARGS__)); \
    } else if (std::type_index(typeid(PersistentCascadeStoreWithStringKey)) == tindex) { \
        result_handler(func <PersistentCascadeStoreWithStringKey>(__VA_ARGS__)); \
    } else if (std::type_index(typeid(TriggerCascadeNoStoreWithStringKey)) == tindex) { \
        result_handler(func <TriggerCascadeNoStoreWithStringKey>(__VA_ARGS__)); \
    } else { \
        throw derecho::derecho_exception(std::string("Unknown type_index:") + tindex.name()); \
    }

bool PerfTestServer::eval_put(uint64_t max_operation_per_second,
                              uint64_t duration_secs,
                              uint32_t subgroup_type_index,
                              uint32_t subgroup_index,
                              uint32_t shard_index) {
        // synchronization data structures
        // 1 - sending window and future queue
        uint32_t                window_size = derecho::getConfUInt32(derecho::Conf::DERECHO_P2P_WINDOW_SIZE);
        uint32_t                window_slots = window_size*2;
        std::mutex              window_slots_mutex;
        std::condition_variable window_slots_cv;
        std::queue<std::pair<uint64_t,derecho::QueryResults<derecho::cascade::version_tuple>>> futures;
        std::mutex                                                                             futures_mutex;
        std::condition_variable                                                                futures_cv;
        std::condition_variable                                                                window_cv;
        // 3 - all sent flag
        std::atomic<bool>                                   all_sent(false);
        // 4 - query thread
        std::thread                                         query_thread(
            [&window_slots,&window_slots_mutex,&window_slots_cv,&futures,&futures_mutex,&futures_cv,&all_sent](){
                std::unique_lock<std::mutex> futures_lck{futures_mutex};
                while(!all_sent || (futures.size()>0)) {
                    // pick pending futures
                    using namespace std::chrono_literals;
                    while(!futures_cv.wait_for(futures_lck,500ms,[&futures,&all_sent]{return (futures.size() > 0) || all_sent;}));
                    std::decay_t<decltype(futures)> pending_futures;
                    futures.swap(pending_futures);
                    futures_lck.unlock();
                    //+---------------------------------------+
                    //|             QUEUE UNLOCKED            |
                    // waiting for futures with lock released.
                    while (pending_futures.size() > 0) {
                        auto& replies = pending_futures.front().second.get();
                        for (auto& reply: replies) {
                            std::get<0>(reply.second.get());
                            break;
                        }
                        pending_futures.pop();
                        {
                            std::lock_guard<std::mutex> window_slots_lock{window_slots_mutex};
                            window_slots ++;
                        }
                        window_slots_cv.notify_one();
                    }
                    //|            QUEUE UNLOCKED             |
                    //+---------------------------------------+
                    // Acquire LOCK
                    futures_lck.lock();
                }
            }
        );

        //TODO: control read_write_ratio
        uint64_t interval_ns = (max_operation_per_second==0)?0:static_cast<uint64_t>(1e9/max_operation_per_second);
        uint64_t next_ns = get_walltime();
        uint64_t end_ns = next_ns + duration_secs*1000000000ull;
        uint64_t message_id = this->capi.get_my_id()*1000000000ull;
        while(true) {
            uint64_t now_ns = get_walltime();
            if (now_ns > end_ns) {
                all_sent.store(true);
                break;
            }
            // we leave 500 ns for loop overhead.
            if (now_ns + 500 < next_ns) {
                usleep((next_ns - now_ns - 500)/1000); // sleep in microseconds.
            }
            {
                std::unique_lock<std::mutex> window_slots_lock{window_slots_mutex};
                window_slots_cv.wait(window_slots_lock,[&window_slots]{return (window_slots > 0);});
                window_slots --;
            }
            next_ns += interval_ns;
            std::function<void(QueryResults<derecho::cascade::version_tuple>&&)> future_appender =
                [&futures,&futures_mutex,&futures_cv](QueryResults<derecho::cascade::version_tuple>&& query_results){
                    std::unique_lock<std::mutex> lock{futures_mutex};
                    uint64_t timestamp_ns = get_walltime();
                    futures.emplace(timestamp_ns,std::move(query_results));
                    lock.unlock();
                    futures_cv.notify_one();
                };
            // set message id.
            // constexpr does not work in non-template functions.
            if (std::is_base_of<IHasMessageID,std::decay_t<decltype(objects[0])>>::value) {
                dynamic_cast<IHasMessageID*>(&objects.at(now_ns%NUMBER_OF_DISTINCT_OBJECTS))->set_message_id(message_id);
            } else {
                throw derecho_exception{"Evaluation requests an object to support IHasMessageID interface."};
            }
            TimestampLogger::log(TLT_READY_TO_SEND,this->capi.get_my_id(),message_id,get_walltime());
            if (subgroup_index == INVALID_SUBGROUP_INDEX ||
                shard_index == INVALID_SHARD_INDEX) {
                future_appender(this->capi.put(objects.at(now_ns%NUMBER_OF_DISTINCT_OBJECTS)));
            } else {
                on_subgroup_type_index_with_return(
                    std::decay_t<decltype(capi)>::subgroup_type_order.at(subgroup_type_index),
                    future_appender,
                    this->capi.template put, objects.at(now_ns%NUMBER_OF_DISTINCT_OBJECTS), subgroup_index, shard_index);
            }
            TimestampLogger::log(TLT_EC_SENT,this->capi.get_my_id(),message_id,get_walltime());
            message_id ++;
        }
        // wait for all pending futures.
        query_thread.join();
        return true;
}

bool PerfTestServer::eval_put_and_forget(uint64_t max_operation_per_second,
                                         uint64_t duration_secs,
                                         uint32_t subgroup_type_index,
                                         uint32_t subgroup_index,
                                         uint32_t shard_index) {
    uint64_t interval_ns = (max_operation_per_second==0)?0:static_cast<uint64_t>(1e9/max_operation_per_second);
    uint64_t next_ns = get_walltime();
    uint64_t end_ns = next_ns + duration_secs*1000000000ull;
    uint64_t message_id = this->capi.get_my_id()*1000000000ull;
    // control read_write_ratio
    while(true) {
        uint64_t now_ns = get_walltime();
        if (now_ns > end_ns) {
            break;
        }
        // we leave 500 ns for loop overhead.
        if (now_ns + 500 < next_ns) {
            usleep((next_ns - now_ns - 500)/1000); // sleep in microseconds.
        }
        next_ns += interval_ns;
        // set message id.
        // constexpr does not work in non-template function, obviously
        if (std::is_base_of<IHasMessageID, std::decay_t<decltype(objects[0])>>::value) {
            dynamic_cast<IHasMessageID*>(&objects.at(now_ns%NUMBER_OF_DISTINCT_OBJECTS))->set_message_id(message_id);
        } else {
            throw derecho_exception{"Evaluation requests an object to support IHasMessageID interface."};
        }
        // log time.
        TimestampLogger::log(TLT_READY_TO_SEND,this->capi.get_my_id(),message_id,get_walltime());
        // send it
        if (subgroup_index == INVALID_SUBGROUP_INDEX || shard_index == INVALID_SHARD_INDEX) {
            this->capi.put_and_forget(objects.at(now_ns%NUMBER_OF_DISTINCT_OBJECTS));
        } else {
            on_subgroup_type_index(std::decay_t<decltype(capi)>::subgroup_type_order.at(subgroup_type_index),
                    this->capi.template put_and_forget, objects.at(now_ns%NUMBER_OF_DISTINCT_OBJECTS), subgroup_index, shard_index);
        }
        // log time.
        TimestampLogger::log(TLT_EC_SENT,this->capi.get_my_id(),message_id,get_walltime());
        message_id ++;
    }
    return true;
}

bool PerfTestServer::eval_trigger_put(uint64_t max_operation_per_second,
                                      uint64_t duration_secs,
                                      uint32_t subgroup_type_index,
                                      uint32_t subgroup_index,
                                      uint32_t shard_index) {
    uint64_t interval_ns = (max_operation_per_second==0)?0:static_cast<uint64_t>(1e9/max_operation_per_second);
    uint64_t next_ns = get_walltime();
    uint64_t end_ns = next_ns + duration_secs*1000000000ull;
    uint64_t message_id = this->capi.get_my_id()*1000000000ull;
    // control read_write_ratio
    while(true) {
        uint64_t now_ns = get_walltime();
        if (now_ns > end_ns) {
            break;
        }
        // we leave 500 ns for loop overhead.
        if (now_ns + 500 < next_ns) {
            usleep((next_ns - now_ns - 500)/1000); // sleep in microseconds.
        }
        next_ns += interval_ns;
        // set message id.
        // constexpr does not work here.
        if (std::is_base_of<IHasMessageID,std::decay_t<decltype(objects[0])>>::value) {
            dynamic_cast<IHasMessageID*>(&objects.at(now_ns%NUMBER_OF_DISTINCT_OBJECTS))->set_message_id(message_id);
        } else {
            throw derecho_exception{"Evaluation requests an object to support IHasMessageID interface."};
        }
        // log time.
        TimestampLogger::log(TLT_READY_TO_SEND,this->capi.get_my_id(),message_id,get_walltime());
        if (subgroup_index == INVALID_SUBGROUP_INDEX || shard_index == INVALID_SHARD_INDEX) {
            this->capi.trigger_put(objects.at(now_ns%NUMBER_OF_DISTINCT_OBJECTS));
        } else {
            on_subgroup_type_index(std::decay_t<decltype(capi)>::subgroup_type_order.at(subgroup_type_index),
                    this->capi.template trigger_put, objects.at(now_ns%NUMBER_OF_DISTINCT_OBJECTS), subgroup_index, shard_index);
        }
        TimestampLogger::log(TLT_EC_SENT,this->capi.get_my_id(),message_id,get_walltime());
        message_id ++;
    }

    return true;
}

bool PerfTestServer::eval_get(uint64_t log_depth,
                              uint64_t max_operations_per_second,
                              uint64_t duration_secs,
                              uint32_t subgroup_type_index,
                              uint32_t subgroup_index,
                              uint32_t shard_index) {
    debug_enter_func_with_args("max_ops={},duration={},subgroup_type_index={},subgroup_index={},shard_index={}",
                               max_operations_per_second, duration_secs, subgroup_type_index, subgroup_index, shard_index);
    // In case the test objects ever change type, use an alias for whatever type is in the objects vector
    using ObjectType = std::decay_t<decltype(objects[0])>;
    // Sending window variables
    uint32_t window_size = derecho::getConfUInt32(derecho::Conf::DERECHO_P2P_WINDOW_SIZE);
    uint32_t window_slots = window_size * 2;
    std::mutex window_slots_mutex;
    std::condition_variable window_slots_cv;
    // Result future queue, which pairs message IDs with a future for that message
    std::queue<std::pair<uint64_t, derecho::QueryResults<const ObjectType>>> futures;
    std::mutex futures_mutex;
    std::condition_variable futures_cv;
    std::condition_variable window_cv;
    // All sent flag
    std::atomic<bool> all_sent(false);
    // Node ID, used for logger calls
    const node_id_t my_node_id = this->capi.get_my_id();
    // Future consuming thread
    std::thread query_thread(
            [&]() {
                std::unique_lock<std::mutex> futures_lck{futures_mutex};
                while(!all_sent || (futures.size() > 0)) {
                    // Wait for the futures queue to be non-empty, then swap it with pending_futures
                    using namespace std::chrono_literals;
                    while(!futures_cv.wait_for(futures_lck, 500ms, [&futures, &all_sent] { return (futures.size() > 0) || all_sent; }))
                        ;
                    std::decay_t<decltype(futures)> pending_futures;
                    futures.swap(pending_futures);
                    futures_lck.unlock();
                    //+---------------------------------------+
                    //|             QUEUE UNLOCKED            |
                    // wait for each future in pending_futures, leaving futures unlocked
                    while(pending_futures.size() > 0) {
                        auto& replies = pending_futures.front().second.get();
                        uint64_t message_id = pending_futures.front().first;
                        // Get only the first reply
                        for(auto& reply : replies) {
                            reply.second.get();
                            // This might not be an accurate time for when the query completed, depending on how long the thread waited to acquire the queue lock
                            TimestampLogger::log(TLT_EC_GET_FINISHED, my_node_id, message_id, get_walltime());
                            break;
                        }
                        pending_futures.pop();
                        {
                            std::lock_guard<std::mutex> window_slots_lock{window_slots_mutex};
                            window_slots++;
                        }
                        window_slots_cv.notify_one();
                    }
                    //|            QUEUE UNLOCKED             |
                    //+---------------------------------------+
                    // Acquire lock on futures queue
                    futures_lck.lock();
                }
            });
    // Put test objects in the target subgroup/object pool, and record which version is log_depth back in the log
    // NOTE: This only works if there is a single client! If there are multiple clients there will be num_clients * log_depth versions
    std::vector<persistent::version_t> oldest_object_versions;
    for(const auto& object : objects) {
        using namespace derecho;
        // Call put with either the object pool interface or the shard interface, and store the future here
        // Use a unique_ptr to work around the fact that QueryResults has a move constructor but no default constructor
        std::unique_ptr<QueryResults<cascade::version_tuple>> put_result_future;
        if(subgroup_index == INVALID_SUBGROUP_INDEX || shard_index == INVALID_SHARD_INDEX) {
            // put returns the QueryResults by value, so we have to move-construct it into a unique_ptr
            put_result_future = std::make_unique<QueryResults<cascade::version_tuple>>(std::move(this->capi.put(object)));
        } else {
            // Manual copy of on_subgroup_type_index macro so I can use make_unique
            std::type_index tindex = std::decay_t<decltype(capi)>::subgroup_type_order.at(subgroup_type_index);
            if(std::type_index(typeid(VolatileCascadeStoreWithStringKey)) == tindex) {
                put_result_future = std::make_unique<QueryResults<cascade::version_tuple>>(
                    std::move(this->capi.template put<VolatileCascadeStoreWithStringKey>(object, subgroup_index, shard_index)));
            } else if(std::type_index(typeid(PersistentCascadeStoreWithStringKey)) == tindex) {
                put_result_future = std::make_unique<QueryResults<cascade::version_tuple>>(
                    std::move(this->capi.template put<PersistentCascadeStoreWithStringKey>(object, subgroup_index, shard_index)));
            } else if(std::type_index(typeid(TriggerCascadeNoStoreWithStringKey)) == tindex) {
                put_result_future = std::make_unique<QueryResults<cascade::version_tuple>>(
                    std::move(this->capi.template put<TriggerCascadeNoStoreWithStringKey>(object, subgroup_index, shard_index)));
            } else {
                throw derecho::derecho_exception(std::string("Unknown type_index:") + tindex.name());
            }
        }
        // Save the version assigned to this version of the object, which will become the oldest version in the log
        auto& replies = put_result_future->get();
        derecho::cascade::version_tuple object_version_tuple = replies.begin()->second.get();
        oldest_object_versions.emplace_back(std::get<0>(object_version_tuple));
        dbg_default_debug("eval_get: Object {} got version {}, putting {} more versions in front of it", object.get_key_ref(), std::get<0>(object_version_tuple), log_depth);
        // Call put_and_forget repeatedly on the same object to give it multiple newer versions in the log, up to the depth needed
        for(uint64_t i = 0; i < log_depth; ++i) {
            if(subgroup_index == INVALID_SUBGROUP_INDEX || shard_index == INVALID_SHARD_INDEX) {
                this->capi.put_and_forget(object);
            } else {
                on_subgroup_type_index(std::decay_t<decltype(capi)>::subgroup_type_order.at(subgroup_type_index),
                                       this->capi.template put_and_forget, object, subgroup_index, shard_index);
            }
        }
    }

    dbg_default_info("eval_get: Puts complete, ready to start experiment");

    // Timing control variables
    uint64_t interval_ns = (max_operations_per_second == 0) ? 0 : static_cast<uint64_t>(1e9 / max_operations_per_second);
    uint64_t next_ns = get_walltime();
    uint64_t end_ns = next_ns + duration_secs * 1000000000ull;
    uint64_t message_id = this->capi.get_my_id() * 1000000000ull;

    while(true) {
        uint64_t now_ns = get_walltime();
        if(now_ns > end_ns) {
            all_sent.store(true);
            break;
        }
        // we leave 500 ns for loop overhead.
        if(now_ns + 500 < next_ns) {
            usleep((next_ns - now_ns - 500) / 1000);  // sleep in microseconds.
        }
        {
            std::unique_lock<std::mutex> window_slots_lock{window_slots_mutex};
            window_slots_cv.wait(window_slots_lock, [&window_slots] { return (window_slots > 0); });
            window_slots--;
        }
        next_ns += interval_ns;
        // Since each loop iteration creates its own future_appender, capture the message_id by copy
        std::function<void(QueryResults<const ObjectType>&&)> future_appender =
                [&futures, &futures_mutex, &futures_cv, message_id](
                        QueryResults<const ObjectType>&& query_results) {
                    std::unique_lock<std::mutex> lock{futures_mutex};
                    futures.emplace(message_id, std::move(query_results));
                    lock.unlock();
                    futures_cv.notify_one();
                };
        std::size_t cur_object_index = now_ns % NUMBER_OF_DISTINCT_OBJECTS;
        // NOTE: Setting the message ID on the object won't do anything because we're doing a Get, not a Put
        TimestampLogger::log(TLT_READY_TO_SEND, my_node_id, message_id, get_walltime());
        // With either the object pool interface or the shard interface, further decide whether to request the current version or an old version
        if(subgroup_index == INVALID_SUBGROUP_INDEX || shard_index == INVALID_SHARD_INDEX) {
            if(log_depth == 0) {
                future_appender(this->capi.multi_get(objects.at(cur_object_index).get_key_ref()));
            } else {
                future_appender(this->capi.get(objects.at(cur_object_index).get_key_ref(), oldest_object_versions.at(cur_object_index)));
            }
        } else {
            if(log_depth == 0) {
                on_subgroup_type_index_with_return(
                        std::decay_t<decltype(capi)>::subgroup_type_order.at(subgroup_type_index),
                        future_appender,
                        this->capi.template multi_get, objects.at(cur_object_index).get_key_ref(), subgroup_index, shard_index);
            } else {
                on_subgroup_type_index_with_return(
                        std::decay_t<decltype(capi)>::subgroup_type_order.at(subgroup_type_index),
                        future_appender,
                        this->capi.template get, objects.at(cur_object_index).get_key_ref(), oldest_object_versions.at(cur_object_index), true, subgroup_index, shard_index);
            }
        }
        TimestampLogger::log(TLT_EC_SENT, my_node_id, message_id, get_walltime());
        message_id++;
    }
    dbg_default_info("eval_get: All messages sent, waiting for queries to complete");
    // wait for all pending futures.
    query_thread.join();
    return true;
}

PerfTestServer::PerfTestServer(ServiceClientAPI& capi, uint16_t port):
    capi(capi),
    server(port) {
    // Initialize objects

    // API 1 : run shard perf
    //
    // @param subgroup_type_index
    // @param subgroup_index
    // @param shard_index
    // @param policy
    // @param user_specified_node_id
    // @param read_write_ratio
    // @param max_operation_per_second
    // @param duration_Secs
    // @param output_filename
    //
    // @return true/false indicating if the RPC call is successful.
    server.bind("perf_put_to_shard",[this](
        uint32_t            subgroup_type_index,
        uint32_t            subgroup_index,
        uint32_t            shard_index,
        uint32_t            policy,
        uint32_t            user_specified_node_id,
        double              read_write_ratio,
        uint64_t            max_operation_per_second,
        int64_t             start_sec,
        uint64_t            duration_secs,
        const std::string&  output_filename) {
        // STEP 1 - set up the shard member selection policy
        on_subgroup_type_index(std::decay_t<decltype(capi)>::subgroup_type_order.at(subgroup_type_index),
            this->capi.template set_member_selection_policy,
            subgroup_index,
            shard_index,
            static_cast<ShardMemberSelectionPolicy>(policy),
            user_specified_node_id);
        // STEP 2 - prepare workload
        objects.clear();
        make_workload<std::string,ObjectWithStringKey>(derecho::getConfUInt32(derecho::Conf::DERECHO_MAX_P2P_REQUEST_PAYLOAD_SIZE),"raw_key_",objects);
        // STEP 3 - start experiment and log
        int64_t sleep_us = (start_sec*1e9 - static_cast<int64_t>(get_walltime()))/1e3;
        if (sleep_us > 1) {
            usleep(sleep_us);
        }
        if (this->eval_put(max_operation_per_second,duration_secs,subgroup_type_index,subgroup_index,shard_index)) {
            TimestampLogger::flush(output_filename);
            return true;
        } else {
            return false;
        }
    });
    // API 1.5 : run shard perf with put_and_forget
    //
    // @param subgroup_type_index
    // @param subgroup_index
    // @param shard_index
    // @param policy
    // @param user_specified_node_id
    // @param read_write_ratio
    // @param max_operation_per_second
    // @param duration_Secs
    // @param output_filename
    //
    // @return true/false indicating if the RPC call is successful.
    server.bind("perf_put_and_forget_to_shard",[this](
        uint32_t            subgroup_type_index,
        uint32_t            subgroup_index,
        uint32_t            shard_index,
        uint32_t            policy,
        uint32_t            user_specified_node_id,
        double              read_write_ratio,
        uint64_t            max_operation_per_second,
        int64_t             start_sec,
        uint64_t            duration_secs,
        const std::string&  output_filename) {
        // STEP 1 - set up the shard member selection policy
        on_subgroup_type_index(std::decay_t<decltype(capi)>::subgroup_type_order.at(subgroup_type_index),
            this->capi.template set_member_selection_policy,
            subgroup_index,
            shard_index,
            static_cast<ShardMemberSelectionPolicy>(policy),
            user_specified_node_id);
        // STEP 2 - prepare workload
        objects.clear();
        make_workload<std::string,ObjectWithStringKey>(derecho::getConfUInt32(derecho::Conf::DERECHO_MAX_P2P_REQUEST_PAYLOAD_SIZE),"raw_key_",objects);
        // STEP 3 - start experiment and log
        int64_t sleep_us = (start_sec*1e9 - static_cast<int64_t>(get_walltime()))/1e3;
        if (sleep_us > 1) {
            usleep(sleep_us);
        }
        if (this->eval_put_and_forget(max_operation_per_second,duration_secs,subgroup_type_index,subgroup_index,shard_index)) {
            TimestampLogger::flush(output_filename);
            return true;
        } else {
            return false;
        }
    });
    // API 1.6 : run shard perf with trigger_put
    //
    // @param subgroup_type_index
    // @param subgroup_index
    // @param shard_index
    // @param policy
    // @param user_specified_node_id
    // @param read_write_ratio
    // @param max_operation_per_second
    // @param duration_Secs
    // @param output_filename
    //
    // @return true/false indicating if the RPC call is successful.
    server.bind("perf_trigger_put_to_shard",[this](
        uint32_t            subgroup_type_index,
        uint32_t            subgroup_index,
        uint32_t            shard_index,
        uint32_t            policy,
        uint32_t            user_specified_node_id,
        double              read_write_ratio,
        uint64_t            max_operation_per_second,
        int64_t             start_sec,
        uint64_t            duration_secs,
        const std::string&  output_filename) {
        // STEP 1 - set up the shard member selection policy
        on_subgroup_type_index(std::decay_t<decltype(capi)>::subgroup_type_order.at(subgroup_type_index),
            this->capi.template set_member_selection_policy,
            subgroup_index,
            shard_index,
            static_cast<ShardMemberSelectionPolicy>(policy),
            user_specified_node_id);
        // STEP 2 - prepare workload
        objects.clear();
        make_workload<std::string,ObjectWithStringKey>(derecho::getConfUInt32(derecho::Conf::DERECHO_MAX_P2P_REQUEST_PAYLOAD_SIZE),"raw_key_",objects);
        int64_t sleep_us = (start_sec*1e9 - static_cast<int64_t>(get_walltime()))/1e3;
        if (sleep_us > 1) {
            usleep(sleep_us);
        }
        // STEP 3 - start experiment and log
        if (this->eval_trigger_put(max_operation_per_second,duration_secs,subgroup_type_index,subgroup_index,shard_index)) {
            TimestampLogger::flush(output_filename);
            return true;
        } else {
            return false;
        }
    });

    /**
     * RPC function that runs perf_get on a specific shard
     *
     * @param subgroup_type_index
     * @param subgroup_index
     * @param shard_index
     * @param member_selection_policy
     * @param user_specified_node_id
     * @param log_depth
     * @param max_operations_per_second
     * @param start_sec
     * @param duration_secs
     * @param output_filename
     * @return true if experiment completed successfully, false if there was an error
     */
    server.bind("perf_get_to_shard", [this](uint32_t subgroup_type_index,
                                            uint32_t subgroup_index,
                                            uint32_t shard_index,
                                            uint32_t member_selection_policy,
                                            uint32_t user_specified_node_id,
                                            uint64_t log_depth,
                                            uint64_t max_operations_per_second,
                                            int64_t start_sec,
                                            uint64_t duration_secs,
                                            const std::string& output_filename) {
        // Set up the shard member selection policy
        on_subgroup_type_index(std::decay_t<decltype(capi)>::subgroup_type_order.at(subgroup_type_index),
                               this->capi.template set_member_selection_policy,
                               subgroup_index,
                               shard_index,
                               static_cast<ShardMemberSelectionPolicy>(member_selection_policy),
                               user_specified_node_id);
        // Create workload objects
        objects.clear();
        make_workload<std::string, ObjectWithStringKey>(derecho::getConfUInt32(derecho::Conf::DERECHO_MAX_P2P_REQUEST_PAYLOAD_SIZE), "raw_key_", objects);
        // Wait for start time
        int64_t sleep_us = (start_sec * 1e9 - static_cast<int64_t>(get_walltime())) / 1e3;
        if(sleep_us > 1) {
            usleep(sleep_us);
        }
        // Run experiment, then log timestamps
        try {
            if(this->eval_get(log_depth, max_operations_per_second, duration_secs, subgroup_type_index, subgroup_index, shard_index)) {
                TimestampLogger::flush(output_filename);
                return true;
            } else {
                return false;
            }
        } catch(const std::exception& e) {
            std::cerr << "eval_get failed with exception: " << typeid(e).name() << ": " << e.what() << std::endl;
            return false;
        }
    });

    // API 2 : run object pool perf
    //
    // @param object_pool_pathname
    // @param policy
    // @param user_specified_node_ids
    // @param read_write_ratio
    // @param max_operation_per_second
    // @param duration_Secs
    // @param output_filename
    //
    // @return true/false indicating if the RPC call is successful.
    server.bind("perf_put_to_objectpool",[this](
        const std::string&  object_pool_pathname,
        uint32_t            policy,
        const std::vector<node_id_t>&  user_specified_node_ids, // one per shard
        double              read_write_ratio,
        uint64_t            max_operation_per_second,
        int64_t             start_sec,
        uint64_t            duration_secs,
        const std::string&  output_filename) {

        auto object_pool = this->capi.find_object_pool(object_pool_pathname);

        uint32_t number_of_shards;
        // STEP 1 - set up the shard member selection policy
        on_subgroup_type_index(std::decay_t<decltype(capi)>::subgroup_type_order.at(object_pool.subgroup_type_index),
            number_of_shards = this->capi.template get_number_of_shards, object_pool.subgroup_index);
        if (user_specified_node_ids.size() < number_of_shards) {
            throw derecho::derecho_exception(std::string("the size of 'user_specified_node_ids' argument does not match shard number."));
        }
        for (uint32_t shard_index = 0; shard_index < number_of_shards; shard_index ++) {
            on_subgroup_type_index(std::decay_t<decltype(capi)>::subgroup_type_order.at(object_pool.subgroup_type_index),
                this->capi.template set_member_selection_policy, object_pool.subgroup_index, shard_index, static_cast<ShardMemberSelectionPolicy>(policy), user_specified_node_ids.at(shard_index));
        }
        // STEP 2 - prepare workload
        objects.clear();
        make_workload<std::string,ObjectWithStringKey>(derecho::getConfUInt32(derecho::Conf::DERECHO_MAX_P2P_REQUEST_PAYLOAD_SIZE),object_pool_pathname+"/key_",objects);
        int64_t sleep_us = (start_sec*1e9 - static_cast<int64_t>(get_walltime()))/1e3;
        if (sleep_us > 1) {
            usleep(sleep_us);
        }
        // STEP 3 - start experiment and log
        if (this->eval_put(max_operation_per_second,duration_secs,object_pool.subgroup_type_index)) {
            TimestampLogger::flush(output_filename);
            return true;
        } else {
            return false;
        }
    });
    // API 2.5 : run shard perf with put_and_forget
    //
    // @param object_pool_pathname
    // @param policy
    // @param user_specified_node_id
    // @param read_write_ratio
    // @param max_operation_per_second
    // @param duration_Secs
    // @param output_filename
    //
    // @return true/false indicating if the RPC call is successful.
    server.bind("perf_put_and_forget_to_objectpool",[this](
        const std::string&  object_pool_pathname,
        uint32_t            policy,
        const std::vector<node_id_t>&  user_specified_node_ids, // one per shard
        double              read_write_ratio,
        uint64_t            max_operation_per_second,
        int64_t             start_sec,
        uint64_t            duration_secs,
        const std::string&  output_filename) {

        auto object_pool = this->capi.find_object_pool(object_pool_pathname);

        uint32_t number_of_shards;
        // STEP 1 - set up the shard member selection policy
        on_subgroup_type_index(std::decay_t<decltype(capi)>::subgroup_type_order.at(object_pool.subgroup_type_index),
            number_of_shards = this->capi.template get_number_of_shards, object_pool.subgroup_index);
        if (user_specified_node_ids.size() < number_of_shards) {
            throw derecho::derecho_exception(std::string("the size of 'user_specified_node_ids' argument does not match shard number."));
        }
        for (uint32_t shard_index = 0; shard_index < number_of_shards; shard_index ++) {
            on_subgroup_type_index(std::decay_t<decltype(capi)>::subgroup_type_order.at(object_pool.subgroup_type_index),
                this->capi.template set_member_selection_policy, object_pool.subgroup_index, shard_index, static_cast<ShardMemberSelectionPolicy>(policy), user_specified_node_ids.at(shard_index));
        }
        // STEP 2 - prepare workload
        objects.clear();
        make_workload<std::string,ObjectWithStringKey>(derecho::getConfUInt32(derecho::Conf::DERECHO_MAX_P2P_REQUEST_PAYLOAD_SIZE),"raw_key_",objects);
        int64_t sleep_us = (start_sec*1e9 - static_cast<int64_t>(get_walltime()))/1e3;
        if (sleep_us > 1) {
            usleep(sleep_us);
        }
        // STEP 3 - start experiment and log
        if (this->eval_put_and_forget(max_operation_per_second,duration_secs,object_pool.subgroup_type_index)) {
            TimestampLogger::flush(output_filename);
            return true;
        } else {
            return false;
        }
    });
    // API 2.6 : run object perf with trigger_put
    //
    // @param object_pool_pathname
    // @param policy
    // @param user_specified_node_id
    // @param read_write_ratio
    // @param max_operation_per_second
    // @param duration_Secs
    // @param output_filename
    //
    // @return true/false indicating if the RPC call is successful.
    server.bind("perf_trigger_put_to_objectpool", [this](
                                                          const std::string& object_pool_pathname,
                                                          uint32_t policy,
                                                          const std::vector<node_id_t>& user_specified_node_ids,  // one per shard
                                                          double read_write_ratio,
                                                          uint64_t max_operation_per_second,
                                                          int64_t start_sec,
                                                          uint64_t duration_secs,
                                                          const std::string& output_filename) {

        auto object_pool = this->capi.find_object_pool(object_pool_pathname);

        uint32_t number_of_shards;
        // STEP 1 - set up the shard member selection policy
        on_subgroup_type_index(std::decay_t<decltype(capi)>::subgroup_type_order.at(object_pool.subgroup_type_index),
            number_of_shards = this->capi.template get_number_of_shards, object_pool.subgroup_index);
        if (user_specified_node_ids.size() < number_of_shards) {
            throw derecho::derecho_exception(std::string("the size of 'user_specified_node_ids' argument does not match shard number."));
        }
        for (uint32_t shard_index = 0; shard_index < number_of_shards; shard_index ++) {
            on_subgroup_type_index(std::decay_t<decltype(capi)>::subgroup_type_order.at(object_pool.subgroup_type_index),
                this->capi.template set_member_selection_policy, object_pool.subgroup_index, shard_index, static_cast<ShardMemberSelectionPolicy>(policy), user_specified_node_ids.at(shard_index));
        }
        // STEP 2 - prepare workload
        objects.clear();
        make_workload<std::string,ObjectWithStringKey>(derecho::getConfUInt32(derecho::Conf::DERECHO_MAX_P2P_REQUEST_PAYLOAD_SIZE),"raw_key_",objects);
        int64_t sleep_us = (start_sec*1e9 - static_cast<int64_t>(get_walltime()))/1e3;
        if (sleep_us > 1) {
            usleep(sleep_us);
        }
        // STEP 3 - start experiment and log
        if (this->eval_trigger_put(max_operation_per_second,duration_secs,object_pool.subgroup_type_index)) {
            TimestampLogger::flush(output_filename);
            return true;
        } else {
            return false;
        }
    });
    /**
     * RPC function that runs perf_get using the object pool interface
     *
     * @param object_pool_pathname
     * @param member_selection_policy
     * @param user_specified_node_ids
     * @param log_depth
     * @param max_operations_per_second
     * @param start_sec
     * @param duration_secs
     * @param output_filename
     * @return true if experiment completed successfully, false if there was an error
     */
    server.bind("perf_get_to_objectpool", [this](const std::string& object_pool_pathname,
                                                 uint32_t member_selection_policy,
                                                 const std::vector<node_id_t>& user_specified_node_ids,
                                                 uint64_t log_depth,
                                                 uint64_t max_operations_per_second,
                                                 int64_t start_sec,
                                                 uint64_t duration_secs,
                                                 const std::string& output_filename) {
        auto object_pool = this->capi.find_object_pool(object_pool_pathname);
        uint32_t number_of_shards;
        // Set up the shard member selection policy
        std::type_index object_pool_type_index = std::decay_t<decltype(capi)>::subgroup_type_order.at(object_pool.subgroup_type_index);
        on_subgroup_type_index(object_pool_type_index,
                               number_of_shards = this->capi.template get_number_of_shards, object_pool.subgroup_index);
        if(user_specified_node_ids.size() < number_of_shards) {
            throw derecho::derecho_exception(std::string("the size of 'user_specified_node_ids' argument does not match shard number."));
        }
        for(uint32_t shard_index = 0; shard_index < number_of_shards; shard_index++) {
            on_subgroup_type_index(object_pool_type_index,
                                   this->capi.template set_member_selection_policy, object_pool.subgroup_index, shard_index, static_cast<ShardMemberSelectionPolicy>(member_selection_policy), user_specified_node_ids.at(shard_index));
        }
        // Create workload objects
        objects.clear();
        make_workload<std::string, ObjectWithStringKey>(derecho::getConfUInt32(derecho::Conf::DERECHO_MAX_P2P_REQUEST_PAYLOAD_SIZE), object_pool_pathname + "/key_", objects);
        // Wait for start time
        int64_t sleep_us = (start_sec * 1e9 - static_cast<int64_t>(get_walltime())) / 1e3;
        if(sleep_us > 1) {
            usleep(sleep_us);
        }
        // Run experiment, then log timestamps
        try {
            if(this->eval_get(log_depth, max_operations_per_second, duration_secs, object_pool.subgroup_type_index)) {
                TimestampLogger::flush(output_filename);
                return true;
            } else {
                return false;
            }
        } catch(const std::exception& e) {
            std::cerr << "eval_get failed with exception: " << typeid(e).name() << ": " << e.what() << std::endl;
            return false;
        }
    });

    // start the worker thread asynchronously
    server.async_run(1);
}

PerfTestServer::~PerfTestServer() {
    server.stop();
}

//////////////////////////////////////
// PerfTestClient implementation    //
//////////////////////////////////////

PerfTestClient::PerfTestClient(ServiceClientAPI& capi):capi(capi) {}

void PerfTestClient::add_or_update_server(const std::string& host, uint16_t port) {
    auto key = std::make_pair(host,port);
    if (connections.find(key) != connections.end()) {
        connections.erase(key);
    }
    connections.emplace(key,std::make_unique<::rpc::client>(host,port));
}

std::vector<std::pair<std::string,uint16_t>> PerfTestClient::get_connections() {
    std::vector<std::pair<std::string,uint16_t>> result;
    for (auto& kv:connections) {
        result.emplace_back(kv.first);
    }
    return result;
}

void PerfTestClient::remove_server(const std::string& host, uint16_t port) {
    auto key = std::make_pair(host,port);
    if (connections.find(key) != connections.end()) {
        connections.erase(key);
    }
}

PerfTestClient::~PerfTestClient() {}

bool PerfTestClient::check_rpc_futures(std::map<std::pair<std::string,uint16_t>,std::future<RPCLIB_MSGPACK::object_handle>>&& futures) {
    bool ret = true;
    for(auto& kv:futures) {
        try {
            bool result = kv.second.get().as<bool>();
            dbg_default_trace("perfserver {}:{} finished with {}.",kv.first.first,kv.first.second,result);
        } catch (::rpc::rpc_error& rpce) {
            dbg_default_warn("perfserver {}:{} throws an exception. function:{}, error:{}",
                             kv.first.first,
                             kv.first.second,
                             rpce.get_function_name(),
                             rpce.get_error().as<std::string>());
            ret = false;
        } catch (...) {
            dbg_default_warn("perfserver {}:{} throws unknown exception.",
                             kv.first.first, kv.first.second);
            ret = false;
        }
    }
    return ret;
}

}
}

#endif//ENABLE_EVALUATION
